/**
 * @file otaupgrade.cpp
 * @brief ESP32 OTA 升级实现，使用 ML307 4G 模块通过 HTTP 下载固件
 *
 * 实现原理：
 * 1. 使用 ML307 的 HTTP AT 命令创建 HTTP 客户端实例
 * 2. 发送 GET 请求下载固件文件
 * 3. 通过 URC 回调接收 HTTP 响应数据
 * 4. 边接收边写入 ESP32 的 OTA 分区（无需在 ML307 文件系统中缓存）
 * 5. 下载完成后设置新的启动分区并重启
 *
 * 优点：
 * - 不占用 ML307 的文件系统空间
 * - 实时流式传输，内存占用小
 * - 支持大文件 OTA 升级
 */

#include "otaupgrade.h"
#include "network_4g.h"
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <string.h>
#include <string>
#include <list>
#include <vector>

static const char* TAG = "OTA";

// OTA 升级状态标志
static bool s_ota_in_progress = false;
static char s_ota_url[256];

// OTA 上下文结构
struct OtaContext {
    esp_ota_handle_t ota_handle;
    const esp_partition_t* update_partition;
    int http_id;
    int total_received;
    int content_length;
    bool header_received;
    bool download_complete;
    bool download_error;
    bool http_created;  // HTTP 实例是否创建成功
    SemaphoreHandle_t data_semaphore;
    EventGroupHandle_t event_group;  // 用于等待 HTTP 创建完成
    std::list<UrcCallback>::iterator urc_iterator;
    std::shared_ptr<AtUart> at_uart;  // AtUart 实例指针，用于 HEX 解码
};

#define OTA_EVENT_HTTP_CREATED  BIT0
#define OTA_EVENT_HEADER_RECEIVED BIT1
#define OTA_EVENT_DOWNLOAD_COMPLETE BIT2
#define OTA_EVENT_ERROR BIT3

static OtaContext* s_ota_context = nullptr;

