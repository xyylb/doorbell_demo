#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char mqtt_host[64];
    uint16_t mqtt_port;
    char mqtt_user[32];
    char mqtt_password[32];
    char device_id[32];
    char base_topic[64];
    uint32_t ice_refresh_interval;
    uint32_t call_timeout;
} app_config_t;

void app_config_init(void);
void app_config_reset(void);
app_config_t* app_config_get(void);
int app_config_set_mqtt(const char* host, uint16_t port, const char* user, const char* password);
int app_config_set_device_id(const char* id);
int app_config_set_base_topic(const char* base_topic);
int app_config_set_ice_interval(uint32_t seconds);
int app_config_set_call_timeout(uint32_t seconds);
void app_config_print(void);

#ifdef __cplusplus
}
#endif
