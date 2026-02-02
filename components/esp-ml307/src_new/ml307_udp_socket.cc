#include "ml307_udp_socket.h"
#include "esp_log.h"
#include "network_4g.h"
#include "at_modem.h"

#include <string>
#include <map>
#include <memory>
#include <cstring>

#include <arpa/inet.h>
#include <netdb.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>

static const char *TAG = "udp_socket";

#ifdef __cplusplus
extern "C" {
#endif

int ml307_linked = 0xDEADBEEF;

// 事件组定义
EventGroupHandle_t udp_event = NULL;
#define UDP_EVT_RX  (1 << 0)  // UDP接收事件

/* ======================= 核心结构 ======================= */

struct UdpConnection {
    std::unique_ptr<Udp> udp;

    std::string last_packet;
    volatile bool has_packet = false;

    std::string remote_ip;
    uint16_t remote_port = 0;

    uint32_t last_used = 0;
};

/* ======================= UDP Manager ======================= */

class UdpManager {
public:
    static constexpr int MAX_CONN = 4;

    UdpManager() {
        // 创建事件组
        udp_event = xEventGroupCreate();
    }

    ~UdpManager() {
        if (udp_event) {
            vEventGroupDelete(udp_event);
            udp_event = NULL;
        }
    }

    UdpConnection* get(const std::string& ip, uint16_t port) {
        std::string key = ip + ":" + std::to_string(port);

        auto it = conns.find(key);
        if (it != conns.end()) {
            it->second->last_used = xTaskGetTickCount();
            return it->second.get();
        }

        if (conns.size() >= MAX_CONN) {
            evict_oldest();
        }

        auto conn = std::make_unique<UdpConnection>();
        conn->remote_ip = ip;
        conn->remote_port = port;
        conn->last_used = xTaskGetTickCount();

        AtModem *modem = Network4g::GetModemInstance();
        if (!modem) {
            ESP_LOGE(TAG, "no modem");
            return nullptr;
        }

        int id = next_id++ % MAX_CONN;
        conn->udp = modem->CreateUdp(id);
        if (!conn->udp) {
            ESP_LOGE(TAG, "CreateUdp failed");
            return nullptr;
        }

        /* 接收回调：设置事件标志 */
        conn->udp->OnMessage([c = conn.get()](const std::string& data) {
            if (data.empty()) return;
            c->last_packet = data;   // 覆盖
            c->has_packet = true;
            c->last_used = xTaskGetTickCount();
            
            // 设置接收事件
            if (udp_event) {
                xEventGroupSetBits(udp_event, UDP_EVT_RX);
            }
        });

        if (!conn->udp->Connect(ip, port)) {
            ESP_LOGE(TAG, "connect %s:%u failed", ip.c_str(), port);
            return nullptr;
        }

        auto *ret = conn.get();
        conns[key] = std::move(conn);

        ESP_LOGI(TAG, "UDP connect %s:%u", ip.c_str(), port);
        return ret;
    }

    int send(const std::string& ip, uint16_t port, const uint8_t *buf, int len, udp_socket_t *sock) {
        auto *c = get(ip, port);
        if (!c || !c->udp) return -1;

        std::string data((const char*)buf, len);
        int r = c->udp->Send(data, sock->timeout_sec);
        return r;
    }

    int recv_any(esp_peer_addr_t *addr, uint8_t *buf, int len) {
        for (auto &kv : conns) {
            auto *c = kv.second.get();
            if (c->has_packet) {
                int n = std::min(len, (int)c->last_packet.size());
                memcpy(buf, c->last_packet.data(), n);
                c->has_packet = false;

                if (addr) {
                    addr->family = AF_INET;
                    addr->port = c->remote_port;
                    in_addr_t ip = inet_addr(c->remote_ip.c_str());
                    memcpy(addr->ipv4, &ip, 4);
                }
                return n;
            }
        }
        return 0;   // ⭐ 非阻塞：没数据
    }

    void close_all() {
        for (auto &kv : conns) {
            if (kv.second->udp) {
                kv.second->udp->Disconnect();
            }
        }
        conns.clear();
        
        // 清除所有事件标志
        if (udp_event) {
            xEventGroupClearBits(udp_event, UDP_EVT_RX);
        }
    }

private:
    void evict_oldest() {
        uint32_t oldest = UINT32_MAX;
        std::string key;

        for (auto &kv : conns) {
            if (kv.second->last_used < oldest) {
                oldest = kv.second->last_used;
                key = kv.first;
            }
        }
        if (!key.empty()) {
            conns.erase(key);
        }
    }

    std::map<std::string, std::unique_ptr<UdpConnection>> conns;
    int next_id = 0;
};

static UdpManager g_udp;

#define UDP_SEND_DEADLINE_US  300000   // 3ms，够 4G 正常情况

/* ======================= socket API ======================= */

int udp_socket_open(udp_socket_t *sock, bool) {
    ESP_LOGW(TAG, "使用4G通信，调用了udp_socket_open打开了");
    sock->fd = 1;
    sock->timeout_sec = 5;
    // 注意：这里不调用 close_all()，因为 g_udp 构造函数已经初始化
    return 0;
}

int udp_socket_sendto(udp_socket_t *sock, esp_peer_addr_t *addr,
                      const uint8_t *buf, int len) {
    if (!buf || len <= 0 || !addr) {
        return -1;
    }
    char ip[16];
    snprintf(ip, sizeof(ip), "%u.%u.%u.%u",
             addr->ipv4[0], addr->ipv4[1],
             addr->ipv4[2], addr->ipv4[3]);
    
    int sent = g_udp.send(ip, addr->port, buf, len, sock);

    if (sent <= 0) {
        return -1;
    }

    return sent;
}

int udp_socket_recvfrom_nowait(udp_socket_t *sock,
                               esp_peer_addr_t *addr,
                               uint8_t *buf,
                               int len,
                               bool nowait)
{
    // 非阻塞模式：直接检查并返回
    if (nowait) {
        return g_udp.recv_any(addr, buf, len);
    }

    // 阻塞模式：获取超时时间
    int timeout_ms = (sock && sock->timeout_sec > 0)
                       ? sock->timeout_sec : 0;

    // 等待接收事件
    EventBits_t bits = xEventGroupWaitBits(
        udp_event,
        UDP_EVT_RX,
        pdTRUE,        // 读一次就清除事件标志
        pdFALSE,       // 不等待所有位
        pdMS_TO_TICKS(timeout_ms)
    );

    if (!(bits & UDP_EVT_RX)) {
        return 0;      // 超时或没有数据
    }

    // 有事件发生，读取数据
    int r = g_udp.recv_any(addr, buf, len);
    return (r > 0) ? r : 0;  // 返回实际读取的数据长度
}

void udp_socket_close(udp_socket_t *) {
    g_udp.close_all();
}

int udp_socket_bind(udp_socket_t *, esp_peer_addr_t *) {
    return 0;
}

void udp_blocking_timeout(udp_socket_t *sock, long long int ms)
{
    if (!sock) return;

    if (ms < 0) {
        sock->timeout_sec = 0;
    } else {
        sock->timeout_sec = ms;   // ⭐ 直接存 ms
    }

    //ESP_LOGI(TAG, "udp blocking timeout = %lld ms", ms);
}

int udp_get_local_address(udp_socket_t *, bool, esp_peer_addr_t *addr) {
    memset(addr, 0, sizeof(*addr));
    addr->family = AF_INET;

    AtModem *m = Network4g::GetModemInstance();
    if (m) {
        std::string ip = m->GetLocalIp();
        if (!ip.empty()) {
            in_addr_t a = inet_addr(ip.c_str());
            memcpy(addr->ipv4, &a, 4);
        }
    }
    return 0;
}

int ports_resolve_addr(const char *host, esp_peer_addr_t *addr) {
    struct hostent *he = gethostbyname(host);
    if (!he) return -1;
    addr->family = AF_INET;
    addr->port = 0;
    memcpy(addr->ipv4, he->h_addr_list[0], 4);
    return 0;
}

int ports_get_host_addr(esp_peer_addr_t *addr) {
    return udp_get_local_address(nullptr, false, addr);
}

#ifdef __cplusplus
}
#endif