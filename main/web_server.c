// Web server module — MIME types, file handler (SPIFFS/SD), URI registration
// Idempotent: repeated calls to stop_webserver are safe (web_server==NULL check)
// Supports: SPIFFS only, SD only, SPIFFS + SD (dual mode)
// In dual mode: captive portal from SPIFFS, media files from SD,
//               SD takes priority for duplicate file names.

#include "smart_ap_common.h"

#include <string.h>
#include <stdbool.h>
#include <dirent.h>
#include <errno.h>

#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_log.h"
#include "esp_http_server.h"

// ============================================================================
// MIME TYPES
// ============================================================================

static const char *get_mime_type(const char *filepath)
{
    const char *ext = strrchr(filepath, '.');
    if (!ext || ext[1] == '\0')
        return "text/plain";

    switch (ext[1])
    {
    case 'a':
        if (strcmp(ext, ".apk") == 0) return "application/vnd.android.package-archive";
        if (strcmp(ext, ".avi") == 0) return "video/x-msvideo";
        if (strcmp(ext, ".aac") == 0) return "audio/aac";
        if (strcmp(ext, ".amr") == 0) return "audio/amr";
        break;
    case 'b':
        if (strcmp(ext, ".bin") == 0) return "application/octet-stream";
        if (strcmp(ext, ".bmp") == 0) return "image/bmp";
        if (strcmp(ext, ".bz2") == 0) return "application/x-bzip2";
        break;
    case 'c':
        if (strcmp(ext, ".css") == 0) return "text/css";
        break;
    case 'e':
        if (strcmp(ext, ".exe") == 0) return "application/octet-stream";
        break;
    case 'f':
        if (strcmp(ext, ".flac") == 0) return "audio/flac";
        if (strcmp(ext, ".flv") == 0) return "video/x-flv";
        break;
    case 'g':
        if (strcmp(ext, ".gif") == 0) return "image/gif";
        if (strcmp(ext, ".gz") == 0) return "application/gzip";
        break;
    case 'h':
        if (strcmp(ext, ".html") == 0 || strcmp(ext, ".htm") == 0) return "text/html";
        break;
    case 'i':
        if (strcmp(ext, ".ico") == 0) return "image/x-icon";
        if (strcmp(ext, ".iso") == 0) return "application/x-iso9660-image";
        break;
    case 'j':
        if (strcmp(ext, ".js") == 0) return "application/javascript";
        if (strcmp(ext, ".json") == 0) return "application/json";
        if (strcmp(ext, ".jpg") == 0 || strcmp(ext, ".jpeg") == 0) return "image/jpeg";
        break;
    case 'm':
        if (strcmp(ext, ".mp3") == 0) return "audio/mpeg";
        if (strcmp(ext, ".mp4") == 0) return "video/mp4";
        if (strcmp(ext, ".m4a") == 0) return "audio/mp4";
        if (strcmp(ext, ".m4v") == 0) return "video/x-m4v";
        if (strcmp(ext, ".mkv") == 0) return "video/x-matroska";
        if (strcmp(ext, ".mov") == 0) return "video/quicktime";
        if (strcmp(ext, ".mid") == 0 || strcmp(ext, ".midi") == 0) return "audio/midi";
        if (strcmp(ext, ".mpeg") == 0 || strcmp(ext, ".mpg") == 0) return "video/mpeg";
        break;
    case 'n':
        if (strcmp(ext, ".ncsi") == 0) return "text/plain";
        break;
    case 'o':
        if (strcmp(ext, ".ogg") == 0 || strcmp(ext, ".oga") == 0) return "audio/ogg";
        if (strcmp(ext, ".opus") == 0) return "audio/opus";
        if (strcmp(ext, ".otf") == 0) return "font/otf";
        break;
    case 'p':
        if (strcmp(ext, ".png") == 0) return "image/png";
        if (strcmp(ext, ".pdf") == 0) return "application/pdf";
        break;
    case 'r':
        if (strcmp(ext, ".rar") == 0) return "application/vnd.rar";
        break;
    case 's':
        if (strcmp(ext, ".svg") == 0) return "image/svg+xml";
        break;
    case 't':
        if (strcmp(ext, ".txt") == 0) return "text/plain";
        if (strcmp(ext, ".ttf") == 0) return "font/ttf";
        if (strcmp(ext, ".ts") == 0) return "video/mp2t";
        if (strcmp(ext, ".tar") == 0) return "application/x-tar";
        break;
    case 'w':
        if (strcmp(ext, ".woff2") == 0) return "font/woff2";
        if (strcmp(ext, ".woff") == 0) return "font/woff";
        if (strcmp(ext, ".webm") == 0) return "video/webm";
        if (strcmp(ext, ".webp") == 0) return "image/webp";
        if (strcmp(ext, ".wav") == 0) return "audio/wav";
        if (strcmp(ext, ".wmv") == 0) return "video/x-ms-wmv";
        if (strcmp(ext, ".wma") == 0) return "audio/x-ms-wma";
        break;
    case 'x':
        if (strcmp(ext, ".xml") == 0) return "application/xml";
        if (strcmp(ext, ".xz") == 0) return "application/x-xz";
        break;
    case 'z':
        if (strcmp(ext, ".zip") == 0) return "application/zip";
        break;
    case '3':
        if (strcmp(ext, ".3gp") == 0) return "video/3gpp";
        if (strcmp(ext, ".3g2") == 0) return "video/3gpp2";
        break;
    case '7':
        if (strcmp(ext, ".7z") == 0) return "application/x-7z-compressed";
        break;
    }

    return "application/octet-stream";
}

