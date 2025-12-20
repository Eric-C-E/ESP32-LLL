#include "app_tasks.h"

#include <stdlib.h>

#include "esp_heap_caps.h"

#include "esp_lcd_gc9a01.h"

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io_interface.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "driver/spi_master.h"
#include "driver/gpio.h"

#define LCD_HOST SPI2_HOST
#define LCD_H_RES 240
#define LCD_V_RES 240
#define LCD_PIXEL_CLOCK_HZ (40 * 1000 * 1000)

#define PIN_NUM_MOSI 11
#define PIN_NUM_SCLK 12
#define PIN_NUM_CS 10
#define PIN_NUM_DC 8
#define PIN_NUM_RST -1
#define PIN_NUM_MISO -1

static const char *TAG = "display_task";
static SemaphoreHandle_t s_flush_sem = NULL;

#define LCD_INVERT_COLORS true

static inline uint16_t lcd_swap_color(uint16_t color)
{
    return (uint16_t)((color << 8) | (color >> 8));
}

static bool lcd_flush_ready(esp_lcd_panel_io_handle_t panel_io,
                            esp_lcd_panel_io_event_data_t *edata,
                            void *user_ctx)
{
    (void)panel_io;
    (void)edata;
    BaseType_t high_task_woken = pdFALSE;
    xSemaphoreGiveFromISR((SemaphoreHandle_t)user_ctx, &high_task_woken);
    return high_task_woken == pdTRUE;
}

static void display_task(void *arg)
{
    (void)arg;

    spi_bus_config_t buscfg = {
        .sclk_io_num = PIN_NUM_SCLK,
        .mosi_io_num = PIN_NUM_MOSI,
        .miso_io_num = PIN_NUM_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * 40 * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    s_flush_sem = xSemaphoreCreateBinary();
    if (!s_flush_sem) {
        ESP_LOGE(TAG, "Failed to create flush semaphore");
        vTaskDelay(portMAX_DELAY);
        return;
    }

    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = PIN_NUM_DC,
        .cs_gpio_num = PIN_NUM_CS,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 1,
        .on_color_trans_done = lcd_flush_ready,
        .user_ctx = s_flush_sem,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(LCD_HOST, &io_config, &io_handle));

    esp_lcd_panel_handle_t panel_handle = NULL;
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = PIN_NUM_RST,
        #if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 0, 0)
            .color_space = ESP_LCD_COLOR_SPACE_BGR,
        #elif ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(6, 0, 0)
            .rgb_endian = LCD_RGB_ENDIAN_BGR,
        #else
            .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        #endif
            .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_gc9a01(io_handle, &panel_config, &panel_handle));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_handle, LCD_INVERT_COLORS));

    const int lines = 40;
    const size_t buf_size = LCD_H_RES * lines * 2;
    uint8_t *buf = heap_caps_malloc(buf_size, MALLOC_CAP_DMA);
    if (!buf) {
        ESP_LOGE(TAG, "Failed to allocate draw buffer");
        vTaskDelay(portMAX_DELAY);
        return;
    }

    for (int y = 0; y < LCD_V_RES; y++) {
        for (int x = 0; x < LCD_H_RES; x++) {
            uint16_t color = (x == LCD_H_RES / 2 || y == LCD_V_RES / 2) ? 0xF800 : 0x0000;
            ((uint16_t *)buf)[(y % lines) * LCD_H_RES + x] = lcd_swap_color(color);
        }
        if ((y + 1) % lines == 0) {
            xSemaphoreTake(s_flush_sem, 0);
            ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(panel_handle, 0, y + 1 - lines, LCD_H_RES, y + 1, buf));
            xSemaphoreTake(s_flush_sem, portMAX_DELAY);
        }
    }
    int remaining_lines = LCD_V_RES % lines;
    if (remaining_lines) {
        xSemaphoreTake(s_flush_sem, 0);
        ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(panel_handle, 0, LCD_V_RES - remaining_lines, LCD_H_RES, LCD_V_RES, buf));
        xSemaphoreTake(s_flush_sem, portMAX_DELAY);
    }

    vTaskDelay(pdMS_TO_TICKS(5000));

    for (int y = 0; y < LCD_V_RES; y++) {
        for (int x = 0; x < LCD_H_RES; x++) {
            uint8_t r = (x * 31) / (LCD_H_RES - 1);
            uint8_t g = (y * 63) / (LCD_V_RES - 1);
            uint8_t b = ((LCD_H_RES - 1 - x) * 31) / (LCD_H_RES - 1);
            uint16_t color = (r << 11) | (g << 5) | b;
            ((uint16_t *)buf)[(y % lines) * LCD_H_RES + x] = lcd_swap_color(color);
        }
        if ((y + 1) % lines == 0) {
            xSemaphoreTake(s_flush_sem, 0);
            ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(panel_handle, 0, y + 1 - lines, LCD_H_RES, y + 1, buf));
            xSemaphoreTake(s_flush_sem, portMAX_DELAY);
        }
    }
    remaining_lines = LCD_V_RES % lines;
    if (remaining_lines) {
        xSemaphoreTake(s_flush_sem, 0);
        ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(panel_handle, 0, LCD_V_RES - remaining_lines, LCD_H_RES, LCD_V_RES, buf));
        xSemaphoreTake(s_flush_sem, portMAX_DELAY);
    }

    free(buf);
    ESP_LOGI(TAG, "Display init done");

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void app_start_display_task(void)
{
    xTaskCreate(display_task, "display_task", 4096, NULL, 8, NULL);
}
