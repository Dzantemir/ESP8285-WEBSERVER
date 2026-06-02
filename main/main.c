// Main entry point — context creation and task launching
// All logic is in separate modules: buzzer.c, dns_server.c, web_server.c,
// orchestrator.c, power_mgmt.c, wifi_ap.c
// Shared definitions: smart_ap_config.h, smart_ap_types.h, smart_ap_api.h

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
#include "nvs_flash.h"
#include "esp_netif.h"

#ifdef CONFIG_SMART_AP_BATTERY_CONTROL_ENABLED
#include "driver/adc.h"
#include "esp_sleep.h"
#endif

// Global context — defined here, declared extern in smart_ap_types.h
sys_ctx_t *ctx = NULL;

// ============================================================================
// CONTEXT CREATION — allocates all FreeRTOS primitives
// ============================================================================

static sys_ctx_t *sys_ctx_create(void)
{
    sys_ctx_t *c = calloc(1, sizeof(sys_ctx_t));
    if (c == NULL)
        return NULL;

    c->events = xEventGroupCreate();
    if (c->events == NULL)
        goto fail_free_ctx;

    c->state_mtx = xSemaphoreCreateMutex();
    if (c->state_mtx == NULL)
        goto fail_delete_events;

    c->shutdown_sem = xSemaphoreCreateBinary();
    if (c->shutdown_sem == NULL)
        goto fail_delete_state_mtx;
    xSemaphoreGive(c->shutdown_sem);

    c->beep_sem = xSemaphoreCreateBinary();
    if (c->beep_sem == NULL)
        goto fail_delete_shutdown_sem;
    xSemaphoreGive(c->beep_sem);

    c->beep_queue = xQueueCreate(1, sizeof(beep_cmd_t));
    if (c->beep_queue == NULL)
        goto fail_delete_beep_sem;

    c->server_cmd_queue = xQueueCreate(CONFIG_SMART_AP_SERVER_CMD_QUEUE_SIZE, sizeof(server_cmd_msg_t));
    if (c->server_cmd_queue == NULL)
        goto fail_delete_beep_queue;

    c->web_server = NULL;
    c->dns_socket = -1;
    c->dns_task = NULL;

    return c;

fail_delete_beep_queue:
    vQueueDelete(c->beep_queue);
fail_delete_beep_sem:
    vSemaphoreDelete(c->beep_sem);
fail_delete_shutdown_sem:
    vSemaphoreDelete(c->shutdown_sem);
fail_delete_state_mtx:
    vSemaphoreDelete(c->state_mtx);
fail_delete_events:
    vEventGroupDelete(c->events);
fail_free_ctx:
    free(c);
    return NULL;
}

// ============================================================================
// APP_MAIN
// ============================================================================

