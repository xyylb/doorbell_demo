#ifndef MQTT_SIGNALING_H
#define MQTT_SIGNALING_H

#include "esp_peer_signaling.h"

#ifdef __cplusplus
extern "C" {
#endif

const esp_peer_signaling_impl_t *esp_signaling_get_mqtt_impl(void);
void mqtt_sig_set_device_id(const char *device_id);
void mqtt_sig_send_ring(const char *target);

#ifdef __cplusplus
}
#endif

#endif // MQTT_SIGNALING_H
