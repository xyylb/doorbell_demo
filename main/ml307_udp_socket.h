#pragma once

#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>  // 补充必要的头文件

#ifdef __cplusplus
extern "C" {
#endif

// 如果没有在其他地方定义过，才定义
#ifndef ESP_PEER_ADDR_T_DEFINED
#define ESP_PEER_ADDR_T_DEFINED
typedef struct  {
    uint8_t     family;   /*!< AF_INET and so on */
    uint16_t    port;     /*!< IP port */
    union {
        uint8_t ipv4[4];  /*!< IPV4 address */
        uint8_t ipv6[16]; /*!< IPV6 address */
    };
} esp_peer_addr_t;
#endif // UDP_SOCKET_T_DEFINED


typedef struct {
    int             fd;
    int             ipv6_fd;
    uint32_t        sin6_scope_id;
    esp_peer_addr_t bind_addr;
    long long int   timeout_sec;
    long int        timeout_usec;
    atomic_char     user_count;
} udp_socket_t;

int udp_socket_open(udp_socket_t *udp_socket, bool ipv6_support);
// 绑定到特定的端口上，目前是绑定到0让系统选
int udp_socket_bind(udp_socket_t *udp_socket, esp_peer_addr_t *addr);

void udp_socket_close(udp_socket_t *udp_socket);
// 发送到指定的地址
int udp_socket_sendto(udp_socket_t *udp_socket, esp_peer_addr_t *addr, const uint8_t *buf, int len);
// 接收数据，并获取接收到的地址
int udp_socket_recvfrom_nowait(udp_socket_t *udp_socket, esp_peer_addr_t *addr, uint8_t *buf, int len, bool nowait);

// 获取UDP的本地地址，主要是获取端口号
int udp_get_local_address(udp_socket_t *udp_socket, bool ipv6, esp_peer_addr_t *addr);

// 设置超时时间，读或者写
void udp_blocking_timeout(udp_socket_t *udp_socket, long long int ms);

//获取当前的主机地址
int ports_get_host_addr(esp_peer_addr_t *addr);

//根据主机名转换为IP地址
int ports_resolve_addr(const char *host, esp_peer_addr_t *addr);

#ifdef __cplusplus
}
#endif