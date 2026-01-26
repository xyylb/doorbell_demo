#include "at_modem_c.h"
#include "at_modem.h"
#include <cstring>
#include <memory>

// 定义不透明指针的实际类型
struct AtModemC {
    std::unique_ptr<AtModem> impl;          // 原C++对象
    NetworkStateCallbackC callback = nullptr;
    void* user_data = nullptr;
};

// 适配C++回调到C回调
static void network_state_adapter(bool network_ready, void* user_data) {
    AtModemC* modem_c = static_cast<AtModemC*>(user_data);
    if (modem_c->callback) {
        modem_c->callback(network_ready, modem_c->user_data);
    }
}

// 检测创建实例
AtModemC* at_modem_detect(gpio_num_t tx_pin, gpio_num_t rx_pin, gpio_num_t dtr_pin, gpio_num_t ri_pin, int baud_rate, int timeout_ms) {
    std::unique_ptr<AtModem> modem;
    
    // ========== 关键修复2：明确区分重载函数调用 ==========
    if (ri_pin == GPIO_NUM_NC) {
        // 显式调用 3个引脚 + 波特率 + 超时 的版本（指定默认参数）
        modem = AtModem::Detect(tx_pin, rx_pin, dtr_pin, baud_rate, timeout_ms);
    } else {
        // 显式调用 4个引脚 + 波特率 + 超时 的版本
        modem = AtModem::Detect(tx_pin, rx_pin, dtr_pin, ri_pin, baud_rate, timeout_ms);
    }

    if (!modem) {
        return nullptr;
    }

    AtModemC* modem_c = new AtModemC;
    modem_c->impl = std::move(modem);
    // 设置回调适配器
    modem_c->impl->OnNetworkStateChanged([modem_c](bool ready) {
        network_state_adapter(ready, modem_c);
    });

    return modem_c;
}

// 销毁实例
void at_modem_destroy(AtModemC* modem) {
    if (modem) {
        delete modem;
    }
}

// 重启模组
void at_modem_reboot(AtModemC* modem) {
    if (modem && modem->impl) {
        modem->impl->Reboot();
    }
}

// 等待网络就绪
NetworkStatusC at_modem_wait_for_network_ready(AtModemC* modem, int timeout_ms) {
    if (!modem || !modem->impl) {
        return NETWORK_STATUS_ERROR;
    }

    NetworkStatus status = modem->impl->WaitForNetworkReady(timeout_ms);
    // 类型转换
    switch (status) {
        case NetworkStatus::ErrorInsertPin:
            return NETWORK_STATUS_ERROR_INSERT_PIN;
        case NetworkStatus::ErrorRegistrationDenied:
            return NETWORK_STATUS_ERROR_REGISTRATION_DENIED;
        case NetworkStatus::ErrorTimeout:
            return NETWORK_STATUS_ERROR_TIMEOUT;
        case NetworkStatus::Ready:
            return NETWORK_STATUS_READY;
        case NetworkStatus::Error:
        default:
            return NETWORK_STATUS_ERROR;
    }
}

// 设置睡眠模式
bool at_modem_set_sleep_mode(AtModemC* modem, bool enable, int delay_seconds) {
    if (!modem || !modem->impl) {
        return false;
    }
    return modem->impl->SetSleepMode(enable, delay_seconds);
}

// 设置飞行模式
void at_modem_set_flight_mode(AtModemC* modem, bool enable) {
    if (modem && modem->impl) {
        modem->impl->SetFlightMode(enable);
    }
}

// 获取IMEI
bool at_modem_get_imei(AtModemC* modem, char* buffer, size_t buffer_len) {
    if (!modem || !modem->impl || !buffer || buffer_len == 0) {
        return false;
    }
    std::string imei = modem->impl->GetImei();
    strncpy(buffer, imei.c_str(), buffer_len - 1);
    buffer[buffer_len - 1] = '\0';  // 确保字符串终止
    return true;
}

// 获取ICCID
bool at_modem_get_iccid(AtModemC* modem, char* buffer, size_t buffer_len) {
    if (!modem || !modem->impl || !buffer || buffer_len == 0) {
        return false;
    }
    std::string iccid = modem->impl->GetIccid();
    strncpy(buffer, iccid.c_str(), buffer_len - 1);
    buffer[buffer_len - 1] = '\0';
    return true;
}

// 获取模组版本
bool at_modem_get_module_revision(AtModemC* modem, char* buffer, size_t buffer_len) {
    if (!modem || !modem->impl || !buffer || buffer_len == 0) {
        return false;
    }
    std::string revision = modem->impl->GetModuleRevision();
    strncpy(buffer, revision.c_str(), buffer_len - 1);
    buffer[buffer_len - 1] = '\0';
    return true;
}

// 获取运营商名称
bool at_modem_get_carrier_name(AtModemC* modem, char* buffer, size_t buffer_len) {
    if (!modem || !modem->impl || !buffer || buffer_len == 0) {
        return false;
    }
    std::string carrier = modem->impl->GetCarrierName();
    strncpy(buffer, carrier.c_str(), buffer_len - 1);
    buffer[buffer_len - 1] = '\0';
    return true;
}

// 获取CSQ值
int at_modem_get_csq(AtModemC* modem) {
    if (!modem || !modem->impl) {
        return -1;
    }
    return modem->impl->GetCsq();
}

// 获取注册状态
bool at_modem_get_registration_state(AtModemC* modem, CeregStateC* cereg_state) {
    if (!modem || !modem->impl || !cereg_state) {
        return false;
    }

    CeregState state = modem->impl->GetRegistrationState();
    cereg_state->stat = state.stat;
    strncpy(cereg_state->tac, state.tac.c_str(), sizeof(cereg_state->tac) - 1);
    cereg_state->tac[sizeof(cereg_state->tac) - 1] = '\0';
    strncpy(cereg_state->ci, state.ci.c_str(), sizeof(cereg_state->ci) - 1);
    cereg_state->ci[sizeof(cereg_state->ci) - 1] = '\0';
    cereg_state->AcT = state.AcT;

    return true;
}

// 检查PIN是否就绪
bool at_modem_pin_ready(AtModemC* modem) {
    if (!modem || !modem->impl) {
        return false;
    }
    return modem->impl->pin_ready();
}

// 检查网络是否就绪
bool at_modem_network_ready(AtModemC* modem) {
    if (!modem || !modem->impl) {
        return false;
    }
    return modem->impl->network_ready();
}

// 设置网络状态回调
void at_modem_set_network_state_callback(AtModemC* modem, NetworkStateCallbackC callback, void* user_data) {
    if (modem) {
        modem->callback = callback;
        modem->user_data = user_data;
    }
}