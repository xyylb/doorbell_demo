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
static char s_ota_md5[64];

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
                    // 提取数据
                    if (arguments[5].type == AtArgumentValue::Type::String) {
                        const std::string& data = arguments[5].string_value;

                        if (!data.empty() && cur_len > 0) {
                            // 写入 OTA 分区
                            esp_err_t err = esp_ota_write(s_ota_context->ota_handle,
                                                          data.data(),
                                                          data.length());
                            if (err != ESP_OK) {
                                ESP_LOGE(TAG, "esp_ota_write 失败: %s", esp_err_to_name(err));
                                s_ota_context->download_error = true;
                                xEventGroupSetBits(s_ota_context->event_group, OTA_EVENT_ERROR);
                            } else {
                                s_ota_context->total_received += data.length();

                                // 每接收 10KB 打印一次进度
                                if (s_ota_context->total_received % (10 * 1024) < data.length()) {
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

                        // 检查是否下载完成
                        // cur_len == 0 表示 chunked 模式下载完成
                        // 或者 sum_len == content_len 表示普通模式下载完成
                        if (cur_len == 0 || (content_len > 0 && sum_len >= content_len)) {
                            ESP_LOGI(TAG, "固件下载完成，总共接收: %d 字节", s_ota_context->total_received);
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

int get_firmware_version(void)
{
    const esp_partition_t* running = esp_ota_get_running_partition();
    esp_app_desc_t running_app_desc;
    if (esp_ota_get_partition_description(running, &running_app_desc) == ESP_OK) {
        return atoi(running_app_desc.version);
    }
    return 0;
}

static bool validate_running_version()
{
    const esp_partition_t* running = esp_ota_get_running_partition();
    esp_app_desc_t running_app_desc;
    if (esp_ota_get_partition_description(running, &running_app_desc) == ESP_OK) {
        ESP_LOGI(TAG, "Current firmware version: %s", running_app_desc.version);
    }
    return true;
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
    ESP_LOGI(TAG, "当前固件版本: %d", get_firmware_version());

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

    // 设置超时时间
    snprintf(at_cmd, sizeof(at_cmd), "AT+MHTTPCFG=\"timeout\",%d,60,0", s_ota_context->http_id);
    at_uart->SendCommand(at_cmd, 1000);
    vTaskDelay(pdMS_TO_TICKS(200));

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

        // 发送 GET 请求（method=1 表示 GET）
        snprintf(at_cmd, sizeof(at_cmd), "AT+MHTTPREQUEST=%d,1,0,\"%s\"",
                 s_ota_context->http_id, path.c_str());
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

    // 等待下载完成（最多 5 分钟）
    {
        auto bits = xEventGroupWaitBits(s_ota_context->event_group,
                                        OTA_EVENT_DOWNLOAD_COMPLETE | OTA_EVENT_ERROR,
                                        pdTRUE,
                                        pdFALSE,
                                        pdMS_TO_TICKS(300000));  // 5 分钟超时

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

    // 步骤 6: 验证固件
    ESP_LOGI(TAG, "步骤 6: 验证固件");

    // 获取当前运行分区信息
    const esp_partition_t* running = esp_ota_get_running_partition();
    esp_app_desc_t running_app_info;
    if (esp_ota_get_partition_description(running, &running_app_info) == ESP_OK) {
        ESP_LOGI(TAG, "当前固件版本: %s", running_app_info.version);
        ESP_LOGI(TAG, "当前固件项目: %s", running_app_info.project_name);
        ESP_LOGI(TAG, "当前固件编译时间: %s %s", running_app_info.date, running_app_info.time);
    }

    // 步骤 7: 完成 OTA 写入
    ESP_LOGI(TAG, "步骤 7: 完成 OTA 写入");

    {
        esp_err_t err = esp_ota_end(s_ota_context->ota_handle);
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

    s_ota_in_progress = true;
    xTaskCreate(ota_task, "ota_task", 8192, NULL, 5, NULL);
}
