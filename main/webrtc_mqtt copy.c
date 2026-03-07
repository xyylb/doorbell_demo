/* Voice Call WebRTC application code

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include "signal_mqtt.h"
#include "esp_webrtc.h"
#include "media_lib_os.h"
#include "driver/gpio.h"
#include "common.h"
#include "esp_log.h"
#include "esp_webrtc_defaults.h"
#include "esp_peer_default.h"
#include "call_state_machine.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "VOICE_CALL"

#define SAME_STR(a, b) (strncmp(a, b, sizeof(b) - 1) == 0)

typedef enum {
    TONE_RING,
    TONE_JOIN_SUCCESS,
} tone_type_t;

typedef struct {
    const uint8_t *start;
    const uint8_t *end;
    int            duration;
} tone_data_t;

static esp_webrtc_handle_t webrtc = NULL;
static esp_peer_signaling_handle_t sig_handle = NULL;
static bool monitor_key = false;
static bool ringing = false;
static bool webrtc_enabled = false;

extern const uint8_t ring_music_start[] asm("_binary_ring_aac_start");
extern const uint8_t ring_music_end[] asm("_binary_ring_aac_end");
extern const uint8_t join_music_start[] asm("_binary_ring_aac_start");
extern const uint8_t join_music_end[] asm("_binary_ring_aac_end");

static void ice_info_update_callback(void* ctx) {
    ESP_LOGI(TAG, "ICE info updated, checking if we need to create peer connection");
    if (webrtc && sig_handle) {
        bool signaling_ready = esp_mqtt_is_signaling_ready(sig_handle);
        ESP_LOGI(TAG, "Signaling ready: %s, webrtc_enabled: %d", signaling_ready ? "yes" : "no", webrtc_enabled);
        
        if (signaling_ready && !webrtc_enabled) {
            ESP_LOGI(TAG, "Creating peer connection with ICE info");
            
            extern int esp_webrtc_debug_get_signaling_state(esp_webrtc_handle_t handle);
            int sig_state = esp_webrtc_debug_get_signaling_state(webrtc);
            ESP_LOGI(TAG, "Signaling state before creating PC: %d", sig_state);
            
            webrtc_enabled = true;
            int ret = esp_webrtc_enable_peer_connection(webrtc, true);
            ESP_LOGI(TAG, "Enable peer connection result: %d", ret);
            
            vTaskDelay(pdMS_TO_TICKS(100));
            
            sig_state = esp_webrtc_debug_get_signaling_state(webrtc);
            ESP_LOGI(TAG, "Signaling state after creating PC: %d", sig_state);
        } else {
            ESP_LOGI(TAG, "Skipping peer connection creation: signaling_ready=%d, webrtc_enabled=%d", 
                     signaling_ready, webrtc_enabled);
        }
    }
}

static int play_tone(tone_type_t type)
{
    tone_data_t tone_data[] = {
        { ring_music_start, ring_music_end, 4000 },
        { join_music_start, join_music_end, 0 },
    };
    if (type >= sizeof(tone_data) / sizeof(tone_data[0])) {
        return 0;
    }
    return play_music(tone_data[type].start, (int)(tone_data[type].end - tone_data[type].start), tone_data[type].duration);
}

static void start_ringing(void) {
    if (!ringing) {
        ringing = true;
        play_tone(TONE_RING);
    }
}

static void stop_ringing(void) {
    if (ringing) {
        ringing = false;
        stop_music();
    }
}

static int webrtc_event_handler(esp_webrtc_event_t *event, void *ctx)
{
    ESP_LOGI(TAG, "WebRTC event: %d", event->type);
    
    if (event->type == ESP_WEBRTC_EVENT_CONNECTING) {
        ESP_LOGI(TAG, "WebRTC connecting...");
    } 
    else if (event->type == ESP_WEBRTC_EVENT_CONNECTED) {
        ESP_LOGI(TAG, "WebRTC connected, call established");
        stop_ringing();
    } 
    else if (event->type == ESP_WEBRTC_EVENT_CONNECT_FAILED) {
        ESP_LOGE(TAG, "WebRTC connection failed");
        stop_ringing();
    } 
    else if (event->type == ESP_WEBRTC_EVENT_DISCONNECTED) {
        ESP_LOGI(TAG, "WebRTC disconnected");
        stop_ringing();
    }
    return 0;
}

static void call_state_callback(call_state_t old_state, call_state_t new_state, const char* peer_id, void* ctx)
{
    ESP_LOGI(TAG, "Call state: %s -> %s (peer: %s)", 
             call_state_machine_get_state_name(old_state),
             call_state_machine_get_state_name(new_state),
             peer_id ? peer_id : "NULL");
    
    switch (new_state) {
        case CALL_STATE_INCOMING:
            ESP_LOGI(TAG, "Incoming call from %s, ring tone playing", peer_id);
            ESP_LOGI(TAG, "Commands: answer / reject");
            start_ringing();
            break;
            
        case CALL_STATE_CALLING:
            ESP_LOGI(TAG, "Calling %s, waiting for answer...", peer_id ? peer_id : "unknown");
            if (peer_id && sig_handle) {
                ESP_LOGI(TAG, "Sending RING message via MQTT...");
                extern void esp_mqtt_send_ring(esp_peer_signaling_handle_t h);
                esp_mqtt_send_ring(sig_handle);
            }
            break;
            
        case CALL_STATE_IN_CALL:
            stop_ringing();
            ESP_LOGI(TAG, "Call established with %s", peer_id);
            ESP_LOGI(TAG, "Command: close (to hang up)");
            break;
            
        case CALL_STATE_IDLE:
            stop_ringing();
            if (webrtc) {
                esp_webrtc_enable_peer_connection(webrtc, false);
            }
            ESP_LOGI(TAG, "Call ended, state: IDLE");
            break;
    }
}

static int mqtt_on_custom_data(esp_webrtc_custom_data_via_t via, uint8_t *data, int size, void *ctx)
{
    if (size == 0 || data == NULL) {
        return 0;
    }
    ESP_LOGI(TAG, "=== mqtt_on_custom_data called === via=%d, size=%d, data=%.*s", via, size, size, (char*)data);
    
    const char *cmd = (const char *)data;
    if (strncmp(cmd, "ACCEPT_CALL", 11) == 0) {
        ESP_LOGI(TAG, "=== ACCEPT_CALL detected ===");
        ESP_LOGI(TAG, "Peer accepted call, enabling peer connection...");
        if (webrtc) {
            webrtc_enabled = true;
            
            ESP_LOGI(TAG, "=== Step 1: Calling esp_webrtc_enable_peer_connection ===");
            int ret = esp_webrtc_enable_peer_connection(webrtc, true);
            ESP_LOGI(TAG, "=== Step 1 result: %d ===", ret);
            
            vTaskDelay(pdMS_TO_TICKS(200));
            
            ESP_LOGI(TAG, "=== Step 2: Getting peer connection handle ===");
            esp_peer_handle_t pc = NULL;
            int ret_pc = esp_webrtc_get_peer_connection(webrtc, &pc);
            ESP_LOGI(TAG, "=== Step 2: Peer connection handle: %p, ret: %d ===", pc, ret_pc);
            
            if (pc) {
                ESP_LOGI(TAG, "=== Step 3: Calling esp_peer_new_connection to generate SDP ===");
                ret = esp_peer_new_connection(pc);
                ESP_LOGI(TAG, "=== Step 3 result: %d ===", ret);
            } else {
                ESP_LOGE(TAG, "=== ERROR: Peer connection is NULL! ===");
            }
        }
    } else if (strncmp(cmd, "REJECT_CALL", 11) == 0) {
        ESP_LOGI(TAG, "Peer rejected call");
        if (webrtc) {
            esp_webrtc_enable_peer_connection(webrtc, false);
        }
    }
    return 0;
}

int voice_call_offer(const char* target_id)
{
    ESP_LOGI(TAG, "voice_call_offer called with target_id: %s", target_id ? target_id : "NULL");
    
    if (!webrtc || !sig_handle) {
        ESP_LOGE(TAG, "WebRTC not initialized: webrtc=%p, sig_handle=%p", webrtc, sig_handle);
        return -1;
    }
    
    call_state_t state = esp_mqtt_get_call_state(sig_handle);
    ESP_LOGI(TAG, "Current call state: %s (%d)", call_state_machine_get_state_name(state), state);
    
    if (state != CALL_STATE_IDLE) {
        ESP_LOGE(TAG, "Cannot offer: current state is %s", call_state_machine_get_state_name(state));
        return -1;
    }
    
    bool signaling_ready = esp_mqtt_is_signaling_ready(sig_handle);
    ESP_LOGI(TAG, "Signaling ready: %s", signaling_ready ? "yes" : "no");
    
    if (!signaling_ready) {
        ESP_LOGE(TAG, "Signaling not ready, wait for ICE response");
        return -1;
    }
    
    ESP_LOGI(TAG, "Initiating call to %s", target_id);
    ESP_LOGI(TAG, "Setting peer...");
    esp_mqtt_set_peer(sig_handle, target_id);
    
    ESP_LOGI(TAG, "Processing CALL_CMD_OFFER...");
    esp_mqtt_process_call_command(sig_handle, CALL_CMD_OFFER, target_id);
    
    ESP_LOGI(TAG, "Like WiFi mode, wait for peer response before enabling peer connection...");
    ESP_LOGI(TAG, "The peer connection will be enabled when peer accepts the call");
    
    return 0;
}

int voice_call_answer(void)
{
    if (!webrtc || !sig_handle) {
        ESP_LOGE(TAG, "WebRTC not initialized");
        return -1;
    }
    
    call_state_t state = esp_mqtt_get_call_state(sig_handle);
    if (state != CALL_STATE_INCOMING) {
        ESP_LOGE(TAG, "Cannot answer: current state is %s", call_state_machine_get_state_name(state));
        return -1;
    }
    
    ESP_LOGI(TAG, "Answering call");
    esp_mqtt_process_call_command(sig_handle, CALL_CMD_ANSWER, NULL);
    
    esp_webrtc_enable_peer_connection(webrtc, true);
    
    return 0;
}

int voice_call_reject(void)
{
    if (!sig_handle) {
        ESP_LOGE(TAG, "Signaling not initialized");
        return -1;
    }
    
    call_state_t state = esp_mqtt_get_call_state(sig_handle);
    if (state != CALL_STATE_INCOMING) {
        ESP_LOGE(TAG, "Cannot reject: current state is %s", call_state_machine_get_state_name(state));
        return -1;
    }
    
    ESP_LOGI(TAG, "Rejecting call");
    esp_mqtt_send_reject(sig_handle);
    
    return 0;
}

int voice_call_close(void)
{
    if (!webrtc || !sig_handle) {
        ESP_LOGE(TAG, "WebRTC not initialized");
        return -1;
    }
    
    call_state_t state = esp_mqtt_get_call_state(sig_handle);
    if (state == CALL_STATE_IDLE) {
        ESP_LOGW(TAG, "Already idle, nothing to close");
        return 0;
    }
    
    ESP_LOGI(TAG, "Hanging up call");
    
    esp_mqtt_process_call_command(sig_handle, CALL_CMD_CLOSE, NULL);
    
    esp_webrtc_enable_peer_connection(webrtc, false);
    
    return 0;
}

const char* voice_call_get_state_name(void)
{
    if (!sig_handle) return "UNKNOWN";
    call_state_t state = esp_mqtt_get_call_state(sig_handle);
    return call_state_machine_get_state_name(state);
}

const char* voice_call_get_peer_id(void)
{
    if (!sig_handle) return "";
    return esp_mqtt_get_peer_id(sig_handle);
}

static void key_monitor_thread(void *arg)
{
    gpio_config_t io_conf;
    memset(&io_conf, 0, sizeof(io_conf));
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = BIT64(DOOR_BELL_RING_BUTTON);
    io_conf.pull_down_en = 1;
    gpio_config(&io_conf);

    media_lib_thread_sleep(50);
    int last_level = gpio_get_level(DOOR_BELL_RING_BUTTON);
    int init_level = last_level;

    while (monitor_key) {
        media_lib_thread_sleep(50);
        int level = gpio_get_level(DOOR_BELL_RING_BUTTON);
        if (level != last_level) {
            last_level = level;
            if (level != init_level) {
                call_state_t state = esp_mqtt_get_call_state(sig_handle);
                if (state == CALL_STATE_IDLE) {
                    ESP_LOGI(TAG, "Button pressed, initiating call");
                    voice_call_offer("web_client_001");
                } else if (state == CALL_STATE_INCOMING) {
                    ESP_LOGI(TAG, "Button pressed, answering call");
                    voice_call_answer();
                } else if (state == CALL_STATE_IN_CALL || state == CALL_STATE_CALLING) {
                    ESP_LOGI(TAG, "Button pressed, hanging up");
                    voice_call_close();
                }
            }
        }
    }
    media_lib_thread_destroy(NULL);
}

int start_webrtc_mqtt(void)
{
    if (webrtc) {
        esp_webrtc_close(webrtc);
        webrtc = NULL;
    }
    
    monitor_key = true;
    media_lib_thread_handle_t key_thread;
    media_lib_thread_create_from_scheduler(&key_thread, "Key", key_monitor_thread, NULL);

    esp_peer_default_cfg_t peer_cfg = {
        .agent_recv_timeout = 5000,
        .ipv6_support = false,
    };
    
    esp_webrtc_cfg_t cfg = {
        .peer_cfg = {
            .audio_info = {
                .codec = ESP_PEER_AUDIO_CODEC_OPUS,
                .sample_rate = 16000,
                .channel = 1,
            },
            .video_info = {
                .codec = ESP_PEER_VIDEO_CODEC_NONE,
            },
            .audio_dir = ESP_PEER_MEDIA_DIR_SEND_RECV,
            .video_dir = ESP_PEER_MEDIA_DIR_NONE,
            .enable_data_channel = true,
            .on_custom_data = mqtt_on_custom_data,
            .no_auto_reconnect = true,
            .extra_cfg = &peer_cfg,
            .extra_size = sizeof(peer_cfg),
        },
        .signaling_cfg = {
            .signal_url = NULL,
        },
        .peer_impl = esp_peer_get_default_impl(),
        .signaling_impl = esp_signaling_get_mqtt_impl(),
    };
    
    int ret = esp_webrtc_open(&cfg, &webrtc);
    if (ret != 0) {
        ESP_LOGE(TAG, "Fail to open webrtc");
        return ret;
    }
    
    esp_webrtc_media_provider_t media_provider = {};
    media_sys_get_provider(&media_provider);
    esp_webrtc_set_media_provider(webrtc, &media_provider);

    esp_webrtc_set_event_handler(webrtc, webrtc_event_handler, NULL);

    esp_webrtc_enable_peer_connection(webrtc, false);

    ret = esp_webrtc_start(webrtc);
    if (ret != 0) {
        ESP_LOGE(TAG, "Fail to start webrtc");
        return ret;
    }
    
    sig_handle = esp_mqtt_get_signaling_handle();
    if (!sig_handle) {
        ESP_LOGE(TAG, "Failed to get signaling handle");
        return -1;
    }
    
    esp_mqtt_set_call_state_callback(sig_handle, call_state_callback, NULL);
    esp_mqtt_set_ice_update_callback(sig_handle, ice_info_update_callback, NULL);
    
    play_tone(TONE_JOIN_SUCCESS);
    
    ESP_LOGI(TAG, "Voice call system ready");
    ESP_LOGI(TAG, "Commands: offer <target_id> / answer / reject / close");
    
    return 0;
}

void query_webrtc(void)
{
    if (webrtc) {
        esp_webrtc_query(webrtc);
        
        if (sig_handle) {
            esp_mqtt_update_state_machine(sig_handle);
        }
    }
}

int stop_webrtc(void)
{
    if (webrtc) {
        monitor_key = false;
        stop_ringing();
        esp_webrtc_handle_t handle = webrtc;
        webrtc = NULL;
        sig_handle = NULL;
        ESP_LOGI(TAG, "Closing webrtc %p", handle);
        esp_webrtc_close(handle);
    }
    return 0;
}

void send_cmd(char *cmd)
{
    if (!cmd) return;
    
    if (SAME_STR(cmd, "ring")) {
        call_state_t state = esp_mqtt_get_call_state(sig_handle);
        if (state == CALL_STATE_IDLE) {
            voice_call_offer("web_client_001");
        }
    }
}
