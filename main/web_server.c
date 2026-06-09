// Web server module — MODERN STYLE using the local esp_http_server component.
//
// Architecture: single HTTP_ANY /* handler — one entry point for ALL requests.
//   - GET  + valid file path  → serve file from SPIFFS / SD
//   - GET  + captive detect   → 302 redirect to /
//   - GET  + captive page     → index.html
//   - HEAD + any path         → 200 OK, no body, Connection: close
//                               (phone connectivity check — MUST NOT redirect,
//                                otherwise phone loops HEAD → 302 → HEAD → 302)
//   - POST /beep              → beep_handler (delegate)
//   - POST anything else      → drain body + 302 close
//   - PUT/DELETE/PATCH etc.   → drain body + 302 close
//
// CRITICAL: All handlers return ESP_FAIL to force IMMEDIATE socket closure.
// The framework uses the handler's return value to decide socket fate:
//   ESP_OK  → socket stays open for keep-alive (waits for next request)
//   ESP_FAIL → httpd_sess_delete() → close(fd) → socket freed instantly
// We set Connection: close in every response, but the framework does NOT
// interpret that header — it only looks at the return value. With ESP_OK,
// sockets stay open until recv timeout (5 sec), which causes exhaustion
// when the phone floods 60+ HEAD/POST requests in 3 seconds.
//
// SO_LINGER is DISABLED to prevent close() from blocking the single-threaded
// server for up to 2 seconds per socket. Without linger, close() returns
// immediately and the OS handles FIN delivery in the background.
//
// Parse error handlers (400,408,411,413,414,431) return ESP_FAIL without
// sending a response — the socket may be in a bad state after parse failure,
// so trying to send a response is risky and pointless for malformed requests.
//
// Side effect: framework logs "uri handler execution failed" for every
// request because we return ESP_FAIL. This is cosmetic, not a real error.
//
// Idempotent: repeated calls to stop_webserver are safe (web_server==NULL check)
// Supports: SPIFFS only, SD only, SPIFFS + SD (dual mode)
// In dual mode: captive portal from SPIFFS, media files from SD,
//               SD takes priority for duplicate file names.

#include "smart_ap_common.h"

#include <string.h>
#include <stdbool.h>
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
// HELPERS
// ============================================================================

/**
 * @brief Send 302 redirect to / with Connection: close, then force socket closure.
 *
 * Used for captive portal redirects and "go away" responses.
 *
 * CRITICAL: Returns ESP_FAIL to force the framework to close the socket IMMEDIATELY.
 * Returning ESP_OK would keep the socket open for keep-alive, even though we
 * sent Connection: close — the framework does NOT interpret that header to decide
 * whether to close the socket. It only looks at the handler's return value:
 *   ESP_OK  → socket stays open (keep-alive)
 *   ESP_FAIL → httpd_sess_delete() → close(fd) → socket freed
 *
 * On ESP8285 with 4-7 sockets and phones flooding HEAD/POST requests,
 * we MUST close sockets immediately after every response to prevent exhaustion.
 */
static esp_err_t send_302_close(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
    httpd_resp_set_type(req, HTTPD_TYPE_TEXT);
    httpd_resp_send(req, NULL, 0);
    return ESP_FAIL;  // Force socket closure — MUST NOT return ESP_OK
}

// ============================================================================
// PATH VALIDATION — prevents path traversal attacks
// ============================================================================

/**
 * @brief Validate filepath before passing to fopen().
 *
 * Security checks:
 *   1. Must start with '/' (absolute path)
 *   2. No ".." anywhere — blocks traversal: /spiffs/../../etc/passwd
 *   3. Must start with an allowed mount point (/spiffs/ or /sdcard/)
 *
 * NOTE: query string must be stripped BEFORE calling this function,
 * otherwise paths like /api/v1/domain/list?terminalType=... will be
 * rejected because they don't start with /spiffs/ or /sdcard/.
 */
