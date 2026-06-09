#ifndef SMART_AP_CONFIG_H
#define SMART_AP_CONFIG_H

// ============================================================================
// KONFIGURATION — via Kconfig (menuconfig) with fallback defaults
// All settings available: make menuconfig -> Component config -> SMART AP
//
// Naming convention (Bug #9 fix — single naming system):
//   All values use CONFIG_SMART_AP_* names directly in .c files.
//   Only computed/derived values get short #define aliases.
//   No more dual naming (alias + CONFIG_ for the same value).
// ============================================================================

// --- Wi-Fi ---
#ifndef CONFIG_SMART_AP_WIFI_SSID
#define CONFIG_SMART_AP_WIFI_SSID "WIFI"
#endif

// Wi-Fi AP authentication mode
// CONFIG_SMART_AP_WIFI_AUTH_OPEN        = Open (no password)
// CONFIG_SMART_AP_WIFI_AUTH_WPA2_PSK    = WPA2-Personal
// CONFIG_SMART_AP_WIFI_AUTH_WPA_WPA2_PSK = WPA/WPA2-Personal

// Wi-Fi AP password (only used when auth mode is not OPEN)
#ifndef CONFIG_SMART_AP_WIFI_PASSWORD
#define CONFIG_SMART_AP_WIFI_PASSWORD "12345678"
#endif

// Wi-Fi AP SSID hidden mode
// 0 = visible (default), 1 = hidden
#ifndef CONFIG_SMART_AP_WIFI_SSID_HIDDEN
#define CONFIG_SMART_AP_WIFI_SSID_HIDDEN 0
#endif

// Derived: AP auth mode as wifi_auth_mode_t value
#if defined(CONFIG_SMART_AP_WIFI_AUTH_WPA2_PSK)
#define ESP_WIFI_AUTH_MODE WIFI_AUTH_WPA2_PSK
#define ESP_WIFI_PASSWORD CONFIG_SMART_AP_WIFI_PASSWORD
#elif defined(CONFIG_SMART_AP_WIFI_AUTH_WPA_WPA2_PSK)
#define ESP_WIFI_AUTH_MODE WIFI_AUTH_WPA_WPA2_PSK
#define ESP_WIFI_PASSWORD CONFIG_SMART_AP_WIFI_PASSWORD
#elif defined(CONFIG_SMART_AP_WIFI_AUTH_WPA_PSK)
#define ESP_WIFI_AUTH_MODE WIFI_AUTH_WPA_PSK
#define ESP_WIFI_PASSWORD CONFIG_SMART_AP_WIFI_PASSWORD
#else
// Default: OPEN (no password)
#define ESP_WIFI_AUTH_MODE WIFI_AUTH_OPEN
#define ESP_WIFI_PASSWORD ""
#endif

// Bug #17 fix: compile-time WPA password length check (min AND max)
#if (defined(CONFIG_SMART_AP_WIFI_AUTH_WPA2_PSK) || defined(CONFIG_SMART_AP_WIFI_AUTH_WPA_WPA2_PSK) || defined(CONFIG_SMART_AP_WIFI_AUTH_WPA_PSK))
_Static_assert(sizeof(CONFIG_SMART_AP_WIFI_PASSWORD) - 1 >= 8,
    "WPA2/WPA password must be at least 8 characters");
_Static_assert(sizeof(CONFIG_SMART_AP_WIFI_PASSWORD) - 1 <= 63,
    "WPA2/WPA password must be at most 63 characters");
#endif

// Derived: SSID and SSID hidden — part of the ESP_WIFI_* API group
// (ESP_WIFI_SSID, ESP_WIFI_AUTH_MODE, ESP_WIFI_PASSWORD, ESP_WIFI_SSID_HIDDEN)
// used together in wifi_config_t initialization.
#define ESP_WIFI_SSID CONFIG_SMART_AP_WIFI_SSID
#define ESP_WIFI_SSID_HIDDEN CONFIG_SMART_AP_WIFI_SSID_HIDDEN

#ifndef CONFIG_SMART_AP_MAX_CONNECTIONS
#define CONFIG_SMART_AP_MAX_CONNECTIONS 4
#endif