// ============================================================================
// FILE HANDLER + Captive Portal
// Serves files from SPIFFS and/or SD card based on storage configuration.
// URI path = VFS path: /spiffs/file.css or /sdcard/file.mp3
// ============================================================================

static esp_err_t file_handler(httpd_req_t *req)
{
    // Bug #14 fix: use CONFIG_SMART_AP_MAX_PATH_LEN consistently (was hardcoded 256)
    char filepath[CONFIG_SMART_AP_MAX_PATH_LEN];

    // --- Captive portal detection URIs → 302 redirect to / ---
    static const char *captive_detect_uris[] = {
        "/generate_204", "/gen_204", "/hotspot-detect.html",
        "/library/test/success.html", "/success.txt", "/connecttest.txt",
        "/ncsi.txt", "/redirect", "/canonical.html", "/wpad.dat"};
    for (int i = 0; i < sizeof(captive_detect_uris) / sizeof(captive_detect_uris[0]); i++)
    {
        if (strcmp(req->uri, captive_detect_uris[i]) == 0)
        {
            httpd_resp_set_hdr(req, "Connection", "close");
            httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
            httpd_resp_set_hdr(req, "Pragma", "no-cache");
            httpd_resp_set_status(req, "302 Found");
            httpd_resp_set_hdr(req, "Location", "/");
            httpd_resp_set_type(req, HTTPD_TYPE_TEXT);
            httpd_resp_send(req, NULL, 0);
            return ESP_OK;
        }
    }

    // --- Captive portal pages → index.html from captive path ---
    static const char *captive_page_uris[] = {"/", "/?", "/portal", "/portal?"};
    for (int i = 0; i < sizeof(captive_page_uris) / sizeof(captive_page_uris[0]); i++)
    {
        if (strcmp(req->uri, captive_page_uris[i]) == 0)
        {
            snprintf(filepath, sizeof(filepath), "%s/index.html", storage_get_captive_path());
            goto serve_file;
        }
    }

    // --- Regular file: try URI as VFS path directly ---
    strlcpy(filepath, req->uri, sizeof(filepath));

serve_file:;

    FILE *fd = fopen(filepath, "rb");

    // --- Fallback in dual mode: if file not found on primary, try secondary ---
    if (!fd)
    {
        const char *secondary = storage_get_secondary_path();
        if (secondary != NULL)
        {
            // Reconstruct path: replace primary prefix with secondary prefix
            const char *primary = storage_get_primary_path();
            size_t primary_len = strlen(primary);
            if (strncmp(filepath, primary, primary_len) == 0)
            {
                // Bug #14 fix: use CONFIG_SMART_AP_MAX_PATH_LEN consistently
                char alt_path[CONFIG_SMART_AP_MAX_PATH_LEN];
                snprintf(alt_path, sizeof(alt_path), "%s%s", secondary, filepath + primary_len);
                fd = fopen(alt_path, "rb");
                if (fd)
                    strlcpy(filepath, alt_path, sizeof(filepath));
            }
        }
    }

    if (!fd)
    {
        httpd_resp_send_404(req);
        return ESP_OK;
    }

    httpd_resp_set_type(req, get_mime_type(filepath));
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=3600");

    char chunk[CONFIG_SMART_AP_HTTPD_SCRATCH_BUFSIZE];
    size_t chunksize;
    esp_err_t ret = ESP_OK;
    int chunk_count = 0;

    while ((chunksize = fread(chunk, 1, CONFIG_SMART_AP_HTTPD_SCRATCH_BUFSIZE, fd)) > 0)
    {
        EventBits_t bits = xEventGroupGetBits(ctx->events);
        if ((bits & EVT_SHUTDOWN_BIT) || !(bits & EVT_SERVERS_RUNNING_BIT))
        {
            ESP_LOGW(TAG, "Transfer aborted (servers stopping): %s", filepath);
            ret = ESP_FAIL;
            break;
        }
        if (httpd_resp_send_chunk(req, (const char *)chunk, chunksize) != ESP_OK)
        {
            ESP_LOGE(TAG, "Send failed");
            ret = ESP_FAIL;
            break;
        }

        if (++chunk_count % CONFIG_SMART_AP_HTTP_YIELD_EVERY_N_CHUNKS == 0)
            vTaskDelay(pdMS_TO_TICKS(CONFIG_SMART_AP_HTTP_YIELD_DELAY_MS));
    }

    if (ret == ESP_OK && ferror(fd))
    {
        ESP_LOGE(TAG, "fread error occurred! errno: %d", errno);
        ret = ESP_FAIL;
    }

    fclose(fd);

    if (ret == ESP_OK)
    {
        httpd_resp_send_chunk(req, NULL, 0);
        ESP_LOGD(TAG, "WEB server send file: %s", filepath);
    }
    else
    {
        ESP_LOGE(TAG, "Failed to send complete file: %s", filepath);
    }

    return ret;
}

