/*
 * SPDX-FileCopyrightText: 2018-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * ESP8266 RTOS SDK adaptation:
 *   - Added conditional IPv4/IPv6 support via CONFIG_LWIP_IPV6
 *   - Uses IPv4 loopback (127.0.0.1) or IPv6 loopback (::1) depending on config
 */

#include <string.h>
#include <unistd.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "ctrl_sock.h"

#ifndef CONFIG_LWIP_NETIF_LOOPBACK
#define LOOPBACK_ENABLED 0
#else
#define LOOPBACK_ENABLED CONFIG_LWIP_NETIF_LOOPBACK
#endif

/* Control socket, because in some network stacks select can't be woken up any
 * other way
 */
int cs_create_ctrl_sock(int port)
{
#if !LOOPBACK_ENABLED
    ESP_LOGE("esp_http_server", "Please enable LWIP_NETIF_LOOPBACK for %s API", __func__);
    return -1;
#endif

#if CONFIG_LWIP_IPV6
    int fd = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
#else
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
#endif
    if (fd < 0) {
        return -1;
    }

#if CONFIG_LWIP_IPV6
    struct sockaddr_in6 addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin6_family = AF_INET6;
    addr.sin6_port = htons(port);
    inet6_aton("::1", &addr.sin6_addr);
#else
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_aton("127.0.0.1", &addr.sin_addr);
#endif

    int ret = bind(fd, (struct sockaddr *)&addr, sizeof(addr));
    if (ret < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

void cs_free_ctrl_sock(int fd)
{
    close(fd);
}

int cs_send_to_ctrl_sock(int send_fd, int port, void *data, unsigned int data_len)
{
#if CONFIG_LWIP_IPV6
    struct sockaddr_in6 to_addr;
    memset(&to_addr, 0, sizeof(to_addr));
    to_addr.sin6_family = AF_INET6;
    to_addr.sin6_port = htons(port);
    inet6_aton("::1", &to_addr.sin6_addr);
#else
    struct sockaddr_in to_addr;
    memset(&to_addr, 0, sizeof(to_addr));
    to_addr.sin_family = AF_INET;
    to_addr.sin_port = htons(port);
    inet_aton("127.0.0.1", &to_addr.sin_addr);
#endif

    int ret = sendto(send_fd, data, data_len, 0,
                     (struct sockaddr *)&to_addr, sizeof(to_addr));

    if (ret < 0) {
        return -1;
    }
    return ret;
}

int cs_recv_from_ctrl_sock(int fd, void *data, unsigned int data_len)
{
    int ret;
    ret = recvfrom(fd, data, data_len, 0, NULL, NULL);

    if (ret < 0) {
        return -1;
    }
    return ret;
}
