// Buzzer module — buzzer GPIO control, beep task, beep HTTP handler
// Extracted from main.c for modularity

#include "smart_ap_common.h"

#include <string.h>
#include <stdbool.h>

#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_http_server.h"
#include "driver/gpio.h"

#ifdef BUZZER_PASSIVE
#include "driver/pwm.h"

#define PWM_CHANNEL_NUM 1

static const uint32_t buzzer_pwm_pins[PWM_CHANNEL_NUM] = {CONFIG_SMART_AP_BUZZER_GPIO};
static uint32_t buzzer_pwm_duties[PWM_CHANNEL_NUM] = {PWM_DUTY_50};
static float phase[PWM_CHANNEL_NUM] = {0.0};

#endif // BUZZER_PASSIVE

void buzzer_off(void)
{
#ifdef CONFIG_SMART_AP_BUZZER_ENABLED
#ifdef BUZZER_PASSIVE
    pwm_stop(0);
#else
    gpio_set_level(CONFIG_SMART_AP_BUZZER_GPIO, CONFIG_SMART_AP_BUZZER_ACTIVE_HIGH ? 0 : 1);
#endif
#endif
}

void buzzer_gpio_init(void)
{
#ifdef CONFIG_SMART_AP_BUZZER_ENABLED
#ifdef BUZZER_PASSIVE
    ESP_ERROR_CHECK(pwm_init(PWM_PERIOD_US, buzzer_pwm_duties, PWM_CHANNEL_NUM, buzzer_pwm_pins));
    pwm_set_phases(phase);
    buzzer_off();
#else
    gpio_config_t io_conf = {
        .pin_bit_mask = (1UL << CONFIG_SMART_AP_BUZZER_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    gpio_set_level(CONFIG_SMART_AP_BUZZER_GPIO, CONFIG_SMART_AP_BUZZER_ACTIVE_HIGH ? 0 : 1);
#endif
#endif
}

void buzzer_on(void)
{
#ifdef CONFIG_SMART_AP_BUZZER_ENABLED
#ifdef BUZZER_PASSIVE
    pwm_start();
#else
    gpio_set_level(CONFIG_SMART_AP_BUZZER_GPIO, CONFIG_SMART_AP_BUZZER_ACTIVE_HIGH ? 1 : 0);
#endif
#endif
}

// ============================================================================
// BEEP TASK — persistent task, accepts commands from queue
// Pattern: beep_handler takes beep_sem (checks "busy"),
//          sends command to beep_queue,
//          beep_task processes and gives beep_sem when done.
// ============================================================================

void beep_task(void *pvParameters)
{
    CTX_CHECK();

    buzzer_gpio_init();

    ESP_LOGI(TAG, "Beeper task started, duration=%d sec", CONFIG_SMART_AP_BEEP_TOTAL_SEC);

    while (1)
    {
        if (xEventGroupGetBits(ctx->events) & EVT_SHUTDOWN_BIT)
            break;

        beep_cmd_t cmd;
        if (xQueueReceive(ctx->beep_queue, &cmd, pdMS_TO_TICKS(1000)) != pdTRUE)
            continue;

        ESP_LOGI(TAG, "Beeper: start beeping");

        int64_t end_us = esp_timer_get_time() + (int64_t)CONFIG_SMART_AP_BEEP_TOTAL_SEC * 1000000LL;

        while (esp_timer_get_time() < end_us)
        {
            for (int i = 0; i < 3; i++)
            {
                if (xEventGroupGetBits(ctx->events) & EVT_SHUTDOWN_BIT)
                    goto beep_stop;

                buzzer_on();
                vTaskDelay(pdMS_TO_TICKS(500));
                buzzer_off();
                vTaskDelay(pdMS_TO_TICKS(500));
            }
            for (int i = 0; i < 6; i++)
            {
                if (xEventGroupGetBits(ctx->events) & EVT_SHUTDOWN_BIT)
                    goto beep_stop;

                vTaskDelay(pdMS_TO_TICKS(500));
            }
        }
    beep_stop:;
        buzzer_off();
        ESP_LOGI(TAG, "Beeper: finished");

        if (xEventGroupGetBits(ctx->events) & EVT_SHUTDOWN_BIT)
        {
            break;
        }

        xSemaphoreGive(ctx->beep_sem);
    }

    xEventGroupSetBits(ctx->events, EVT_BEEP_DONE_BIT);
    ESP_LOGI(TAG, "Beeper task exiting gracefully");
    vTaskDelete(NULL);
}

esp_err_t beep_handler(httpd_req_t *req)
{
    CTX_CHECK_RET(ESP_FAIL);

    char xrw[16] = {0};
    bool is_js = (httpd_req_get_hdr_value_str(req, "X-Requested-With", xrw, sizeof(xrw)) == ESP_OK);

    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    if (xSemaphoreTake(ctx->beep_sem, 0) != pdTRUE)
    {
        if (is_js)
        {
            httpd_resp_set_type(req, HTTPD_TYPE_JSON);
            httpd_resp_send(req, "{\"status\":\"busy\"}", HTTPD_RESP_USE_STRLEN);
        }
        else
        {
            httpd_resp_set_status(req, "302 Found");
            httpd_resp_set_hdr(req, "Location", "/");
            httpd_resp_send(req, NULL, 0);
        }
        return ESP_OK;
    }

    if (xEventGroupGetBits(ctx->events) & EVT_SHUTDOWN_BIT)
    {
        xSemaphoreGive(ctx->beep_sem);
        httpd_resp_send_500(req);
        return ESP_OK;
    }

    beep_cmd_t cmd = BEEP_CMD_START;

    if (xQueueSend(ctx->beep_queue, &cmd, 0) != pdTRUE)
    {
        xSemaphoreGive(ctx->beep_sem);
        if (is_js)
        {
            httpd_resp_set_type(req, HTTPD_TYPE_JSON);
            httpd_resp_set_status(req, HTTPD_500);
            httpd_resp_send(req, "{\"status\":\"error\"}", HTTPD_RESP_USE_STRLEN);
        }
        else
        {
            httpd_resp_set_status(req, "302 Found");
            httpd_resp_set_hdr(req, "Location", "/");
            httpd_resp_send(req, NULL, 0);
        }
        return ESP_OK;
    }

    if (is_js)
    {
        httpd_resp_set_type(req, HTTPD_TYPE_JSON);
        httpd_resp_send(req, "{\"status\":\"started\"}", HTTPD_RESP_USE_STRLEN);
    }
    else
    {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "/");
        httpd_resp_send(req, NULL, 0);
    }
    return ESP_OK;
}