#ifndef CONFIG_SMART_AP_BEACON_INTERVAL
#define CONFIG_SMART_AP_BEACON_INTERVAL 250
#endif

#ifndef CONFIG_SMART_AP_COUNTRY_CODE
#define CONFIG_SMART_AP_COUNTRY_CODE "RU"
#endif

#ifndef CONFIG_SMART_AP_MAX_CHANNELS
#define CONFIG_SMART_AP_MAX_CHANNELS 13
#endif

// TX power is configured in menuconfig -> Component config -> PHY
// (CONFIG_ESP8266_PHY_MAX_WIFI_TX_POWER, in dBm). Not duplicated here.
// country_config.max_tx_power uses 0.25 dBm units: PHY_dBm * 4

// --- AP Network ---
#ifndef CONFIG_SMART_AP_IP_ADDR
#define CONFIG_SMART_AP_IP_ADDR "192.168.4.1"
#endif

#ifndef CONFIG_SMART_AP_NETMASK
#define CONFIG_SMART_AP_NETMASK "255.255.255.0"
#endif

#ifndef CONFIG_SMART_AP_DHCP_LEASE_START
#define CONFIG_SMART_AP_DHCP_LEASE_START "192.168.4.2"
#endif

#ifndef CONFIG_SMART_AP_DHCP_LEASE_END
#define CONFIG_SMART_AP_DHCP_LEASE_END "192.168.4.10"
#endif

// --- Channel Scanning ---
#ifndef CONFIG_SMART_AP_SCAN_INTERVAL_MIN_HOUR
#define CONFIG_SMART_AP_SCAN_INTERVAL_MIN_HOUR 7
#endif

#ifndef CONFIG_SMART_AP_SCAN_INTERVAL_MAX_HOUR
#define CONFIG_SMART_AP_SCAN_INTERVAL_MAX_HOUR 21
#endif

// Bug #15 fix: compile-time guard — MIN must be < MAX (strict, not just <=)
// When MIN == MAX, the modulo expression becomes % 1 which is always 0,
// and the random interval has zero range — semantically wrong.
#if CONFIG_SMART_AP_SCAN_INTERVAL_MIN_HOUR > CONFIG_SMART_AP_SCAN_INTERVAL_MAX_HOUR
#error "SCAN_INTERVAL_MIN_HOUR must be <= SCAN_INTERVAL_MAX_HOUR"
#endif

// Computed values (not simple aliases — these transform the config)
#define SCAN_INTERVAL_MIN_SEC (CONFIG_SMART_AP_SCAN_INTERVAL_MIN_HOUR * 3600)
#define SCAN_INTERVAL_MAX_SEC (CONFIG_SMART_AP_SCAN_INTERVAL_MAX_HOUR * 3600)

#ifndef CONFIG_SMART_AP_WIFI_SCAN_TIMEOUT_SEC
#define CONFIG_SMART_AP_WIFI_SCAN_TIMEOUT_SEC 60
#endif

#ifndef CONFIG_SMART_AP_SCAN_SHOW_HIDDEN
#define CONFIG_SMART_AP_SCAN_SHOW_HIDDEN 1
#endif

// --- DNS Server ---
#ifndef CONFIG_SMART_AP_DNS_PORT
#define CONFIG_SMART_AP_DNS_PORT 53
#endif

#ifndef CONFIG_SMART_AP_DNS_TASK_STACK_SIZE
#define CONFIG_SMART_AP_DNS_TASK_STACK_SIZE 2048
#endif

#ifndef CONFIG_SMART_AP_DNS_TASK_PRIORITY
#define CONFIG_SMART_AP_DNS_TASK_PRIORITY 4
#endif

#ifndef CONFIG_SMART_AP_DNS_RATE_LIMIT
#define CONFIG_SMART_AP_DNS_RATE_LIMIT 50
#endif

#ifndef CONFIG_SMART_AP_DNS_RECV_TIMEOUT_SEC
#define CONFIG_SMART_AP_DNS_RECV_TIMEOUT_SEC 2
#endif

#ifndef CONFIG_SMART_AP_DNS_STOP_TIMEOUT_SEC
#define CONFIG_SMART_AP_DNS_STOP_TIMEOUT_SEC 5
#endif

