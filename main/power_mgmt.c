// Power management module — battery, deep sleep, shutdown, reset timer

#include "smart_ap_common.h"

#include <stdbool.h>

#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"

#include "esp_system.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_sleep.h"
#include "nvs_flash.h"
#include "driver/gpio.h"

#ifdef CONFIG_SMART_AP_BATTERY_CONTROL_ENABLED
#include "driver/adc.h"
#endif

// ============================================================================
// BATTERY VOLTAGE
// ============================================================================

#ifdef CONFIG_SMART_AP_BATTERY_CONTROL_ENABLED

uint32_t get_battery_voltage(void)
{
    uint32_t adc_sum = 0;
    int valid_samples = 0;
    const int samples = CONFIG_SMART_AP_ADC_SAMPLES;

    for (int i = 0; i < samples; i++)
    {
        if (ctx != NULL && (xEventGroupGetBits(ctx->events) & EVT_SHUTDOWN_BIT))
            break;
        uint16_t val = 0;
        if (adc_read(&val) != ESP_OK)
            continue;
        adc_sum += val;
        valid_samples++;
        vTaskDelay(pdMS_TO_TICKS(CONFIG_SMART_AP_ADC_SAMPLE_DELAY_MS));
    }

    if (valid_samples == 0)
        return 0;

    uint32_t adc_avg = adc_sum / valid_samples;
    return (uint32_t)((adc_avg * CONFIG_SMART_AP_VOLTAGE_DIVIDER_RATIO) / 1024);
}

#endif // CONFIG_SMART_AP_BATTERY_CONTROL_ENABLED

// ============================================================================
// SHUTDOWN PROCEDURE
// ============================================================================

