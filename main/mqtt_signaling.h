#ifndef MQTT_SIGNALING_H
#define MQTT_SIGNALING_H

#include "esp_peer_signaling.h"

#ifdef __cplusplus
extern "C" {
#endif

const esp_peer_signaling_impl_t *esp_signaling_get_mqtt_impl(void);
void mqtt_sig_set_device_id(const char *device_id);
void mqtt_sig_set_webrtc_handle(void *handle);
void mqtt_sig_send_ring(const char *target); //呼出
void mqtt_sig_send_timeout(const char *target); //超时
void mqtt_sig_trigger_offer(void); //触发Offer
void mqtt_sig_answer_call(void); //接听
void mqtt_sig_reject_call(void); //拒绝
void mqtt_sig_close_call(void); //挂断
void mqtt_sig_restart(void); //重启

#ifdef __cplusplus
}
#endif

#endif // MQTT_SIGNALING_H