#ifndef CONFIG_SMART_AP_DNS_RX_BUFFER_SIZE
#define CONFIG_SMART_AP_DNS_RX_BUFFER_SIZE 1024
#endif

#ifndef CONFIG_SMART_AP_DNS_BACKOFF_MAX_SEC
#define CONFIG_SMART_AP_DNS_BACKOFF_MAX_SEC 30
#endif

// --- HTTP Server ---
#ifndef CONFIG_SMART_AP_HTTPD_TASK_PRIORITY
#define CONFIG_SMART_AP_HTTPD_TASK_PRIORITY 4
#endif

#ifndef CONFIG_SMART_AP_HTTPD_MAX_OPEN_SOCKETS
#define CONFIG_SMART_AP_HTTPD_MAX_OPEN_SOCKETS 12
#endif

#ifndef CONFIG_SMART_AP_HTTPD_SCRATCH_BUFSIZE
#define CONFIG_SMART_AP_HTTPD_SCRATCH_BUFSIZE 1024
#endif

#ifndef CONFIG_SMART_AP_HTTP_YIELD_EVERY_N_CHUNKS
#define CONFIG_SMART_AP_HTTP_YIELD_EVERY_N_CHUNKS 16
#endif

#ifndef CONFIG_SMART_AP_HTTP_YIELD_DELAY_MS
#define CONFIG_SMART_AP_HTTP_YIELD_DELAY_MS 25
#endif

// --- Storage ---
// Storage mode is selected in menuconfig -> SMART AP Configuration -> Storage
// CONFIG_SMART_AP_STORAGE_SPIFFS    = SPIFFS only
// CONFIG_SMART_AP_STORAGE_SD        = SD card only
// CONFIG_SMART_AP_SPIFFS_SD         = SPIFFS + SD card

// SPIFFS settings (used when SPIFFS or SPIFFS+SD selected)
#ifdef CONFIG_SMART_AP_STORAGE_SPIFFS
#define STORAGE_USES_SPIFFS 1
#else
#ifdef CONFIG_SMART_AP_STORAGE_SPIFFS_SD
#define STORAGE_USES_SPIFFS 1
#else
#define STORAGE_USES_SPIFFS 0
#endif
#endif

#ifdef CONFIG_SMART_AP_STORAGE_SD
#define STORAGE_USES_SD 1
#else
#ifdef CONFIG_SMART_AP_STORAGE_SPIFFS_SD
#define STORAGE_USES_SD 1
#else
#define STORAGE_USES_SD 0
#endif
#endif

#if STORAGE_USES_SPIFFS

#ifndef CONFIG_SMART_AP_SPIFFS_MAX_FILES
#define CONFIG_SMART_AP_SPIFFS_MAX_FILES 20
#endif

#ifndef CONFIG_SMART_AP_SPIFFS_MAX_OPEN_FILES
#define CONFIG_SMART_AP_SPIFFS_MAX_OPEN_FILES 15
#endif

#ifndef CONFIG_SMART_AP_SPIFFS_BASE_PATH
#define CONFIG_SMART_AP_SPIFFS_BASE_PATH "/spiffs"
#endif

#ifndef CONFIG_SMART_AP_SPIFFS_PARTITION_LABEL
#define CONFIG_SMART_AP_SPIFFS_PARTITION_LABEL "storage"
#endif

#endif // STORAGE_USES_SPIFFS

#if STORAGE_USES_SD

#ifndef CONFIG_SMART_AP_SD_CS_GPIO
#define CONFIG_SMART_AP_SD_CS_GPIO 5
#endif

#ifndef CONFIG_SMART_AP_SD_BASE_PATH
#define CONFIG_SMART_AP_SD_BASE_PATH "/sdcard"
#endif

#ifndef CONFIG_SMART_AP_SD_MAX_FILES
#define CONFIG_SMART_AP_SD_MAX_FILES 30
#endif

#ifndef CONFIG_SMART_AP_SD_MAX_OPEN_FILES
#define CONFIG_SMART_AP_SD_MAX_OPEN_FILES 5
#endif

