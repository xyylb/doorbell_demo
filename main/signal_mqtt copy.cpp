/**
 * ESPRESSIF MIT License
 *
 * Copyright (c) 2025 ESPRESSIF SYSTEMS (SHANGHAI) CO., LTD
 *
 * Permission is hereby granted for use on all ESPRESSIF SYSTEMS products...
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include "cJSON.h"
#include "esp_log.h"
#include "media_lib_os.h"
#include "esp_peer_signaling.h"
#include "esp_peer_types.h"
#include "network_4g.h"
#include <string>
#include <cstring>
#include "signal_mqtt.h"
#include "call_state_machine.h"

#define TAG "MQTT_SIG"

#define UP_TOPIC_TEMPLATE   "/voice/project/%s/up"
#define DOWN_TOPIC_TEMPLATE "/voice/project/%s/down"

extern const char* g_device_id;

typedef struct {
    esp_peer_signaling_ice_info_t ice_info;
    esp_peer_signaling_cfg_t      cfg;
    std::string                   peer_device_id;
    bool                          is_initiator;
    bool                          ice_reloading;
    bool                          ice_reload_stop;
    uint32_t                      last_ice_request_time;
    call_state_machine_t          state_machine;
    bool                          signaling_ready;
    ice_info_update_callback_t    ice_update_callback;
    void*                         ice_update_ctx;
} mqtt_sig_t;

static mqtt_sig_t* g_mqtt_sig_instance = nullptr;

static void free_ice_info(esp_peer_signaling_ice_info_t *info) {
    if (info->server_info.stun_url) { free(info->server_info.stun_url); info->server_info.stun_url = nullptr; }
    if (info->server_info.user) { free(info->server_info.user); info->server_info.user = nullptr; }
    if (info->server_info.psw) { free(info->server_info.psw); info->server_info.psw = nullptr; }
    memset(info, 0, sizeof(esp_peer_signaling_ice_info_t));
}

static void parse_ice_response(mqtt_sig_t* sg, cJSON* ice_servers_json) {
    if (!ice_servers_json || !cJSON_IsArray(ice_servers_json) || cJSON_GetArraySize(ice_servers_json) == 0) {
        ESP_LOGE(TAG, "Invalid ice_servers in response");
        return;
    }

    cJSON* server = cJSON_GetArrayItem(ice_servers_json, 0);
    if (!server) return;

    cJSON* urls_arr = cJSON_GetObjectItem(server, "urls");
    if (urls_arr && cJSON_IsArray(urls_arr)) {
        cJSON* url = cJSON_GetArrayItem(urls_arr, 0);
        if (url && cJSON_IsString(url)) {
            if (sg->ice_info.server_info.stun_url) free(sg->ice_info.server_info.stun_url);
            sg->ice_info.server_info.stun_url = strdup(url->valuestring);
        }
    }

    cJSON* user = cJSON_GetObjectItem(server, "username");
    if (user && cJSON_IsString(user)) {
        if (sg->ice_info.server_info.user) free(sg->ice_info.server_info.user);
        sg->ice_info.server_info.user = strdup(user->valuestring);
    }

    cJSON* cred = cJSON_GetObjectItem(server, "credential");
    if (cred && cJSON_IsString(cred)) {
        if (sg->ice_info.server_info.psw) free(sg->ice_info.server_info.psw);
        sg->ice_info.server_info.psw = strdup(cred->valuestring);
    }

    ESP_LOGI(TAG, "ICE server updated: url=%s, user=%s, psw=%s", 
             sg->ice_info.server_info.stun_url ? sg->ice_info.server_info.stun_url : "N/A",
             sg->ice_info.server_info.user ? sg->ice_info.server_info.user : "N/A",
             sg->ice_info.server_info.psw ? sg->ice_info.server_info.psw : "N/A");
}

static void mqtt_publish_up(const char* payload) {
    char topic[128];
    snprintf(topic, sizeof(topic), UP_TOPIC_TEMPLATE, g_device_id);
    Mqtt* mqtt = Network4g::GetMqttInstance();
    if (mqtt) {
        mqtt->Publish(topic, payload);
        ESP_LOGI(TAG, "MQTT PUB [%s]: %s", topic, payload);
    } else {
        ESP_LOGE(TAG, "MQTT instance unavailable");
    }
}

static void ice_refresh_task(void *arg) {
    mqtt_sig_t *sg = (mqtt_sig_t *)arg;
    sg->ice_reloading = true;
    sg->last_ice_request_time = time(nullptr);

    while (!sg->ice_reload_stop) {
        uint32_t now = time(nullptr);
        if (now - sg->last_ice_request_time >= 1800) {
            cJSON *req = cJSON_CreateObject();
            cJSON_AddStringToObject(req, "type", "ice_request");
            cJSON_AddNumberToObject(req, "timestamp", now);
            char *payload = cJSON_PrintUnformatted(req);
            if (payload) {
                mqtt_publish_up(payload);
                free(payload);
                sg->last_ice_request_time = now;
                ESP_LOGI(TAG, "Sent periodic ice_request");
            }
            cJSON_Delete(req);
        }
        media_lib_thread_sleep(1000);
    }
    sg->ice_reloading = false;
    sg->ice_reload_stop = false;
    media_lib_thread_destroy(nullptr);
}

static int start_ice_refresh_timer(mqtt_sig_t *sg) {
    media_lib_thread_handle_t handle;
    media_lib_thread_create_from_scheduler(&handle, "ice_refresh", ice_refresh_task, sg);
    return handle ? 0 : -1;
}

static int stop_ice_refresh_timer(mqtt_sig_t *sg) {
    sg->ice_reload_stop = true;
    while (sg->ice_reloading) {
        media_lib_thread_sleep(100);
    }
    return 0;
}

static void handle_downstream_message(mqtt_sig_t* sg, const char* payload) {
    cJSON *root = cJSON_Parse(payload);
    if (!root) {
        ESP_LOGE(TAG, "Invalid JSON: %s", payload);
        return;
    }

    cJSON *type_item = cJSON_GetObjectItem(root, "type");
    if (!type_item || !cJSON_IsString(type_item)) {
        cJSON_Delete(root);
        return;
    }

    const char* msg_type = type_item->valuestring;
    esp_peer_signaling_msg_t sig_msg = {};
    bool clear_peer = false;

    ESP_LOGI(TAG, "Recv msg type=%s", msg_type);

    if (strcmp(msg_type, "ice_response") == 0) {
        cJSON *ice_servers = cJSON_GetObjectItem(root, "iceServers");
        if (!ice_servers) {
            ice_servers = cJSON_GetObjectItem(root, "ice_servers");
        }
        if (ice_servers) {
            parse_ice_response(sg, ice_servers);
            sg->signaling_ready = true;
            ESP_LOGI(TAG, "ICE credentials configured, signaling ready");
            ESP_LOGI(TAG, "ICE info: stun_url=%s, user=%s, is_initiator=%d",
                     sg->ice_info.server_info.stun_url ? sg->ice_info.server_info.stun_url : "NULL",
                     sg->ice_info.server_info.user ? sg->ice_info.server_info.user : "NULL",
                     sg->ice_info.is_initiator);
            
            if (sg->cfg.on_ice_info) {
                ESP_LOGI(TAG, "Calling on_ice_info callback...");
                sg->cfg.on_ice_info(&sg->ice_info, sg->cfg.ctx);
                ESP_LOGI(TAG, "on_ice_info callback completed");
            } else {
                ESP_LOGE(TAG, "WARNING: on_ice_info callback is NULL!");
            }
            
            if (sg->ice_update_callback) {
                sg->ice_update_callback(sg->ice_update_ctx);
            }
        } else {
            ESP_LOGW(TAG, "ice_response missing iceServers field");
        }
    }
    else if (strcmp(msg_type, "offer") == 0) {
        cJSON *from = cJSON_GetObjectItem(root, "from");
        const char* from_id = (from && cJSON_IsString(from)) ? from->valuestring : "unknown";
        
        call_state_machine_process_command(&sg->state_machine, CALL_CMD_RECV_OFFER, from_id);
        sg->peer_device_id = from_id;
        sg->is_initiator = false;
        ESP_LOGI(TAG, "Incoming call from %s", from_id);
        
        cJSON *sdp = cJSON_GetObjectItem(root, "sdp");
        if (sdp && cJSON_IsString(sdp)) {
            sig_msg.type = ESP_PEER_SIGNALING_MSG_SDP;
            sig_msg.data = (uint8_t*)sdp->valuestring;
            sig_msg.size = strlen(sdp->valuestring);
            if (sg->cfg.on_msg) sg->cfg.on_msg(&sig_msg, sg->cfg.ctx);
        }
    }
    else if (strcmp(msg_type, "answer") == 0) {
        call_state_machine_process_command(&sg->state_machine, CALL_CMD_RECV_ANSWER, sg->peer_device_id.c_str());
        cJSON *sdp = cJSON_GetObjectItem(root, "sdp");
        if (sdp && cJSON_IsString(sdp)) {
            sig_msg.type = ESP_PEER_SIGNALING_MSG_SDP;
            sig_msg.data = (uint8_t*)sdp->valuestring;
            sig_msg.size = strlen(sdp->valuestring);
            if (sg->cfg.on_msg) sg->cfg.on_msg(&sig_msg, sg->cfg.ctx);
        }
    }
    else if (strcmp(msg_type, "candidate") == 0) {
        cJSON *candidate = cJSON_GetObjectItem(root, "candidate");
        if (candidate && cJSON_IsString(candidate)) {
            sig_msg.type = ESP_PEER_SIGNALING_MSG_CANDIDATE;
            sig_msg.data = (uint8_t*)candidate->valuestring;
            sig_msg.size = strlen(candidate->valuestring);
            if (sg->cfg.on_msg) sg->cfg.on_msg(&sig_msg, sg->cfg.ctx);
        }
    }
    else if (strcmp(msg_type, "bye") == 0) {
        call_state_machine_process_command(&sg->state_machine, CALL_CMD_RECV_BYE, sg->peer_device_id.c_str());
        sig_msg.type = ESP_PEER_SIGNALING_MSG_BYE;
        clear_peer = true;
    }
    else if (strcmp(msg_type, "reject") == 0) {
        call_state_machine_process_command(&sg->state_machine, CALL_CMD_RECV_REJECT, sg->peer_device_id.c_str());
        sig_msg.type = ESP_PEER_SIGNALING_MSG_BYE;
        clear_peer = true;
        ESP_LOGI(TAG, "Call rejected by peer");
    }
    else if (strcmp(msg_type, "customized") == 0) {
        cJSON *data = cJSON_GetObjectItem(root, "data");
        if (data && cJSON_IsString(data)) {
            const char* custom_data = data->valuestring;
            ESP_LOGI(TAG, "Received customized data: %s", custom_data);
            
            sig_msg.type = ESP_PEER_SIGNALING_MSG_CUSTOMIZED;
            sig_msg.data = (uint8_t*)custom_data;
            sig_msg.size = strlen(custom_data);
            if (sg->cfg.on_msg) {
                ESP_LOGI(TAG, "Calling on_msg callback for custom data");
                sg->cfg.on_msg(&sig_msg, sg->cfg.ctx);
            }
            
            if (strncmp(custom_data, "ACCEPT_CALL", 11) == 0) {
                ESP_LOGI(TAG, "Call accepted by peer, transitioning to IN_CALL state");
                call_state_machine_process_command(&sg->state_machine, CALL_CMD_RECV_ANSWER, sg->peer_device_id.c_str());
            }
        }
    }
    else if (strcmp(msg_type, "timeout") == 0) {
        call_state_machine_process_command(&sg->state_machine, CALL_CMD_RECV_TIMEOUT, sg->peer_device_id.c_str());
        sig_msg.type = ESP_PEER_SIGNALING_MSG_BYE;
        clear_peer = true;
        ESP_LOGI(TAG, "Call timeout from peer");
    }

    if (sig_msg.type == ESP_PEER_SIGNALING_MSG_BYE && sg->cfg.on_msg) {
        sg->cfg.on_msg(&sig_msg, sg->cfg.ctx);
    }

    if (clear_peer) {
        sg->peer_device_id.clear();
        sg->is_initiator = false;
    }

    cJSON_Delete(root);
}

static void mqtt_message_handler(const std::string& topic, const std::string& payload) {
    if (!g_mqtt_sig_instance) return;

    char expected_topic[128];
    snprintf(expected_topic, sizeof(expected_topic), DOWN_TOPIC_TEMPLATE, g_device_id);
    if (topic != expected_topic) return;

    ESP_LOGI(TAG, "MQTT SUB [%s]: %s", topic.c_str(), payload.c_str());
    handle_downstream_message(g_mqtt_sig_instance, payload.c_str());
}

static int mqtt_signal_start(esp_peer_signaling_cfg_t *cfg, esp_peer_signaling_handle_t *h) {
    ESP_LOGI(TAG, "mqtt_signal_start initializing");
    if (!cfg || !h || !g_device_id) {
        ESP_LOGE(TAG, "Invalid config or missing g_device_id");
        return ESP_PEER_ERR_INVALID_ARG;
    }

    mqtt_sig_t *sg = new(std::nothrow) mqtt_sig_t();
    if (!sg) return ESP_PEER_ERR_NO_MEM;
    sg->cfg = *cfg;
    sg->signaling_ready = false;
    sg->is_initiator = false;
    sg->ice_reload_stop = false;
    sg->last_ice_request_time = 0;
    
    ESP_LOGI(TAG, "mqtt_signal_start: cfg=%p, on_ice_info=%p, on_msg=%p", 
             cfg, cfg->on_ice_info, cfg->on_msg);
    
    call_state_machine_init(&sg->state_machine);

    g_mqtt_sig_instance = sg;

    Mqtt* mqtt = Network4g::GetMqttInstance();
    if (!mqtt) {
        ESP_LOGE(TAG, "MQTT not initialized");
        delete sg;
        return ESP_PEER_ERR_FAIL;
    }
    
    mqtt->OnConnected([sg]() {
        ESP_LOGI(TAG, "MQTT connection established, calling on_connected callback");
        if (sg->cfg.on_connected) {
            sg->cfg.on_connected(sg->cfg.ctx);
        }
    });
    
    if (mqtt->IsConnected()) {
        ESP_LOGI(TAG, "MQTT already connected, calling on_connected callback immediately");
        if (sg->cfg.on_connected) {
            ESP_LOGI(TAG, "Calling sg->cfg.on_connected at startup");
            sg->cfg.on_connected(sg->cfg.ctx);
        }
    }
    
    mqtt->OnMessage([](const std::string& topic, const std::string& payload) {
        mqtt_message_handler(topic, payload);
    });

    char down_topic[128];
    snprintf(down_topic, sizeof(down_topic), DOWN_TOPIC_TEMPLATE, g_device_id);
    mqtt->Subscribe(down_topic, 1);
    ESP_LOGI(TAG, "Subscribed to %s", down_topic);

    cJSON *ice_req = cJSON_CreateObject();
    cJSON_AddStringToObject(ice_req, "type", "ice_request");
    cJSON_AddNumberToObject(ice_req, "timestamp", time(nullptr));
    char *req_payload = cJSON_PrintUnformatted(ice_req);
    if (req_payload) {
        mqtt_publish_up(req_payload);
        free(req_payload);
        sg->last_ice_request_time = time(nullptr);
        ESP_LOGI(TAG, "Sent initial ice_request");
    }
    cJSON_Delete(ice_req);

    if (start_ice_refresh_timer(sg) != 0) {
        ESP_LOGW(TAG, "Failed to start ICE refresh timer");
    }

    if (sg->cfg.on_connected) {
        sg->cfg.on_connected(sg->cfg.ctx);
    }

    *h = sg;
    return ESP_PEER_ERR_NONE;
}

static int mqtt_signal_send_msg(esp_peer_signaling_handle_t h, esp_peer_signaling_msg_t *msg) {
    if (!h || !msg || g_mqtt_sig_instance != h) return ESP_PEER_ERR_INVALID_ARG;
    mqtt_sig_t *sg = (mqtt_sig_t *)h;

    ESP_LOGI(TAG, "=== mqtt_signal_send_msg called ===, msg->type=%d, msg->size=%d, peer_device_id=%s", 
             msg->type, msg->size, sg->peer_device_id.c_str());

    if (sg->peer_device_id.empty()) {
        ESP_LOGW(TAG, "=== Peer device ID is empty, cannot send message! ===");
        return ESP_PEER_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "=== Proceeding to send message, peer_id=%s, is_initiator=%d ===", sg->peer_device_id.c_str(), sg->is_initiator);

    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "target", sg->peer_device_id.c_str());
    cJSON_AddNumberToObject(json, "timestamp", time(nullptr));

    if (msg->type == ESP_PEER_SIGNALING_MSG_SDP) {
        const char* sdp_type = sg->is_initiator ? "offer" : "answer";
        ESP_LOGI(TAG, "=== Processing SDP message: %s ===", sdp_type);
        cJSON_AddStringToObject(json, "type", sdp_type);
        cJSON_AddStringToObject(json, "sdp", (char*)msg->data);
        cJSON_AddStringToObject(json, "from", g_device_id);
        ESP_LOGI(TAG, ">>> Sending %s SDP to %s, size=%d", sdp_type, sg->peer_device_id.c_str(), msg->size);
    }
    else if (msg->type == ESP_PEER_SIGNALING_MSG_CANDIDATE) {
        cJSON_AddStringToObject(json, "type", "candidate");
        cJSON_AddStringToObject(json, "candidate", (char*)msg->data);
        cJSON_AddStringToObject(json, "from", g_device_id);
    }
    else if (msg->type == ESP_PEER_SIGNALING_MSG_BYE) {
        cJSON_AddStringToObject(json, "type", "bye");
        cJSON_AddStringToObject(json, "from", g_device_id);
        sg->peer_device_id.clear();
        sg->is_initiator = false;
    }
    else if (msg->type == ESP_PEER_SIGNALING_MSG_CUSTOMIZED) {
        cJSON_AddStringToObject(json, "type", "customized");
        cJSON_AddStringToObject(json, "data", (char*)msg->data);
        cJSON_AddStringToObject(json, "from", g_device_id);
    }
    else {
        cJSON_Delete(json);
        return ESP_PEER_ERR_INVALID_ARG;
    }

    char *payload = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (!payload) return ESP_PEER_ERR_NO_MEM;

    ESP_LOGI(TAG, ">>> Publishing to MQTT: %s", payload);
    mqtt_publish_up(payload);
    free(payload);
    ESP_LOGI(TAG, "<<< mqtt_signal_send_msg completed");
    return ESP_PEER_ERR_NONE;
}

static int mqtt_signal_stop(esp_peer_signaling_handle_t h) {
    if (!h || g_mqtt_sig_instance != h) return ESP_PEER_ERR_NONE;
    mqtt_sig_t *sg = (mqtt_sig_t *)h;

    if (!sg->peer_device_id.empty()) {
        esp_peer_signaling_msg_t bye = {};
        bye.type = ESP_PEER_SIGNALING_MSG_BYE;
        mqtt_signal_send_msg(h, &bye);
        media_lib_thread_sleep(200);
    }

    stop_ice_refresh_timer(sg);
    free_ice_info(&sg->ice_info);
    g_mqtt_sig_instance = nullptr;
    delete sg;
    return ESP_PEER_ERR_NONE;
}

extern "C" void esp_mqtt_set_peer(esp_peer_signaling_handle_t h, const char* peer_id) {
    if (!h || !peer_id) return;
    mqtt_sig_t *sg = (mqtt_sig_t *)h;
    sg->peer_device_id = peer_id;
    sg->is_initiator = true;
    sg->ice_info.is_initiator = true;
    call_state_machine_set_peer(&sg->state_machine, peer_id, true);
    ESP_LOGI(TAG, "Set peer to %s (initiator)", peer_id);
}

extern "C" void esp_mqtt_send_reject(esp_peer_signaling_handle_t h) {
    if (!h || g_mqtt_sig_instance != h) return;
    mqtt_sig_t *sg = (mqtt_sig_t *)h;
    if (sg->peer_device_id.empty()) return;

    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "target", sg->peer_device_id.c_str());
    cJSON_AddStringToObject(json, "type", "reject");
    cJSON_AddStringToObject(json, "from", g_device_id);
    cJSON_AddNumberToObject(json, "timestamp", time(nullptr));
    char *payload = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (payload) {
        mqtt_publish_up(payload);
        free(payload);
        call_state_machine_process_command(&sg->state_machine, CALL_CMD_REJECT, sg->peer_device_id.c_str());
        sg->peer_device_id.clear();
        sg->is_initiator = false;
        ESP_LOGI(TAG, "Sent reject");
    }
}

extern "C" void esp_mqtt_send_ring(esp_peer_signaling_handle_t h) {
    if (!h || g_mqtt_sig_instance != h) return;
    mqtt_sig_t *sg = (mqtt_sig_t *)h;
    if (sg->peer_device_id.empty()) return;

    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "target", sg->peer_device_id.c_str());
    cJSON_AddStringToObject(json, "type", "ring");
    cJSON_AddStringToObject(json, "from", g_device_id);
    cJSON_AddNumberToObject(json, "timestamp", time(nullptr));
    char *payload = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (payload) {
        mqtt_publish_up(payload);
        free(payload);
        ESP_LOGI(TAG, "Sent ring");
    }
}

extern "C" void esp_mqtt_send_timeout(esp_peer_signaling_handle_t h) {
    if (!h || g_mqtt_sig_instance != h) return;
    mqtt_sig_t *sg = (mqtt_sig_t *)h;
    if (sg->peer_device_id.empty()) return;

    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "target", sg->peer_device_id.c_str());
    cJSON_AddStringToObject(json, "type", "timeout");
    cJSON_AddStringToObject(json, "from", g_device_id);
    cJSON_AddNumberToObject(json, "timestamp", time(nullptr));
    char *payload = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (payload) {
        mqtt_publish_up(payload);
        free(payload);
        call_state_machine_process_command(&sg->state_machine, CALL_CMD_TIMEOUT, NULL);
        sg->peer_device_id.clear();
        sg->is_initiator = false;
        ESP_LOGI(TAG, "Sent timeout");
    }
}

extern "C" call_state_t esp_mqtt_get_call_state(esp_peer_signaling_handle_t h) {
    if (!h || g_mqtt_sig_instance != h) return CALL_STATE_IDLE;
    mqtt_sig_t *sg = (mqtt_sig_t *)h;
    return call_state_machine_get_state(&sg->state_machine);
}

extern "C" const char* esp_mqtt_get_peer_id(esp_peer_signaling_handle_t h) {
    if (!h || g_mqtt_sig_instance != h) return "";
    mqtt_sig_t *sg = (mqtt_sig_t *)h;
    return sg->peer_device_id.c_str();
}

extern "C" bool esp_mqtt_is_signaling_ready(esp_peer_signaling_handle_t h) {
    if (!h || g_mqtt_sig_instance != h) return false;
    mqtt_sig_t *sg = (mqtt_sig_t *)h;
    return sg->signaling_ready;
}

extern "C" esp_peer_signaling_handle_t esp_mqtt_get_signaling_handle(void) {
    return g_mqtt_sig_instance;
}

extern "C" void esp_mqtt_set_call_state_callback(esp_peer_signaling_handle_t h, call_state_callback_t callback, void* ctx) {
    if (!h || g_mqtt_sig_instance != h) return;
    mqtt_sig_t *sg = (mqtt_sig_t *)h;
    call_state_machine_set_callback(&sg->state_machine, callback, ctx);
}

extern "C" void esp_mqtt_update_state_machine(esp_peer_signaling_handle_t h) {
    if (!h || g_mqtt_sig_instance != h) return;
    mqtt_sig_t *sg = (mqtt_sig_t *)h;
    call_state_machine_update(&sg->state_machine);
}

extern "C" void esp_mqtt_process_call_command(esp_peer_signaling_handle_t h, call_command_t cmd, const char* peer_id) {
    if (!h || g_mqtt_sig_instance != h) return;
    mqtt_sig_t *sg = (mqtt_sig_t *)h;
    call_state_machine_process_command(&sg->state_machine, cmd, peer_id);
}

extern "C" void esp_mqtt_set_ice_update_callback(esp_peer_signaling_handle_t h, ice_info_update_callback_t callback, void* ctx) {
    if (!h || g_mqtt_sig_instance != h) return;
    mqtt_sig_t *sg = (mqtt_sig_t *)h;
    sg->ice_update_callback = callback;
    sg->ice_update_ctx = ctx;
}

const esp_peer_signaling_impl_t* esp_signaling_get_mqtt_impl(void) {
    static const esp_peer_signaling_impl_t impl = {
        .start = mqtt_signal_start,
        .send_msg = mqtt_signal_send_msg,
        .stop = mqtt_signal_stop,
    };
    return &impl;
}
