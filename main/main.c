#include "nvs_flash.h"

#include "esp_log.h"
#include "esp_heap_caps.h"

#include "app_runtime.h"
#include "app_display.h"
#include "unity.h"
#include "unity_test_runner.h"

static const char *TAG = "app_main";

static void log_heap_caps(const char *label)
{
    size_t free_8bit = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    size_t free_32bit = heap_caps_get_free_size(MALLOC_CAP_32BIT);
    size_t free_dma = heap_caps_get_free_size(MALLOC_CAP_DMA);
    ESP_LOGI(TAG, "%s: free 8BIT=%u, 32BIT=%u, DMA=%u", label, free_8bit, free_32bit, free_dma);
}

TEST_CASE("Test main LVGL port", "[lvgl port]")
{
    size_t start_freemem_8bit = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    size_t start_freemem_32bit = heap_caps_get_free_size(MALLOC_CAP_32BIT);

    ESP_LOGI(TAG, "Initiliaze LCD.");
    log_heap_caps("heap before lcd_init");

    /* LCD HW initialization */
    TEST_ASSERT_EQUAL(app_lcd_init(), ESP_OK);
    log_heap_caps("heap after lcd_init");

    size_t start_lvgl_freemem_8bit = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    size_t start_lvgl_freemem_32bit = heap_caps_get_free_size(MALLOC_CAP_32BIT);

    ESP_LOGI(TAG, "Initilize LVGL.");
    log_heap_caps("heap before lvgl_init");

    /* LVGL initialization */
    TEST_ASSERT_EQUAL(app_lvgl_init(), ESP_OK);
    log_heap_caps("heap after lvgl_init");

    /* Show LVGL objects */
    app_main_display();
    log_heap_caps("heap after app_main_display");

    vTaskDelay(5000 / portTICK_PERIOD_MS);
    log_heap_caps("heap after display delay");

    /* LVGL deinit */
    TEST_ASSERT_EQUAL(app_lvgl_deinit(), ESP_OK);
    log_heap_caps("heap after lvgl_deinit");

    /* When using LVGL8, it takes some time to release all memory */
    vTaskDelay(1000 / portTICK_PERIOD_MS);
    log_heap_caps("heap after lvgl_deinit delay");

    ESP_LOGI(TAG, "LVGL deinitialized.");

    size_t end_lvgl_freemem_8bit = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    size_t end_lvgl_freemem_32bit = heap_caps_get_free_size(MALLOC_CAP_32BIT);


    /* LCD deinit */
    TEST_ASSERT_EQUAL(app_lcd_deinit(), ESP_OK);
    log_heap_caps("heap after lcd_deinit");

    vTaskDelay(1000 / portTICK_PERIOD_MS);
    log_heap_caps("heap after lcd_deinit delay");

    ESP_LOGI(TAG, "LCD deinitilized.");

    size_t end_freemem_8bit = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    size_t end_freemem_32bit = heap_caps_get_free_size(MALLOC_CAP_32BIT);

    check_leak(start_lvgl_freemem_8bit, end_lvgl_freemem_8bit, "8BIT LVGL");
    check_leak(start_lvgl_freemem_32bit, end_lvgl_freemem_32bit, "32BIT LVGL");

    check_leak(start_freemem_8bit, end_freemem_8bit, "8BIT");
    check_leak(start_freemem_32bit, end_freemem_32bit, "32BIT");

    }

//unity test case to be called from main.


void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(err);
    }

    app_runtime_init();

    unity_run_menu();
}