// HTTP URC 回调处理函数
static void http_urc_callback(const std::string& command, const std::vector<AtArgumentValue>& arguments)
{
    if (!s_ota_context) {
        return;
    }

    // 处理 MHTTPCREATE 响应（注意：command 不包含前导的 +）
    if (command == "MHTTPCREATE" && arguments.size() >= 1) {
        s_ota_context->http_id = arguments[0].int_value;
        s_ota_context->http_created = true;
        ESP_LOGI(TAG, "URC: HTTP 实例创建成功，ID: %d", s_ota_context->http_id);
        xEventGroupSetBits(s_ota_context->event_group, OTA_EVENT_HTTP_CREATED);
        return;
    }

    // 处理 MHTTPURC（注意：command 不包含前导的 +）
    if (command == "MHTTPURC" && arguments.size() >= 4) {
        if (arguments[0].type == AtArgumentValue::Type::String &&
            arguments[0].string_value == "header") {

            // +MHTTPURC: "header",<httpid>,<code>,<header_len>,<data>
            int httpid = arguments[1].int_value;
            int code = arguments[2].int_value;
            // int header_len = arguments[3].int_value;  // 暂未使用

            if (httpid == s_ota_context->http_id) {
                ESP_LOGI(TAG, "收到 HTTP 响应头，状态码: %d", code);

                if (code == 200) {
                    s_ota_context->header_received = true;

                    // 尝试从 header 中解析 Content-Length
                    if (arguments.size() >= 5 && arguments[4].type == AtArgumentValue::Type::String) {
                        std::string header_data = arguments[4].string_value;
                        size_t pos = header_data.find("Content-Length:");
                        if (pos != std::string::npos) {
                            size_t start = pos + 15;
                            while (start < header_data.length() && header_data[start] == ' ') start++;
                            size_t end = start;
                            while (end < header_data.length() && isdigit(header_data[end])) end++;
                            if (end > start) {
                                s_ota_context->content_length = atoi(header_data.substr(start, end - start).c_str());
                                ESP_LOGI(TAG, "固件大小: %d 字节", s_ota_context->content_length);
                            }
                        }
                    }

                    xEventGroupSetBits(s_ota_context->event_group, OTA_EVENT_HEADER_RECEIVED);
                } else {
                    ESP_LOGE(TAG, "HTTP 请求失败，状态码: %d", code);
                    s_ota_context->download_error = true;
                    xEventGroupSetBits(s_ota_context->event_group, OTA_EVENT_ERROR);
                }
            }
        }
        // 处理 +MHTTPURC: "content"
        else if (arguments[0].type == AtArgumentValue::Type::String &&
                 arguments[0].string_value == "content") {

            // +MHTTPURC: "content",<httpid>,<content_len>,<sum_len>,<cur_len>,<data>
            if (arguments.size() >= 6) {
                int httpid = arguments[1].int_value;
                int content_len = arguments[2].int_value;
                int sum_len = arguments[3].int_value;
                int cur_len = arguments[4].int_value;

                if (httpid == s_ota_context->http_id) {
                    // 调试日志：打印关键参数
                    static int packet_count = 0;
                    packet_count++;

                    // 前 10 个包和每 50 个包打印一次详细信息
                    if (packet_count <= 10 || packet_count % 50 == 0) {
                        ESP_LOGI(TAG, "数据包 #%d: content_len=%d, sum_len=%d, cur_len=%d, 已接收=%d",
                                packet_count, content_len, sum_len, cur_len, s_ota_context->total_received);
                    }

                    // 提取数据
                    if (arguments[5].type == AtArgumentValue::Type::String) {
                        const std::string& hex_data = arguments[5].string_value;

                        // 调试：打印 HEX 数据长度
                        if (packet_count <= 3) {
                            ESP_LOGI(TAG, "数据包 #%d: HEX 字符串长度=%d, cur_len=%d",
                                    packet_count, hex_data.length(), cur_len);
                        }

                        if (!hex_data.empty() && cur_len > 0) {
                            // 解码 HEX 数据为二进制数据
                            // ML307 使用 HEX 编码传输数据以避免二进制数据中的特殊字符干扰 AT 命令解析
                            std::string binary_data;
                            s_ota_context->at_uart->DecodeHexAppend(binary_data, hex_data.c_str(), hex_data.length());

                            // 验证解码后的数据长度
                            if (binary_data.length() != (size_t)cur_len) {
                                ESP_LOGW(TAG, "数据包 #%d: 解码后长度不匹配！期望 %d，实际 %d",
                                        packet_count, cur_len, binary_data.length());
                            }

                            // 调试：打印前几个包的数据
                            if (packet_count <= 3 && binary_data.length() >= 16) {
                                ESP_LOGI(TAG, "数据包 #%d 前 16 字节: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
                                        packet_count,
                                        (uint8_t)binary_data[0], (uint8_t)binary_data[1], (uint8_t)binary_data[2], (uint8_t)binary_data[3],
                                        (uint8_t)binary_data[4], (uint8_t)binary_data[5], (uint8_t)binary_data[6], (uint8_t)binary_data[7],
                                        (uint8_t)binary_data[8], (uint8_t)binary_data[9], (uint8_t)binary_data[10], (uint8_t)binary_data[11],
                                        (uint8_t)binary_data[12], (uint8_t)binary_data[13], (uint8_t)binary_data[14], (uint8_t)binary_data[15]);
                            }

                            // 写入 OTA 分区
                            esp_err_t err = esp_ota_write(s_ota_context->ota_handle,
                                                          binary_data.data(),
                                                          binary_data.length());
                            if (err != ESP_OK) {
                                ESP_LOGE(TAG, "esp_ota_write 失败: %s", esp_err_to_name(err));
                                s_ota_context->download_error = true;
                                xEventGroupSetBits(s_ota_context->event_group, OTA_EVENT_ERROR);
                            } else {
                                s_ota_context->total_received += binary_data.length();

                                // 每接收 100KB 打印一次进度
                                static int last_progress = 0;
                                int current_progress = s_ota_context->total_received / (100 * 1024);
                                if (current_progress > last_progress) {
                                    last_progress = current_progress;
                                    if (s_ota_context->content_length > 0) {
                                        ESP_LOGI(TAG, "OTA 进度: %d/%d 字节 (%.1f%%)",
                                                s_ota_context->total_received,
                                                s_ota_context->content_length,
                                                (float)s_ota_context->total_received * 100 / s_ota_context->content_length);
                                    } else {
                                        ESP_LOGI(TAG, "OTA 进度: %d 字节", s_ota_context->total_received);
                                    }
                                }
                            }
                        }

                        // 改进的下载完成判断逻辑
                        // 1. cur_len == 0 表示这是最后一个数据包（chunked 模式或普通模式的结束标志）
                        // 2. 如果知道 content_length，则验证 sum_len 是否达到预期大小
                        bool is_complete = false;

                        if (cur_len == 0) {
                            // cur_len == 0 是明确的结束信号
                            ESP_LOGI(TAG, "收到结束信号 (cur_len=0), packet_count=%d", packet_count);
                            is_complete = true;
                        } else if (content_len > 0 && sum_len >= content_len) {
                            // sum_len 达到了 content_len，也认为完成
                            ESP_LOGI(TAG, "sum_len (%d) 达到 content_len (%d), packet_count=%d",
                                    sum_len, content_len, packet_count);
                            is_complete = true;
                        }

                        if (is_complete) {
                            ESP_LOGI(TAG, "固件下载完成！");
                            ESP_LOGI(TAG, "  - 数据包总数: %d", packet_count);
                            ESP_LOGI(TAG, "  - 总共接收: %d 字节", s_ota_context->total_received);
                            ESP_LOGI(TAG, "  - Content-Length: %d 字节", s_ota_context->content_length);
                            ESP_LOGI(TAG, "  - sum_len: %d, content_len: %d", sum_len, content_len);

                            // 验证下载完整性
                            if (s_ota_context->content_length > 0) {
                                if (s_ota_context->total_received < s_ota_context->content_length) {
                                    ESP_LOGW(TAG, "警告：接收字节数 (%d) 小于 Content-Length (%d)，可能数据不完整！",
                                            s_ota_context->total_received, s_ota_context->content_length);
                                }
                            }

                            s_ota_context->download_complete = true;
                            xEventGroupSetBits(s_ota_context->event_group, OTA_EVENT_DOWNLOAD_COMPLETE);
                        }
                    }
                }
            }
        }
        // 处理 +MHTTPURC: "err"
        else if (arguments[0].type == AtArgumentValue::Type::String &&
                 arguments[0].string_value == "err") {

            // +MHTTPURC: "err",<httpid>,<error_code>
            if (arguments.size() >= 3) {
                int httpid = arguments[1].int_value;
                int error_code = arguments[2].int_value;

                if (httpid == s_ota_context->http_id) {
                    ESP_LOGE(TAG, "HTTP 下载错误，错误码: %d", error_code);
                    s_ota_context->download_error = true;
                    xEventGroupSetBits(s_ota_context->event_group, OTA_EVENT_ERROR);
                }
            }
        }
    }
}

