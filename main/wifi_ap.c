// WiFi AP module — event handler, channel scanning, smart AP task

#include "smart_ap_common.h"

#include <string.h>
#include <stdbool.h>

#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"

#include "esp_system.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "tcpip_adapter.h"
#include "lwip/ip_addr.h"

// ============================================================================
// WIFI EVENT HANDLER — non-blocking, sends commands to orchestrator
// ============================================================================

void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base != WIFI_EVENT || ctx == NULL)
        return;

    if (xEventGroupGetBits(ctx->events) & EVT_SHUTDOWN_BIT)
        return;

    if (event_id == WIFI_EVENT_AP_START)
    {
       
        //  ESP_ERROR_CHECK(esp_wifi_set_inactive_time(ESP_IF_WIFI_AP, 300));
        // ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
        ESP_ERROR_CHECK(esp_wifi_set_protocol(ESP_IF_WIFI_AP, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N));

        ESP_LOGI(TAG, "AP started");

        // Configure static AP IP and DHCP server
        tcpip_adapter_dhcps_stop(ESP_IF_WIFI_AP);

        tcpip_adapter_ip_info_t ip_info = {0};
        ip_info.ip.addr = ipaddr_addr(CONFIG_SMART_AP_IP_ADDR);
        ip_info.gw.addr = ipaddr_addr(CONFIG_SMART_AP_IP_ADDR);
        ip_info.netmask.addr = ipaddr_addr(CONFIG_SMART_AP_NETMASK);

        esp_err_t ret = tcpip_adapter_set_ip_info(ESP_IF_WIFI_AP, &ip_info);
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to set AP IP: %s", esp_err_to_name(ret));

            // Fallback: cache default ESP8266 AP IP so DNS still works
            xSemaphoreTake(ctx->state_mtx, portMAX_DELAY);
            ctx->ap_ip_addr = ipaddr_addr("192.168.4.1");
            xSemaphoreGive(ctx->state_mtx);
        }
        else
        {
            dhcps_lease_t lease = {0};
            lease.start_ip.addr = ipaddr_addr(CONFIG_SMART_AP_DHCP_LEASE_START);
            lease.end_ip.addr = ipaddr_addr(CONFIG_SMART_AP_DHCP_LEASE_END);
            ESP_ERROR_CHECK(tcpip_adapter_dhcps_option(TCPIP_ADAPTER_OP_SET, TCPIP_ADAPTER_REQUESTED_IP_ADDRESS, &lease, sizeof(lease)));

            ESP_LOGI(TAG, "AP IP: %s, DHCP: %s - %s", CONFIG_SMART_AP_IP_ADDR, CONFIG_SMART_AP_DHCP_LEASE_START, CONFIG_SMART_AP_DHCP_LEASE_END);

            // Cache configured AP IP
            xSemaphoreTake(ctx->state_mtx, portMAX_DELAY);
            ctx->ap_ip_addr = ipaddr_addr(CONFIG_SMART_AP_IP_ADDR);
            xSemaphoreGive(ctx->state_mtx);
        }

        ESP_ERROR_CHECK(tcpip_adapter_dhcps_start(ESP_IF_WIFI_AP));
    }
    else if (event_id == WIFI_EVENT_STA_START)
    {
        // Протокол STA (802.11 b/g/n). STA используется только для сканирования.
        //  ESP_ERROR_CHECK(esp_wifi_set_inactive_time(ESP_IF_WIFI_STA, 6));
        // ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
        ESP_ERROR_CHECK(esp_wifi_set_protocol(ESP_IF_WIFI_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N));

        ESP_LOGD(TAG, "STA started");
    }
    else if (event_id == WIFI_EVENT_SCAN_DONE)
    {
        xEventGroupSetBits(ctx->events, EVT_SCAN_DONE_BIT);
    }
    else if (event_id == WIFI_EVENT_AP_STACONNECTED)
    {
        wifi_sta_list_t sta_list;
        int num = -1;
        if (esp_wifi_ap_get_sta_list(&sta_list) == ESP_OK)
            num = sta_list.num;

        if (xEventGroupGetBits(ctx->events) & EVT_SERVERS_RUNNING_BIT)
        {
            ESP_LOGI(TAG, "STA_CONNECTED: clients=%d, servers already running", num);
        }
        else
        {
            ESP_LOGI(TAG, "STA_CONNECTED: first client (clients=%d), requesting START", num);
            server_cmd_msg_t msg = {.cmd = SERVER_CMD_START};
            if (!(xEventGroupGetBits(ctx->events) & EVT_SHUTDOWN_BIT))
            {
                if (xQueueSend(ctx->server_cmd_queue, &msg, pdMS_TO_TICKS(100)) != pdTRUE)
                    ESP_LOGW(TAG, "Failed to send SERVER_CMD_START (queue full)");
            }
        }
    }
    else if (event_id == WIFI_EVENT_AP_STADISCONNECTED)
    {
        wifi_sta_list_t sta_list;
        esp_err_t ret = esp_wifi_ap_get_sta_list(&sta_list);
        if (ret == ESP_OK && sta_list.num == 0)
        {
            ESP_LOGI(TAG, "STA_DISCONNECTED: last client left, requesting STOP (heap: %u)",
                     (unsigned)esp_get_free_heap_size());
            server_cmd_msg_t msg = {.cmd = SERVER_CMD_STOP};
            if (!(xEventGroupGetBits(ctx->events) & EVT_SHUTDOWN_BIT))
            {
                if (xQueueSend(ctx->server_cmd_queue, &msg, pdMS_TO_TICKS(100)) != pdTRUE)
                    ESP_LOGW(TAG, "Failed to send SERVER_CMD_STOP (queue full)");
            }
        }
        else if (ret == ESP_OK)
        {
            ESP_LOGI(TAG, "STA_DISCONNECTED: %d client(s) remain, servers stay running",
                     sta_list.num);
        }
        else
        {
            ESP_LOGW(TAG, "STA_DISCONNECTED: sta_list failed (0x%x), servers stay running", ret);
        }
    }
}

