#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <string>
#include "cJSON.h"
#include "esp_log.h"
#include "media_lib_os.h"
#include "esp_peer_signaling.h"
#include <esp_event.h>
#include <esp_log.h>
#include <esp_system.h>
#include <sys/param.h>
#include "esp_netif.h"

#include "mqtt_signaling.h"
#include "esp_webrtc.h"
#include "esp_peer.h"
#include "network_4g.h"
#include "door_bell_audio.h"

extern "C" {
    const esp_peer_signaling_impl_t *esp_signaling_get_mqtt_impl(void);
    void mqtt_sig_set_device_id(const char *device_id);
    void mqtt_sig_send_ring(const char *target);
    void mqtt_sig_restart(void);
}

static void *g_webrtc_handle = NULL;

#define TAG "MQTT_SIG"

#define UP_TOPIC_TEMPLATE "/voice/project/%s/up"
#define DOWN_TOPIC_TEMPLATE "/voice/project/%s/down"

static char g_device_id[32] = "esp32_001";
static bool g_mqtt_initialized = false;
static bool g_mqtt_connected = false;

typedef struct {
    char *device_id;
    char *peer_id;
    char *ice_url;
    char *ice_user;
    char *ice_psw;
    bool signaling_ready;
    bool is_initiator;
    bool ice_reload_started;
    esp_peer_signaling_cfg_t cfg;
    TaskHandle_t ice_timer;
    bool ice_reload_stop;
    char *pending_sdp;
    char incoming_offer_from[32];
    bool pending_offer_waiting;
    char *pending_candidates[10];
    int pending_candidate_count;
} mqtt_sig_t;

void mqtt_sig_set_device_id(const char *device_id)
{
    if (device_id) {
        strncpy(g_device_id, device_id, sizeof(g_device_id) - 1);
    }
}

void mqtt_sig_answer_call(void)
{
    extern esp_peer_signaling_handle_t g_mqtt_sig_handle;
    
    if (g_mqtt_sig_handle == NULL) {
        ESP_LOGE(TAG, "MQTT signaling not initialized");
        return;
    }
    
    mqtt_sig_t *sg = (mqtt_sig_t *)g_mqtt_sig_handle;
    
    if (sg->pending_sdp) {
        ESP_LOGI(TAG, "Processing pending offer SDP, answering call from %s", sg->incoming_offer_from);
        
        sg->is_initiator = false;
        
        if (g_webrtc_handle && sg->ice_url && sg->ice_user && sg->ice_psw) {
            ESP_LOGI(TAG, "ICE URL: %s, User: %s, PSW: %s", sg->ice_url, sg->ice_user, sg->ice_psw);
            ESP_LOGI(TAG, "Re-sending ICE info with is_initiator=false");
            esp_peer_signaling_ice_info_t ice_info = {
                .server_info = {
                    .stun_url = sg->ice_url,
                    .user = sg->ice_user,
                    .psw = sg->ice_psw,
                },
                .is_initiator = false,
            };
            if (sg->cfg.on_ice_info) {
                sg->cfg.on_ice_info(&ice_info, sg->cfg.ctx);
            }
        }
        
        if (g_webrtc_handle) {
            ESP_LOGI(TAG, "Enabling peer connection before sending SDP");
            esp_webrtc_enable_peer_connection(g_webrtc_handle, true);
        }
        
        if (sg->cfg.on_msg) {
            esp_peer_signaling_msg_t msg = {
                .type = ESP_PEER_SIGNALING_MSG_SDP,
                .data = (uint8_t *)strdup(sg->pending_sdp),
                .size = (int)strlen(sg->pending_sdp),
            };
            sg->cfg.on_msg(&msg, sg->cfg.ctx);
            free(msg.data);
        }
        ESP_LOGI(TAG, "wait press candidates %d", sg->pending_candidate_count);
        
        for (int i = 0; i < sg->pending_candidate_count; i++) {
            if (sg->pending_candidates[i] && sg->cfg.on_msg) {
            ESP_LOGI(TAG, "press candidates %s", sg->pending_candidates[i]);
                esp_peer_signaling_msg_t cand_msg = {
                    .type = ESP_PEER_SIGNALING_MSG_CANDIDATE,
                    .data = (uint8_t *)strdup(sg->pending_candidates[i]),
                    .size = (int)strlen(sg->pending_candidates[i]),
                };
                sg->cfg.on_msg(&cand_msg, sg->cfg.ctx);
                free(cand_msg.data);
                free(sg->pending_candidates[i]);
                sg->pending_candidates[i] = NULL;
            }
        }
        sg->pending_candidate_count = 0;
        
        
        if (sg->cfg.on_connected) {
            sg->cfg.on_connected(sg->cfg.ctx);
        }
        
        free(sg->pending_sdp);
        sg->pending_sdp = NULL;
        sg->pending_offer_waiting = false;
    } else {
        ESP_LOGW(TAG, "No pending offer to answer");
    }
}

