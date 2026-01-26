/* Media system

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include "codec_init.h"
#include "codec_board.h"
#include "av_render.h"
#include "av_render_default.h"
#include "common.h"
#include "esp_log.h"
#include "settings.h"
#include "media_lib_os.h"
#include "esp_timer.h"
#include "esp_audio_enc_default.h"
#include "esp_capture_defaults.h"
#include "esp_capture_sink.h"
#include "esp_audio_dec_default.h"

#define TAG "MEDIA_SYS"

#define RET_ON_NULL(ptr, v) do {                                \
    if (ptr == NULL) {                                          \
        ESP_LOGE(TAG, "Memory allocate fail on %d", __LINE__);  \
        return v;                                               \
    }                                                           \
} while (0)

// 简化capture_system，移除video_src
typedef struct {
    esp_capture_sink_handle_t   capture_handle;
    esp_capture_audio_src_if_t *aud_src;
} capture_system_t;

// 简化player_system，移除video_render
typedef struct {
    audio_render_handle_t audio_render;
    av_render_handle_t    player;
} player_system_t;

static capture_system_t capture_sys;
static player_system_t  player_sys;

static bool           music_playing  = false;
static bool           music_stopping = false;
static const uint8_t *music_to_play;
static int            music_size;
static int            music_duration;




static int build_capture_system(void)
{
    //先设置声音采集参数

   esp_capture_audio_aec_src_cfg_t codec_cfg = {
        .record_handle = get_record_handle(),
#if CONFIG_IDF_TARGET_ESP32S3
         .channel = 2,
         .channel_mask = 0x1,
         .data_on_vad = true,
         .mic_layout = "M"
 #endif
    };
    capture_sys.aud_src = esp_capture_new_audio_aec_src(&codec_cfg);
    

/*    esp_capture_audio_dev_src_cfg_t codec_cfg = {
        .record_handle = get_record_handle(),
    };
    capture_sys.aud_src = esp_capture_new_audio_dev_src(&codec_cfg);*/

    esp_capture_cfg_t cfg = {
        .sync_mode = ESP_CAPTURE_SYNC_MODE_AUDIO,
        .audio_src = capture_sys.aud_src,
        .video_src = NULL  // 禁用视频
    };
    esp_capture_open(&cfg, &capture_sys.capture_handle);

    // 强制启动采集，防止通路被默认禁用（兼容芯片必需）
    esp_capture_start(capture_sys.capture_handle);
    
    //设置麦克风增益
    int assda = esp_codec_dev_set_in_gain(get_playback_handle(), 20.0f);
    ESP_LOGE(TAG, "设置麦克风增益效果结果:%d", assda);
    ESP_LOGE(TAG, "ESP_CODEC_DEV_OK:%d", ESP_CODEC_DEV_OK);
    ESP_LOGE(TAG, "ESP_CODEC_DEV_INVALID_ARG:%d", ESP_CODEC_DEV_INVALID_ARG);
    ESP_LOGE(TAG, "ESP_CODEC_DEV_NOT_SUPPORT:%d", ESP_CODEC_DEV_NOT_SUPPORT);
    ESP_LOGE(TAG, "ESP_CODEC_DEV_WRONG_STATE:%d", ESP_CODEC_DEV_WRONG_STATE);
    
    ESP_LOGI(TAG, "兼容版INMP441采集系统启动成功");
    return 0;
}

static int build_player_system()
{
    // 初始化音频渲染（MAX98357）
    i2s_render_cfg_t i2s_cfg = {
        .fixed_clock = true,
        .play_handle = get_playback_handle(),
    };
    esp_codec_dev_set_out_vol(get_playback_handle(), 100);
    player_sys.audio_render = av_render_alloc_i2s_render(&i2s_cfg);
    
    if (player_sys.audio_render == NULL) {
        ESP_LOGE(TAG, "Fail to create audio render");
        return -1;
    }

    // 配置音频播放器，禁用视频
    av_render_cfg_t render_cfg = {
        .audio_render = player_sys.audio_render,
        .video_render = NULL,  // 禁用视频渲染
        .audio_raw_fifo_size = 4096,
        .audio_render_fifo_size = 6 * 1024,
        .video_raw_fifo_size = 0,  // 视频FIFO置0
        .allow_drop_data = false,
    };
    player_sys.player = av_render_open(&render_cfg);
    if (player_sys.player == NULL) {
        ESP_LOGE(TAG, "Fail to create player");
        return -1;
    }
    return 0;
}

int media_sys_buildup(void)
{
    // 仅注册音频编解码器
    esp_audio_enc_register_default();
    esp_audio_dec_register_default();
    
    // 构建音频采集/播放系统
    build_capture_system();
    build_player_system();
    return 0;
}

