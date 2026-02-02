#include "ml307_udp.h"
#include "esp_log.h"
#include "esp_timer.h"

#define TAG "Ml307Udp"

Ml307Udp::Ml307Udp(std::shared_ptr<AtUart> at_uart, int udp_id) 
    : at_uart_(at_uart), udp_id_(udp_id) {
    
    event_group_handle_ = xEventGroupCreate();
    
    if (!event_group_handle_) {
        ESP_LOGE(TAG, "Failed to create event group");
        return;
    }

    // 注册URC回调
    urc_callback_it_ = at_uart_->RegisterUrcCallback(
        [this](const std::string& command, const std::vector<AtArgumentValue>& arguments) {
            this->handleUrc(command, arguments);
        }
    );
    
    ESP_LOGI(TAG, "Ml307Udp created with id=%d", udp_id_);
}

// 处理URC消息的私有方法
void Ml307Udp::handleUrc(const std::string& command, const std::vector<AtArgumentValue>& arguments) {
    if (command == "MIPOPEN" && arguments.size() == 2) {
        if (arguments[0].int_value == udp_id_) {
            connected_ = arguments[1].int_value == 0;
            if (connected_) {
                instance_active_ = true;
                xEventGroupClearBits(event_group_handle_, 
                    ML307_UDP_DISCONNECTED | ML307_UDP_ERROR);
                xEventGroupSetBits(event_group_handle_, ML307_UDP_CONNECTED);
                ESP_LOGI(TAG, "[%d] UDP connected", udp_id_);
            } else {
                last_error_ = arguments[1].int_value;
                xEventGroupSetBits(event_group_handle_, ML307_UDP_ERROR);
                ESP_LOGE(TAG, "[%d] UDP connection failed, error=%d", 
                        udp_id_, last_error_);
            }
        }
    } else if (command == "MIPCLOSE" && arguments.size() == 1) {
        if (arguments[0].int_value == udp_id_) {
            instance_active_ = false;
            connected_ = false;
            xEventGroupSetBits(event_group_handle_, ML307_UDP_DISCONNECTED);
            ESP_LOGI(TAG, "[%d] UDP disconnected", udp_id_);
        }
    } else if (command == "MIPSEND" && arguments.size() == 2) {
        if (arguments[0].int_value == udp_id_) {
            xEventGroupSetBits(event_group_handle_, ML307_UDP_SEND_COMPLETE);
            ESP_LOGD(TAG, "[%d] Send complete", udp_id_);
        }
    } else if (command == "MIPURC" && arguments.size() >= 4) {
        if (arguments[1].int_value == udp_id_) {
            if (arguments[0].string_value == "rudp") {
                // 提取数据并调用回调
                std::string hex_data = arguments[3].string_value;
                std::string data = at_uart_->DecodeHex(hex_data);
                
                if (!data.empty() && message_callback_) {
                    ESP_LOGD(TAG, "[%d] Received %d bytes", udp_id_, data.length());
                    message_callback_(data);
                }
            } else if (arguments[0].string_value == "disconn") {
                connected_ = false;
                instance_active_ = false;
                xEventGroupSetBits(event_group_handle_, ML307_UDP_DISCONNECTED);
                ESP_LOGI(TAG, "[%d] UDP disconnected by remote", udp_id_);
            } else {
                ESP_LOGE(TAG, "[%d] Unknown MIPURC command: %s", 
                        udp_id_, arguments[0].string_value.c_str());
            }
        }
    } else if (command == "MIPSTATE" && arguments.size() >= 5) {
        if (arguments[0].int_value == udp_id_) {
            connected_ = (arguments[4].string_value == "CONNECTED");
            instance_active_ = (arguments[4].string_value != "INITIAL");
            xEventGroupSetBits(event_group_handle_, ML307_UDP_INITIALIZED);
            ESP_LOGI(TAG, "[%d] UDP state: %s", udp_id_, 
                    arguments[4].string_value.c_str());
        }
    } else if (command == "FIFO_OVERFLOW") {
        xEventGroupSetBits(event_group_handle_, ML307_UDP_ERROR);
        ESP_LOGE(TAG, "[%d] FIFO overflow", udp_id_);
        Disconnect();
    }
}

Ml307Udp::~Ml307Udp() {
    Disconnect();
    
    if (urc_callback_it_ != std::list<UrcCallback>::iterator()) {
        at_uart_->UnregisterUrcCallback(urc_callback_it_);
    }
    
    if (event_group_handle_) {
        vEventGroupDelete(event_group_handle_);
    }
    
    ESP_LOGI(TAG, "Ml307Udp destroyed id=%d", udp_id_);
}

