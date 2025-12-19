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

    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = PIN_NUM_DC,
        .cs_gpio_num = PIN_NUM_CS,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(LCD_HOST, &io_config, &io_handle));

    esp_lcd_panel_handle_t panel_handle = NULL;
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = PIN_NUM_RST,
        #if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 0, 0)
            .color_space = ESP_LCD_COLOR_SPACE_RGB,
        #elif ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(6, 0, 0)
            .rgb_endian = LCD_RGB_ENDIAN_RGB,
        #else
            .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        #endif
            .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_gc9a01(io_handle, &panel_config, &panel_handle));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

    const int lines = 40;
    const size_t buf_size = LCD_H_RES * lines * 2;
    uint8_t *buf = heap_caps_malloc(buf_size, MALLOC_CAP_DMA);
    if (!buf) {
        ESP_LOGE(TAG, "Failed to allocate draw buffer");
        vTaskDelay(portMAX_DELAY);
        return;
    }

    const uint16_t red = 0xF800;
    for (size_t i = 0; i < buf_size; i += 2) {
        buf[i + 0] = (red >> 8) & 0xFF;
        buf[i + 1] = (red >> 0) & 0xFF;
    }

    for (int y = 0; y < LCD_V_RES; y += lines) {
        int y_end = y + lines;
        if (y_end > LCD_V_RES) {
            y_end = LCD_V_RES;
        }
        ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(panel_handle, 0, y, LCD_H_RES, y_end, buf));
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
