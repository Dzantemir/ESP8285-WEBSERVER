// Storage module — SPIFFS and SD card mount/unmount
// Supports: SPIFFS only, SD only, SPIFFS + SD (dual mode)
// SD card uses HSPI hardware: MOSI=GPIO13, MISO=GPIO12, SCLK=GPIO14
// Only CS pin is configurable (default GPIO5).

#include "smart_ap_common.h"

#include <string.h>
#include <stdbool.h>

#include "freertos/semphr.h"

#include "esp_log.h"

#if STORAGE_USES_SPIFFS
#include "esp_spiffs.h"
#endif

#if STORAGE_USES_SD
#include "diskio_sd_spi.h"
#include "esp_vfs_fat.h"
#endif

// ============================================================================
// RUNTIME STATE
// ============================================================================

static bool spiffs_mounted = false;
static bool sd_mounted = false;
static SemaphoreHandle_t s_storage_mutex = NULL;

// ============================================================================
// INIT — mount configured storage backends
// ============================================================================

esp_err_t storage_init(void)
{
    esp_err_t ret;

    // Создаём мьютекс при первой инициализации.
    // Защищает spiffs_mounted / sd_mounted от гонок между
    // main task (init/deinit) и httpd task (query functions).
    if (s_storage_mutex == NULL)
    {
        s_storage_mutex = xSemaphoreCreateMutex();
        if (s_storage_mutex == NULL)
        {
            ESP_LOGE(TAG, "Failed to create storage mutex");
            return ESP_ERR_NO_MEM;
        }
    }

#if STORAGE_USES_SPIFFS
    ESP_LOGI(TAG, "Mounting SPIFFS...");

    esp_vfs_spiffs_conf_t spiffs_conf = {
        .base_path = CONFIG_SMART_AP_SPIFFS_BASE_PATH,
        .partition_label = CONFIG_SMART_AP_SPIFFS_PARTITION_LABEL,
        .max_files = CONFIG_SMART_AP_SPIFFS_MAX_OPEN_FILES,
        .format_if_mount_failed = false,
    };

    ret = esp_vfs_spiffs_register(&spiffs_conf);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "SPIFFS mount failed: %s", esp_err_to_name(ret));
#if !STORAGE_USES_SD
        // SPIFFS is the only storage — fatal
        return ret;
#endif
    }
    else
    {
        xSemaphoreTake(s_storage_mutex, portMAX_DELAY);
        spiffs_mounted = true;
        xSemaphoreGive(s_storage_mutex);
        ESP_LOGI(TAG, "SPIFFS mounted at %s", CONFIG_SMART_AP_SPIFFS_BASE_PATH);
    }
#endif // STORAGE_USES_SPIFFS

#if STORAGE_USES_SD
    ESP_LOGI(TAG, "Mounting SD card (CS=GPIO%d)...", CONFIG_SMART_AP_SD_CS_GPIO);

    {
        const char *pwd = CONFIG_SMART_AP_SD_PASSWORD;
        uint8_t pwd_len = (uint8_t)strlen(pwd);

        sd_spi_config_t sd_config = {
            .cs_io_num = CONFIG_SMART_AP_SD_CS_GPIO,
            .password = (pwd_len > 0) ? pwd : NULL,
            .pwd_len = pwd_len,
        };

        ret = esp_vfs_fat_sd_spi_mount(CONFIG_SMART_AP_SD_BASE_PATH, 0, &sd_config, CONFIG_SMART_AP_SD_MAX_OPEN_FILES);
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "SD card mount failed: %s", esp_err_to_name(ret));
#if !STORAGE_USES_SPIFFS
            return ret;
#endif
            goto sd_mount_done;
        }

        xSemaphoreTake(s_storage_mutex, portMAX_DELAY);
        sd_mounted = true;
        xSemaphoreGive(s_storage_mutex);
        ESP_LOGI(TAG, "SD card mounted at %s", CONFIG_SMART_AP_SD_BASE_PATH);
    }