void mqtt_sig_reject_call(void)
{
    extern esp_peer_signaling_handle_t g_mqtt_sig_handle;
    
    if (g_mqtt_sig_handle == NULL) {
        ESP_LOGE(TAG, "MQTT signaling not initialized");
        return;
    }
    
    mqtt_sig_t *sg = (mqtt_sig_t *)g_mqtt_sig_handle;
    
    ESP_LOGI(TAG, "Rejecting call from %s", sg->incoming_offer_from);
    
    if (sg->pending_sdp) {
        free(sg->pending_sdp);
        sg->pending_sdp = NULL;
    }
    
    for (int i = 0; i < sg->pending_candidate_count; i++) {
        free(sg->pending_candidates[i]);
        sg->pending_candidates[i] = NULL;
    }
    sg->pending_candidate_count = 0;
    sg->pending_offer_waiting = false;
    
    Mqtt* mqtt = Network4g::GetMqttInstance();
    if (!mqtt || !mqtt->IsConnected()) {
        ESP_LOGE(TAG, "MQTT not connected");
        return;
    }
    
    char topic[128];
    char payload[128];
    snprintf(topic, sizeof(topic), UP_TOPIC_TEMPLATE, g_device_id);
    snprintf(payload, sizeof(payload), 
             "{\"type\":\"reject\",\"from\":\"%s\",\"timestamp\":%ld}",
             g_device_id, (long)time(NULL));
    
    mqtt->Publish(topic, payload, 1);
    ESP_LOGI(TAG, "Reject sent: %s", payload);
    
    sg->incoming_offer_from[0] = '\0';
}

void mqtt_sig_close_call(void)
{
    extern esp_peer_signaling_handle_t g_mqtt_sig_handle;
    
    if (g_mqtt_sig_handle == NULL) {
        ESP_LOGE(TAG, "MQTT signaling not initialized");
        return;
    }
    
    mqtt_sig_t *sg = (mqtt_sig_t *)g_mqtt_sig_handle;
    
    ESP_LOGI(TAG, "Closing call");
    
    if (sg->pending_sdp) {
        free(sg->pending_sdp);
        sg->pending_sdp = NULL;
    }
    for (int i = 0; i < sg->pending_candidate_count; i++) {
        free(sg->pending_candidates[i]);
        sg->pending_candidates[i] = NULL;
    }
    sg->pending_candidate_count = 0;
    sg->pending_offer_waiting = false;
    
    Mqtt* mqtt = Network4g::GetMqttInstance();
    if (!mqtt || !mqtt->IsConnected()) {
        ESP_LOGE(TAG, "MQTT not connected");
        return;
    }
    
    char topic[128];
    char payload[128];
    snprintf(topic, sizeof(topic), UP_TOPIC_TEMPLATE, g_device_id);
    snprintf(payload, sizeof(payload), 
             "{\"type\":\"bye\",\"from\":\"%s\",\"timestamp\":%ld}",
             g_device_id, (long)time(NULL));
    
    mqtt->Publish(topic, payload, 1);
    ESP_LOGI(TAG, "Bye sent: %s", payload);
    
    sg->incoming_offer_from[0] = '\0';
}

void mqtt_sig_set_peer_id(const char *peer_id)
{
}