// OTA 任务：边下载边写入 OTA 分区
static void ota_task(void *param)
{
    AtModem* modem = Network4g::GetModemInstance();
    if (!modem) {
        ESP_LOGW(TAG, "Modem 实例未找到");
        s_ota_in_progress = false;
        vTaskDelete(NULL);
        return;
    }

    // 检查网络状态
    if (!modem->network_ready()) {
        ESP_LOGE(TAG, "网络未就绪，无法进行 OTA 升级");
        s_ota_in_progress = false;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "开始 OTA 升级，URL: %s", s_ota_url);
    ESP_LOGI(TAG, "当前固件版本: %d", FIRMWARE_VERSION);

    // 获取 AtUart 实例
    auto at_uart = modem->GetAtUart();
    if (!at_uart) {
        ESP_LOGE(TAG, "无法获取 AtUart 实例");
        s_ota_in_progress = false;
        vTaskDelete(NULL);
        return;
    }

    char at_cmd[512];
    bool success = false;

    // 创建 OTA 上下文
    s_ota_context = new OtaContext();
    if (!s_ota_context) {
        ESP_LOGE(TAG, "内存分配失败");
        s_ota_in_progress = false;
        vTaskDelete(NULL);
        return;
    }

    memset(s_ota_context, 0, sizeof(OtaContext));
    s_ota_context->at_uart = at_uart;  // 保存 AtUart 指针用于 HEX 解码
    s_ota_context->data_semaphore = xSemaphoreCreateBinary();
    s_ota_context->event_group = xEventGroupCreate();

    if (!s_ota_context->data_semaphore || !s_ota_context->event_group) {
        ESP_LOGE(TAG, "创建同步对象失败");
        if (s_ota_context->data_semaphore) {
            vSemaphoreDelete(s_ota_context->data_semaphore);
        }
        if (s_ota_context->event_group) {
            vEventGroupDelete(s_ota_context->event_group);
        }
        delete s_ota_context;
        s_ota_context = nullptr;
        s_ota_in_progress = false;
        vTaskDelete(NULL);
        return;
    }

    // 注册 URC 回调
    s_ota_context->urc_iterator = at_uart->RegisterUrcCallback(http_urc_callback);

    // 步骤 1: 创建 HTTP 实例
    ESP_LOGI(TAG, "步骤 1: 创建 HTTP 实例");

    // 解析 URL，提取 host 和 port
    std::string url_str(s_ota_url);
    std::string host_with_port;

    // 查找协议结束位置
    size_t protocol_end = url_str.find("://");
    if (protocol_end != std::string::npos) {
        // 查找路径开始位置
        size_t path_start = url_str.find('/', protocol_end + 3);
        if (path_start != std::string::npos) {
            // 提取 host:port 部分
            host_with_port = url_str.substr(0, path_start);
        } else {
            // 没有路径，整个 URL 就是 host
            host_with_port = url_str;
        }
    } else {
        ESP_LOGE(TAG, "URL 格式错误: %s", s_ota_url);
        goto cleanup;
    }

    ESP_LOGI(TAG, "HTTP Host: %s", host_with_port.c_str());

    snprintf(at_cmd, sizeof(at_cmd), "AT+MHTTPCREATE=\"%s\"", host_with_port.c_str());
    ESP_LOGI(TAG, "发送命令: %s", at_cmd);

    // 发送命令
    if (!at_uart->SendCommand(at_cmd, 10000)) {
        ESP_LOGE(TAG, "发送 AT 命令失败");
        goto cleanup;
    }

    // 等待 URC 回调设置 HTTP 创建完成事件
    ESP_LOGI(TAG, "等待 HTTP 实例创建完成...");
    {
        auto bits = xEventGroupWaitBits(s_ota_context->event_group,
                                        OTA_EVENT_HTTP_CREATED,
                                        pdTRUE,
                                        pdFALSE,
                                        pdMS_TO_TICKS(10000));  // 10 秒超时

        if (!(bits & OTA_EVENT_HTTP_CREATED)) {
            ESP_LOGE(TAG, "等待 HTTP 实例创建超时");
            goto cleanup;
        }

        if (!s_ota_context->http_created) {
            ESP_LOGE(TAG, "HTTP 实例创建失败");
            goto cleanup;
        }

        ESP_LOGI(TAG, "HTTP 实例创建成功，ID: %d", s_ota_context->http_id);
    }

    // 步骤 2: 配置 HTTP 参数
    ESP_LOGI(TAG, "步骤 2: 配置 HTTP 参数");

    // 设置为非缓存模式（实时接收数据）
    snprintf(at_cmd, sizeof(at_cmd), "AT+MHTTPCFG=\"cached\",%d,0", s_ota_context->http_id);
    at_uart->SendCommand(at_cmd, 1000);
    vTaskDelay(pdMS_TO_TICKS(200));

    // 设置编码模式为原始数据（0=ASCII原始数据，1=HEX，2=转义字符）
    // input_format=0, output_format=0 表示输入输出都是原始数据
    snprintf(at_cmd, sizeof(at_cmd), "AT+MHTTPCFG=\"encoding\",%d,0,0", s_ota_context->http_id);
    at_uart->SendCommand(at_cmd, 1000);
    vTaskDelay(pdMS_TO_TICKS(200));
    ESP_LOGI(TAG, "设置编码模式为原始数据（临时）");

    // 设置数据输出流控，提高传输速度
    // frag_size: 每次最多输出 512 字节
    // interval: 每次输出间隔 1ms
    // 理论速度：512字节/1ms = 512KB/s，约5秒下载2.64MB
    // 4096 有概率溢出，改成了2048
    snprintf(at_cmd, sizeof(at_cmd), "AT+MHTTPCFG=\"fragment\",%d,2048,1", s_ota_context->http_id);
    at_uart->SendCommand(at_cmd, 1000);
    vTaskDelay(pdMS_TO_TICKS(200));
    ESP_LOGI(TAG, "设置数据流控：512 字节/包，间隔 1ms（高速模式）");

    // 设置超时时间
    snprintf(at_cmd, sizeof(at_cmd), "AT+MHTTPCFG=\"timeout\",%d,60,0", s_ota_context->http_id);
    at_uart->SendCommand(at_cmd, 1000);
    vTaskDelay(pdMS_TO_TICKS(200));

    // 重新设置为 HEX 编码模式（用于接收数据）
    // 这样可以避免二进制数据中的特殊字符干扰 AT 命令解析
    snprintf(at_cmd, sizeof(at_cmd), "AT+MHTTPCFG=\"encoding\",%d,1,1", s_ota_context->http_id);
    at_uart->SendCommand(at_cmd, 1000);
    vTaskDelay(pdMS_TO_TICKS(200));
    ESP_LOGI(TAG, "设置编码模式为 HEX（用于接收数据） ");

    // 步骤 3: 初始化 OTA 分区
    ESP_LOGI(TAG, "步骤 3: 初始化 OTA 分区");

    s_ota_context->update_partition = esp_ota_get_next_update_partition(NULL);
    if (!s_ota_context->update_partition) {
        ESP_LOGE(TAG, "无法获取 OTA 更新分区");
        goto cleanup;
    }

    ESP_LOGI(TAG, "OTA 更新分区: %s, 偏移: 0x%lx, 大小: %lu",
             s_ota_context->update_partition->label,
             s_ota_context->update_partition->address,
             s_ota_context->update_partition->size);

    {
        esp_err_t err = esp_ota_begin(s_ota_context->update_partition, OTA_SIZE_UNKNOWN,
                                       &s_ota_context->ota_handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_begin 失败: %s", esp_err_to_name(err));
            goto cleanup;
        }
    }

    // 步骤 4: 发送 GET 请求开始下载
    ESP_LOGI(TAG, "步骤 4: 发送 GET 请求");

    // 解析 URL 获取 path
    {
        std::string url_str(s_ota_url);
        std::string path = "/";

        // 查找第三个斜杠后的内容作为 path
        size_t protocol_end = url_str.find("://");
        if (protocol_end != std::string::npos) {
            size_t path_start = url_str.find('/', protocol_end + 3);
            if (path_start != std::string::npos) {
                path = url_str.substr(path_start);
            }
        }

        ESP_LOGI(TAG, "请求路径: %s", path.c_str());

        // 由于设置了 encoding=1,1，path 需要进行 HEX 编码
        std::string hex_path = at_uart->EncodeHex(path);

        // 发送 GET 请求（method=1 表示 GET）
        snprintf(at_cmd, sizeof(at_cmd), "AT+MHTTPREQUEST=%d,1,0,%s",
                 s_ota_context->http_id, hex_path.c_str());
        at_uart->SendCommand(at_cmd, 10000);
    }

    // 步骤 5: 等待下载完成
    ESP_LOGI(TAG, "步骤 5: 等待固件下载完成");

    // 等待 header
    {
        auto bits = xEventGroupWaitBits(s_ota_context->event_group,
                                        OTA_EVENT_HEADER_RECEIVED | OTA_EVENT_ERROR,
                                        pdTRUE,
                                        pdFALSE,
                                        pdMS_TO_TICKS(30000));  // 30 秒超时

        if (bits & OTA_EVENT_ERROR) {
            ESP_LOGE(TAG, "HTTP 请求出错");
            goto cleanup;
        }

        if (!(bits & OTA_EVENT_HEADER_RECEIVED)) {
            ESP_LOGE(TAG, "等待 HTTP 响应头超时");
            goto cleanup;
        }

        if (!s_ota_context->header_received) {
            ESP_LOGE(TAG, "HTTP 请求失败");
            goto cleanup;
        }
    }

    // 等待下载完成（最多 2 分钟）
    // 高速模式（512字节/1ms），下载 2.64MB 约需 5-10 秒
    {
        auto bits = xEventGroupWaitBits(s_ota_context->event_group,
                                        OTA_EVENT_DOWNLOAD_COMPLETE | OTA_EVENT_ERROR,
                                        pdTRUE,
                                        pdFALSE,
                                        pdMS_TO_TICKS(1200000));  // 2 分钟超时

        if (bits & OTA_EVENT_ERROR) {
            ESP_LOGE(TAG, "固件下载出错");
            goto cleanup;
        }

        if (!(bits & OTA_EVENT_DOWNLOAD_COMPLETE)) {
            ESP_LOGE(TAG, "固件下载超时");
            goto cleanup;
        }

        if (!s_ota_context->download_complete) {
            ESP_LOGE(TAG, "固件下载失败");
            goto cleanup;
        }
    }

    ESP_LOGI(TAG, "固件下载完成，总共接收: %d 字节", s_ota_context->total_received);

    // 检查下载大小是否与 Content-Length 匹配
    if (s_ota_context->content_length > 0) {
        if (s_ota_context->total_received != s_ota_context->content_length) {
            ESP_LOGE(TAG, "错误：下载大小不匹配！");
            ESP_LOGE(TAG, "期望: %d 字节, 实际: %d 字节",
                     s_ota_context->content_length, s_ota_context->total_received);
            ESP_LOGE(TAG, "固件可能不完整，终止 OTA 升级");
            goto cleanup;
        } else {
            ESP_LOGI(TAG, "下载大小验证通过：%d 字节", s_ota_context->total_received);
        }
    } else {
        ESP_LOGW(TAG, "警告：无法获取 Content-Length，跳过大小验证");
    }

    // 步骤 6: 验证固件
    ESP_LOGI(TAG, "步骤 6: 验证固件");

    // 获取当前运行分区信息
    {
        const esp_partition_t* running = esp_ota_get_running_partition();
        esp_app_desc_t running_app_info;
        if (esp_ota_get_partition_description(running, &running_app_info) == ESP_OK) {
            ESP_LOGI(TAG, "当前固件版本: %s", running_app_info.version);
            ESP_LOGI(TAG, "当前固件项目: %s", running_app_info.project_name);
            ESP_LOGI(TAG, "当前固件编译时间: %s %s", running_app_info.date, running_app_info.time);
        }
    }

    // 步骤 7: 完成 OTA 写入
    ESP_LOGI(TAG, "步骤 7: 完成 OTA 写入");
    ESP_LOGI(TAG, "正在验证固件，这可能需要几十秒时间...");

    {
        // 在固件验证期间，临时从看门狗中删除当前任务
        // 因为 SHA256 计算可能需要很长时间（对于 2.64MB 固件可能需要 30-60 秒）
        TaskHandle_t current_task = xTaskGetCurrentTaskHandle();
        esp_err_t wdt_err = esp_task_wdt_delete(current_task);
        if (wdt_err == ESP_OK) {
            ESP_LOGI(TAG, "已临时禁用看门狗");
        } else {
            ESP_LOGW(TAG, "无法禁用看门狗: %s (任务可能未注册)", esp_err_to_name(wdt_err));
        }

        esp_err_t err = esp_ota_end(s_ota_context->ota_handle);

        // 验证完成后，重新添加到看门狗（如果之前成功删除）
        if (wdt_err == ESP_OK) {
            esp_task_wdt_add(current_task);
            ESP_LOGI(TAG, "已重新启用看门狗");
        }

        if (err != ESP_OK) {
            if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
                ESP_LOGE(TAG, "固件验证失败！");
                ESP_LOGE(TAG, "可能的原因：");
                ESP_LOGE(TAG, "1. 固件文件损坏");
                ESP_LOGE(TAG, "2. 固件与硬件版本不匹配");
                ESP_LOGE(TAG, "3. 固件不是为当前芯片编译的");

                // 尝试获取新固件的信息
                esp_app_desc_t new_app_info;
                if (esp_ota_get_partition_description(s_ota_context->update_partition, &new_app_info) == ESP_OK) {
                    ESP_LOGI(TAG, "新固件版本: %s", new_app_info.version);
                    ESP_LOGI(TAG, "新固件项目: %s", new_app_info.project_name);
                    ESP_LOGI(TAG, "新固件编译时间: %s %s", new_app_info.date, new_app_info.time);
                } else {
                    ESP_LOGE(TAG, "无法读取新固件信息，固件可能已损坏");
                }
            } else {
                ESP_LOGE(TAG, "esp_ota_end 失败: %s", esp_err_to_name(err));
            }
            goto cleanup;
        }
    }

    // 步骤 8: 设置启动分区
    ESP_LOGI(TAG, "步骤 8: 设置启动分区");

    {
        esp_err_t err = esp_ota_set_boot_partition(s_ota_context->update_partition);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_set_boot_partition 失败: %s", esp_err_to_name(err));
            goto cleanup;
        }
    }

    success = true;
    ESP_LOGI(TAG, "OTA 升级成功！");

