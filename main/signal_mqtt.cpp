/**
 * ESPRESSIF MIT License
 *
 * Copyright (c) 2025 ESPRESSIF SYSTEMS (SHANGHAI) CO., LTD
 *
 * Permission is hereby granted for use on all ESPRESSIF SYSTEMS products...
 * (Full license text retained from original file)
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
#include "network_4g.h"
#include <string>
#include <cstring>
#include <signal_mqtt.h>


#define TAG "APPRTC_MQTT_SIG"

// 全局设备ID（需在主程序定义：extern const char* g_device_id;）
extern const char* g_device_id;

// MQTT主题模板
#define UP_TOPIC_TEMPLATE   "/voice/project/%s/up"
#define DOWN_TOPIC_TEMPLATE "/voice/project/%s/down"

typedef struct {
    esp_peer_signaling_ice_info_t ice_info;      // TURN凭证存储
    esp_peer_signaling_cfg_t      cfg;           // 回调配置
    std::string                   peer_device_id; // 当前通话对端ID
    bool                          is_initiator;   // ✅ 新增：true=呼出方(发offer), false=呼入方(发answer)
    bool                          ice_reloading;  // ICE刷新任务状态
    bool                          ice_reload_stop;
    uint32_t                      last_ice_request_time;
} mqtt_sig_t;

// 全局实例（用于MQTT回调）
static mqtt_sig_t* g_mqtt_sig_instance = nullptr;

// ==================== 辅助函数 ====================

static void free_ice_info(esp_peer_signaling_ice_info_t *info) {
    if (info->server_info.stun_url) { free(info->server_info.stun_url); info->server_info.stun_url = nullptr; }
    if (info->server_info.user) { free(info->server_info.user); info->server_info.user = nullptr; }
    if (info->server_info.psw) { free(info->server_info.psw); info->server_info.psw = nullptr; }
    memset(info, 0, sizeof(esp_peer_signaling_ice_info_t));
}

// 从服务器返回的ice_response解析TURN凭证
static void parse_ice_response(mqtt_sig_t* sg, cJSON* ice_servers_json) {
    if (!ice_servers_json || !cJSON_IsArray(ice_servers_json) || cJSON_GetArraySize(ice_servers_json) == 0) {
        ESP_LOGE(TAG, "Invalid ice_servers in response");
        return;
    }

    // 仅使用第一个服务器（简化处理）
    cJSON* server = cJSON_GetArrayItem(ice_servers_json, 0);
    if (!server) return;

    cJSON* urls_arr = cJSON_GetObjectItem(server, "urls");
    if (urls_arr && cJSON_IsArray(urls_arr)) {
        cJSON* url = cJSON_GetArrayItem(urls_arr, 0);
        if (url && cJSON_IsString(url)) {
            sg->ice_info.server_info.stun_url = strdup(url->valuestring);
        }
    }

    cJSON* user = cJSON_GetObjectItem(server, "username");
    if (user && cJSON_IsString(user)) {
        sg->ice_info.server_info.user = strdup(user->valuestring);
    }

    cJSON* cred = cJSON_GetObjectItem(server, "credential");
    if (cred && cJSON_IsString(cred)) {
        sg->ice_info.server_info.psw = strdup(cred->valuestring);
    }

    sg->ice_info.is_initiator = false; // 由具体通话流程决定
    ESP_LOGI(TAG, "TURN updated: url=%s, user=%s", 
             sg->ice_info.server_info.stun_url ? sg->ice_info.server_info.stun_url : "N/A",
             sg->ice_info.server_info.user ? sg->ice_info.server_info.user : "N/A");
}

// 发布消息到UP_TOPIC
static void mqtt_publish_up(const char* payload) {
    char topic[128];
    snprintf(topic, sizeof(topic), UP_TOPIC_TEMPLATE, g_device_id);
    Mqtt* mqtt = Network4g::GetMqttInstance();
    if (mqtt) {
        mqtt->Publish(topic, payload);
        ESP_LOGD(TAG, "MQTT PUB [%s]: %s", topic, payload);
    } else {
        ESP_LOGE(TAG, "MQTT instance unavailable");
    }
}

// ==================== ICE刷新任务 ====================
static void ice_refresh_task(void *arg) {
    mqtt_sig_t *sg = (mqtt_sig_t *)arg;
    sg->ice_reloading = true;
    sg->last_ice_request_time = time(nullptr);

    while (!sg->ice_reload_stop) {
        uint32_t now = time(nullptr);
        // 每小时请求一次（服务器可决定是否更新凭证）
        if (now - sg->last_ice_request_time >= 3600) {
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
        media_lib_thread_sleep(1000); // 每秒检查
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

// ==================== MQTT消息处理 ====================
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
        // 1. 接收TURN信息（处理保存留用）
        cJSON *ice_servers = cJSON_GetObjectItem(root, "ice_servers");
        if (ice_servers) {
            parse_ice_response(sg, ice_servers);
            if (sg->cfg.on_ice_info) {
                sg->cfg.on_ice_info(&sg->ice_info, sg->cfg.ctx);
            }
        }
    }
    else if (strcmp(msg_type, "offer") == 0) {
        // 8. 呼入（接收） + 4. 对方接听处理（实际是收到offer）
        cJSON *from = cJSON_GetObjectItem(root, "from");
        if (from && cJSON_IsString(from)) {
            sg->peer_device_id = from->valuestring;
            sg->is_initiator = false; // 明确角色：被叫方
            ESP_LOGI(TAG, "Incoming call from %s", sg->peer_device_id.c_str());
        }
        cJSON *sdp = cJSON_GetObjectItem(root, "sdp");
        if (sdp && cJSON_IsString(sdp)) {
            sig_msg.type = ESP_PEER_SIGNALING_MSG_SDP;
            sig_msg.data = (uint8_t*)sdp->valuestring;
            sig_msg.size = strlen(sdp->valuestring);
            if (sg->cfg.on_msg) sg->cfg.on_msg(&sig_msg, sg->cfg.ctx);
        }
    }
    else if (strcmp(msg_type, "answer") == 0) {
        // 4. 对方接听处理（收到answer）
        cJSON *sdp = cJSON_GetObjectItem(root, "sdp");
        if (sdp && cJSON_IsString(sdp)) {
            sig_msg.type = ESP_PEER_SIGNALING_MSG_SDP;
            sig_msg.data = (uint8_t*)sdp->valuestring;
            sig_msg.size = strlen(sdp->valuestring);
            if (sg->cfg.on_msg) sg->cfg.on_msg(&sig_msg, sg->cfg.ctx);
        }
    }
    else if (strcmp(msg_type, "candidate") == 0) {
        // 10. 接听后处理ICE信息
        cJSON *candidate = cJSON_GetObjectItem(root, "candidate");
        if (candidate && cJSON_IsString(candidate)) {
            sig_msg.type = ESP_PEER_SIGNALING_MSG_CANDIDATE;
            sig_msg.data = (uint8_t*)candidate->valuestring;
            sig_msg.size = strlen(candidate->valuestring);
            if (sg->cfg.on_msg) sg->cfg.on_msg(&sig_msg, sg->cfg.ctx);
        }
    }
    else if (strcmp(msg_type, "bye") == 0) {
        // 12. 对方挂断（接收）
        sig_msg.type = ESP_PEER_SIGNALING_MSG_BYE;
        clear_peer = true;
    }
    else if (strcmp(msg_type, "reject") == 0) {
        // 5. 对方拒接处理（接收）
        sig_msg.type = ESP_PEER_SIGNALING_MSG_BYE;
        clear_peer = true;
        ESP_LOGI(TAG, "Call rejected by peer");
    }
    else if (strcmp(msg_type, "timeout") == 0) {
        // 11. 对方呼叫超时（接收）
        sig_msg.type = ESP_PEER_SIGNALING_MSG_BYE;
        clear_peer = true;
        ESP_LOGI(TAG, "Call timeout from peer");
    }

    // 触发BYE类消息
    if (sig_msg.type == ESP_PEER_SIGNALING_MSG_BYE && sg->cfg.on_msg) {
        sg->cfg.on_msg(&sig_msg, sg->cfg.ctx);
    }

    // 清理会话
    if (clear_peer) {
        sg->peer_device_id.clear();
        sg->is_initiator = false; // 重置角色状态
    }

    cJSON_Delete(root);
}

// 全局MQTT回调（仅处理DOWN_TOPIC）
static void mqtt_message_handler(const std::string& topic, const std::string& payload) {
    if (!g_mqtt_sig_instance) return;

    char expected_topic[128];
    snprintf(expected_topic, sizeof(expected_topic), DOWN_TOPIC_TEMPLATE, g_device_id);
    if (topic != expected_topic) return;

    ESP_LOGD(TAG, "MQTT SUB [%s]: %s", topic.c_str(), payload.c_str());
    handle_downstream_message(g_mqtt_sig_instance, payload.c_str());
}

// ==================== 信令接口实现 ====================

static int mqtt_signal_start(esp_peer_signaling_cfg_t *cfg, esp_peer_signaling_handle_t *h) {
    ESP_LOGI(TAG, "初始化调用mqtt_signal_start");
    if (!cfg || !h || !g_device_id) {
        ESP_LOGE(TAG, "Invalid config or missing g_device_id");
        return -1;
    }

    mqtt_sig_t *sg = new(std::nothrow) mqtt_sig_t();
    if (!sg) return -1;
    memset(sg, 0, sizeof(mqtt_sig_t));
    sg->cfg = *cfg;

    // 设置全局实例供回调使用
    g_mqtt_sig_instance = sg;

    // 订阅下行主题
    Mqtt* mqtt = Network4g::GetMqttInstance();
    if (!mqtt) {
        ESP_LOGE(TAG, "MQTT not initialized");
        delete sg;
        return -1;
    }
    //mqtt->OnMessage(mqtt_message_handler);
	mqtt->OnMessage([](const std::string& topic, const std::string& payload) {
		mqtt_message_handler(topic, payload);
	});

    char down_topic[128];
    snprintf(down_topic, sizeof(down_topic), DOWN_TOPIC_TEMPLATE, g_device_id);
    ESP_LOGI(TAG, "Subscribed to %s", down_topic);

    // 立即请求TURN凭证（替代原HTTP方式）
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

    // 启动定时刷新任务
    if (start_ice_refresh_timer(sg) != 0) {
        ESP_LOGW(TAG, "Failed to start ICE refresh timer");
    }

    // 通知上层连接成功
    if (sg->cfg.on_connected) {
        sg->cfg.on_connected(sg->cfg.ctx);
    }

    *h = sg;
    return 0;
}

static int mqtt_signal_send_msg(esp_peer_signaling_handle_t h, esp_peer_signaling_msg_t *msg) {
    if (!h || !msg || g_mqtt_sig_instance != h) return -1;
    mqtt_sig_t *sg = (mqtt_sig_t *)h;

    // 构建基础消息
    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "target", sg->peer_device_id.c_str()); // 服务器路由关键字段
    cJSON_AddNumberToObject(json, "timestamp", time(nullptr));

    // 根据消息类型填充
    if (msg->type == ESP_PEER_SIGNALING_MSG_SDP) {
        const char* sdp_type = sg->is_initiator ? "offer" : "answer";
        // 3. 呼出信令（发出） / 9. 接听（发出自己的SDP）
        //const char* sdp_type = sg->peer_device_id.empty() ? "offer" : "answer"; // 简化：无peer时视为呼出
        cJSON_AddStringToObject(json, "type", sdp_type);
        cJSON_AddStringToObject(json, "sdp", (char*)msg->data);
        ESP_LOGI(TAG, "Sending %s to %s", sdp_type, sg->peer_device_id.c_str());
    }
    else if (msg->type == ESP_PEER_SIGNALING_MSG_CANDIDATE) {
        // ICE候选（发出）
        cJSON_AddStringToObject(json, "type", "candidate");
        cJSON_AddStringToObject(json, "candidate", (char*)msg->data);
    }
    else if (msg->type == ESP_PEER_SIGNALING_MSG_BYE) {
        // 7. 挂断（发出）
        cJSON_AddStringToObject(json, "type", "bye");
        sg->peer_device_id.clear(); // 清理会话
        sg->is_initiator = false;
    }
    else if (msg->type == ESP_PEER_SIGNALING_MSG_CUSTOMIZED) {
        cJSON_AddStringToObject(json, "type", "customized");
        cJSON_AddStringToObject(json, "data", (char*)msg->data);
    }
    else {
        cJSON_Delete(json);
        return -1;
    }

    char *payload = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (!payload) return -1;

    mqtt_publish_up(payload);
    free(payload);
    return 0;
}

static int mqtt_signal_stop(esp_peer_signaling_handle_t h) {
    if (!h || g_mqtt_sig_instance != h) return 0;
    mqtt_sig_t *sg = (mqtt_sig_t *)h;

    // 发送bye（如果存在会话）
    if (!sg->peer_device_id.empty()) {
        esp_peer_signaling_msg_t bye = {}; // ✅ 修复：C++安全初始化
        bye.type = ESP_PEER_SIGNALING_MSG_BYE;
        mqtt_signal_send_msg(h, &bye);
        media_lib_thread_sleep(200);
    }

    // 清理资源
    stop_ice_refresh_timer(sg);
    free_ice_info(&sg->ice_info);
    g_mqtt_sig_instance = nullptr;
    delete sg;
    return 0;
}

// ==================== 辅助API（供上层调用） ====================

/**
 * 设置通话目标设备ID（呼出前必须调用）
 * @param h 信令句柄
 * @param peer_id 目标设备ID
 */
