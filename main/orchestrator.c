// Server orchestrator — serializes ALL server lifecycle operations
// Solves: TOCTOU race in wifi_event_handler, event loop blocking,
//         DNS task restart on error

#include "smart_ap_common.h"

#include <stdbool.h>

#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_system.h"

void server_orchestrator_task(void *pvParameters)
{
    CTX_CHECK();

    ESP_LOGI(TAG, "Server orchestrator task started");

    server_cmd_msg_t msg;
    int dns_error_count = 0;

    while (1)
    {
        if (xQueueReceive(ctx->server_cmd_queue, &msg, pdMS_TO_TICKS(2000)) != pdTRUE)
        {
            if (xEventGroupGetBits(ctx->events) & EVT_SHUTDOWN_BIT)
                goto orch_exit;
            continue;
        }

        switch (msg.cmd)
        {
        case SERVER_CMD_START:
            if (xEventGroupGetBits(ctx->events) & EVT_SERVERS_RUNNING_BIT)
            {
                ESP_LOGD(TAG, "Orchestrator: START ignored (already running)");
                break;
            }
            // Double-check: are there any clients still connected?
            // Handles the race: START queued → client disconnects → STOP queued
            // → STOP processed first (servers not running, ignored) → START
            // processed → would start servers with NO clients connected!
            {
                wifi_sta_list_t sta_list;
                if (esp_wifi_ap_get_sta_list(&sta_list) == ESP_OK && sta_list.num == 0)
                {
                    ESP_LOGI(TAG, "Orchestrator: START skipped — no clients connected");
                    break;
                }
            }
            start_dns_server();
            start_webserver();
            xEventGroupSetBits(ctx->events, EVT_SERVERS_RUNNING_BIT);
            dns_error_count = 0; // Reset backoff — fresh connection, clean slate
            ESP_LOGI(TAG, "Orchestrator: servers STARTED (heap free: %u, min: %u)",
                     (unsigned)esp_get_free_heap_size(),
                     (unsigned)esp_get_minimum_free_heap_size());
            break;

        case SERVER_CMD_STOP:
            if (xEventGroupGetBits(ctx->events) & EVT_SERVERS_RUNNING_BIT)
            {
                // Double-check client count to handle the race
                // where a new client connects between the STADISCONNECTED
                // event and this STOP being processed.
                wifi_sta_list_t sta_list;
                esp_err_t ret = esp_wifi_ap_get_sta_list(&sta_list);
                if (ret != ESP_OK)
                {
                    ESP_LOGE(TAG, "Orchestrator: STOP — sta_list failed (0x%x), skipping to be safe", ret);
                    break;
                }
                if (sta_list.num > 0)
                {
                    ESP_LOGI(TAG, "Orchestrator: STOP skipped — %d client(s) still connected", sta_list.num);
                    break;
                }
                xEventGroupClearBits(ctx->events, EVT_SERVERS_RUNNING_BIT);
                stop_dns_server();
                stop_webserver();
                ESP_LOGI(TAG, "Orchestrator: servers STOPPED (heap free: %u, min: %u)",
                         (unsigned)esp_get_free_heap_size(),
                         (unsigned)esp_get_minimum_free_heap_size());
                xEventGroupSetBits(ctx->events, EVT_SERVERS_STOPPED_BIT);
            }
            else
            {
                ESP_LOGD(TAG, "Orchestrator: STOP ignored (not running)");
                xEventGroupSetBits(ctx->events, EVT_SERVERS_STOPPED_BIT);
            }
            break;

        case SERVER_CMD_DNS_ERROR:
            if (xEventGroupGetBits(ctx->events) & EVT_DNS_RUNNING_BIT)
            {
                dns_error_count++;

                // Exponential backoff: 1s, 2s, 4s, 8s, 16s, 30s, 30s, ...
                // Prevents CPU/heap thrashing in crash-loop scenarios.
                uint32_t delay_sec = 1U << (dns_error_count - 1);
                if (delay_sec > CONFIG_SMART_AP_DNS_BACKOFF_MAX_SEC)
                    delay_sec = CONFIG_SMART_AP_DNS_BACKOFF_MAX_SEC;

                // Jitter: 0..500ms random offset to avoid thundering herd
                // if multiple devices restart DNS simultaneously
                uint32_t jitter_ms = esp_random() % 500;

                ESP_LOGW(TAG, "Orchestrator: DNS error #%d, restarting in %us (+%ums)...",
                         dns_error_count, delay_sec, jitter_ms);

                stop_dns_server();
                vTaskDelay(pdSEC_TO_TICKS(delay_sec) + pdMS_TO_TICKS(jitter_ms));
                start_dns_server();

                ESP_LOGI(TAG, "Orchestrator: DNS RESTARTED after error (attempt #%d)", dns_error_count);
            }
            else
            {
                ESP_LOGD(TAG, "Orchestrator: DNS error ignored (not supposed to be running)");
            }
            break;

        case SERVER_CMD_SHUTDOWN:
            if (xEventGroupGetBits(ctx->events) & EVT_SERVERS_RUNNING_BIT)
            {
                xEventGroupClearBits(ctx->events, EVT_SERVERS_RUNNING_BIT);
                stop_dns_server();
                stop_webserver();
            }
            ESP_LOGI(TAG, "Orchestrator: shutdown complete, exiting");
            xEventGroupSetBits(ctx->events, EVT_SERVERS_STOPPED_BIT);
            xEventGroupSetBits(ctx->events, EVT_ORCHESTRATOR_DONE_BIT);
            vTaskDelete(NULL);
            return;

        default:
            ESP_LOGW(TAG, "Orchestrator: unknown command %d", msg.cmd);
            break;
        }

        if (xEventGroupGetBits(ctx->events) & EVT_SHUTDOWN_BIT)
            goto orch_exit;
    }

orch_exit:
    if (xEventGroupGetBits(ctx->events) & EVT_SERVERS_RUNNING_BIT)
    {
        xEventGroupClearBits(ctx->events, EVT_SERVERS_RUNNING_BIT);
        stop_dns_server();
        stop_webserver();
    }
    xEventGroupSetBits(ctx->events, EVT_SERVERS_STOPPED_BIT);
    ESP_LOGI(TAG, "Orchestrator task exiting gracefully");
    xEventGroupSetBits(ctx->events, EVT_ORCHESTRATOR_DONE_BIT);
    vTaskDelete(NULL);
}