cleanup:
    // 清理资源
    if (s_ota_context) {
        // 删除 HTTP 实例
        if (s_ota_context->http_id >= 0) {
            snprintf(at_cmd, sizeof(at_cmd), "AT+MHTTPDEL=%d", s_ota_context->http_id);
            at_uart->SendCommand(at_cmd, 1000);
        }

        // 注销 URC 回调
        at_uart->UnregisterUrcCallback(s_ota_context->urc_iterator);

        // 删除同步对象
        if (s_ota_context->data_semaphore) {
            vSemaphoreDelete(s_ota_context->data_semaphore);
        }
        if (s_ota_context->event_group) {
            vEventGroupDelete(s_ota_context->event_group);
        }

        delete s_ota_context;
        s_ota_context = nullptr;
    }

    s_ota_in_progress = false;

    if (success) {
        ESP_LOGI(TAG, "准备重启系统...");
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
    } else {
        ESP_LOGE(TAG, "OTA 升级失败");
    }

    vTaskDelete(NULL);
}

void ota_start_upgrade(const char* url, const char* md5)
{
    if (s_ota_in_progress) {
        ESP_LOGW(TAG, "OTA already in progress");
        return;
    }

    strncpy(s_ota_url, url, sizeof(s_ota_url) - 1);
    s_ota_url[sizeof(s_ota_url) - 1] = '\0';

    // md5 参数暂未使用，保留用于未来扩展
    (void)md5;

    s_ota_in_progress = true;
    xTaskCreate(ota_task, "ota_task", 8192, NULL, 5, NULL);
}
