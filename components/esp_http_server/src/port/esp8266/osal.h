/*
 * SPDX-FileCopyrightText: 2018-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * ESP8266 RTOS SDK adaptation of OS abstraction layer.
 * Replaces ESP32-specific xTaskCreatePinnedToCoreWithCaps/vTaskDeleteWithCaps
 * with standard FreeRTOS xTaskCreate/vTaskDelete (ESP8266 is single-core).
 */

#ifndef _OSAL_H_
#define _OSAL_H_

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <unistd.h>
#include <stdint.h>
#include <esp_timer.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OS_SUCCESS ESP_OK
#define OS_FAIL    ESP_FAIL

typedef TaskHandle_t othread_t;

/**
 * @brief Create a new HTTP server thread.
 *
 * On ESP8266 (single-core), the core_id and caps parameters are ignored.
 * Uses standard xTaskCreate() instead of xTaskCreatePinnedToCoreWithCaps().
 */
static inline int httpd_os_thread_create(othread_t *thread,
                                 const char *name, uint16_t stacksize, int prio,
                                 void (*thread_routine)(void *arg), void *arg,
                                 BaseType_t core_id, uint32_t caps)
{
    (void)core_id;
    (void)caps;
    int ret = xTaskCreate(thread_routine, name, stacksize, arg, prio, thread);
    if (ret == pdPASS) {
        return OS_SUCCESS;
    }
    return OS_FAIL;
}

/* Only self delete is supported */
static inline void httpd_os_thread_delete(void)
{
    vTaskDelete(xTaskGetCurrentTaskHandle());
}

static inline void httpd_os_thread_sleep(int msecs)
{
    vTaskDelay(msecs / portTICK_PERIOD_MS);
}

static inline othread_t httpd_os_thread_handle(void)
{
    return xTaskGetCurrentTaskHandle();
}

#ifdef __cplusplus
}
#endif

#endif /* ! _OSAL_H_ */
