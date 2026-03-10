#include "app_config.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_mac.h"
#include <string.h>

static const char *TAG = "APP_CONFIG";
static const char *NVS_NAMESPACE = "app_config";

static app_config_t s_config = {
    .mqtt_host = "visit-repair.2811.top",
    .mqtt_port = 1883,
    .mqtt_user = "esp32_client",
    .mqtt_password = "",
    .device_id = "esp32_001",
    .base_topic = "/voice/project",
    .ice_refresh_interval = 1800,
    .call_timeout = 120,
};

static void generate_device_id_if_needed(void)
{
    nvs_handle_t nvs;
    char stored_id[32] = {0};
    size_t len = sizeof(stored_id);
    
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        nvs_get_str(nvs, "device_id", stored_id, &len);
        nvs_close(nvs);
        
        if (strlen(stored_id) > 0) {
            strncpy(s_config.device_id, stored_id, sizeof(s_config.device_id) - 1);
            ESP_LOGI(TAG, "Using stored device_id: %s", s_config.device_id);
            return;
        }
    }
    
    // 第一次启动，生成设备 ID（MAC 地址转 10 进制）
    uint8_t mac[6];
    esp_efuse_mac_get_default(mac);
    // MAC: XX:XX:XX:XX:XX:XX -> 转为 12 位 10 进制数字
    uint64_t mac_num = ((uint64_t)mac[0] << 40) | ((uint64_t)mac[1] << 32) |
                       ((uint64_t)mac[2] << 24) | ((uint64_t)mac[3] << 16) |
                       ((uint64_t)mac[4] << 8)  | ((uint64_t)mac[5]);
    snprintf(s_config.device_id, sizeof(s_config.device_id), "%012llu", mac_num);
    
    // 保存到 NVS
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_str(nvs, "device_id", s_config.device_id);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
    
    ESP_LOGI(TAG, "Generated new device_id: %s", s_config.device_id);
}

void app_config_init(void)
{
    // 先检查/生成设备 ID
    generate_device_id_if_needed();
    
    esp_err_t err;
    nvs_handle_t nvs_handle;

    err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "NVS not found, using defaults");
        return;
    }

    size_t len;
    
    len = sizeof(s_config.mqtt_host);
    nvs_get_str(nvs_handle, "mqtt_host", s_config.mqtt_host, &len);
    
    nvs_get_u16(nvs_handle, "mqtt_port", &s_config.mqtt_port);
    
    len = sizeof(s_config.mqtt_user);
    nvs_get_str(nvs_handle, "mqtt_user", s_config.mqtt_user, &len);
    
    len = sizeof(s_config.mqtt_password);
    nvs_get_str(nvs_handle, "mqtt_password", s_config.mqtt_password, &len);
    
    len = sizeof(s_config.device_id);
    nvs_get_str(nvs_handle, "device_id", s_config.device_id, &len);
    
    len = sizeof(s_config.base_topic);
    nvs_get_str(nvs_handle, "base_topic", s_config.base_topic, &len);
    
    nvs_get_u32(nvs_handle, "ice_interval", &s_config.ice_refresh_interval);
    nvs_get_u32(nvs_handle, "call_timeout", &s_config.call_timeout);

    nvs_close(nvs_handle);
    
    ESP_LOGI(TAG, "Config loaded from NVS");
    app_config_print();
}

void app_config_reset(void)
{
    nvs_handle_t nvs_handle;
    nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    nvs_erase_all(nvs_handle);
    nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
    
    s_config.mqtt_port = 1883;
    strcpy(s_config.mqtt_host, "visit-repair.2811.top");
    strcpy(s_config.mqtt_user, "esp32_client");
    strcpy(s_config.mqtt_password, "");
    strcpy(s_config.device_id, "esp32_001");
    strcpy(s_config.base_topic, "/voice/project");
    s_config.ice_refresh_interval = 1800;
    s_config.call_timeout = 120;
    
    ESP_LOGI(TAG, "Config reset to defaults");
}

app_config_t* app_config_get(void)
{
    return &s_config;
}

static int save_string(nvs_handle_t nvs, const char* key, const char* value)
{
    return nvs_set_str(nvs, key, value);
}

int app_config_set_mqtt(const char* host, uint16_t port, const char* user, const char* password)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        return -1;
    }
    
    strncpy(s_config.mqtt_host, host, sizeof(s_config.mqtt_host) - 1);
    s_config.mqtt_port = port;
    strncpy(s_config.mqtt_user, user, sizeof(s_config.mqtt_user) - 1);
    strncpy(s_config.mqtt_password, password, sizeof(s_config.mqtt_password) - 1);
    
    save_string(nvs, "mqtt_host", s_config.mqtt_host);
    nvs_set_u16(nvs, "mqtt_port", s_config.mqtt_port);
    save_string(nvs, "mqtt_user", s_config.mqtt_user);
    save_string(nvs, "mqtt_password", s_config.mqtt_password);
    
    nvs_commit(nvs);
    nvs_close(nvs);
    
    ESP_LOGI(TAG, "MQTT config saved");
    return 0;
}

int app_config_set_device_id(const char* id)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        return -1;
    }
    
    strncpy(s_config.device_id, id, sizeof(s_config.device_id) - 1);
    save_string(nvs, "device_id", s_config.device_id);
    
    nvs_commit(nvs);
    nvs_close(nvs);
    
    ESP_LOGI(TAG, "Device ID saved: %s", id);
    return 0;
}

int app_config_set_base_topic(const char* base_topic)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        return -1;
    }
    
    strncpy(s_config.base_topic, base_topic, sizeof(s_config.base_topic) - 1);
    save_string(nvs, "base_topic", s_config.base_topic);
    
    nvs_commit(nvs);
    nvs_close(nvs);
    
    ESP_LOGI(TAG, "Base topic saved: %s", base_topic);
    return 0;
}

int app_config_set_ice_interval(uint32_t seconds)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        return -1;
    }
    
    s_config.ice_refresh_interval = seconds;
    nvs_set_u32(nvs, "ice_interval", seconds);
    
    nvs_commit(nvs);
    nvs_close(nvs);
    
    ESP_LOGI(TAG, "ICE interval saved: %lu seconds", seconds);
    return 0;
}

int app_config_set_call_timeout(uint32_t seconds)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        return -1;
    }
    
    s_config.call_timeout = seconds;
    nvs_set_u32(nvs, "call_timeout", seconds);
    
    nvs_commit(nvs);
    nvs_close(nvs);
    
    ESP_LOGI(TAG, "Call timeout saved: %lu seconds", seconds);
    return 0;
}

void app_config_print(void)
{
    ESP_LOGI(TAG, "=== App Config ===");
    ESP_LOGI(TAG, "MQTT: %s:%d user=%s", s_config.mqtt_host, s_config.mqtt_port, s_config.mqtt_user);
    ESP_LOGI(TAG, "Device ID: %s", s_config.device_id);
    ESP_LOGI(TAG, "Base topic: %s", s_config.base_topic);
    ESP_LOGI(TAG, "Up topic: %s/%s/up", s_config.base_topic, s_config.device_id);
    ESP_LOGI(TAG, "Down topic: %s/%s/down", s_config.base_topic, s_config.device_id);
    ESP_LOGI(TAG, "ICE refresh: %lu sec", s_config.ice_refresh_interval);
    ESP_LOGI(TAG, "Call timeout: %lu sec", s_config.call_timeout);
}
