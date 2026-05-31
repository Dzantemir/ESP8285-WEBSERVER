// DNS server module — DNS task, start/stop functions
// Called ONLY from orchestrator — no race conditions between tasks.
// Uses EventGroup (dns_running, dns_stopped), Mutex (dns_socket, dns_task)

#include "smart_ap_common.h"

#include <string.h>
#include <stdbool.h>
#include <errno.h>

#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"

#include "esp_system.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "lwip/sockets.h"
#include "lwip/ip_addr.h"

// ============================================================================
// START DNS SERVER
// ============================================================================

void start_dns_server(void)
{
    CTX_CHECK();

    xSemaphoreTake(ctx->state_mtx, portMAX_DELAY);
    if (ctx->dns_task == NULL)
    {
        xEventGroupClearBits(ctx->events, EVT_DNS_STOPPED_BIT);
        xEventGroupSetBits(ctx->events, EVT_DNS_RUNNING_BIT);

        if (xTaskCreate(dns_server_task, "dns_task", CONFIG_SMART_AP_DNS_TASK_STACK_SIZE, NULL, CONFIG_SMART_AP_DNS_TASK_PRIORITY, &ctx->dns_task) != pdPASS)
        {
            ESP_LOGE(TAG, "FATAL: Failed to create DNS task! Restarting...");
            xEventGroupClearBits(ctx->events, EVT_DNS_RUNNING_BIT);
            xSemaphoreGive(ctx->state_mtx);
            esp_restart();
        }
    }
    xSemaphoreGive(ctx->state_mtx);
}

// ============================================================================
// STOP DNS SERVER
// ============================================================================

void stop_dns_server(void)
{
    CTX_CHECK();

    // Идемпотентность: если DNS задача уже не работает — ничего не делаем.
    xSemaphoreTake(ctx->state_mtx, portMAX_DELAY);
    if (ctx->dns_task == NULL)
    {
        xSemaphoreGive(ctx->state_mtx);
        ESP_LOGD(TAG, "stop_dns_server: already stopped (idempotent)");
        return;
    }
    xSemaphoreGive(ctx->state_mtx);

    xEventGroupClearBits(ctx->events, EVT_DNS_RUNNING_BIT);

    xSemaphoreTake(ctx->state_mtx, portMAX_DELAY);
    int sock = ctx->dns_socket;
    ctx->dns_socket = -1;
    xSemaphoreGive(ctx->state_mtx);

    if (sock != -1)
    {
        shutdown(sock, SHUT_RDWR);
    }
    else
    {
        ESP_LOGD(TAG, "stop_dns_server: socket already closed (idempotent)");
    }

    EventBits_t dns_bits = xEventGroupWaitBits(
        ctx->events,
        EVT_DNS_STOPPED_BIT,
        pdTRUE,
        pdTRUE,
        pdSEC_TO_TICKS(CONFIG_SMART_AP_DNS_STOP_TIMEOUT_SEC));

    if (!(dns_bits & EVT_DNS_STOPPED_BIT))
    {
        ESP_LOGE(TAG, "FATAL: DNS task hung (%d sec timeout)! Network stack unresponsive, forcing restart.", CONFIG_SMART_AP_DNS_STOP_TIMEOUT_SEC);
        esp_restart();
    }
    else
    {
        xSemaphoreTake(ctx->state_mtx, portMAX_DELAY);
        ctx->dns_task = NULL;
        xSemaphoreGive(ctx->state_mtx);
    }
}

// ============================================================================
// DNS SERVER TASK
// ============================================================================