void mqtt_sig_send_ring(const char *target)
{
    extern esp_peer_signaling_handle_t g_mqtt_sig_handle;
    
    if (g_mqtt_sig_handle == NULL) {
        ESP_LOGE(TAG, "MQTT signaling not initialized");
        return;
    }
    
    mqtt_sig_t *sg = (mqtt_sig_t *)g_mqtt_sig_handle;
    
    sg->is_initiator = true;
    
    if (sg->cfg.on_msg) {
        const char *cmd = "ACCEPT_CALL";
        esp_peer_signaling_msg_t msg = {
            .type = ESP_PEER_SIGNALING_MSG_CUSTOMIZED,
            .data = (uint8_t *)strdup(cmd),
            .size = (int)strlen(cmd),
        };
        ESP_LOGI(TAG, "Triggering offer via on_msg callback");
        sg->cfg.on_msg(&msg, sg->cfg.ctx);
        free(msg.data);
    }
}

void mqtt_sig_restart(void)
{
    extern esp_peer_signaling_handle_t g_mqtt_sig_handle;
    
    ESP_LOGI(TAG, "Restarting MQTT signaling...");
    
    Mqtt* mqtt = Network4g::GetMqttInstance();
    if (mqtt == NULL) {
        ESP_LOGE(TAG, "MQTT not available");
        return;
    }
    
    if (!mqtt->IsConnected()) {
        ESP_LOGW(TAG, "MQTT not connected, waiting...");
        return;
    }
    
    if (g_mqtt_sig_handle != NULL) {
        mqtt_sig_t *sg = (mqtt_sig_t *)g_mqtt_sig_handle;
        
        char topic[128];
        snprintf(topic, sizeof(topic), DOWN_TOPIC_TEMPLATE, g_device_id);
        mqtt->Subscribe(topic, 1);
        
        sg->signaling_ready = true;
        ESP_LOGI(TAG, "MQTT signaling restarted, subscribed to %s", topic);
    }
}

void mqtt_sig_trigger_offer(void)
{
    extern esp_peer_signaling_handle_t g_mqtt_sig_handle;
    
    if (g_mqtt_sig_handle == NULL) {
        ESP_LOGE(TAG, "MQTT signaling not initialized");
        return;
    }
    
    mqtt_sig_t *sg = (mqtt_sig_t *)g_mqtt_sig_handle;
    sg->is_initiator = true;
    
    if (sg->cfg.on_connected) {
        ESP_LOGI(TAG, "mqtt_sig_trigger_offer: triggering peer connection");
        sg->cfg.on_connected(sg->cfg.ctx);
    }
}

static int mqtt_send_up(mqtt_sig_t *sg, const char *type, const char *data)
{
    char topic[128];
    snprintf(topic, sizeof(topic), UP_TOPIC_TEMPLATE, sg->device_id);

    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "type", type);
    if (data) {
        cJSON_AddStringToObject(json, "data", data);
    }
    cJSON_AddStringToObject(json, "from", sg->device_id);
    cJSON_AddNumberToObject(json, "timestamp", time(NULL));

    char *payload = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);

    if (payload) {
        ESP_LOGI(TAG, "MQTT UP [%s]: %s", topic, payload);
        
        Mqtt* mqtt = Network4g::GetMqttInstance();
        if (mqtt && mqtt->IsConnected()) {
            mqtt->Publish(topic, payload, 1);
        }
        
        free(payload);
        return 0;
    }
    return -1;
}

void mqtt_sig_send_timeout(const char *target)
{
    extern esp_peer_signaling_handle_t g_mqtt_sig_handle;
    
    if (g_mqtt_sig_handle == NULL) {
        ESP_LOGE(TAG, "MQTT signaling not initialized");
        return;
    }
    
    mqtt_sig_t *sg = (mqtt_sig_t *)g_mqtt_sig_handle;
    
    ESP_LOGI(TAG, "Sending timeout to %s", target);
    
    if (sg->pending_sdp) {
        free(sg->pending_sdp);
        sg->pending_sdp = NULL;
    }
    for (int i = 0; i < sg->pending_candidate_count; i++) {
        free(sg->pending_candidates[i]);
        sg->pending_candidates[i] = NULL;
    }
    sg->pending_candidate_count = 0;
    sg->pending_offer_waiting = false;
    
    Mqtt* mqtt = Network4g::GetMqttInstance();
    if (!mqtt || !mqtt->IsConnected()) {
        ESP_LOGE(TAG, "MQTT not connected");
        return;
    }
    
    char topic[128];
    char payload[128];
    snprintf(topic, sizeof(topic), UP_TOPIC_TEMPLATE, sg->device_id);
    snprintf(payload, sizeof(payload), 
             "{\"type\":\"timeout\",\"from\":\"%s\",\"timestamp\":%ld}",
             sg->device_id, (long)time(NULL));
    
    mqtt->Publish(topic, payload, 1);
    ESP_LOGI(TAG, "Timeout sent: %s", payload);
}