// ============================================================================
// FIND BEST CHANNEL
// ============================================================================

uint8_t find_best_channel(void)
{
    CTX_CHECK_RET(1);

    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = CONFIG_SMART_AP_SCAN_SHOW_HIDDEN,
    };

    ESP_LOGI(TAG, "Starting Wi-Fi scan to find the best channel...");

    uint8_t best_channel = (uint8_t)((esp_random() % CONFIG_SMART_AP_MAX_CHANNELS) + 1);

    xEventGroupClearBits(ctx->events, EVT_SCAN_DONE_BIT);
    esp_err_t scan_ret = esp_wifi_scan_start(&scan_config, false);
    if (scan_ret != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_wifi_scan_start failed: %s", esp_err_to_name(scan_ret));
        goto no_mem_give_random;
    }

    EventBits_t bits = xEventGroupWaitBits(
        ctx->events,
        EVT_SCAN_DONE_BIT | EVT_SHUTDOWN_BIT,
        pdFALSE,
        pdFALSE,
        pdSEC_TO_TICKS(CONFIG_SMART_AP_WIFI_SCAN_TIMEOUT_SEC));

    if (bits & EVT_SHUTDOWN_BIT)
    {
        esp_wifi_scan_stop();
        ESP_LOGW(TAG, "Shutdown during Wi-Fi scan, aborting");
        goto no_mem_give_random;
    }

    if (!(bits & EVT_SCAN_DONE_BIT))
    {
        esp_wifi_scan_stop();
        ESP_LOGW(TAG, "Wi-Fi scan timeout (%d sec)", CONFIG_SMART_AP_WIFI_SCAN_TIMEOUT_SEC);
        goto no_mem_give_random;
    }

    uint16_t ap_count = 0;
    scan_ret = esp_wifi_scan_get_ap_num(&ap_count);
    if (scan_ret != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_wifi_scan_get_ap_num failed: %s", esp_err_to_name(scan_ret));
        goto no_mem_give_random;
    }

    if (ap_count == 0)
    {
        ESP_LOGI(TAG, "No other APs found, using random channel %d", best_channel);
        goto no_mem_give_random;
    }

    wifi_ap_record_t *ap_records = malloc(sizeof(wifi_ap_record_t) * ap_count);

    if (ap_records == NULL)
    {
        ESP_LOGE(TAG, "Failed to allocate memory for AP records");
        goto no_mem_give_random;
    }

    scan_ret = esp_wifi_scan_get_ap_records(&ap_count, ap_records);
    if (scan_ret != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_wifi_scan_get_ap_records failed: %s", esp_err_to_name(scan_ret));
        free(ap_records);
        goto no_mem_give_random;
    }

    uint32_t channel_score[CONFIG_SMART_AP_MAX_CHANNELS + 1] = {0};

    for (int i = 0; i < ap_count; i++)
    {
        uint8_t ch = ap_records[i].primary;
        if (ch >= 1 && ch <= CONFIG_SMART_AP_MAX_CHANNELS)
        {
            int penalty = 100 + ap_records[i].rssi;
            if (penalty < 1)
                penalty = 1;

            channel_score[ch] += penalty;
            if (ch > 1)
                channel_score[ch - 1] += (penalty / 2);
            if (ch < CONFIG_SMART_AP_MAX_CHANNELS)
                channel_score[ch + 1] += (penalty / 2);
            if (ch > 2)
                channel_score[ch - 2] += (penalty / 4);
            if (ch < CONFIG_SMART_AP_MAX_CHANNELS - 1)
                channel_score[ch + 2] += (penalty / 4);
        }
    }

    free(ap_records);

    uint32_t min_score = 0xFFFFFFFF;

    for (int i = 1; i <= CONFIG_SMART_AP_MAX_CHANNELS; i++)
    {
        ESP_LOGD(TAG, "Channel %d penalty score: %u", i, channel_score[i]);
        if (channel_score[i] < min_score)
        {
            min_score = channel_score[i];
            best_channel = i;
        }
    }

