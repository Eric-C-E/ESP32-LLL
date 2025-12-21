#include "app_display.h"
#include "app_tasks.h"
#include <stdlib.h>

#include "esp_err.h"
#include "esp_check.h"
#include "esp_heap_caps.h"

#include "esp_lcd_gc9a01.h"

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "driver/spi_master.h"
#include "driver/gpio.h"

#include "esp_lvgl_port.h"

//LCD SPI host and size
#define LCD_HOST SPI2_HOST
#define LCD_H_RES 240
#define LCD_V_RES 240
#define LCD_PIXEL_CLOCK_HZ (40 * 1000 * 1000)
#define LCD_DRAW_BUF_HEIGHT 40
#define LCD_DRAW_BUF_DOUBLE 1
#define LCD_CMD_BITS 8
#define LCD_PARAM_BITS 8
#define LCD_BITS_PER_PIXEL 16

//LCD PIN definitions
#define PIN_NUM_MOSI 11
#define PIN_NUM_SCLK 12
#define PIN_NUM_CS 10
#define PIN_NUM_DC 8
#define PIN_NUM_RST -1
#define PIN_NUM_MISO -1

static const char *TAG = "display_task";

#define LCD_INVERT_COLORS false
//lcd panel Io
static esp_lcd_panel_io_handle_t io_handle = NULL;
static esp_lcd_panel_handle_t panel_handle = NULL;
//lvgl display handle
static lv_display_t *lvgl_disp = NULL;

esp_err_t app_lcd_init(void)
{
    esp_err_t ret = ESP_OK;

    ESP_LOGD(TAG, "Initialize SPI bus");
    const spi_bus_config_t buscfg = {
        .miso_io_num = PIN_NUM_MISO,
        .mosi_io_num = PIN_NUM_MOSI,
        .sclk_io_num = PIN_NUM_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * LCD_DRAW_BUF_HEIGHT * sizeof(uint16_t),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO), TAG, "Failed to initialize SPI bus");

    ESP_LOGD(TAG, "Install panel IO");
    const esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = PIN_NUM_DC,
        .cs_gpio_num = PIN_NUM_CS,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &io_handle), err, TAG, "Failed to install panel IO");

    ESP_LOGD(TAG, "Install LCD panel driver");
    const esp_lcd_panel_dev_config_t panel_config = {
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
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_gc9a01(io_handle, &panel_config, &panel_handle), err, TAG, "New panel failed");
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_handle, LCD_INVERT_COLORS));

    return ret;

    err:
        if (panel_handle) {
            esp_lcd_panel_del(panel_handle);
            panel_handle = NULL;
        }
        if (io_handle) {
            esp_lcd_panel_io_del(io_handle);
            io_handle = NULL;
        }
        spi_bus_free(LCD_HOST);

    return ret;
}

esp_err_t app_lcd_deinit(void)
{
    esp_err_t ret = ESP_OK;
    ESP_RETURN_ON_ERROR(esp_lcd_panel_del(panel_handle), TAG, "LCD panel de-initialization failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_del(io_handle), TAG, "LCD panel IO de-initialization failed");
    ESP_RETURN_ON_ERROR(spi_bus_free(LCD_HOST), TAG, "SPI bus de-initialization failed");
    return ret;
}

esp_err_t app_lvgl_init(void)
{
    const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_RETURN_ON_ERROR(lvgl_port_init(&lvgl_cfg), TAG, "Failed to initialize LVGL port");
    
    /* Add LCD Screen */
    ESP_LOGD(TAG, "Add LCD screen to LVGL");
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io_handle,
        .panel_handle = panel_handle,
        .buffer_size = LCD_H_RES * LCD_DRAW_BUF_HEIGHT * sizeof(uint16_t),
        .double_buffer = LCD_DRAW_BUF_DOUBLE,
        .hres = LCD_H_RES,
        .vres = LCD_V_RES,
        .monochrome = false,
        //rotations
        .rotation = {
            .swap_xy = false,
            .mirror_x = false,
            .mirror_y = false,
        },
        .flags = {
            .buff_dma = true,
            #if LVGL_VERSION_MAJOR >= 9
            .swap_bytes = true,
            #endif
        }
    };
    lvgl_disp = lvgl_port_add_disp(&disp_cfg);

    return ESP_OK;
}

esp_err_t app_lvgl_deinit(void)
{
    ESP_RETURN_ON_ERROR(lvgl_port_remove_disp(lvgl_disp), TAG, "LVGL disp removing failed");
    ESP_RETURN_ON_ERROR(lvgl_port_deinit(), TAG, "LVGL deinit failed");

    return ESP_OK;
}

void display_task(void *arg)
{   
    lvgl_port_lock(0);
    lv_obj_t *scr = lv_scr_act();
    lv_obj_t *dot = NULL;
    const int16_t dot_size = 10;
    const int16_t y = (LCD_V_RES - dot_size) / 2;
    const int16_t min_x = 0;
    const int16_t max_x = LCD_H_RES - dot_size;
    int16_t x = min_x;
    int16_t dir = 2;

    dot = lv_obj_create(scr);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, dot_size, dot_size);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(0x00FF80), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_pos(dot, x, y);
    ESP_LOGI(TAG, "display task running");
    lvgl_port_unlock();

    while (1) {
        lvgl_port_lock(0);
        x += dir;
        if (x >= max_x) {
            x = max_x;
            dir = -dir;
        } else if (x <= min_x) {
            x = min_x;
            dir = -dir;
        }
        lv_obj_set_x(dot, x);
        lvgl_port_unlock();
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

void display_make_tasks(void)
{
    ESP_ERROR_CHECK(app_lcd_init());
    ESP_ERROR_CHECK(app_lvgl_init());
    xTaskCreate(display_task, "display_task", 4096, NULL, 5, NULL);
}
