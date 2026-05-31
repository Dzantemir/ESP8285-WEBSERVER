#ifndef SMART_AP_TYPES_H
#define SMART_AP_TYPES_H

// ============================================================================
// TYPE DEFINITIONS — enums, structs, context pointer
// Minimal header: only includes what's needed for type definitions.
// Each .c file includes its own system headers as needed.
// ============================================================================

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "esp_http_server.h"
#include "esp_log.h"

// ============================================================================
// LOG TAG — shared across all modules
// ============================================================================

#define TAG "SMART_AP"

// ============================================================================
// SAFE SECONDS-TO-TICKS CONVERSION
// ============================================================================

#ifndef pdSEC_TO_TICKS
#define pdSEC_TO_TICKS(xTimeInSec)           \
    ((TickType_t)((TickType_t)(xTimeInSec) * \
                  (TickType_t)configTICK_RATE_HZ))
#endif

// ============================================================================
// EVENT GROUP BITS
// ============================================================================

#define EVT_SHUTDOWN_BIT (1U << 0)
#define EVT_DNS_RUNNING_BIT (1U << 1)
#define EVT_BEEP_DONE_BIT (1U << 3)
#define EVT_AP_DONE_BIT (1U << 4)
#define EVT_BATT_DONE_BIT (1U << 5)
#define EVT_SCAN_DONE_BIT (1U << 6)
#define EVT_SERVERS_RUNNING_BIT (1U << 7)
#define EVT_SERVERS_STOPPED_BIT (1U << 8)
#define EVT_ORCHESTRATOR_DONE_BIT (1U << 9)
#define EVT_DNS_STOPPED_BIT (1U << 10)
#define EVT_RESET_DONE_BIT (1U << 11)

// ============================================================================
// SERVER ORCHESTRATOR COMMANDS
// ============================================================================

typedef enum
{
    SERVER_CMD_START,
    SERVER_CMD_STOP,
    SERVER_CMD_SHUTDOWN,
    SERVER_CMD_DNS_ERROR
} server_cmd_t;

typedef struct
{
    server_cmd_t cmd;
} server_cmd_msg_t;

// ============================================================================
// BEEP COMMANDS
// ============================================================================

typedef enum
{
    BEEP_CMD_START = 1
} beep_cmd_t;

// ============================================================================
// SHARED SYSTEM CONTEXT — replaces all global variables
// ============================================================================

typedef struct
{
    EventGroupHandle_t events;
    SemaphoreHandle_t state_mtx;
    SemaphoreHandle_t shutdown_sem;
    httpd_handle_t web_server;
    int dns_socket;
    TaskHandle_t dns_task;
    SemaphoreHandle_t beep_sem;
    QueueHandle_t beep_queue;
    QueueHandle_t server_cmd_queue;
    volatile uint32_t ap_ip_addr;
} sys_ctx_t;

// Global context pointer — defined in main.c, used by all modules
extern sys_ctx_t *ctx;

// ============================================================================
// CTX VALIDATION MACROS (Bug #8 — protection against use before init)
// ============================================================================

// Use CTX_CHECK() in void functions
#define CTX_CHECK()                                      \
    do                                                   \
    {                                                    \
        if (ctx == NULL)                                 \
        {                                                \
            ESP_LOGE(TAG, "FATAL: ctx is NULL!");        \
            return;                                      \
        }                                                \
    } while (0)

// Use CTX_CHECK_RET(retval) in functions that return a value
#define CTX_CHECK_RET(retval)                            \
    do                                                   \
    {                                                    \
        if (ctx == NULL)                                 \
        {                                                \
            ESP_LOGE(TAG, "FATAL: ctx is NULL!");        \
            return (retval);                             \
        }                                                \
    } while (0)

#endif // SMART_AP_TYPES_H
