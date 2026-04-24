/*
 * SPDX-FileCopyrightText: 2024-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "unity.h"
#include "memory_checks.h"

#if CONFIG_HEAP_TRACING
#include "esp_heap_trace.h"
#define NUM_RECORDS 100
static heap_trace_record_t trace_record[NUM_RECORDS];
#endif

/* setUp runs before every test */
void setUp(void)
{
#if CONFIG_HEAP_TRACING
    heap_trace_init_standalone(trace_record, NUM_RECORDS);
    heap_trace_start(HEAP_TRACE_LEAKS);
#endif

    test_utils_record_free_mem();
    test_utils_set_leak_level(CONFIG_UNITY_CRITICAL_LEAK_LEVEL_GENERAL, ESP_LEAK_TYPE_CRITICAL, ESP_COMP_LEAK_GENERAL);
    test_utils_set_leak_level(CONFIG_UNITY_WARN_LEAK_LEVEL_GENERAL, ESP_LEAK_TYPE_WARNING, ESP_COMP_LEAK_GENERAL);
    test_utils_set_leak_level(0, ESP_LEAK_TYPE_CRITICAL, ESP_COMP_LEAK_LWIP);
}

/* tearDown runs after every test */
void tearDown(void)
{
    /* some FreeRTOS stuff is cleaned up by idle task */
    vTaskDelay(5);

    /* clean up some of the newlib's lazy allocations */
    esp_reent_cleanup();

#if CONFIG_HEAP_TRACING
    heap_trace_stop();
#endif

    /* check if unit test has caused heap corruption in any heap */
    if (!heap_caps_check_integrity_all(true)) {
#if CONFIG_HEAP_TRACING
        heap_trace_dump();
#endif
        TEST_FAIL_MESSAGE("The test has corrupted the heap");
    }

    test_utils_finish_and_evaluate_leaks(test_utils_get_leak_level(ESP_LEAK_TYPE_WARNING, ESP_COMP_LEAK_ALL),
                                         test_utils_get_leak_level(ESP_LEAK_TYPE_CRITICAL, ESP_COMP_LEAK_ALL));

}

static void test_task(void *pvParameters)
{
    vTaskDelay(10); /* Delay a bit to let the main task be deleted */
    unity_run_menu();
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    xTaskCreatePinnedToCore(test_task, "testTask", CONFIG_UNITY_FREERTOS_STACK_SIZE, NULL, CONFIG_UNITY_FREERTOS_PRIORITY, NULL, CONFIG_UNITY_FREERTOS_CPU);
}
