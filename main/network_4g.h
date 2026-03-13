#ifndef NETWORK_4G_H
#define NETWORK_4G_H

#include "at_modem.h"

namespace Network4g {
    void init(void);
    void initMqtt(void);
    AtModem* GetModemInstance(void);
    Mqtt* GetMqttInstance(void);
    std::string resolveDomain(const char* domain);
}

#endif // NETWORK_4G_H