// 初始化UDP连接
bool Ml307Udp::initializeUdp() {
    std::string command = "AT+MIPSTATE=" + std::to_string(udp_id_);
    
    // 发送查询状态命令
    if (!at_uart_->SendCommand(command, 2000)) {
        ESP_LOGE(TAG, "[%d] Failed to query UDP state", udp_id_);
        return false;
    }
    
    // 等待初始化完成
    EventBits_t bits = xEventGroupWaitBits(
        event_group_handle_,
        ML307_UDP_INITIALIZED,
        pdTRUE,
        pdFALSE,
        pdMS_TO_TICKS(2000)
    );
    
    return (bits & ML307_UDP_INITIALIZED) != 0;
}

// 配置UDP参数
bool Ml307Udp::configureUdp() {
    // 设置HEX编码
    std::string command = "AT+MIPCFG=\"encoding\"," + 
                         std::to_string(udp_id_) + ",1,1";
    if (!at_uart_->SendCommand(command, 1000)) {
        ESP_LOGE(TAG, "[%d] Failed to set HEX encoding", udp_id_);
        return false;
    }
    
    // 关闭SSL（UDP不需要）
    command = "AT+MIPCFG=\"ssl\"," + 
             std::to_string(udp_id_) + ",0,0";
    if (!at_uart_->SendCommand(command, 1000)) {
        ESP_LOGE(TAG, "[%d] Failed to set SSL configuration", udp_id_);
        return false;
    }
    
    return true;
}

bool Ml307Udp::Connect(const std::string& host, int port) {
    ESP_LOGI(TAG, "[%d] Connecting to %s:%d", udp_id_, host.c_str(), port);
    
    // 保存当前连接地址
    current_host_ = host;
    current_port_ = port;
    
    // 清除事件标志
    xEventGroupClearBits(event_group_handle_, 
        ML307_UDP_CONNECTED | ML307_UDP_DISCONNECTED | ML307_UDP_ERROR);
    
    // 1. 初始化UDP
    if (!initializeUdp()) {
        ESP_LOGE(TAG, "[%d] Failed to initialize UDP", udp_id_);
        return false;
    }
    
    // 2. 如果已经连接，先断开
    if (instance_active_) {
        ESP_LOGI(TAG, "[%d] Closing existing connection", udp_id_);
        Disconnect();
        
        // 等待断开完成
        EventBits_t bits = xEventGroupWaitBits(
            event_group_handle_,
            ML307_UDP_DISCONNECTED,
            pdTRUE,
            pdFALSE,
            pdMS_TO_TICKS(2000)
        );
        
        if (!(bits & ML307_UDP_DISCONNECTED)) {
            ESP_LOGE(TAG, "[%d] Timeout waiting for disconnect", udp_id_);
        }
    }
    
    // 3. 配置UDP参数
    if (!configureUdp()) {
        return false;
    }
    
    // 4. 打开UDP连接
    std::string command = "AT+MIPOPEN=" + std::to_string(udp_id_) + 
                         ",\"UDP\",\"" + host + "\"," + 
                         std::to_string(port) + ",,0";
    
    int64_t start_time = esp_timer_get_time() / 1000;
    
    if (!at_uart_->SendCommand(command, UDP_CONNECT_TIMEOUT_MS)) {
        last_error_ = at_uart_->GetCmeErrorCode();
        ESP_LOGE(TAG, "[%d] Failed to send open command, error=%d", 
                udp_id_, last_error_);
        return false;
    }
    
    // 5. 等待连接完成
    EventBits_t bits = xEventGroupWaitBits(
        event_group_handle_,
        ML307_UDP_CONNECTED | ML307_UDP_ERROR,
        pdTRUE,
        pdFALSE,
        pdMS_TO_TICKS(UDP_CONNECT_TIMEOUT_MS)
    );
    
    int64_t elapsed_time = (esp_timer_get_time() / 1000) - start_time;
    
    if (bits & ML307_UDP_CONNECTED) {
        ESP_LOGI(TAG, "[%d] Connected to %s:%d in %lldms", 
                udp_id_, host.c_str(), port, elapsed_time);
        return true;
    } else if (bits & ML307_UDP_ERROR) {
        ESP_LOGE(TAG, "[%d] Connection failed to %s:%d after %lldms, error=%d", 
                udp_id_, host.c_str(), port, elapsed_time, last_error_);
        return false;
    } else {
        ESP_LOGE(TAG, "[%d] Connection timeout to %s:%d after %lldms", 
                udp_id_, host.c_str(), port, elapsed_time);
        return false;
    }
}