void dns_server_task(void *pvParameters)
{
    bool exited_by_error = false;
    uint8_t rx_buffer[CONFIG_SMART_AP_DNS_RX_BUFFER_SIZE];
    int sock = -1;

    int dns_pkt_count = 0;
    int64_t dns_window_start = 0;

    struct sockaddr_in server_addr = {0}, client_addr = {0};

    if (!(xEventGroupGetBits(ctx->events) & EVT_DNS_RUNNING_BIT))
        goto dns_cleanup;

    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0)
    {
        ESP_LOGE(TAG, "Failed to create DNS socket: errno %d", errno);
        exited_by_error = true;
        goto dns_cleanup;
    }

    xSemaphoreTake(ctx->state_mtx, portMAX_DELAY);
    ctx->dns_socket = sock;
    xSemaphoreGive(ctx->state_mtx);

    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(CONFIG_SMART_AP_DNS_PORT);

    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct timeval tv = {.tv_sec = CONFIG_SMART_AP_DNS_RECV_TIMEOUT_SEC, .tv_usec = 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if (bind(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
    {
        ESP_LOGE(TAG, "Failed to bind DNS socket: errno %d", errno);
        exited_by_error = true;
        goto dns_cleanup;
    }

    ESP_LOGI(TAG, "DNS Server listening on port 53");

    while (xEventGroupGetBits(ctx->events) & EVT_DNS_RUNNING_BIT)
    {
        socklen_t client_addr_len = sizeof(client_addr);

        ssize_t len = recvfrom(sock, rx_buffer, sizeof(rx_buffer), 0, (struct sockaddr *)&client_addr, &client_addr_len);

        if (len < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                continue;

            if (!(xEventGroupGetBits(ctx->events) & EVT_DNS_RUNNING_BIT))
                break;

            if (errno == ENOMEM || errno == ENOBUFS || errno == ECONNREFUSED ||
                errno == EHOSTUNREACH || errno == ENETUNREACH)
            {
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }

            ESP_LOGE(TAG, "recvfrom fatal error: errno %d", errno);
            exited_by_error = true;
            break;
        }

        if (len == 0)
            continue;

        if (len < 12)
            continue;
        if (rx_buffer[2] & 0x80)
            continue;

        // Rate limiter — до ЛЮБЫХ отправок ответов (включая error-ответы),
        // чтобы злоумышленник не мог обойти ограничение через нестандартные запросы.
        {
            int64_t now = esp_timer_get_time();
            if (now - dns_window_start > 1000000LL)
            {
                if (dns_pkt_count > CONFIG_SMART_AP_DNS_RATE_LIMIT)
                    ESP_LOGD(TAG, "DNS: rate limited %d pkts in last window", dns_pkt_count);
                dns_pkt_count = 0;
                dns_window_start = now;
            }
            if (++dns_pkt_count > CONFIG_SMART_AP_DNS_RATE_LIMIT)
                continue;
        }

        {
            uint8_t opcode = (rx_buffer[2] >> 3) & 0x0F;
            if (opcode != 0)
            {
                rx_buffer[2] = (rx_buffer[2] & 0x01) | 0x84;
                rx_buffer[3] = 0x04; // RA=0, RCODE=4 (NOTIMP)
                rx_buffer[4] = 0;
                rx_buffer[5] = 0;
                rx_buffer[6] = 0;
                rx_buffer[7] = 0;
                rx_buffer[8] = 0;
                rx_buffer[9] = 0;
                rx_buffer[10] = 0;
                rx_buffer[11] = 0;
                sendto(sock, rx_buffer, 12, 0, (struct sockaddr *)&client_addr, client_addr_len);
                continue;
            }
        }

        {
            uint16_t qdcount = (rx_buffer[4] << 8) | rx_buffer[5];
            if (qdcount != 1)
                continue;
        }

        {
            int i = 12;
            bool compression_pointer = false;
            while (i < len && rx_buffer[i] != 0)
            {
                if ((rx_buffer[i] & 0xC0) == 0xC0)
                {
                    if (i + 1 >= len)
                    {
                        rx_buffer[2] = (rx_buffer[2] & 0x01) | 0x84;
                        rx_buffer[3] = 0x01; // RA=0, RCODE=1 (FORMERR)
                        rx_buffer[4] = 0;
                        rx_buffer[5] = 0;
                        rx_buffer[6] = 0;
                        rx_buffer[7] = 0;
                        rx_buffer[8] = 0;
                        rx_buffer[9] = 0;
                        rx_buffer[10] = 0;
                        rx_buffer[11] = 0;
                        sendto(sock, rx_buffer, 12, 0, (struct sockaddr *)&client_addr, client_addr_len);
                        goto skip_packet;
                    }
                    i += 2;
                    compression_pointer = true;
                    break;
                }
                if (rx_buffer[i] > 63)
                {
                    rx_buffer[2] = (rx_buffer[2] & 0x01) | 0x84;
                    rx_buffer[3] = 0x01; // RA=0, RCODE=1 (FORMERR)
                    rx_buffer[4] = 0;
                    rx_buffer[5] = 0;
                    rx_buffer[6] = 0;
                    rx_buffer[7] = 0;
                    rx_buffer[8] = 0;
                    rx_buffer[9] = 0;
                    rx_buffer[10] = 0;
                    rx_buffer[11] = 0;
                    sendto(sock, rx_buffer, 12, 0, (struct sockaddr *)&client_addr, client_addr_len);
                    goto skip_packet;
                }
                i += rx_buffer[i] + 1;
            }
            if (!compression_pointer && i < len && rx_buffer[i] == 0)
                i++;

            if (i + 4 > len)
            {
                rx_buffer[2] = (rx_buffer[2] & 0x01) | 0x84;
                rx_buffer[3] = 0x01; // RA=0, RCODE=1 (FORMERR)
                rx_buffer[4] = 0;
                rx_buffer[5] = 0;
                rx_buffer[6] = 0;
                rx_buffer[7] = 0;
                rx_buffer[8] = 0;
                rx_buffer[9] = 0;
                rx_buffer[10] = 0;
                rx_buffer[11] = 0;
                sendto(sock, rx_buffer, 12, 0, (struct sockaddr *)&client_addr, client_addr_len);
                continue;
            }
            int pos_question_type = i;
            int pos_question_class = i + 2;
            uint16_t qtype = (rx_buffer[pos_question_type] << 8) | rx_buffer[pos_question_type + 1];
            uint16_t qclass = (rx_buffer[pos_question_class] << 8) | rx_buffer[pos_question_class + 1];

            if (qclass != 0x0001)
            {
                rx_buffer[2] = (rx_buffer[2] & 0x01) | 0x84;
                rx_buffer[3] = 0x05; // RA=0, RCODE=5 (REFUSED)
                rx_buffer[4] = 0;
                rx_buffer[5] = 1;
                rx_buffer[6] = 0;
                rx_buffer[7] = 0;
                rx_buffer[8] = 0;
                rx_buffer[9] = 0;
                rx_buffer[10] = 0;
                rx_buffer[11] = 0;
                sendto(sock, rx_buffer, pos_question_class + 2, 0, (struct sockaddr *)&client_addr, client_addr_len);
                continue;
            }

            {
                uint8_t rd_bit = rx_buffer[2] & 0x01;
                rx_buffer[2] = rd_bit | 0x84;
                rx_buffer[3] = 0x00; // RA=0, RCODE=0 (NOERROR)
            }
            rx_buffer[4] = 0;
            rx_buffer[5] = 1;

            if (qtype == 0x0001)
            {
                // Bug #6 fix: read ap_ip_addr under mutex protection
                uint32_t ip;
                xSemaphoreTake(ctx->state_mtx, portMAX_DELAY);
                ip = ctx->ap_ip_addr;
                xSemaphoreGive(ctx->state_mtx);

                if (ip == 0)
                    continue;

                ESP_LOGD(TAG, "DNS: Answer A record (IPv4)");

                rx_buffer[6] = 0;
                rx_buffer[7] = 1;
                rx_buffer[8] = 0;
                rx_buffer[9] = 0;
                rx_buffer[10] = 0;
                rx_buffer[11] = 0;
                int pos = pos_question_class + 2;
                if (pos + 16 > (int)sizeof(rx_buffer))
                    continue;

                static const uint8_t dns_answer_prefix[12] = {
                    0xc0,
                    0x0c,
                    0x00,
                    0x01,
                    0x00,
                    0x01,
                    0x00,
                    0x00,
                    0x00,
                    0x3c,
                    0x00,
                    0x04,
                };
                memcpy(&rx_buffer[pos], dns_answer_prefix, 12);
                pos += 12;

                memcpy(&rx_buffer[pos], &ip, 4);
                pos += 4;

                ESP_LOGD(TAG, "DNS: sent A record to client");
                if (sendto(sock, rx_buffer, pos, 0, (struct sockaddr *)&client_addr, client_addr_len) < 0)
                {
                    ESP_LOGD(TAG, "sendto failed: errno %d", errno);
                }
            }
            else
            {
                ESP_LOGD(TAG, "DNS: Answer empty (NODATA) for type %04x", qtype);
                rx_buffer[6] = 0;
                rx_buffer[7] = 0;
                rx_buffer[8] = 0;
                rx_buffer[9] = 0;
                rx_buffer[10] = 0;
                rx_buffer[11] = 0;

                int resp_len = pos_question_class + 2;
                if (resp_len > (int)sizeof(rx_buffer) || resp_len > len)
                    continue;

                if (sendto(sock, rx_buffer, resp_len, 0, (struct sockaddr *)&client_addr, client_addr_len) < 0)
                {
                    ESP_LOGD(TAG, "sendto failed: errno %d", errno);
                }
            }
        skip_packet:;
        }
    }

    ESP_LOGI(TAG, "DNS Server shutting down");
dns_cleanup:;
    xSemaphoreTake(ctx->state_mtx, portMAX_DELAY);
    ctx->dns_socket = -1;
    xSemaphoreGive(ctx->state_mtx);

    if (sock != -1)
    {
        close(sock);
    }

    xEventGroupSetBits(ctx->events, EVT_DNS_STOPPED_BIT);

    if (exited_by_error && (xEventGroupGetBits(ctx->events) & EVT_DNS_RUNNING_BIT))
    {
        ESP_LOGW(TAG, "DNS task exited with error, notifying orchestrator");
        server_cmd_msg_t msg = {.cmd = SERVER_CMD_DNS_ERROR};
        if (xQueueSend(ctx->server_cmd_queue, &msg, pdMS_TO_TICKS(1000)) != pdTRUE)
            ESP_LOGE(TAG, "Failed to send DNS_ERROR to orchestrator!");
    }

    vTaskDelete(NULL);
}