// SD card unlock password (empty = no unlock attempt)
#ifndef CONFIG_SMART_AP_SD_PASSWORD
#define CONFIG_SMART_AP_SD_PASSWORD ""
#endif

#endif // STORAGE_USES_SD

#ifndef CONFIG_SMART_AP_MAX_PATH_LEN
#define CONFIG_SMART_AP_MAX_PATH_LEN 128
#endif

// --- Reset Timer ---
#ifndef CONFIG_SMART_AP_RESET_TIME_DAY_MIN
#define CONFIG_SMART_AP_RESET_TIME_DAY_MIN 1
#endif

#ifndef CONFIG_SMART_AP_RESET_TIME_DAY_MAX
#define CONFIG_SMART_AP_RESET_TIME_DAY_MAX 31
#endif

// Bug #15 fix: compile-time guard — MIN must be < MAX (strict, not just <=)
#if CONFIG_SMART_AP_RESET_TIME_DAY_MIN > CONFIG_SMART_AP_RESET_TIME_DAY_MAX
#error "RESET_TIME_DAY_MIN must be <= RESET_TIME_DAY_MAX"
#endif

// --- Battery & Power ---
#ifdef CONFIG_SMART_AP_BATTERY_CONTROL_ENABLED

#ifndef CONFIG_SMART_AP_BATT_CRITICAL_MV
#define CONFIG_SMART_AP_BATT_CRITICAL_MV 3700
#endif

#ifndef CONFIG_SMART_AP_BATT_START_MV
#define CONFIG_SMART_AP_BATT_START_MV 3900
#endif

#ifndef CONFIG_SMART_AP_BATT_BAD_MV
#define CONFIG_SMART_AP_BATT_BAD_MV 2500
#endif

#ifndef CONFIG_SMART_AP_VOLTAGE_DIVIDER_RATIO
#define CONFIG_SMART_AP_VOLTAGE_DIVIDER_RATIO 5711
#endif

#ifndef CONFIG_SMART_AP_SLEEP_TIME_MIN
#define CONFIG_SMART_AP_SLEEP_TIME_MIN 30
#endif

#ifndef CONFIG_SMART_AP_ACTIVE_CHECK_MIN
#define CONFIG_SMART_AP_ACTIVE_CHECK_MIN 1
#endif

#ifndef CONFIG_SMART_AP_ADC_SAMPLES
#define CONFIG_SMART_AP_ADC_SAMPLES 15
#endif

#ifndef CONFIG_SMART_AP_ADC_SAMPLE_DELAY_MS
#define CONFIG_SMART_AP_ADC_SAMPLE_DELAY_MS 50
#endif

#else
// Battery control disabled — provide safe stubs/defaults (zero values)
#endif // CONFIG_SMART_AP_BATTERY_CONTROL_ENABLED

#ifndef CONFIG_SMART_AP_RESET_CHECK_INTERVAL_SEC
#define CONFIG_SMART_AP_RESET_CHECK_INTERVAL_SEC 30
#endif

// --- Orchestrator & Shutdown ---
#ifndef CONFIG_SMART_AP_SERVER_CMD_QUEUE_SIZE
#define CONFIG_SMART_AP_SERVER_CMD_QUEUE_SIZE 16
#endif

#ifndef CONFIG_SMART_AP_ORCHESTRATOR_TASK_STACK
#define CONFIG_SMART_AP_ORCHESTRATOR_TASK_STACK 2048
#endif

#ifndef CONFIG_SMART_AP_ORCHESTRATOR_TASK_PRIORITY
#define CONFIG_SMART_AP_ORCHESTRATOR_TASK_PRIORITY 3
#endif

#ifndef CONFIG_SMART_AP_SHUTDOWN_GRACE_SEC
#define CONFIG_SMART_AP_SHUTDOWN_GRACE_SEC 60
#endif

// --- Task Stacks & Priorities ---
#ifndef CONFIG_SMART_AP_HTTPD_TASK_STACK
#define CONFIG_SMART_AP_HTTPD_TASK_STACK 4096
#endif

#ifndef CONFIG_SMART_AP_AP_TASK_STACK
#define CONFIG_SMART_AP_AP_TASK_STACK 2048
#endif

