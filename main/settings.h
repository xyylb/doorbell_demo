/* General settings

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#pragma once

#include "sdkconfig.h"
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif


/**
 * @brief  自定义BOARD名称（需在codec_board的board_cfg.txt中添加对应配置）
 */
#if CONFIG_IDF_TARGET_ESP32P4
#define TEST_BOARD_NAME "CUSTOM_AUDIO_BOARD"  // 自定义音频板卡
#else
#define TEST_BOARD_NAME "CUSTOM_AUDIO_BOARD"
#endif

/**
 * @brief  禁用视频分辨率配置
 */
#define VIDEO_WIDTH  0
#define VIDEO_HEIGHT 0
#define VIDEO_FPS    0

#ifdef CONFIG_DOORBELL_SUPPORT_PEDESTRIAN_DETECT
// 禁用行人检测（无视频）
#undef CONFIG_DOORBELL_SUPPORT_PEDESTRIAN_DETECT
#endif

/**
 * @brief  Set for wifi ssid
 */
#define WIFI_SSID     "XXXX"

/**
 * @brief  Set for wifi password
 */
#define WIFI_PASSWORD "XXXX"


/**
 * @brief  If defined will use OPUS codec
 */
#define WEBRTC_SUPPORT_OPUS (true)

/**
 * @brief  Whether enable data channel
 */
#define DATA_CHANNEL_ENABLED (false)

#if CONFIG_IDF_TARGET_ESP32P4
/**
 * @brief  GPIO for ring button
 *
 * @note  When use ESP32P4-Fuction-Ev-Board, GPIO35(boot button) is connected RMII_TXD1
 *        When enable `NETWORK_USE_ETHERNET` will cause socket error
 *        User must replace it to a unused GPIO instead (like GPIO27)
 */
#define DOOR_BELL_RING_BUTTON  GPIO_NUM_0
#else
/**
 * @brief  GPIO for ring button
 *
 * @note  When use ESP32S3-KORVO-V3 Use ADC button as ring button
 */
#define DOOR_BELL_RING_BUTTON  GPIO_NUM_0
#endif

#ifdef __cplusplus
}
#endif