// ============================================================================
// URI REGISTRATION
// ============================================================================

static void register_uri(httpd_handle_t server, const char *uri)
{
    httpd_uri_t uri_get = {
        .uri = uri,
        .method = HTTP_GET,
        .handler = file_handler,
        .user_ctx = NULL,
    };
    esp_err_t err = httpd_register_uri_handler(server, &uri_get);
    if (err == ESP_OK)
        ESP_LOGI(TAG, "Registered URI: %s", uri);
    else if (err == ESP_ERR_HTTPD_HANDLER_EXISTS)
        ESP_LOGD(TAG, "URI already registered (skip): %s", uri);
    else
        ESP_LOGW(TAG, "Failed to register URI: %s (err=0x%x)", uri, err);
}

static void register_files_simple(httpd_handle_t server, const char *base_path)
{
    DIR *dir = opendir(base_path);
    if (!dir)
        return;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL)
    {
        // Bug #14 fix: use CONFIG_SMART_AP_MAX_PATH_LEN consistently (was MAX_PATH_LEN=128)
        char path[CONFIG_SMART_AP_MAX_PATH_LEN];

        int written = snprintf(path, sizeof(path), "%s/%s", base_path, entry->d_name);
        if (written < 0 || written >= (int)sizeof(path))
            continue;

        register_uri(server, path);
    }
    closedir(dir);
}

// ============================================================================
// START WEBSERVER
// ============================================================================

void start_webserver(void)
{
    CTX_CHECK();

    xSemaphoreTake(ctx->state_mtx, portMAX_DELAY);
    if (ctx->web_server != NULL)
    {
        xSemaphoreGive(ctx->state_mtx);
        return;
    }
    xSemaphoreGive(ctx->state_mtx);

    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = CONFIG_SMART_AP_HTTPD_TASK_STACK;
    config.max_uri_handlers = MAX_URI_FILES + 10;
    config.task_priority = CONFIG_SMART_AP_HTTPD_TASK_PRIORITY;
    config.max_open_sockets = CONFIG_SMART_AP_HTTPD_MAX_OPEN_SOCKETS;

    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "httpd_start failed: 0x%x", err);
        return;
    }

    xSemaphoreTake(ctx->state_mtx, portMAX_DELAY);
    if (ctx->web_server != NULL)
    {
        xSemaphoreGive(ctx->state_mtx);
        httpd_stop(server);
        ESP_LOGW(TAG, "start_webserver: race detected, stopping duplicate server");
        return;
    }
    ctx->web_server = server;
    xSemaphoreGive(ctx->state_mtx);

    // --- Captive portal URIs (always registered) ---
    static const char *captive_register_uris[] = {
        "/", "/generate_204", "/gen_204", "/hotspot-detect.html",
        "/library/test/success.html", "/success.txt", "/connecttest.txt",
        "/ncsi.txt", "/portal", "/redirect", "/canonical.html",
        "/wpad.dat", "/portal?", "/?"};
    for (int i = 0; i < sizeof(captive_register_uris) / sizeof(captive_register_uris[0]); i++)
        register_uri(server, captive_register_uris[i]);

    // --- File URIs from SD (primary, registered first = higher priority) ---
    if (storage_is_sd_mounted())
        register_files_simple(server, SD_BASE_PATH);

    // --- File URIs from SPIFFS (secondary in dual mode, primary in SPIFFS-only) ---
    if (storage_is_spiffs_mounted())
        register_files_simple(server, SPIFFS_BASE_PATH);

    // --- Beep endpoint ---
    httpd_uri_t beep_uri = {
        .uri = "/beep",
        .method = HTTP_POST,
        .handler = beep_handler,
        .user_ctx = NULL,
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &beep_uri));

    ESP_LOGI(TAG, "Registered URI: /beep");
    ESP_LOGI(TAG, "Web server started (SPIFFS=%d, SD=%d)",
             storage_is_spiffs_mounted(), storage_is_sd_mounted());
}

// ============================================================================
// STOP WEBSERVER
// ============================================================================

void stop_webserver(void)
{
    CTX_CHECK();

    xSemaphoreTake(ctx->state_mtx, portMAX_DELAY);
    httpd_handle_t server = ctx->web_server;
    ctx->web_server = NULL;
    xSemaphoreGive(ctx->state_mtx);

    if (server != NULL)
    {
        esp_err_t err = httpd_stop(server);
        if (err != ESP_OK)
            ESP_LOGW(TAG, "httpd_stop failed: 0x%x", err);
        else
            ESP_LOGI(TAG, "Web server stopped");
    }
}