#ifndef CONFIG_SMART_AP_AP_TASK_PRIORITY
#define CONFIG_SMART_AP_AP_TASK_PRIORITY 1
#endif

#ifndef CONFIG_SMART_AP_BEEP_TASK_STACK
#define CONFIG_SMART_AP_BEEP_TASK_STACK 1024
#endif

#ifndef CONFIG_SMART_AP_BEEP_TASK_PRIORITY
#define CONFIG_SMART_AP_BEEP_TASK_PRIORITY 2
#endif

#ifndef CONFIG_SMART_AP_BATT_TASK_STACK
#define CONFIG_SMART_AP_BATT_TASK_STACK 1024
#endif

#ifndef CONFIG_SMART_AP_BATT_TASK_PRIORITY
#define CONFIG_SMART_AP_BATT_TASK_PRIORITY 3
#endif

#ifndef CONFIG_SMART_AP_RESET_TASK_STACK
#define CONFIG_SMART_AP_RESET_TASK_STACK 1024
#endif

#ifndef CONFIG_SMART_AP_RESET_TASK_PRIORITY
#define CONFIG_SMART_AP_RESET_TASK_PRIORITY 1
#endif

// --- Buzzer ---
#ifdef CONFIG_SMART_AP_BUZZER_ENABLED

#ifdef CONFIG_SMART_AP_BUZZER_PASSIVE
#define BUZZER_PASSIVE
#endif

#ifndef CONFIG_SMART_AP_BUZZER_GPIO
#define CONFIG_SMART_AP_BUZZER_GPIO 4
#endif

#ifndef CONFIG_SMART_AP_BUZZER_ACTIVE_HIGH
#define CONFIG_SMART_AP_BUZZER_ACTIVE_HIGH 1
#endif

#ifndef CONFIG_SMART_AP_BUZZER_FREQ_HZ
#define CONFIG_SMART_AP_BUZZER_FREQ_HZ 2000
#endif

#ifndef CONFIG_SMART_AP_BEEP_TOTAL_SEC
#define CONFIG_SMART_AP_BEEP_TOTAL_SEC 45
#endif

// Computed values for passive buzzer PWM
#ifdef BUZZER_PASSIVE
#define PWM_PERIOD_US (1000000UL / CONFIG_SMART_AP_BUZZER_FREQ_HZ)
#define PWM_DUTY_50 (PWM_PERIOD_US / 2)
#endif

#else
// Buzzer disabled in menuconfig — compilation stubs
#endif // CONFIG_SMART_AP_BUZZER_ENABLED

// ============================================================================
// DERIVED CONSTANTS (computed from multiple config values — NOT simple aliases)
// ============================================================================

#if STORAGE_USES_SPIFFS
#define SPIFFS_BASE_PATH CONFIG_SMART_AP_SPIFFS_BASE_PATH
#define SPIFFS_PARTITION_LABEL CONFIG_SMART_AP_SPIFFS_PARTITION_LABEL
#define MAX_SPIFFS_FILES CONFIG_SMART_AP_SPIFFS_MAX_FILES
#else
#define SPIFFS_BASE_PATH ""
#define SPIFFS_PARTITION_LABEL ""
#define MAX_SPIFFS_FILES 0
#endif

#if STORAGE_USES_SD
#define SD_BASE_PATH CONFIG_SMART_AP_SD_BASE_PATH
#define SD_CS_GPIO CONFIG_SMART_AP_SD_CS_GPIO
#define MAX_SD_FILES CONFIG_SMART_AP_SD_MAX_FILES
#define SD_PASSWORD CONFIG_SMART_AP_SD_PASSWORD
#else
#define SD_BASE_PATH ""
#define SD_CS_GPIO 0
#define MAX_SD_FILES 0
#define SD_PASSWORD ""
#endif


// HTTPD_RESP_USE_STRLEN is now defined by the local esp_http_server component.
// Keeping the fallback guard for safety in case of header inclusion order issues.
#ifndef HTTPD_RESP_USE_STRLEN
#define HTTPD_RESP_USE_STRLEN (-1)
#endif

#endif // SMART_AP_CONFIG_H