sd_mount_done:
#endif // STORAGE_USES_SD

    xSemaphoreTake(s_storage_mutex, portMAX_DELAY);
    bool any_mounted = spiffs_mounted || sd_mounted;
    bool spiffs = spiffs_mounted;
    bool sd = sd_mounted;
    xSemaphoreGive(s_storage_mutex);

    if (!any_mounted)
    {
        ESP_LOGE(TAG, "FATAL: No storage mounted!");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Storage ready (SPIFFS=%d, SD=%d)", spiffs, sd);
    return ESP_OK;
}

// ============================================================================
// DEINIT — unmount all storage (for shutdown / deep sleep)
// ============================================================================

void storage_deinit(void)
{
#if STORAGE_USES_SD
    xSemaphoreTake(s_storage_mutex, portMAX_DELAY);
    bool sd = sd_mounted;
    xSemaphoreGive(s_storage_mutex);

    if (sd)
    {
        esp_vfs_fat_sd_spi_unmount(CONFIG_SMART_AP_SD_BASE_PATH, 0);
        xSemaphoreTake(s_storage_mutex, portMAX_DELAY);
        sd_mounted = false;
        xSemaphoreGive(s_storage_mutex);
        ESP_LOGI(TAG, "SD card unmounted");
    }
#endif

#if STORAGE_USES_SPIFFS
    xSemaphoreTake(s_storage_mutex, portMAX_DELAY);
    bool spiffs = spiffs_mounted;
    xSemaphoreGive(s_storage_mutex);

    if (spiffs)
    {
        esp_vfs_spiffs_unregister(CONFIG_SMART_AP_SPIFFS_PARTITION_LABEL);
        xSemaphoreTake(s_storage_mutex, portMAX_DELAY);
        spiffs_mounted = false;
        xSemaphoreGive(s_storage_mutex);
        ESP_LOGI(TAG, "SPIFFS unmounted");
    }
#endif
}

// ============================================================================
// QUERY FUNCTIONS — used by web_server.c to decide where to serve from
// ============================================================================

bool storage_is_spiffs_mounted(void)
{
    if (s_storage_mutex == NULL)
        return false;
    xSemaphoreTake(s_storage_mutex, portMAX_DELAY);
    bool mounted = spiffs_mounted;
    xSemaphoreGive(s_storage_mutex);
    return mounted;
}

bool storage_is_sd_mounted(void)
{
    if (s_storage_mutex == NULL)
        return false;
    xSemaphoreTake(s_storage_mutex, portMAX_DELAY);
    bool mounted = sd_mounted;
    xSemaphoreGive(s_storage_mutex);
    return mounted;
}

const char *storage_get_captive_path(void)
{
    // Captive portal HTML: prefer SPIFFS (always available, small, fast)
    // Fall back to SD if no SPIFFS
    if (s_storage_mutex == NULL)
        return "";
    xSemaphoreTake(s_storage_mutex, portMAX_DELAY);
    bool spiffs = spiffs_mounted;
    bool sd = sd_mounted;
    xSemaphoreGive(s_storage_mutex);

    if (spiffs)
        return SPIFFS_BASE_PATH;
    if (sd)
        return SD_BASE_PATH;
    return "";
}

const char *storage_get_primary_path(void)
{
    // Primary file serving: prefer SD (larger storage for media)
    // Fall back to SPIFFS if no SD
    if (s_storage_mutex == NULL)
        return "";
    xSemaphoreTake(s_storage_mutex, portMAX_DELAY);
    bool sd = sd_mounted;
    bool spiffs = spiffs_mounted;
    xSemaphoreGive(s_storage_mutex);

    if (sd)
        return SD_BASE_PATH;
    if (spiffs)
        return SPIFFS_BASE_PATH;
    return "";
}

const char *storage_get_secondary_path(void)
{
    // Secondary / fallback path in dual mode
#if STORAGE_USES_SPIFFS && STORAGE_USES_SD
    if (s_storage_mutex == NULL)
        return NULL;
    xSemaphoreTake(s_storage_mutex, portMAX_DELAY);
    bool dual = sd_mounted && spiffs_mounted;
    xSemaphoreGive(s_storage_mutex);

    if (dual)
        return SPIFFS_BASE_PATH; // SD is primary, SPIFFS is secondary
#endif
    return NULL; // no secondary in single-storage mode
}