static void send_ice_request(void)
{
    cJSON *ice_req = cJSON_CreateObject();
    cJSON_AddStringToObject(ice_req, "type", "ice_request");
    cJSON_AddNumberToObject(ice_req, "timestamp", time(NULL));
    
    char *req_payload = cJSON_PrintUnformatted(ice_req);
    if (req_payload) {
        char topic[128];
        snprintf(topic, sizeof(topic), UP_TOPIC_TEMPLATE, g_device_id);
        
        Mqtt* mqtt = Network4g::GetMqttInstance();
        if (mqtt && mqtt->IsConnected()) {
            mqtt->Publish(topic, req_payload, 1);
            ESP_LOGI(TAG, "Sent ice_request");
        }
        free(req_payload);
    }
    cJSON_Delete(ice_req);
}

static void __attribute__((unused)) ice_refresh_task(void *param)
{
    mqtt_sig_t *sg = (mqtt_sig_t *)param;
    while (!sg->ice_reload_stop) {
        vTaskDelay(pdMS_TO_TICKS(30 * 60 * 1000));
        if (sg->ice_reload_stop) break;
        ESP_LOGI(TAG, "Refreshing ICE servers...");
        send_ice_request();
    }
    vTaskDelete(NULL);
}

static void mqtt_message_handler(const std::string& topic, const std::string& payload)
{
    ESP_LOGI(TAG, "MQTT SUB [%s]: %s", topic.c_str(), payload.c_str());
    
    // Only handle messages from device topic
    char expected_topic[128];
    snprintf(expected_topic, sizeof(expected_topic), DOWN_TOPIC_TEMPLATE, g_device_id);
    
    if (topic.find(expected_topic) == std::string::npos) {
        return;
    }

    cJSON *json = cJSON_Parse(payload.c_str());
    if (!json) {
        ESP_LOGE(TAG, "Failed to parse JSON");
        return;
    }

    cJSON *type = cJSON_GetObjectItem(json, "type");
    if (!type) {
        cJSON_Delete(json);
        return;
    }

    const char *msg_type = type->valuestring;
    ESP_LOGI(TAG, "Received message type: %s", msg_type);

    if (strcmp(msg_type, "ice_response") == 0) {
        cJSON *iceServers = cJSON_GetObjectItem(json, "iceServers");
        if (iceServers && cJSON_IsArray(iceServers)) {
            cJSON *firstServer = cJSON_GetArrayItem(iceServers, 0);
            if (firstServer) {
                cJSON *urls = cJSON_GetObjectItem(firstServer, "urls");
                cJSON *username = cJSON_GetObjectItem(firstServer, "username");
                cJSON *credential = cJSON_GetObjectItem(firstServer, "credential");
                
                const char *ice_url = NULL;
                if (urls && cJSON_IsArray(urls)) {
                    cJSON *firstUrl = cJSON_GetArrayItem(urls, 0);
                    if (firstUrl) {
                        ice_url = firstUrl->valuestring;
                    }
                }
                
                if (ice_url && username && credential) {
                    ESP_LOGI(TAG, "Got ICE: %s user=%s", ice_url, username->valuestring);
                    
                    extern esp_peer_signaling_handle_t g_mqtt_sig_handle;
                    if (g_mqtt_sig_handle) {
                        mqtt_sig_t *sg = (mqtt_sig_t *)g_mqtt_sig_handle;
                        
                        if (sg->ice_url) free(sg->ice_url);
                        if (sg->ice_user) free(sg->ice_user);
                        if (sg->ice_psw) free(sg->ice_psw);
                        
                        sg->ice_url = strdup(ice_url);
                        sg->ice_user = strdup(username->valuestring);
                        sg->ice_psw = strdup(credential->valuestring);
            ESP_LOGI(TAG, "ICE信息保存: %s user=%s psw=%s", ice_url, username->valuestring, credential->valuestring);

                        esp_peer_signaling_ice_info_t ice_info = {
                            .server_info = {
                                .stun_url = sg->ice_url,
                                .user = sg->ice_user,
                                .psw = sg->ice_psw,
                            },
                            .is_initiator = sg->is_initiator,
                        };

                        if (sg->cfg.on_ice_info) {
                            sg->cfg.on_ice_info(&ice_info, sg->cfg.ctx);
                        }
                    }
                }
            }
        }
    } else if (strcmp(msg_type, "offer") == 0) {
        cJSON *sdp = cJSON_GetObjectItem(json, "data");
        cJSON *from = cJSON_GetObjectItem(json, "from");
        if (sdp) {
            ESP_LOGI(TAG, "Received offer SDP from %s, waiting for user to answer", from ? from->valuestring : "unknown");
            
            extern esp_peer_signaling_handle_t g_mqtt_sig_handle;
            if (g_mqtt_sig_handle) {
                mqtt_sig_t *sg = (mqtt_sig_t *)g_mqtt_sig_handle;
                sg->is_initiator = false;
                
                if (from && from->valuestring) {
                    strncpy(sg->incoming_offer_from, from->valuestring, sizeof(sg->incoming_offer_from) - 1);
                }
                
                if (sg->pending_sdp) free(sg->pending_sdp);
                sg->pending_sdp = strdup(sdp->valuestring);
                sg->pending_offer_waiting = true;
                sg->pending_candidate_count = 0;
                
                play_tone(DOOR_BELL_TONE_RING);
                ESP_LOGI(TAG, "Ringing, waiting for user to answer/reject");
            }
        }
    } else if (strcmp(msg_type, "answer") == 0) {
        cJSON *sdp = cJSON_GetObjectItem(json, "data");
        if (sdp) {
            ESP_LOGI(TAG, "Received answer SDP");
            
            extern esp_peer_signaling_handle_t g_mqtt_sig_handle;
            if (g_mqtt_sig_handle) {
                mqtt_sig_t *sg = (mqtt_sig_t *)g_mqtt_sig_handle;
                sg->is_initiator = false;
                
                if (sg->cfg.on_msg) {
                    esp_peer_signaling_msg_t msg = {
                        .type = ESP_PEER_SIGNALING_MSG_SDP,
                        .data = (uint8_t *)strdup(sdp->valuestring),
                        .size = (int)strlen(sdp->valuestring),
                    };
                    sg->cfg.on_msg(&msg, sg->cfg.ctx);
                    free(msg.data);
                }
            }
        }
    } else if (strcmp(msg_type, "candidate") == 0) {
        cJSON *candidate = cJSON_GetObjectItem(json, "candidate");
        if (candidate) {
            ESP_LOGI(TAG, "Received ICE candidate");
            
            extern esp_peer_signaling_handle_t g_mqtt_sig_handle;
            if (g_mqtt_sig_handle) {
                mqtt_sig_t *sg = (mqtt_sig_t *)g_mqtt_sig_handle;
                
                if (sg->pending_offer_waiting && sg->pending_candidate_count < 10) {
                    sg->pending_candidates[sg->pending_candidate_count] = strdup(candidate->valuestring);
                    sg->pending_candidate_count++;
                    ESP_LOGI(TAG, "Cached candidate, count=%d", sg->pending_candidate_count);
                } else if (sg->cfg.on_msg) {
                    esp_peer_signaling_msg_t msg = {
                        .type = ESP_PEER_SIGNALING_MSG_CANDIDATE,
                        .data = (uint8_t *)strdup(candidate->valuestring),
                        .size = (int)strlen(candidate->valuestring),
                    };
                    sg->cfg.on_msg(&msg, sg->cfg.ctx);
                    free(msg.data);
                }
            }
        }
    } else if (strcmp(msg_type, "customized") == 0) {
        cJSON *data = cJSON_GetObjectItem(json, "data");
        if (data && data->valuestring) {
            ESP_LOGI(TAG, "Received customized command: %s", data->valuestring);
            
            // data = "ACCEPT_CALL"时候可以触发offer
            extern esp_peer_signaling_handle_t g_mqtt_sig_handle;
            if (g_mqtt_sig_handle) {
                mqtt_sig_t *sg = (mqtt_sig_t *)g_mqtt_sig_handle;
                
                if (sg->cfg.on_msg) {
                    esp_peer_signaling_msg_t msg = {
                        .type = ESP_PEER_SIGNALING_MSG_CUSTOMIZED,
                        .data = (uint8_t *)strdup(data->valuestring),
                        .size = (int)strlen(data->valuestring),
                    };
                    sg->cfg.on_msg(&msg, sg->cfg.ctx);
                    free(msg.data);
                }
            }
        }
    } else if (strcmp(msg_type, "bye") == 0) {
        // Peer closed
        ESP_LOGI(TAG, "Received bye, closing connection");
        extern esp_peer_signaling_handle_t g_mqtt_sig_handle;
        if (g_mqtt_sig_handle) {
            mqtt_sig_t *sg = (mqtt_sig_t *)g_mqtt_sig_handle;
            if (sg->cfg.on_msg) {
                esp_peer_signaling_msg_t msg = {
                    .type = ESP_PEER_SIGNALING_MSG_BYE,
                    .data = NULL,
                    .size = 0,
                };
                sg->cfg.on_msg(&msg, sg->cfg.ctx);
            }
        }
    } else if (strcmp(msg_type, "reject") == 0) {
        // Peer closed
        ESP_LOGI(TAG, "Received reject, closing connection");
        extern esp_peer_signaling_handle_t g_mqtt_sig_handle;
        if (g_mqtt_sig_handle) {
            mqtt_sig_t *sg = (mqtt_sig_t *)g_mqtt_sig_handle;
            if (sg->cfg.on_msg) {
                esp_peer_signaling_msg_t msg = {
                    .type = ESP_PEER_SIGNALING_MSG_BYE,
                    .data = NULL,
                    .size = 0,
                };
                sg->cfg.on_msg(&msg, sg->cfg.ctx);
            }
        }
    }

    cJSON_Delete(json);
}