static bool is_path_safe(const char *path)
{
    if (path == NULL || path[0] != '/')
        return false;

    // Reject ".." anywhere in path (directory traversal)
    if (strstr(path, "..") != NULL)
        return false;

    // Path must start with one of the allowed VFS mount points
    bool allowed = false;

#if STORAGE_USES_SPIFFS
    {
        const size_t prefix_len = strlen(SPIFFS_BASE_PATH);
        if (strncmp(path, SPIFFS_BASE_PATH, prefix_len) == 0 &&
            (path[prefix_len] == '/' || path[prefix_len] == '\0'))
            allowed = true;
    }
#endif

#if STORAGE_USES_SD
    {
        const size_t prefix_len = strlen(SD_BASE_PATH);
        if (strncmp(path, SD_BASE_PATH, prefix_len) == 0 &&
            (path[prefix_len] == '/' || path[prefix_len] == '\0'))
            allowed = true;
    }
#endif

    return allowed;
}

// ============================================================================
// SINGLE HTTP_ANY WILDCARD HANDLER
//
// One handler to rule them all. HTTP_ANY /* matches EVERY method + path.
// Dispatch logic:
//
//   POST /beep              → beep_handler (delegate)
//   HEAD + any path         → 200 OK, no body, Connection: close
//                              (MUST NOT redirect — phone loops HEAD→302→HEAD→302)
//   non-GET/non-HEAD        → drain body + 302 close
//   GET + captive detect URI→ 302 redirect to /
//   GET + captive page      → index.html
//   GET + valid path        → serve file from SPIFFS / SD
//   GET + bad path          → 302 close (is_path_safe failed)
//   GET + no file           → 302 close (file not found)
// ============================================================================

static esp_err_t main_handler(httpd_req_t *req)
{
    // ================================================================
    // 1. POST /beep → delegate to beep_handler
    //    Ignore beep_handler's return value — we ALWAYS close the socket
    //    after the response (ESP_FAIL) to prevent socket exhaustion.
    // ================================================================
    if (req->method == HTTP_POST && strcmp(req->uri, "/beep") == 0)
    {
        beep_handler(req);
        return ESP_FAIL;  // Force socket closure regardless of beep_handler result
    }

    // ================================================================
    // 2. HEAD → 200 OK, Connection: close, no body
    //
    //    Phones use HEAD / to check if the server is alive.
    //    MUST NOT send 302 redirect — the phone will follow it and
    //    send another HEAD /, creating an infinite loop:
    //      HEAD / → 302 / → HEAD / → 302 / → ... (60+ in 3 sec!)
    //    A simple 200 OK tells the phone "I'm here".
    //
    //    Returns ESP_FAIL to force socket closure. ESP_OK would keep
    //    the socket open for keep-alive, and with 60+ rapid HEAD
    //    requests from the phone, sockets pile up → server hang.
    // ================================================================
    if (req->method == HTTP_HEAD)
    {
        httpd_resp_set_hdr(req, "Connection", "close");
        httpd_resp_send(req, NULL, 0);
        return ESP_FAIL;  // Force socket closure — MUST NOT return ESP_OK
    }

    // ================================================================
    // 3. Non-GET methods (POST without /beep, PUT, DELETE, PATCH...)
    //    → 302 close (skip drain)
    //    Phones send background POST/PUT/DELETE to random URIs.
    //    We send 302 + return ESP_FAIL → socket closed → kernel discards
    //    unread body bytes automatically. No need to drain — drain is
    //    only needed for keep-alive (ESP_OK), where leftover body would
    //    corrupt the next request on the same socket.
    // ================================================================
    if (req->method != HTTP_GET)
    {
        ESP_LOGI(TAG, "%s %s → 302 (body %d bytes, skip drain)",
                 http_method_str(req->method), req->uri, req->content_len);
        return send_302_close(req);
    }

    // ================================================================
    // 4. GET — captive portal detection URIs → 302 redirect to /
    //    Returns ESP_FAIL to force socket closure.
    // ================================================================
    char filepath[CONFIG_SMART_AP_MAX_PATH_LEN];

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
            return ESP_FAIL;  // Force socket closure
        }
    }

    // ================================================================
    // 5. GET — captive portal pages → index.html
    // ================================================================
    static const char *captive_page_uris[] = {"/", "/?", "/portal", "/portal?"};
    for (int i = 0; i < sizeof(captive_page_uris) / sizeof(captive_page_uris[0]); i++)
    {
        if (strcmp(req->uri, captive_page_uris[i]) == 0)
        {
            snprintf(filepath, sizeof(filepath), "%s/index.html", storage_get_captive_path());
            goto serve_file;
        }
    }

    // ================================================================
    // 6. GET — regular file: URI as VFS path
    // ================================================================
    strlcpy(filepath, req->uri, sizeof(filepath));

    // Strip query string BEFORE is_path_safe()
    {
        char *qs = strchr(filepath, '?');
        if (qs) *qs = '\0';
    }

    // SECURITY: validate filepath before opening
    if (!is_path_safe(filepath))
    {
        ESP_LOGW(TAG, "Path rejected (traversal or invalid mount): %s", req->uri);
        return send_302_close(req);
    }

