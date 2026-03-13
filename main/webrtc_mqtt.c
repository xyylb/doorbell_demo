/* DoorBell WebRTC application code

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include "esp_webrtc.h"
#include "media_lib_os.h"
#include "driver/gpio.h"
#include "common.h"
#include "esp_log.h"
#include "esp_webrtc_defaults.h"
#include "esp_peer_default.h"
#include "mqtt_signaling.h"
#include "door_bell_audio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"

static void start_call_timeout(void);
static void stop_call_timeout(void);

#define TAG "DOOR_BELL"

// Customized commands
#define DOOR_BELL_OPEN_DOOR_CMD     "OPEN_DOOR"
#define DOOR_BELL_DOOR_OPENED_CMD   "DOOR_OPENED"
#define DOOR_BELL_RING_CMD          "RING"
#define DOOR_BELL_CALL_ACCEPTED_CMD "ACCEPT_CALL"
#define DOOR_BELL_CALL_DENIED_CMD   "DENY_CALL"
#define DOOR_BELL_CALL_REJECTED_CMD "REJECT_CALL"

#define SAME_STR(a, b) (strncmp(a, b, sizeof(b) - 1) == 0)
#define SEND_CMD(webrtc, cmd) \
    esp_webrtc_send_custom_data(webrtc, ESP_WEBRTC_CUSTOM_DATA_VIA_SIGNALING, (uint8_t *)cmd, strlen(cmd))

typedef enum {
    DOOR_BELL_STATE_NONE,
    DOOR_BELL_STATE_RINGING,
    DOOR_BELL_STATE_CONNECTING,
    DOOR_BELL_STATE_CONNECTED,
} door_bell_state_t;


static esp_webrtc_handle_t webrtc;
static door_bell_state_t   door_bell_state;
static bool                monitor_key;
static char                call_target[32];
static TimerHandle_t        call_timeout_timer;

extern const uint8_t ring_music_start[] asm("_binary_ring_aac_start");
extern const uint8_t ring_music_end[] asm("_binary_ring_aac_end");
extern const uint8_t open_music_start[] asm("_binary_open_aac_start");
extern const uint8_t open_music_end[] asm("_binary_open_aac_end");
extern const uint8_t join_music_start[] asm("_binary_join_aac_start");
extern const uint8_t join_music_end[] asm("_binary_join_aac_end");

static void door_bell_change_state(door_bell_state_t state)
{
    door_bell_state = state;
    if (state == DOOR_BELL_STATE_CONNECTING || state == DOOR_BELL_STATE_NONE) {
        stop_music();
    }
}

static int signaling_on_msg(esp_peer_signaling_msg_t *msg, void *ctx)
{
    if (msg->type == ESP_PEER_SIGNALING_MSG_CUSTOMIZED && msg->data && msg->size > 0) {
        const char *cmd = (const char *)msg->data;
        ESP_LOGI(TAG, "Received customized message: %s", cmd);
        
        if (SAME_STR(cmd, DOOR_BELL_RING_CMD)) {
            play_tone(DOOR_BELL_TONE_RING);
        }
    }
    return 0;
}

static int door_bell_on_cmd(esp_webrtc_custom_data_via_t via, uint8_t *data, int size, void *ctx)
{
    if (size == 0 || webrtc == NULL) {
        return 0;
    }
    ESP_LOGI(TAG, "Receive command %.*s", size, (char *)data);
    const char *cmd = (const char *)data;
    if (SAME_STR(cmd, DOOR_BELL_RING_CMD)) {
        play_tone(DOOR_BELL_TONE_RING);
        if (door_bell_state < DOOR_BELL_STATE_CONNECTING) {
            door_bell_state = DOOR_BELL_STATE_CONNECTING;
            esp_webrtc_enable_peer_connection(webrtc, true);
        }
    } else if (SAME_STR(cmd, DOOR_BELL_OPEN_DOOR_CMD)) {
        // Reply with door OPENED
        SEND_CMD(webrtc, DOOR_BELL_DOOR_OPENED_CMD);
        // Only play tome when connection not build up
        if (door_bell_state < DOOR_BELL_STATE_CONNECTING) {
            play_tone(DOOR_BELL_TONE_OPEN_DOOR);
        }
    } else if (SAME_STR(cmd, DOOR_BELL_CALL_ACCEPTED_CMD)) {
        stop_call_timeout();
        esp_webrtc_enable_peer_connection(webrtc, true);
    } else if (SAME_STR(cmd, DOOR_BELL_CALL_DENIED_CMD)) {
        stop_call_timeout();
        esp_webrtc_enable_peer_connection(webrtc, false);
        door_bell_change_state(DOOR_BELL_STATE_NONE);
    } else if (SAME_STR(cmd, DOOR_BELL_CALL_REJECTED_CMD)) {
        ESP_LOGI(TAG, "Call rejected by peer");
        stop_call_timeout();
        esp_webrtc_enable_peer_connection(webrtc, false);
        door_bell_change_state(DOOR_BELL_STATE_NONE);
    }
    return 0;
}                                            

static int webrtc_event_handler(esp_webrtc_event_t *event, void *ctx)
{
    if (event->type == ESP_WEBRTC_EVENT_CONNECTING) {
        door_bell_change_state(DOOR_BELL_STATE_CONNECTING);
    } else if (event->type == ESP_WEBRTC_EVENT_CONNECTED) {
        door_bell_change_state(DOOR_BELL_STATE_CONNECTED);
    } else if (event->type == ESP_WEBRTC_EVENT_DISCONNECTED) {
        stop_call_timeout();
        door_bell_change_state(DOOR_BELL_STATE_NONE);
    } else if (event->type == ESP_WEBRTC_EVENT_CONNECT_FAILED) {
        stop_call_timeout();
        door_bell_change_state(DOOR_BELL_STATE_NONE);
    }
    return 0;
}

void send_cmd(char *cmd)
{
    if (SAME_STR(cmd, "ring")) {
        SEND_CMD(webrtc, DOOR_BELL_RING_CMD);
        ESP_LOGI(TAG, "Ring button on state %d", door_bell_state);
        if (door_bell_state < DOOR_BELL_STATE_CONNECTING) {
            door_bell_state = DOOR_BELL_STATE_RINGING;
            play_tone(DOOR_BELL_TONE_RING);
            mqtt_sig_send_ring(NULL);
            start_call_timeout();
        }
    } else if (SAME_STR(cmd, "answer")) {
        ESP_LOGI(TAG, "User answering call");
        mqtt_sig_answer_call();
        stop_call_timeout();
    } else if (SAME_STR(cmd, "reject")) {
        ESP_LOGI(TAG, "User rejecting call");
        mqtt_sig_reject_call();
        stop_call_timeout();
        door_bell_change_state(DOOR_BELL_STATE_NONE);
        door_bell_state = DOOR_BELL_STATE_NONE;
    } else if (SAME_STR(cmd, "close")) {
        ESP_LOGI(TAG, "User closing call");
        mqtt_sig_close_call();
        stop_call_timeout();
        if (webrtc) {
            ESP_LOGI(TAG, "Closing webrtc connection");
            esp_webrtc_close(webrtc);
            webrtc = NULL;
        }
        door_bell_change_state(DOOR_BELL_STATE_NONE);
        door_bell_state = DOOR_BELL_STATE_NONE;
    }
}

static void key_monitor_thread(void *arg);

static TaskHandle_t call_timeout_handle = NULL;

static void call_timeout_task(void *param)
{
    call_timeout_handle = xTaskGetCurrentTaskHandle();
    ESP_LOGI(TAG, "Call timeout task started, waiting 10 seconds...");
    vTaskDelay(pdMS_TO_TICKS(10000));
    
    ESP_LOGI(TAG, "Call timeout, sending timeout to peer");
    mqtt_sig_send_timeout(call_target);
    door_bell_change_state(DOOR_BELL_STATE_NONE);
    door_bell_state = DOOR_BELL_STATE_NONE;
    call_timeout_handle = NULL;
    vTaskDelete(NULL);
}

static void start_call_timeout(void)
{
    if (call_timeout_handle != NULL) {
        ESP_LOGI(TAG, "Deleting existing timeout task");
        vTaskDelete(call_timeout_handle);
        call_timeout_handle = NULL;
    }
    xTaskCreate(call_timeout_task, "call_timeout", 4096, NULL, 5, NULL);
    ESP_LOGI(TAG, "Call timeout task created (10 seconds)");
}

static void stop_call_timeout(void)
{
    if (call_timeout_handle != NULL) {
        ESP_LOGI(TAG, "Stopping call timeout, deleting task");
        vTaskDelete(call_timeout_handle);
        call_timeout_handle = NULL;
    }
}

static void key_monitor_thread(void *arg)
{
    gpio_config_t io_conf;
    memset(&io_conf, 0, sizeof(io_conf));
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = BIT64(DOOR_BELL_RING_BUTTON);
    io_conf.pull_down_en = 1;
    esp_err_t ret = 0;
    ret |= gpio_config(&io_conf);

    media_lib_thread_sleep(50);
    int last_level = gpio_get_level(DOOR_BELL_RING_BUTTON);
    int init_level = last_level;

    while (monitor_key) {
        media_lib_thread_sleep(50);
        int level = gpio_get_level(DOOR_BELL_RING_BUTTON);
        if (level != last_level) {
            last_level = level;
            if (level != init_level) {
                send_cmd("ring");
            }
        }
    }
    media_lib_thread_destroy(NULL);
}

int start_webrtc(char *url)
{
/*     if (network_is_connected() == false) {
        ESP_LOGE(TAG, "Wifi not connected yet");
        return -1;
    } */
    if (url[0] == 0) {
        ESP_LOGE(TAG, "Room Url not set yet");
        return -1;
    }
    if (webrtc) {
        esp_webrtc_close(webrtc);
        webrtc = NULL;
    }
    monitor_key = true;
    media_lib_thread_handle_t key_thread;
    media_lib_thread_create_from_scheduler(&key_thread, "Key", key_monitor_thread, NULL);

    esp_peer_default_cfg_t peer_cfg = {
        .agent_recv_timeout = 500,
    };
    esp_webrtc_cfg_t cfg = {
        .peer_cfg = {
            .audio_info = {
                .codec = ESP_PEER_AUDIO_CODEC_OPUS,
	            .sample_rate = 16000,   // G711A默认8k，也可改为16k
	            .channel = 1,
            },
            // 禁用视频
            .video_info = {
                .codec = ESP_PEER_VIDEO_CODEC_NONE,
            },
            .audio_dir = ESP_PEER_MEDIA_DIR_SEND_RECV,
            .video_dir = ESP_PEER_MEDIA_DIR_NONE,  // 视频方向置为NONE
            .on_custom_data = door_bell_on_cmd,
            .enable_data_channel = DATA_CHANNEL_ENABLED,
            .no_auto_reconnect = true,
            .extra_cfg = &peer_cfg,
            .extra_size = sizeof(peer_cfg),
        },
        .signaling_cfg = {
            .signal_url = url,
        },
        .peer_impl = esp_peer_get_default_impl(),
        .signaling_impl = esp_signaling_get_mqtt_impl(),
    };
    int ret = esp_webrtc_open(&cfg, &webrtc);
    if (ret != 0) {
        ESP_LOGE(TAG, "Fail to open webrtc");
        return ret;
    }
    
    mqtt_sig_set_webrtc_handle(webrtc);
    // 设置媒体提供者（仅音频）
    esp_webrtc_media_provider_t media_provider = {};
    media_sys_get_provider(&media_provider);
    esp_webrtc_set_media_provider(webrtc, &media_provider);

    // 设置事件处理
    esp_webrtc_set_event_handler(webrtc, webrtc_event_handler, NULL);

    // 禁用自动连接
    //esp_webrtc_enable_peer_connection(webrtc, false);
    esp_webrtc_enable_peer_connection(webrtc, false);

    // 启动webrtc
    ret = esp_webrtc_start(webrtc);
    if (ret != 0) {
        ESP_LOGE(TAG, "Fail to start webrtc");
    } else {
        play_tone(DOOR_BELL_TONE_JOIN_SUCCESS);
    }
    return ret;
}


void query_webrtc(void)
{
    if (webrtc) {
        esp_webrtc_query(webrtc);
    }
}

int stop_webrtc(void)
{
    if (webrtc) {
        monitor_key = false;
        esp_webrtc_handle_t handle = webrtc;
        webrtc = NULL;
        ESP_LOGI(TAG, "Start to close webrtc %p", handle);
        esp_webrtc_close(handle);
    }
    return 0;
}

//webrtc.cpp