static void setup_mqtt_callbacks(void)
{
    if (g_mqtt_initialized) {
        return;
    }
    
    Mqtt* mqtt = Network4g::GetMqttInstance();
    if (!mqtt) {
        ESP_LOGE(TAG, "MQTT not available");
        return;
    }

    mqtt->OnConnected([](void) {
        ESP_LOGI(TAG, "MQTT connected");
        g_mqtt_connected = true;
        
        // Subscribe to device topic
        char topic[128];
        snprintf(topic, sizeof(topic), DOWN_TOPIC_TEMPLATE, g_device_id);
        
        Mqtt* mqtt = Network4g::GetMqttInstance();
        if (mqtt) {
            mqtt->Subscribe(topic, 1);
            ESP_LOGI(TAG, "Subscribed to %s", topic);
            
            // Send initial ICE request
            send_ice_request();
        }
        
        // Notify signaling connected
        extern esp_peer_signaling_handle_t g_mqtt_sig_handle;
        if (g_mqtt_sig_handle) {
            mqtt_sig_t *sg = (mqtt_sig_t *)g_mqtt_sig_handle;
            sg->signaling_ready = true;
            if (sg->cfg.on_connected) {
                sg->cfg.on_connected(sg->cfg.ctx);
            }
        }
    });

    mqtt->OnDisconnected([](void) {
        ESP_LOGI(TAG, "MQTT disconnected");
        g_mqtt_connected = false;
    });

    //mqtt->OnMessage(mqtt_message_handler);
    
    mqtt->OnMessage([](const std::string& topic, const std::string& payload) {
        char *topic_copy = strdup(topic.c_str());
        char *payload_copy = strdup(payload.c_str());
        
        auto *ctx = new std::pair<char*, char*>(topic_copy, payload_copy);
        
        xTaskCreate([](void *param) {
            auto *ctx = (std::pair<char*, char*>*)param;
            mqtt_message_handler(std::string(ctx->first), std::string(ctx->second));
            free(ctx->first);
            free(ctx->second);
            delete ctx;
            vTaskDelete(NULL);
        }, "mqtt_msg_async", 8192, ctx, 5, NULL);
    });

    
    g_mqtt_initialized = true;
    ESP_LOGI(TAG, "MQTT callbacks configured");
}

