#ifndef NETWORK_4G_H
#define NETWORK_4G_H

#include "at_modem.h"

namespace Network4g {
    void init(void);
    void test(void);
    AtModem* GetModemInstance(void);
    Mqtt* GetMqttInstance(void);
}

#endif // NETWORK_4G_H