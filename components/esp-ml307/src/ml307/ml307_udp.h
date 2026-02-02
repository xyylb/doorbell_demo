#ifndef ML307_UDP_H
#define ML307_UDP_H

#include "udp.h"
#include "at_uart.h"

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <list>
#include <string>
#include <vector>

#define ML307_UDP_CONNECTED BIT0
#define ML307_UDP_DISCONNECTED BIT1
#define ML307_UDP_ERROR BIT2
#define ML307_UDP_RECEIVE BIT3
#define ML307_UDP_SEND_COMPLETE BIT4
#define ML307_UDP_INITIALIZED BIT5

#define UDP_CONNECT_TIMEOUT_MS 10000
#define UDP_SEND_TIMEOUT_MS 5000

class Ml307Udp : public Udp {
public:
    Ml307Udp(std::shared_ptr<AtUart> at_uart, int udp_id);
    ~Ml307Udp();

    bool Connect(const std::string& host, int port) override;
    void Disconnect() override;
    int Send(const std::string& data, int timeout_ms = -1) override;
    int GetLastError() override;

    // 获取当前连接的IP和端口
    std::string GetRemoteIp() const { return current_host_; }
    int GetRemotePort() const { return current_port_; }

private:
    std::shared_ptr<AtUart> at_uart_;
    int udp_id_;
    bool instance_active_ = false;
    EventGroupHandle_t event_group_handle_;
    std::list<UrcCallback>::iterator urc_callback_it_;
    int last_error_ = 0;
    
    // 记录当前连接的地址
    std::string current_host_;
    int current_port_ = 0;
    
    // 私有方法
    bool initializeUdp();
    bool configureUdp();
    
    // 添加handleUrc方法的声明
    void handleUrc(const std::string& command, const std::vector<AtArgumentValue>& arguments);
};

#endif // ML307_UDP_H