void Ml307Udp::Disconnect() {
    if (!instance_active_) {
        return;
    }
    
    ESP_LOGI(TAG, "[%d] Disconnecting", udp_id_);
    
    // 发送断开命令
    std::string command = "AT+MIPCLOSE=" + std::to_string(udp_id_);
    at_uart_->SendCommand(command, 2000);
    
    connected_ = false;
    instance_active_ = false;
}

int Ml307Udp::Send(const std::string& data, int timeout_ms) {
    if (!connected_) {
        ESP_LOGE(TAG, "[%d] Not connected, cannot send", udp_id_);
        return -1;
    }
    
    if (data.empty()) {
        ESP_LOGW(TAG, "[%d] Empty data to send", udp_id_);
        return 0;
    }
    
    const size_t MAX_DIRECT_MODE_SIZE = 1460 / 2;  // 730字节
    const size_t MAX_DATA_MODE_SIZE = 8192;        // 8192字节
    
    if (data.size() > MAX_DATA_MODE_SIZE) {
        ESP_LOGE(TAG, "[%d] Data size %d exceeds maximum %d", 
                udp_id_, data.size(), MAX_DATA_MODE_SIZE);
        return -1;
    }
    
    int result = -1;
    if(timeout_ms<0) timeout_ms = UDP_SEND_TIMEOUT_MS;
    
    if (data.size() <= MAX_DIRECT_MODE_SIZE) {
        // 方式1：命令中直接包含数据（HEX编码）
        std::string command = "AT+MIPSEND=" + std::to_string(udp_id_) + 
                             "," + std::to_string(data.size()) + ",";
        
        // 添加HEX编码的数据
        at_uart_->EncodeHexAppend(command, data.data(), data.size());
    	command += ",,,1";
    	command += "\r\n";
        
        // 清除发送完成标志
        xEventGroupClearBits(event_group_handle_, ML307_UDP_SEND_COMPLETE);
        
        // 发送命令（包含数据）
        int64_t start_time = esp_timer_get_time() / 1000;
        
        if (!at_uart_->SendCommand(command, timeout_ms, false)) {
            //ESP_LOGE(TAG, "[%d] Failed to send data in direct mode", udp_id_);
            return -1;
        }
        
        // 等待发送完成
        EventBits_t bits = xEventGroupWaitBits(
            event_group_handle_,
            ML307_UDP_SEND_COMPLETE,
            pdTRUE,
            pdFALSE,
            pdMS_TO_TICKS(150)
        );
        
        int64_t elapsed_time = (esp_timer_get_time() / 1000) - start_time;
        
        if (bits & ML307_UDP_SEND_COMPLETE) {
            //ESP_LOGI(TAG, "[%d] Sent %d bytes in direct mode, elapsed %lldms", 
            //        udp_id_, data.size(), elapsed_time);
            result = data.size();
        } else {
            //ESP_LOGE(TAG, "[%d] Direct mode send timeout after %lldms", udp_id_, elapsed_time);
            result = -1;
        }
    } else {
        // 方式2：数据模式（">"模式）
        // 发送命令指定长度，不包含数据
        std::string command = "AT+MIPSEND=" + std::to_string(udp_id_) + 
                             "," + std::to_string(data.size())+",,,,1";
        
        // 清除发送完成标志
        xEventGroupClearBits(event_group_handle_, ML307_UDP_SEND_COMPLETE);
        
        int64_t start_time = esp_timer_get_time() / 1000;
        
        // 使用 SendCommandWithData 发送命令，然后发送原始数据
        if (!at_uart_->SendCommandWithData(command, timeout_ms, true, 
                                          data.data(), data.size())) {
            ESP_LOGE(TAG, "[%d] Failed to send data in data mode", udp_id_);
            return -1;
        }
        
        // 等待发送完成
        EventBits_t bits = xEventGroupWaitBits(
            event_group_handle_,
            ML307_UDP_SEND_COMPLETE,
            pdTRUE,
            pdFALSE,
            pdMS_TO_TICKS(timeout_ms)
        );
        
        int64_t elapsed_time = (esp_timer_get_time() / 1000) - start_time;
        
        if (bits & ML307_UDP_SEND_COMPLETE) {
            //ESP_LOGI(TAG, "[%d] Sent %d bytes in data mode, elapsed %lldms", 
            //        udp_id_, data.size(), elapsed_time);
            result = data.size();
        } else {
            ESP_LOGE(TAG, "[%d] Data mode send timeout after %lldms", udp_id_, elapsed_time);
            result = -1;
        }
    }
    
    return result;
}

int Ml307Udp::GetLastError() {
    return last_error_;
}