esp_peer_signaling_handle_t g_mqtt_sig_handle = NULL;

void mqtt_sig_set_webrtc_handle(void *handle)
{
    g_webrtc_handle = handle;
    ESP_LOGI(TAG, "WebRTC handle set: %p", handle);
}

static int mqtt_signal_start(esp_peer_signaling_cfg_t *cfg, esp_peer_signaling_handle_t *h)
{
    if (cfg == NULL || h == NULL) {
        return -1;
    }

    mqtt_sig_t *sg = (mqtt_sig_t *)calloc(1, sizeof(mqtt_sig_t));
    if (sg == NULL) {
        return -1;
    }

    sg->cfg = *cfg;
    sg->device_id = strdup(g_device_id);
    sg->signaling_ready = false;
    sg->is_initiator = true;
    sg->ice_reload_started = false;
    sg->pending_sdp = NULL;
    sg->pending_offer_waiting = false;
    sg->pending_candidate_count = 0;

    ESP_LOGI(TAG, "Starting MQTT signaling for device %s", sg->device_id);

    // Setup MQTT callbacks
    setup_mqtt_callbacks();
    
    // Check if MQTT already connected
    Mqtt* mqtt = Network4g::GetMqttInstance();
    if (mqtt && mqtt->IsConnected()) {
        ESP_LOGI(TAG, "MQTT already connected");
        sg->signaling_ready = true;
        
        // Subscribe to topic
        char topic[128];
        snprintf(topic, sizeof(topic), DOWN_TOPIC_TEMPLATE, g_device_id);
        mqtt->Subscribe(topic, 1);
        
        // Send initial ICE request
        send_ice_request();
        
        if (sg->cfg.on_connected) {
            sg->cfg.on_connected(sg->cfg.ctx);
        }
    }

    g_mqtt_sig_handle = sg;
    *h = sg;
    return 0;
}

