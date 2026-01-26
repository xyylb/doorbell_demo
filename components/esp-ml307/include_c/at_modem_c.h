#ifndef _AT_MODEM_C_H_
#define _AT_MODEM_C_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// 不透明指针，隐藏C++对象实现
typedef struct AtModemC AtModemC;

// 网络状态枚举（C语言版本）
typedef enum {
    NETWORK_STATUS_ERROR_INSERT_PIN = -1,
    NETWORK_STATUS_ERROR_REGISTRATION_DENIED = -2,
    NETWORK_STATUS_ERROR_TIMEOUT = -3,
    NETWORK_STATUS_READY = 0,
    NETWORK_STATUS_ERROR = 1,
} NetworkStatusC;

// CEREG状态结构体（C语言版本）
typedef struct {
    int stat;
    char tac[16];      // 固定长度数组，避免C++ string依赖
    char ci[16];
    int AcT;
} CeregStateC;

// ========== 关键修复1：删除重复的 GPIO_NUM_NC 定义 ==========
// 直接包含ESP-IDF的GPIO头文件，使用官方定义
#include "driver/gpio.h"

// 回调函数类型定义（网络状态变化）
typedef void (*NetworkStateCallbackC)(bool network_ready, void* user_data);

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 检测并创建AT Modem实例
 * @param tx_pin UART TX引脚
 * @param rx_pin UART RX引脚
 * @param dtr_pin DTR引脚（GPIO_NUM_NC表示未使用）
 * @param ri_pin RI引脚（GPIO_NUM_NC表示未使用）
 * @param baud_rate 波特率
 * @param timeout_ms 超时时间（-1表示无限超时）
 * @return 成功返回实例指针，失败返回NULL
 */
AtModemC* at_modem_detect(gpio_num_t tx_pin, gpio_num_t rx_pin, gpio_num_t dtr_pin, gpio_num_t ri_pin, int baud_rate, int timeout_ms);

/**
 * @brief 销毁AT Modem实例
 * @param modem 实例指针
 */
void at_modem_destroy(AtModemC* modem);

/**
 * @brief 重启模组
 * @param modem 实例指针
 */
void at_modem_reboot(AtModemC* modem);

/**
 * @brief 等待网络就绪
 * @param modem 实例指针
 * @param timeout_ms 超时时间（-1表示无限超时）
 * @return 网络状态
 */
NetworkStatusC at_modem_wait_for_network_ready(AtModemC* modem, int timeout_ms);

/**
 * @brief 设置睡眠模式
 * @param modem 实例指针
 * @param enable 是否启用
 * @param delay_seconds 延迟秒数
 * @return 成功返回true，失败返回false
 */
bool at_modem_set_sleep_mode(AtModemC* modem, bool enable, int delay_seconds);

/**
 * @brief 设置飞行模式
 * @param modem 实例指针
 * @param enable 是否启用
 */
void at_modem_set_flight_mode(AtModemC* modem, bool enable);

/**
 * @brief 获取IMEI
 * @param modem 实例指针
 * @param buffer 输出缓冲区
 * @param buffer_len 缓冲区长度
 * @return 成功返回true，失败返回false
 */
bool at_modem_get_imei(AtModemC* modem, char* buffer, size_t buffer_len);

/**
 * @brief 获取ICCID
 * @param modem 实例指针
 * @param buffer 输出缓冲区
 * @param buffer_len 缓冲区长度
 * @return 成功返回true，失败返回false
 */
bool at_modem_get_iccid(AtModemC* modem, char* buffer, size_t buffer_len);

/**
 * @brief 获取模组版本
 * @param modem 实例指针
 * @param buffer 输出缓冲区
 * @param buffer_len 缓冲区长度
 * @return 成功返回true，失败返回false
 */
bool at_modem_get_module_revision(AtModemC* modem, char* buffer, size_t buffer_len);

/**
 * @brief 获取运营商名称
 * @param modem 实例指针
 * @param buffer 输出缓冲区
 * @param buffer_len 缓冲区长度
 * @return 成功返回true，失败返回false
 */
bool at_modem_get_carrier_name(AtModemC* modem, char* buffer, size_t buffer_len);

/**
 * @brief 获取CSQ值（信号质量）
 * @param modem 实例指针
 * @return CSQ值（-1表示获取失败）
 */
int at_modem_get_csq(AtModemC* modem);

/**
 * @brief 获取注册状态
 * @param modem 实例指针
 * @param cereg_state 输出CEREG状态
 * @return 成功返回true，失败返回false
 */
bool at_modem_get_registration_state(AtModemC* modem, CeregStateC* cereg_state);

/**
 * @brief 检查PIN是否就绪
 * @param modem 实例指针
 * @return true表示就绪，false表示未就绪
 */
bool at_modem_pin_ready(AtModemC* modem);

/**
 * @brief 检查网络是否就绪
 * @param modem 实例指针
 * @return true表示就绪，false表示未就绪
 */
bool at_modem_network_ready(AtModemC* modem);

/**
 * @brief 设置网络状态变化回调
 * @param modem 实例指针
 * @param callback 回调函数
 * @param user_data 用户自定义数据
 */
void at_modem_set_network_state_callback(AtModemC* modem, NetworkStateCallbackC callback, void* user_data);

#ifdef __cplusplus
}
#endif

#endif // _AT_MODEM_C_H_