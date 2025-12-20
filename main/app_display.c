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

#include "unity.h"
#include "unity_test_runner.h"

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

//current main display function

void app_main_display(void)
{
    lv_obj_t *scr = lv_scr_act();

    /*task lock*/
    lvgl_port_lock(0);

    /* put APPLICATION CODE HERE */

    //label
    lv_obj_t *label = lv_label_create(scr);
    lv_obj_set_width(label, LCD_H_RES);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    #if LVGL_VERSION_MAJOR == 8
    lv_label_set_recolor(label, true);
    lv_label_set_text(label, "#FF0000 "LV_SYMBOL_BELL" Hello world Espressif and LVGL "LV_SYMBOL_BELL"#\n#FF9400 "LV_SYMBOL_WARNING" For simplier initialization, use BSP "LV_SYMBOL_WARNING" #");
    #else
    lv_label_set_text(label, LV_SYMBOL_BELL" Hello world Espressif and LVGL "LV_SYMBOL_BELL"\n "LV_SYMBOL_WARNING" For simplier initialization, use BSP "LV_SYMBOL_WARNING);
    #endif
    lv_obj_align(label, LV_ALIGN_CENTER, 0, -30);

    /*button code would go here*/
    

    /*task unlock*/
    lvgl_port_unlock();
}


//scaffolding for later

void display_task(void *arg)
{
    while(1){
        printf("hi");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void app_start_display_task(void)
{
    xTaskCreate(display_task, "display_task", 4096, NULL, 8, NULL);
}

// for testing
#define TEST_MEMORY_LEAK_THRESHOLD (50)

void check_leak(size_t start_free, size_t end_free, const char *type)
{
    ssize_t delta = start_free - end_free;
    printf("MALLOC_CAP_%s: Before %u bytes free, After %u bytes free (delta %d)\n", type, start_free, end_free, delta);
    TEST_ASSERT_GREATER_OR_EQUAL_MESSAGE (delta, TEST_MEMORY_LEAK_THRESHOLD, "memory leak");
}

//helper function to swap color bytes, keep for now.
uint16_t lcd_swap_color(uint16_t color)
{
    return (uint16_t)((color << 8) | (color >> 8));
}

//unity test case to be called from main.