serve_file:;

    FILE *fd = fopen(filepath, "rb");

    // --- Fallback in dual mode: if file not found on primary, try secondary ---
    if (!fd)
    {
        const char *secondary = storage_get_secondary_path();
        if (secondary != NULL)
        {
            const char *primary = storage_get_primary_path();
            size_t primary_len = strlen(primary);
            if (strncmp(filepath, primary, primary_len) == 0)
            {
                char alt_path[CONFIG_SMART_AP_MAX_PATH_LEN];
                snprintf(alt_path, sizeof(alt_path), "%s%s", secondary, filepath + primary_len);
                if (is_path_safe(alt_path))
                {
                    fd = fopen(alt_path, "rb");
                    if (fd)
                        strlcpy(filepath, alt_path, sizeof(filepath));
                }
            }
        }
    }

    if (!fd)
    {
        // File not found → redirect to / for captive portal
        // send_302_close returns ESP_FAIL → socket closed
        return send_302_close(req);
    }

    // ================================================================
    // 7. Serve file
    // ================================================================
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

    // ALWAYS return ESP_FAIL to force socket closure.
    // Even for successful file sends, we set Connection: close,
    // so there's no point keeping the socket open for keep-alive.
    // On ESP8285 with 4-7 sockets, keeping sockets open risks
    // exhaustion when the phone floods HEAD/POST requests.
    // Side effect: framework logs "uri handler execution failed"
    // for every request — this is cosmetic, not a real error.
    return ESP_FAIL;
}

// ============================================================================
// ERROR HANDLERS
//
// Parse-level errors (400, 408, 411, 413, 414, 431) happen BEFORE our
// main_handler runs. The framework's default error handler:
//   1. Sends error response (no Connection: close)
//   2. Does NOT drain the request body
//   3. Returns ESP_FAIL → socket closed, but body data left in TCP buffer
//      can corrupt the next connection if the same socket fd is reused
//
// Our custom handlers:
//   1. Try to send 302 redirect + Connection: close
//   2. Return ESP_FAIL to force immediate socket closure
//   3. Socket close discards any remaining TCP buffer data
//
// Safety-net 404/405 handlers should never fire with HTTP_ANY /*,
// but are registered as defense in depth.
// ============================================================================

/**
 * @brief Generic parse error handler — force socket closure.
 *
 * Used for: 400 (bad request syntax), 408 (timeout), 411 (length required),
 * 413 (content too large), 414 (URI too long), 431 (headers too large).
 *
 * These errors occur BEFORE our main_handler runs — the HTTP parser
 * failed on a malformed/incomplete request.
 *
 * Does NOT try to send a response because:
 *   - The request may be only partially parsed (uri/method may be invalid)
 *   - httpd_resp_send() on a partially parsed request can fail or behave
 *     unexpectedly
 *   - The socket is being closed anyway (ESP_FAIL) — the client will
 *     see TCP RST/FIN, which is sufficient for a malformed request
 *   - Sending a 302 redirect to a client that can't even format a valid
 *     HTTP request is pointless
 *
 * Returns ESP_FAIL to force immediate socket closure via httpd_sess_delete().
 */