int media_sys_get_provider(esp_webrtc_media_provider_t *provide)
{
    provide->capture = capture_sys.capture_handle;
    provide->player = player_sys.player;
    return 0;
}


int test_capture_to_player(void)
{
    esp_capture_sink_cfg_t sink_cfg = {
        .audio_info = {
            .format_id = ESP_CAPTURE_FMT_ID_PCM,
            .sample_rate = 16000,
            .channel = 1,
            .bits_per_sample = 16,
        },
        .video_info = { .format_id = ESP_CAPTURE_FMT_ID_NONE }
    };

    esp_capture_sink_handle_t capture_path = NULL;
    esp_capture_sink_setup(capture_sys.capture_handle, 0, &sink_cfg, &capture_path);
    esp_capture_sink_enable(capture_path, ESP_CAPTURE_RUN_MODE_ALWAYS);

    av_render_audio_info_t render_aud_info = {
        .codec = AV_RENDER_AUDIO_CODEC_PCM,
        .sample_rate = 16000,
        .channel = 1,
        .bits_per_sample = 16,
    };
    av_render_add_audio_stream(player_sys.player, &render_aud_info);

    uint32_t start_time = (uint32_t)(esp_timer_get_time() / 1000);
    esp_capture_start(capture_sys.capture_handle);

    int frame_count = 0;
    while ((uint32_t)(esp_timer_get_time() / 1000) < start_time + 100000) {
        media_lib_thread_sleep(30);
        esp_capture_stream_frame_t frame = {
            .stream_type = ESP_CAPTURE_STREAM_TYPE_AUDIO,
        };
        while (esp_capture_sink_acquire_frame(capture_path, &frame, true) == ESP_CAPTURE_ERR_OK) {
            frame_count++;
            // 直接播放所有采集数据，不做任何过滤
            av_render_audio_data_t audio_data = {
                .data = frame.data,
                .size = frame.size,
                .pts = frame.pts,
            };
            av_render_add_audio_data(player_sys.player, &audio_data);
            esp_capture_sink_release_frame(capture_path, &frame);
        }
    }

    ESP_LOGI(TAG, "采集完成，共获取 %d 帧音频数据", frame_count);

    esp_capture_stop(capture_sys.capture_handle);
    av_render_reset(player_sys.player);
    return 0;
}

/*
int test_capture_to_player(void)
{
    // 创建G711A转换上下文
    pcm2g711a_ctx_t *g711a_ctx = pcm2g711a_create(16000, 1);
    if (g711a_ctx == NULL) {
        ESP_LOGE(TAG, "Failed to create G711A converter");
        return -1;
    }

    esp_capture_sink_cfg_t sink_cfg = {
        .audio_info = {
            .format_id = ESP_CAPTURE_FMT_ID_PCM,
            .sample_rate = 16000,
            .channel = 1,
            .bits_per_sample = 16,
        },
        .video_info = { .format_id = ESP_CAPTURE_FMT_ID_NONE }
    };

    esp_capture_sink_handle_t capture_path = NULL;
    esp_err_t ret = esp_capture_sink_setup(capture_sys.capture_handle, 0, &sink_cfg, &capture_path);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to setup capture sink: %d", ret);
        pcm2g711a_destroy(g711a_ctx);
        return ret;
    }
    esp_capture_sink_enable(capture_path, ESP_CAPTURE_RUN_MODE_ALWAYS);

    // PCM播放配置（回环测试）
    av_render_audio_info_t render_aud_info = {
        .codec = AV_RENDER_AUDIO_CODEC_PCM,
        .sample_rate = 16000,
        .channel = 1,
        .bits_per_sample = 16,
    };
    ret = av_render_add_audio_stream(player_sys.player, &render_aud_info);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add PCM audio stream: %d", ret);
        pcm2g711a_destroy(g711a_ctx);
        return ret;
    }

    uint32_t start_time = (uint32_t)(esp_timer_get_time() / 1000);
    esp_capture_start(capture_sys.capture_handle);

    int frame_count = 0;
    int g711a_frame_count = 0;
    uint8_t *g711a_buffer = NULL;

    while ((uint32_t)(esp_timer_get_time() / 1000) < start_time + 30000) {
        media_lib_thread_sleep(30);
        esp_capture_stream_frame_t frame = {
            .stream_type = ESP_CAPTURE_STREAM_TYPE_AUDIO,
        };
        
        while (esp_capture_sink_acquire_frame(capture_path, &frame, true) == ESP_CAPTURE_ERR_OK) {
            frame_count++;
            
            // 分配G711A缓冲区
            size_t required_g711a_size = frame.size / 2;
            if (g711a_buffer) free(g711a_buffer);
            g711a_buffer = (uint8_t *)malloc(required_g711a_size);
            if (!g711a_buffer) {
                esp_capture_sink_release_frame(capture_path, &frame);
                continue;
            }
            
            // PCM转G711A（标准算法）
            size_t g711a_len = 0;
            esp_err_t conv_ret = pcm2g711a((const int16_t *)frame.data, frame.size, 
                                         g711a_buffer, &g711a_len);
            
            if (conv_ret == ESP_OK && g711a_len > 0) {
                g711a_frame_count++;
                
                // G711A转回PCM（标准算法）
                int16_t *pcm_back = (int16_t *)malloc(g711a_len * 2);
                if (pcm_back) {
                    for (size_t i = 0; i < g711a_len; i++) {
                        pcm_back[i] = g711a_to_pcm16(g711a_buffer[i]);
                    }
                    
                    // 播放转回的PCM
                    av_render_audio_data_t audio_data = {
                        .data = (void *)pcm_back,
                        .size = g711a_len * 2,
                        .pts = frame.pts,
                    };
                    av_render_add_audio_data(player_sys.player, &audio_data);
                    
                    free(pcm_back);
                }
            }
            
            esp_capture_sink_release_frame(capture_path, &frame);
        }
    }

    ESP_LOGI(TAG, "采集完成，共获取 %d 帧PCM数据，转换为 %d 帧G711A数据", 
             frame_count, g711a_frame_count);

    // 清理资源
    if (g711a_buffer) free(g711a_buffer);
    esp_capture_stop(capture_sys.capture_handle);
    av_render_reset(player_sys.player);
    pcm2g711a_destroy(g711a_ctx);

    return 0;
}*/

