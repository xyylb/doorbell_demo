#include "esp_log.h"
#include "at_modem.h"
#include "network_4g.h"

static const char *TAG = "network_4g";

namespace Network4g {
    // 将 modem 声明为命名空间级别的静态常量指针
    // 使用 nullptr 初始化，在 init() 函数中完成实际赋值
    
    static std::unique_ptr<AtModem> modem = nullptr;
    
    static std::unique_ptr<Mqtt> mqtt = nullptr;
    
    void init(void) {
        ESP_LOGI(TAG, "开始检测4G网络");
        // 自动检测并初始化模组
        modem = AtModem::Detect(GPIO_NUM_17, GPIO_NUM_18, GPIO_NUM_4, 921600);
        
        
        if (!modem) {
            ESP_LOGE(TAG, "模组检测失败");
            return;
        }
        
        // 设置网络状态回调
        modem->OnNetworkStateChanged([](bool ready) {
            ESP_LOGI(TAG, "网络状态: %s", ready ? "已连接" : "已断开");
            if (ready) {
                // 网络连接成功，重新连接 MQTT
                //Network4g::test();
            }
        });
        
        // 等待网络就绪
        NetworkStatus status = modem->WaitForNetworkReady(30000);
        if (status != NetworkStatus::Ready) {
            ESP_LOGE(TAG, "网络连接失败");
            return;
        }
        
        // 打印模组信息
        ESP_LOGI(TAG, "模组版本: %s", modem->GetModuleRevision().c_str());
        ESP_LOGI(TAG, "IMEI: %s", modem->GetImei().c_str());
        ESP_LOGI(TAG, "ICCID: %s", modem->GetIccid().c_str());
        ESP_LOGI(TAG, "运营商: %s", modem->GetCarrierName().c_str());
        ESP_LOGI(TAG, "信号强度: %d", modem->GetCsq());
    }
    
    void test(){
        // 先检查 modem 是否初始化成功，避免空指针访问
        if (!modem) {
            ESP_LOGE(TAG, "模组未初始化，无法执行 HTTP 请求");
            return;
        }
        
        // 创建 HTTP 客户端
/*        auto http = modem->CreateHttp(0);
        // 设置请求头
        http->SetHeader("User-Agent", "Xiaozhi/3.0.0");
        http->SetTimeout(10000);
        
        // 发送 GET 请求
        if (http->Open("GET", "https://httpbin.org/json")) {
            ESP_LOGI(TAG, "HTTP 状态码: %d", http->GetStatusCode());
            ESP_LOGI(TAG, "响应内容长度: %zu bytes", http->GetBodyLength());
            
            // 读取响应内容
            std::string response = http->ReadAll();
            ESP_LOGI(TAG, "响应内容: %s", response.c_str());
            
            http->Close();
        } else {
            ESP_LOGE(TAG, "HTTP 请求失败");
        }*/
        
        
	    ESP_LOGI(TAG, "开始 MQTT 测试");
	
	    // 创建 MQTT 客户端
	    mqtt = modem->CreateMqtt(0);
	    
	    // 设置回调函数
	    mqtt->OnConnected([]() {
	        ESP_LOGI(TAG, "MQTT 连接成功");
	    });
	    
	    mqtt->OnDisconnected([]() {
	        ESP_LOGI(TAG, "MQTT 连接断开");
	    });
	    
/*	    mqtt->OnMessage([](const std::string& topic, const std::string& payload) {
	        ESP_LOGI(TAG, "收到消息 [%s]: %s", topic.c_str(), payload.c_str());
	    });*/
	    
	    // 连接到 MQTT 代理
	    if (mqtt->Connect("visit-repair.2811.top", 1883, "esp32_client", "", "")) {
	        // 订阅主题
	        mqtt->Subscribe("test/esp32/message");
	        
	        // 发布消息
	        mqtt->Publish("test/esp32/message", "Hello from ESP32!");
	        
	        ESP_LOGI(TAG, "订阅主题并发送消息");
	        // 等待一段时间接收消息
	        //vTaskDelay(pdMS_TO_TICKS(5000));
	        
	        // 发布消息
	        mqtt->Publish("test/esp32/hello", "Hello from ESP32111!");
	        //先不断开
	        //mqtt->Disconnect();
	    } else {
	        ESP_LOGE(TAG, "MQTT 连接失败");
	    }
    
    }
    
    // 对外提供智能指针的原始指针（供外部调用）
    AtModem* GetModemInstance() {
        // 只有智能指针有效时才返回原始指针
        return modem ? modem.get() : nullptr;
    }
    
    // 对外提供智能指针的原始指针（供外部调用）
    Mqtt* GetMqttInstance() {
        // 只有智能指针有效时才返回原始指针
        return mqtt ? mqtt.get() : nullptr;
    }
}