static int mqtt_signal_send_msg(esp_peer_signaling_handle_t h, esp_peer_signaling_msg_t *msg)
{
    mqtt_sig_t *sg = (mqtt_sig_t *)h;
    if (sg == NULL || msg == NULL) {
        return -1;
    }

    if (msg->type == ESP_PEER_SIGNALING_MSG_SDP) {
        if (sg->is_initiator) {
            ESP_LOGI(TAG, "Sending offer SDP");
            return mqtt_send_up(sg, "offer", (char *)msg->data);
        } else {
            ESP_LOGI(TAG, "Sending answer SDP");
            return mqtt_send_up(sg, "answer", (char *)msg->data);
        }
    } else if (msg->type == ESP_PEER_SIGNALING_MSG_CANDIDATE) {
        ESP_LOGI(TAG, "Sending ICE candidate");
        return mqtt_send_up(sg, "candidate", (char *)msg->data);
    } else if (msg->type == ESP_PEER_SIGNALING_MSG_BYE) {
        return mqtt_send_up(sg, "bye", NULL);
    }

    return 0;
}

static int mqtt_signal_stop(esp_peer_signaling_handle_t h)
{
    mqtt_sig_t *sg = (mqtt_sig_t *)h;
    if (sg == NULL) {
        return -1;
    }

    sg->ice_reload_stop = true;
    
    if (g_mqtt_sig_handle == sg) {
        g_mqtt_sig_handle = NULL;
    }

    if (sg->device_id) free(sg->device_id);
    if (sg->peer_id) free(sg->peer_id);
    if (sg->ice_url) free(sg->ice_url);
    if (sg->ice_user) free(sg->ice_user);
    if (sg->ice_psw) free(sg->ice_psw);
    if (sg->pending_sdp) free(sg->pending_sdp);
    for (int i = 0; i < sg->pending_candidate_count; i++) {
        free(sg->pending_candidates[i]);
    }

    free(sg);
    return 0;
}

const esp_peer_signaling_impl_t *esp_signaling_get_mqtt_impl(void)
{
    static const esp_peer_signaling_impl_t impl = {
        .start = mqtt_signal_start,
        .send_msg = mqtt_signal_send_msg,
        .stop = mqtt_signal_stop,
    };
    return &impl;
}

//mqtt_signaling.cpp