static esp_err_t parse_err_handler(httpd_req_t *req, httpd_err_code_t err)
{
    ESP_LOGW(TAG, "Parse error %d → force close", err);
    // Do NOT try to send a response — socket may be in a bad state.
    // Just return ESP_FAIL to force socket closure.
    return ESP_FAIL;
}

/** 404 safety-net — should never fire with HTTP_ANY  */
static esp_err_t custom_404_handler(httpd_req_t *req, httpd_err_code_t err)
{
    ESP_LOGW(TAG, "404 fallback (should not happen): %s", req->uri);
    return send_302_close(req);
}

/** 405 safety-net — should never fire with HTTP_ANY  */
static esp_err_t custom_405_handler(httpd_req_t *req, httpd_err_code_t err)
{
    ESP_LOGW(TAG, "405 fallback (should not happen): %s %s (body %d bytes)",
             http_method_str(req->method), req->uri, req->content_len);
    // No drain needed — send_302_close returns ESP_FAIL → socket closed
    // → kernel discards unread body automatically
    return send_302_close(req);
}

// ============================================================================
// START WEBSERVER
//
// Single HTTP_ANY /* handler + error handlers for robustness.
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

    // Wildcard URI matching — single handler replaces per-file registration
    config.uri_match_fn = httpd_uri_match_wildcard;

    // Only 1 URI handler (HTTP_ANY /*) + error handlers
    config.max_uri_handlers = 4;

    config.stack_size = CONFIG_SMART_AP_HTTPD_TASK_STACK;
    config.task_priority = CONFIG_SMART_AP_HTTPD_TASK_PRIORITY;
    config.max_open_sockets = CONFIG_SMART_AP_HTTPD_MAX_OPEN_SOCKETS;

    // TCP keep-alive: detect dead connections (TCP-level probes, not HTTP keep-alive)
    config.keep_alive_enable = true;
    config.keep_alive_idle = 5;
    config.keep_alive_interval = 5;
    config.keep_alive_count = 3;

    // SO_LINGER: DISABLED.
    // With linger_timeout=2, every close() blocked the server thread for up to
    // 2 seconds. With many rapid connections from the phone, this caused the
    // single-threaded server to stall: 7 sockets × 2 sec = 14 sec of blocking.
    // Without SO_LINGER, close() returns immediately and the OS handles the
    // FIN in the background — fast and safe.
    config.enable_so_linger = false;

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

    // --- Single HTTP_ANY /* handler — catches ALL methods + ALL paths ---
    httpd_uri_t any_wildcard = {
        .uri = "/*",
        .method = HTTP_ANY,
        .handler = main_handler,
        .user_ctx = NULL,
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &any_wildcard));
    ESP_LOGI(TAG, "Registered HTTP_ANY /* - main_handler");

    // --- Parse error handlers (force socket close) ---
    httpd_register_err_handler(server, HTTPD_400_BAD_REQUEST,            parse_err_handler);
    httpd_register_err_handler(server, HTTPD_408_REQ_TIMEOUT,            parse_err_handler);
    httpd_register_err_handler(server, HTTPD_411_LENGTH_REQUIRED,        parse_err_handler);
    httpd_register_err_handler(server, HTTPD_413_CONTENT_TOO_LARGE,      parse_err_handler);
    httpd_register_err_handler(server, HTTPD_414_URI_TOO_LONG,           parse_err_handler);
    httpd_register_err_handler(server, HTTPD_431_REQ_HDR_FIELDS_TOO_LARGE, parse_err_handler);
    ESP_LOGI(TAG, "Registered parse error handlers (400,408,411,413,414,431)");

    // --- Safety-net 404/405 handlers (should never fire with HTTP_ANY /*) ---
    httpd_register_err_handler(server, HTTPD_404_NOT_FOUND,      custom_404_handler);
    httpd_register_err_handler(server, HTTPD_405_METHOD_NOT_ALLOWED, custom_405_handler);
    ESP_LOGI(TAG, "Registered safety-net 404 + 405 handlers");

    ESP_LOGI(TAG, "Web server started (HTTP_ANY wildcard, all sockets force-close, no linger; SPIFFS=%d, SD=%d)",
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
