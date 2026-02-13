#ifndef SINAL_MQTT_H_
#define SINAL_MQTT_H_

#include "esp_peer_signaling.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 获取 MQTT 信令实现接口
 *
 * 此函数返回一个符合 esp_peer_signaling_impl_t 接口的结构体，
 * 供 esp_webrtc_open() 使用。
 *
 * @return const esp_peer_signaling_impl_t*
 */
const esp_peer_signaling_impl_t *esp_signaling_get_mqtt_impl(void);


// 辅助函数（如果需要在 C 文件中调用）
void esp_mqtt_set_peer(esp_peer_signaling_handle_t h, const char* peer_id);
void esp_mqtt_send_reject(esp_peer_signaling_handle_t h);
void esp_mqtt_send_timeout(esp_peer_signaling_handle_t h);

#ifdef __cplusplus
}
#endif

#endif // SINAL_MQTT_H_