extern "C" void esp_mqtt_set_peer(esp_peer_signaling_handle_t h, const char* peer_id) {
    if (!h || !peer_id) return;
    mqtt_sig_t *sg = (mqtt_sig_t *)h;
    sg->peer_device_id = peer_id;
    sg->is_initiator = true; // ✅ 关键：设置为发起方
    ESP_LOGI(TAG, "Set peer to %s", peer_id);
}

/**
 * 发送拒接消息（设备主动拒接呼入）
 * @param h 信令句柄
 */
extern "C" void esp_mqtt_send_reject(esp_peer_signaling_handle_t h) {
    if (!h || g_mqtt_sig_instance != h) return;
    mqtt_sig_t *sg = (mqtt_sig_t *)h;
    if (sg->peer_device_id.empty()) return;

    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "target", sg->peer_device_id.c_str());
    cJSON_AddStringToObject(json, "type", "reject");
    cJSON_AddNumberToObject(json, "timestamp", time(nullptr));
    char *payload = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (payload) {
        mqtt_publish_up(payload);
        free(payload);
        sg->peer_device_id.clear(); // 清理会话
        sg->is_initiator = false;
        ESP_LOGI(TAG, "Sent reject to %s", sg->peer_device_id.c_str());
    }
}

/**
 * 发送呼叫超时通知（设备侧超时）
 * @param h 信令句柄
 */
extern "C" void esp_mqtt_send_timeout(esp_peer_signaling_handle_t h) {
    if (!h || g_mqtt_sig_instance != h) return;
    mqtt_sig_t *sg = (mqtt_sig_t *)h;
    if (sg->peer_device_id.empty()) return;

    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "target", sg->peer_device_id.c_str());
    cJSON_AddStringToObject(json, "type", "timeout");
    cJSON_AddNumberToObject(json, "timestamp", time(nullptr));
    char *payload = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (payload) {
        mqtt_publish_up(payload);
        free(payload);
        sg->peer_device_id.clear();
        ESP_LOGI(TAG, "Sent timeout to %s", sg->peer_device_id.c_str());
    }
}

// ==================== 导出信令实现 ====================
const esp_peer_signaling_impl_t* esp_signaling_get_mqtt_impl(void) {
    static const esp_peer_signaling_impl_t impl = {
        .start = mqtt_signal_start,
        .send_msg = mqtt_signal_send_msg,
        .stop = mqtt_signal_stop,
    };
    return &impl;
}