bool free_proc(void)
{
    CTX_CHECK_RET(false);

    if (xSemaphoreTake(ctx->shutdown_sem, 0) != pdTRUE)
    {
        ESP_LOGW(TAG, "Shutdown already in progress, skipping free_proc()");
        return false;
    }

    xEventGroupSetBits(ctx->events, EVT_SHUTDOWN_BIT);

    server_cmd_msg_t msg = {.cmd = SERVER_CMD_SHUTDOWN};
    if (xQueueSend(ctx->server_cmd_queue, &msg, pdMS_TO_TICKS(2000)) != pdTRUE)
    {
        ESP_LOGW(TAG, "Failed to send SHUTDOWN to orchestrator (queue full), "
                      "orchestrator will detect EVT_SHUTDOWN_BIT via timeout");
    }

    EventBits_t done_bits = xEventGroupWaitBits(
        ctx->events,
        EVT_AP_DONE_BIT | EVT_BEEP_DONE_BIT
#ifdef CONFIG_SMART_AP_BATTERY_CONTROL_ENABLED
            | EVT_BATT_DONE_BIT
#endif
            | EVT_ORCHESTRATOR_DONE_BIT | EVT_RESET_DONE_BIT,
        pdFALSE,
        pdTRUE,
        pdSEC_TO_TICKS(CONFIG_SMART_AP_SHUTDOWN_GRACE_SEC));

    EventBits_t all_done = EVT_AP_DONE_BIT | EVT_BEEP_DONE_BIT
#ifdef CONFIG_SMART_AP_BATTERY_CONTROL_ENABLED
                           | EVT_BATT_DONE_BIT
#endif
                           | EVT_ORCHESTRATOR_DONE_BIT | EVT_RESET_DONE_BIT;

    if ((done_bits & all_done) == all_done)
    {
        ESP_LOGI(TAG, "All tasks finished gracefully.");
    }
    else
    {
        ESP_LOGW(TAG, "Graceful shutdown timeout! bits=0x%x", done_bits);
        EventBits_t critical_done = EVT_ORCHESTRATOR_DONE_BIT | EVT_AP_DONE_BIT;
        if ((done_bits & critical_done) != critical_done)
        {
            ESP_LOGE(TAG, "Critical tasks hung during shutdown! Forcing restart. "
                          "orchestrator=%d ap=%d",
                     (done_bits & EVT_ORCHESTRATOR_DONE_BIT) ? 1 : 0,
                     (done_bits & EVT_AP_DONE_BIT) ? 1 : 0);
            esp_restart();
        }
    }

#ifdef CONFIG_SMART_AP_BUZZER_ENABLED
#ifdef BUZZER_PASSIVE
    buzzer_off();
    gpio_config_t buzzer_shutdown_cfg = {
        .pin_bit_mask = (1UL << CONFIG_SMART_AP_BUZZER_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&buzzer_shutdown_cfg);
#endif
    gpio_set_level(CONFIG_SMART_AP_BUZZER_GPIO, CONFIG_SMART_AP_BUZZER_ACTIVE_HIGH ? 0 : 1);
#endif // CONFIG_SMART_AP_BUZZER_ENABLED

    esp_wifi_deauth_sta(0);

    esp_wifi_stop();

    esp_event_handler_unregister(WIFI_EVENT, WIFI_EVENT_SCAN_DONE, wifi_event_handler);
    esp_event_handler_unregister(WIFI_EVENT, WIFI_EVENT_AP_STACONNECTED, wifi_event_handler);
    esp_event_handler_unregister(WIFI_EVENT, WIFI_EVENT_AP_STADISCONNECTED, wifi_event_handler);

    esp_event_handler_unregister(WIFI_EVENT, WIFI_EVENT_STA_START, wifi_event_handler);
    esp_event_handler_unregister(WIFI_EVENT, WIFI_EVENT_AP_START, wifi_event_handler);

    esp_wifi_deinit();
    esp_event_loop_delete_default();
    storage_deinit();
    nvs_flash_deinit();
#ifdef CONFIG_SMART_AP_BATTERY_CONTROL_ENABLED
    adc_deinit();
#endif

    return true;
}

// ============================================================================
// DEEP SLEEP
// ============================================================================

#ifdef CONFIG_SMART_AP_BATTERY_CONTROL_ENABLED

void enter_deep_sleep(uint64_t time_us)
{
    ESP_LOGI(TAG, "Configuring deep sleep for %u min...", (uint32_t)(time_us / 60000000ULL));

    esp_deep_sleep_set_rf_option(2);
    esp_deep_sleep(time_us);

    ESP_LOGE(TAG, "esp_deep_sleep returned unexpectedly! Restarting...");
    esp_restart();
}

static void deep_sleep_logic(uint32_t minutes)
{
    ESP_LOGI(TAG, "Preparing for deep sleep...");

    bool we_did_shutdown = free_proc();
    if (!we_did_shutdown)
    {
        ESP_LOGW(TAG, "Shutdown already in progress, skipping deep sleep");
        vTaskDelete(NULL);
        return;
    }

    enter_deep_sleep((uint64_t)minutes * 60ULL * 1000000ULL);
}

#endif // CONFIG_SMART_AP_BATTERY_CONTROL_ENABLED

// ============================================================================
// BATTERY MONITOR TASK
// ============================================================================

#ifdef CONFIG_SMART_AP_BATTERY_CONTROL_ENABLED

void battery_monitor_task(void *pvParameters)
{
    CTX_CHECK();

    while (1)
    {

        uint32_t v_batt = get_battery_voltage();

        if (v_batt < CONFIG_SMART_AP_BATT_CRITICAL_MV && v_batt != 0 && v_batt > CONFIG_SMART_AP_BATT_BAD_MV)
        {
            ESP_LOGW(TAG, "Battery low (%u mV). Shutting down...", v_batt);
            xEventGroupSetBits(ctx->events, EVT_BATT_DONE_BIT);
            deep_sleep_logic(CONFIG_SMART_AP_SLEEP_TIME_MIN);
            return;
        }

        EventBits_t bits = xEventGroupWaitBits(
            ctx->events,
            EVT_SHUTDOWN_BIT,
            pdFALSE,
            pdFALSE,
            pdSEC_TO_TICKS(CONFIG_SMART_AP_ACTIVE_CHECK_MIN * 60));

        if (bits & EVT_SHUTDOWN_BIT)
            break;
    }

    ESP_LOGI(TAG, "Battery monitor task exiting gracefully");
    xEventGroupSetBits(ctx->events, EVT_BATT_DONE_BIT);
    vTaskDelete(NULL);
}

#endif // CONFIG_SMART_AP_BATTERY_CONTROL_ENABLED

// ============================================================================
// RESET TIMER TASK
// ============================================================================

void reset_timer_task(void *pvParameters)
{
    CTX_CHECK();

    // Bug #15 fix: explicit handling when MIN == MAX (modulo 1 = always 0)
    uint32_t random_days;
    if (CONFIG_SMART_AP_RESET_TIME_DAY_MIN >= CONFIG_SMART_AP_RESET_TIME_DAY_MAX)
    {
        // MIN >= MAX: use the exact value, no randomization range
        random_days = CONFIG_SMART_AP_RESET_TIME_DAY_MIN;
    }
    else
    {
        random_days = CONFIG_SMART_AP_RESET_TIME_DAY_MIN + (esp_random() % (CONFIG_SMART_AP_RESET_TIME_DAY_MAX - CONFIG_SMART_AP_RESET_TIME_DAY_MIN + 1));
    }
    uint32_t target_mins_rtc = random_days * 24 * 60;

    ESP_LOGI(TAG, "Scheduled hard reset in %u mins", target_mins_rtc);

    while (1)
    {
        EventBits_t bits = xEventGroupWaitBits(
            ctx->events,
            EVT_SHUTDOWN_BIT,
            pdFALSE,
            pdFALSE,
            pdSEC_TO_TICKS(CONFIG_SMART_AP_RESET_CHECK_INTERVAL_SEC));

        if (bits & EVT_SHUTDOWN_BIT)
        {
            ESP_LOGI(TAG, "Shutdown during reset timer wait, exiting");
            xEventGroupSetBits(ctx->events, EVT_RESET_DONE_BIT);
            vTaskDelete(NULL);
            return;
        }

        uint32_t uptime_mins = (uint32_t)(esp_timer_get_time() / 60000000ULL);

        if (uptime_mins >= target_mins_rtc)
        {
            ESP_LOGI(TAG, "Scheduled reset time reached. Initiating graceful shutdown...");

            xEventGroupSetBits(ctx->events, EVT_RESET_DONE_BIT);

            if (free_proc())
            {
                ESP_LOGI(TAG, "Performing hard reset.");
                esp_restart();
            }
            else
            {
                ESP_LOGI(TAG, "Shutdown already in progress by another task, exiting reset_timer");
                vTaskDelete(NULL);
                return;
            }
        }
    }
}