no_mem_give_random:

    xEventGroupClearBits(ctx->events, EVT_SCAN_DONE_BIT);

    ESP_LOGI(TAG, "Selected BEST channel: %d", best_channel);

    return best_channel;
}

// ============================================================================
// SMART AP TASK — main AP management loop
// Channel switching: deauth clients, change channel, clients reconnect.
// Servers may briefly stop/restart — orchestrator handles it safely.
// ============================================================================

void smart_ap_task(void *pvParameters)
{
    CTX_CHECK();

    while (1)
    {
        // Bug #15 fix: explicit handling when MIN == MAX (modulo 1 = always 0)
        uint32_t total_delay_sec;
        if (SCAN_INTERVAL_MIN_SEC >= SCAN_INTERVAL_MAX_SEC)
        {
            // MIN >= MAX: use the exact value, no randomization range
            total_delay_sec = SCAN_INTERVAL_MIN_SEC;
        }
        else
        {
            total_delay_sec = SCAN_INTERVAL_MIN_SEC + (esp_random() % (SCAN_INTERVAL_MAX_SEC - SCAN_INTERVAL_MIN_SEC + 1));
        }
        ESP_LOGI(TAG, "Next channel scan in %u seconds", total_delay_sec);

        EventBits_t bits = xEventGroupWaitBits(
            ctx->events,
            EVT_SHUTDOWN_BIT,
            pdFALSE,
            pdFALSE,
            pdSEC_TO_TICKS(total_delay_sec));

        if (bits & EVT_SHUTDOWN_BIT)
            goto ap_shutdown;

        uint8_t best_channel = find_best_channel();

        if (xEventGroupGetBits(ctx->events) & EVT_SHUTDOWN_BIT)
            break;

        wifi_config_t ap_config;
        esp_err_t cfg_ret = esp_wifi_get_config(ESP_IF_WIFI_AP, &ap_config);
        if (cfg_ret != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to get AP config: %s — skipping channel check", esp_err_to_name(cfg_ret));
            continue;
        }

        if (ap_config.ap.channel != best_channel)
        {
            uint8_t old_channel = ap_config.ap.channel;

            // Kick current clients — they will reconnect on the new channel
            esp_err_t deauth_ret = esp_wifi_deauth_sta(0);
            if (deauth_ret != ESP_OK)
                ESP_LOGW(TAG, "deauth_sta: %s", esp_err_to_name(deauth_ret));

            ap_config.ap.channel = best_channel;
            esp_err_t ret = esp_wifi_set_config(ESP_IF_WIFI_AP, &ap_config);
            if (ret == ESP_OK)
            {
                ESP_LOGI(TAG, "Channel changed %d -> %d", old_channel, best_channel);
            }
            else
            {
                ESP_LOGE(TAG, "Channel change failed: %s", esp_err_to_name(ret));
            }
        }
        else
        {
            ESP_LOGI(TAG, "Channel %d unchanged, no switch needed", best_channel);
        }

        ESP_LOGI(TAG, "Smart AP on channel %d (heap: %u)", best_channel, (unsigned)esp_get_free_heap_size());
    }

ap_shutdown:
    xEventGroupSetBits(ctx->events, EVT_AP_DONE_BIT);
    ESP_LOGI(TAG, "AP task exiting gracefully");

    vTaskDelete(NULL);
}