void app_main(void)
{
#ifdef CONFIG_SMART_AP_BATTERY_CONTROL_ENABLED
    adc_config_t adc_cfg = {.mode = ADC_READ_TOUT_MODE, .clk_div = 8};
    ESP_ERROR_CHECK(adc_init(&adc_cfg));
#endif

    ctx = sys_ctx_create();
    if (ctx == NULL)
    {
        ESP_LOGE(TAG, "FATAL: Failed to create FreeRTOS primitives! Restarting...");
        esp_restart();
    }

#ifdef CONFIG_SMART_AP_BATTERY_CONTROL_ENABLED
    uint32_t v_batt = get_battery_voltage();

    if (v_batt < CONFIG_SMART_AP_BATT_START_MV && v_batt != 0 && v_batt > CONFIG_SMART_AP_BATT_BAD_MV)
    {
        ESP_LOGW(TAG, "Battery low (%u mV). Sleeping immediately...", v_batt);
        enter_deep_sleep((uint64_t)CONFIG_SMART_AP_SLEEP_TIME_MIN * 60ULL * 1000000ULL);
    }
    ESP_LOGI(TAG, "Power OK (%u mV). Starting Services...", v_batt);
#else
    ESP_LOGI(TAG, "Battery control disabled. Starting Services...");
#endif

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "Initializing storage");
    ESP_ERROR_CHECK(storage_init());

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_SCAN_DONE, wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STACONNECTED, wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STADISCONNECTED, wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_START, wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_START, wifi_event_handler, NULL));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    // TX power is configured in menuconfig -> Component config -> PHY
    // (CONFIG_ESP8266_PHY_MAX_WIFI_TX_POWER, in dBm). No need to set it again.
    // country_config.max_tx_power uses 0.25 dBm units: PHY_dBm * 4
    wifi_country_t country_config = {
        .cc = CONFIG_SMART_AP_COUNTRY_CODE,
        .schan = 1,
        .nchan = CONFIG_SMART_AP_MAX_CHANNELS,
        .policy = WIFI_COUNTRY_POLICY_MANUAL,
        .max_tx_power = CONFIG_ESP8266_PHY_MAX_WIFI_TX_POWER * 4,
    };
    ESP_ERROR_CHECK(esp_wifi_set_country(&country_config));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    ESP_ERROR_CHECK(esp_wifi_start());
    // Step 2: Scan for best channel in STA mode, then stop WiFi
    wifi_config_t ap_init_config = {
        .ap = {
            .ssid = ESP_WIFI_SSID,
            .ssid_len = strlen(ESP_WIFI_SSID),
            .password = ESP_WIFI_PASSWORD,
            .max_connection = CONFIG_SMART_AP_MAX_CONNECTIONS,
            .authmode = ESP_WIFI_AUTH_MODE,
            .channel = find_best_channel(),
            .beacon_interval = CONFIG_SMART_AP_BEACON_INTERVAL,
            .ssid_hidden = ESP_WIFI_SSID_HIDDEN,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_stop());

    // Step 3: Switch to APSTA, set AP config, then start
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_AP, &ap_init_config));

    ESP_ERROR_CHECK(esp_wifi_start());

    // ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(CONFIG_ESP8266_PHY_MAX_WIFI_TX_POWER * 4));

    if (xTaskCreate(server_orchestrator_task, "orchestrator", CONFIG_SMART_AP_ORCHESTRATOR_TASK_STACK, NULL, CONFIG_SMART_AP_ORCHESTRATOR_TASK_PRIORITY, NULL) != pdPASS)
    {
        ESP_LOGE(TAG, "FATAL: Failed to create orchestrator task! Restarting...");
        esp_restart();
    }

    if (xTaskCreate(reset_timer_task, "reset_tmr", CONFIG_SMART_AP_RESET_TASK_STACK, NULL, CONFIG_SMART_AP_RESET_TASK_PRIORITY, NULL) != pdPASS)
    {
        ESP_LOGE(TAG, "FATAL: Failed to create reset_timer_task! Restarting...");
        esp_restart();
    }

    if (xTaskCreate(smart_ap_task, "smart_ap", CONFIG_SMART_AP_AP_TASK_STACK, NULL, CONFIG_SMART_AP_AP_TASK_PRIORITY, NULL) != pdPASS)
    {
        ESP_LOGE(TAG, "FATAL: Failed to create smart_ap_task! Restarting...");
        esp_restart();
    }

    if (xTaskCreate(beep_task, "beep_task", CONFIG_SMART_AP_BEEP_TASK_STACK, NULL, CONFIG_SMART_AP_BEEP_TASK_PRIORITY, NULL) != pdPASS)
    {
        ESP_LOGE(TAG, "FATAL: Failed to create beep_task! Restarting...");
        esp_restart();
    }

#ifdef CONFIG_SMART_AP_BATTERY_CONTROL_ENABLED
    if (xTaskCreate(battery_monitor_task, "bat", CONFIG_SMART_AP_BATT_TASK_STACK, NULL, CONFIG_SMART_AP_BATT_TASK_PRIORITY, NULL) != pdPASS)
    {
        ESP_LOGE(TAG, "FATAL: Failed to create battery_monitor_task! Restarting...");
        esp_restart();
    }
#endif

    ESP_LOGI(TAG, "System Ready!");
}
