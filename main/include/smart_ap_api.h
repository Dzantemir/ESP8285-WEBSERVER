#ifndef SMART_AP_API_H
#define SMART_AP_API_H

// ============================================================================
// PUBLIC FUNCTION PROTOTYPES
// Grouped by module — minimal includes for type resolution.
// ============================================================================

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_event.h"

// --- buzzer.c ---
void buzzer_off(void);
void buzzer_on(void);
void buzzer_gpio_init(void);
void beep_task(void *pvParameters);
esp_err_t beep_handler(httpd_req_t *req);

// --- dns_server.c ---
void dns_server_task(void *pvParameters);
void start_dns_server(void);
void stop_dns_server(void);

// --- web_server.c ---
void start_webserver(void);
void stop_webserver(void);

// --- orchestrator.c ---
void server_orchestrator_task(void *pvParameters);

// --- power_mgmt.c ---
#ifdef CONFIG_SMART_AP_BATTERY_CONTROL_ENABLED
uint32_t get_battery_voltage(void);
void battery_monitor_task(void *pvParameters);
void enter_deep_sleep(uint64_t time_us);
#endif
bool free_proc(void);
void reset_timer_task(void *pvParameters);

// --- wifi_ap.c ---
void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);
uint8_t find_best_channel(void);
void smart_ap_task(void *pvParameters);

// --- storage.c ---
esp_err_t storage_init(void);
void storage_deinit(void);
bool storage_is_spiffs_mounted(void);
bool storage_is_sd_mounted(void);
const char *storage_get_captive_path(void);
const char *storage_get_primary_path(void);
const char *storage_get_secondary_path(void);

#endif // SMART_AP_API_H