static void music_play_thread(void *arg)
{
    // 播放AAC格式音乐
    av_render_audio_info_t render_aud_info = {
        .codec = AV_RENDER_AUDIO_CODEC_AAC,
    };
    av_render_add_audio_stream(player_sys.player, &render_aud_info);
    
    int music_pos = 0;
    while (!music_stopping && music_duration >= 0) {
        uint32_t start_time = esp_timer_get_time() / 1000;
        int send_size = music_size - music_pos;
        const uint8_t *adts_header = music_to_play + music_pos;
        
        // 解析AAC ADTS头获取帧大小
        if (adts_header[0] != 0xFF) {
            send_size = 0;
        } else {
            int frame_size = ((adts_header[3] & 0x03) << 11) | (adts_header[4] << 3) | (adts_header[5] >> 5);
            if (frame_size < send_size) {
                send_size = frame_size;
            }
        }
        
        // 发送音频数据到播放器
        if (send_size) {
            av_render_audio_data_t audio_data = {
                .data = (uint8_t *)adts_header,
                .size = send_size,
            };
            int ret = av_render_add_audio_data(player_sys.player, &audio_data);
            if (ret != 0) {
                break;
            }
            music_pos += send_size;
        }
        
        // 循环播放控制
        if (music_pos >= music_size || send_size == 0) {
            music_pos = 0;
            if (music_duration == 0) {
                // 播放完后等待FIFO排空
                av_render_fifo_stat_t stat = { 0 };
                while (!music_stopping) {
                    av_render_get_audio_fifo_level(player_sys.player, &stat);
                    if (stat.data_size > 0) {
                        media_lib_thread_sleep(50);
                        continue;
                    }
                    break;
                }
                break;
            }
        }
        
        // 更新播放时长
        uint32_t end_time = esp_timer_get_time() / 1000;
        if (music_duration) {
            music_duration -= end_time - start_time;
        }
    }
    
    av_render_reset(player_sys.player);
    music_stopping = false;
    music_playing = false;
    media_lib_thread_destroy(NULL);
}

int play_music(const uint8_t *data, int size, int duration)
{
    if (music_playing) {
        ESP_LOGE(TAG, "Music is playing, stop automatically");
        stop_music();
    }
    
    music_playing = true;
    music_to_play = data;
    music_size = size;
    music_duration = duration;
    
    media_lib_thread_handle_t thread;
    int ret = media_lib_thread_create_from_scheduler(&thread, "music_player", music_play_thread, NULL);
    if (ret != 0) {
        music_playing = false;
        ESP_LOGE(TAG, "Fail to create music_player thread");
        return ret;
    }
    
    return 0;
}

int stop_music()
{
    if (music_playing) {
        music_stopping = true;
        while (music_stopping) {
            media_lib_thread_sleep(20);
        }
    }
    return 0;
}