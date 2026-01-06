/* 2025 Eric Liu
This program defines a freeRTOS task to drive two SPI displays. The displays are multiplexed with their CS pins. 
There is one SPI channel defined on SPI2, which drives LCD1 and LCD2 with their own io handles, panel handles, and LVGL objects.
In use is esp_lcd component for GC9A01 (primary disp) and NV3041A (secondary disp)


Inputs: Queue
Outputs: None
*/


#include "app_display.h"
#include "app_tasks.h"
#include "app_tcp.h"
#include "app_wifi.h"
#include "app_gpio.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "esp_err.h"
#include "esp_check.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_lcd_gc9a01.h"
#include "esp_lcd_nv3041.h"
#include "esp_lcd_panel_commands.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lvgl_port.h"

/* LCD SPI host and CLK */

#define LCD_HOST SPI2_HOST
#define LCD_PIXEL_CLOCK_HZ (32 * 1000 * 1000)


/* LCD PIN definitions */

#define PIN_NUM_MOSI 11
#define PIN_NUM_SCLK 12
#define PIN_NUM_CS 10
#define PIN_NUM_CS_2 17
#define PIN_NUM_DC 8
#define PIN_NUM_RST -1
#define PIN_NUM_RST_2 18
#define PIN_NUM_MISO -1

static const char *TAG = "display_task";

#define LCD_INVERT_COLORS true

/* LCD one and two specific definitions */
#define LCD_H_RES 240
#define LCD_V_RES 240
#define LCD_DRAW_BUF_HEIGHT 40
#define LCD_DRAW_BUF_DOUBLE 1
#define LCD_CMD_BITS 8
#define LCD_PARAM_BITS 8
#define LCD_BITS_PER_PIXEL 16
/*--------------------------------------*/
#define LCD_H_RES_2 480
#define LCD_V_RES_2 128
#define LCD_DRAW_BUF_HEIGHT_2 20
#define LCD_DRAW_BUF_DOUBLE_2 0
#define LCD_CMD_BITS_2 8
#define LCD_PARAM_BITS_2 8
#define LCD_BITS_PER_PIXEL_2 16
#define LVGL_DRAW_BUF_HEIGHT_2 4
/*--------------------------------------*/
#define DISPLAY_MAX_LINES 16
#define DISPLAY_LINE_MAX_AGE_MS 10000
/*--------------------------------------*/
#define DISPLAY_MAX_LINES_2 8
#define DISPLAY_LINE_MAX_AGE_MS_2 10000

/* lcd panel Ios */

static esp_lcd_panel_io_handle_t io_handle = NULL;
static esp_lcd_panel_handle_t panel_handle = NULL;

static esp_lcd_panel_io_handle_t io_handle_2 = NULL;
static esp_lcd_panel_handle_t panel_handle_2 = NULL;

/* lvgl display handles */

static lv_display_t *lvgl_disp = NULL;
static lv_display_t *lvgl_disp_2 = NULL;

static void screen1_fill_color(uint16_t color)
{
    static uint16_t line[LCD_H_RES];
    if (!panel_handle || LCD_H_RES <= 0 || LCD_V_RES <= 0) {
        return;
    }
    for (int x = 0; x < LCD_H_RES; x++) {
        line[x] = color;
    }
    for (int y = 0; y < LCD_V_RES; y++) {
        esp_lcd_panel_draw_bitmap(panel_handle, 0, y, LCD_H_RES, y + 1, line);
    }
}

static void screen2_fill_color(uint16_t color)
{
    static uint16_t line[LCD_H_RES_2];
    if (!panel_handle_2 || LCD_H_RES_2 <= 0 || LCD_V_RES_2 <= 0) {
        ESP_LOGW(TAG, "screen2_fill_color skipped: panel_handle_2=%p", panel_handle_2);
        return;
    }
    for (int x = 0; x < LCD_H_RES_2; x++) {
        line[x] = color;
    }
    int err_count = 0;
    for (int y = 0; y < LCD_V_RES_2; y++) {
        esp_err_t ret = esp_lcd_panel_draw_bitmap(panel_handle_2, 0, y, LCD_H_RES_2, y + 1, line);
        if (ret != ESP_OK) {
            if (err_count == 0) {
                ESP_LOGE(TAG, "screen2_fill_color draw failed at y=%d: %s", y, esp_err_to_name(ret));
            }
            err_count++;
        }
    }
    if (err_count > 0) {
        ESP_LOGE(TAG, "screen2_fill_color errors: %d", err_count);
    }
}

static void screen2_draw_color_bars(void)
{
    static const uint16_t bars[] = {
        0xFFFF, // white
        0xFFE0, // yellow
        0x07FF, // cyan
        0x07E0, // green
        0xF81F, // magenta
        0xF800, // red
        0x001F, // blue
        0x0000, // black
    };
    static uint16_t line[LCD_H_RES_2];
    if (!panel_handle_2 || LCD_H_RES_2 <= 0 || LCD_V_RES_2 <= 0) {
        ESP_LOGW(TAG, "screen2_draw_color_bars skipped: panel_handle_2=%p", panel_handle_2);
        return;
    }
    const size_t bar_count = sizeof(bars) / sizeof(bars[0]);
    for (int x = 0; x < LCD_H_RES_2; x++) {
        size_t idx = (size_t)(x * bar_count) / (size_t)LCD_H_RES_2;
        if (idx >= bar_count) {
            idx = bar_count - 1;
        }
        line[x] = bars[idx];
    }
    int err_count = 0;
    for (int y = 0; y < LCD_V_RES_2; y++) {
        esp_err_t ret = esp_lcd_panel_draw_bitmap(panel_handle_2, 0, y, LCD_H_RES_2, y + 1, line);
        if (ret != ESP_OK) {
            if (err_count == 0) {
                ESP_LOGE(TAG, "screen2_draw_color_bars failed at y=%d: %s", y, esp_err_to_name(ret));
            }
            err_count++;
        }
    }
    if (err_count > 0) {
        ESP_LOGE(TAG, "screen2_draw_color_bars errors: %d", err_count);
    }
}

static void screen2_draw_checkerboard(uint16_t a, uint16_t b, int block)
{
    static uint16_t line[LCD_H_RES_2];
    if (!panel_handle_2 || LCD_H_RES_2 <= 0 || LCD_V_RES_2 <= 0 || block <= 0) {
        ESP_LOGW(TAG, "screen2_draw_checkerboard skipped: panel_handle_2=%p", panel_handle_2);
        return;
    }
    int err_count = 0;
    for (int y = 0; y < LCD_V_RES_2; y++) {
        int row = (y / block) & 1;
        for (int x = 0; x < LCD_H_RES_2; x++) {
            int col = (x / block) & 1;
            line[x] = (row ^ col) ? a : b;
        }
        esp_err_t ret = esp_lcd_panel_draw_bitmap(panel_handle_2, 0, y, LCD_H_RES_2, y + 1, line);
        if (ret != ESP_OK) {
            if (err_count == 0) {
                ESP_LOGE(TAG, "screen2_draw_checkerboard failed at y=%d: %s", y, esp_err_to_name(ret));
            }
            err_count++;
        }
    }
    if (err_count > 0) {
        ESP_LOGE(TAG, "screen2_draw_checkerboard errors: %d", err_count);
    }
}

static void screen2_draw_horizontal_gradient(void)
{
    static uint16_t line[LCD_H_RES_2];
    if (!panel_handle_2 || LCD_H_RES_2 <= 1 || LCD_V_RES_2 <= 0) {
        ESP_LOGW(TAG, "screen2_draw_horizontal_gradient skipped: panel_handle_2=%p", panel_handle_2);
        return;
    }
    for (int x = 0; x < LCD_H_RES_2; x++) {
        uint8_t level = (uint8_t)((x * 31) / (LCD_H_RES_2 - 1));
        uint16_t gray = (uint16_t)((level << 11) | (level << 6) | level);
        line[x] = gray;
    }
    int err_count = 0;
    for (int y = 0; y < LCD_V_RES_2; y++) {
        esp_err_t ret = esp_lcd_panel_draw_bitmap(panel_handle_2, 0, y, LCD_H_RES_2, y + 1, line);
        if (ret != ESP_OK) {
            if (err_count == 0) {
                ESP_LOGE(TAG, "screen2_draw_horizontal_gradient failed at y=%d: %s", y, esp_err_to_name(ret));
            }
            err_count++;
        }
    }
    if (err_count > 0) {
        ESP_LOGE(TAG, "screen2_draw_horizontal_gradient errors: %d", err_count);
    }
}

static void screen2_test_task(void *arg)
{
    TaskHandle_t notify_task = (TaskHandle_t)arg;
    const TickType_t delay = pdMS_TO_TICKS(5000);
    ESP_LOGI(TAG, "screen2 test patterns: start");
    ESP_LOGI(TAG, "screen2 pattern: black");
    screen2_fill_color(0x0000);
    vTaskDelay(delay);
    ESP_LOGI(TAG, "screen2 pattern: white");
    screen2_fill_color(0xFFFF);
    vTaskDelay(delay);
    ESP_LOGI(TAG, "screen2 invert on");
    esp_lcd_panel_invert_color(panel_handle_2, true);
    vTaskDelay(delay);
    ESP_LOGI(TAG, "screen2 invert off");
    esp_lcd_panel_invert_color(panel_handle_2, false);
    vTaskDelay(delay);
    ESP_LOGI(TAG, "screen2 pattern: color bars");
    screen2_draw_color_bars();
    vTaskDelay(delay);
    ESP_LOGI(TAG, "screen2 pattern: checkerboard");
    screen2_draw_checkerboard(0xFFFF, 0x0000, 16);
    vTaskDelay(delay);
    ESP_LOGI(TAG, "screen2 pattern: gradient");
    screen2_draw_horizontal_gradient();
    vTaskDelay(delay);
    ESP_LOGI(TAG, "screen2 pattern: black end");
    screen2_fill_color(0x0000);
    ESP_LOGI(TAG, "screen2 test patterns: done");
    if (notify_task) {
        xTaskNotifyGive(notify_task);
    }
    vTaskDelete(NULL);
}

typedef struct {
    TickType_t ts;
    char text[TEXT_BUF_SIZE + 1];
} log_line_t;

/* terminal line style rendering -> pad all space on top so new lines appear bottom */
/* safely discards excess transcript to avoid memory error                          */
static void rebuild_log_textarea(lv_obj_t *log_area, const log_line_t *lines, int line_count,
                                 int max_lines)
{
    static char log_text[1024];
    size_t pos = 0;
    size_t remaining = sizeof(log_text) - 1;

    int padding_lines = max_lines - line_count;
    for (int i = 0; i < padding_lines && remaining > 0; i++) {
        if (remaining == 0) {
            break;
        }
        log_text[pos++] = '\n';
        log_text[pos] = '\0';
        remaining--;
    }

    for (int i = 0; i < line_count && remaining > 0; i++) {
        size_t len = strnlen(lines[i].text, TEXT_BUF_SIZE);
        if (len > remaining) {
            len = remaining;
        }
        memcpy(log_text + pos, lines[i].text, len);
        pos += len;
        log_text[pos] = '\0';
        remaining -= len;
        if (i < line_count - 1 && remaining > 0) {
            log_text[pos++] = '\n';
            log_text[pos] = '\0';
            remaining--;
        }
    }

    lv_textarea_set_text(log_area, log_text);
    lv_textarea_set_cursor_pos(log_area, LV_TEXTAREA_CURSOR_LAST);
}

/* removes the oldest line for when the lines reach the top */
static void drop_oldest_line(log_line_t *lines, int *line_count)
{
    if (*line_count <= 0) {
        return;
    }
    for (int i = 1; i < *line_count; i++) {
        lines[i - 1] = lines[i];
    }
    (*line_count)--;
}

/* removes expired lines from the log; lifetime is defined by DISPLAY_LINE_MAX_AGE_MS */
static void prune_expired_lines(log_line_t *lines, int *line_count, TickType_t now)
{
    while (*line_count > 0 &&
           (now - lines[0].ts) > pdMS_TO_TICKS(DISPLAY_LINE_MAX_AGE_MS)) {
        drop_oldest_line(lines, line_count);
    }
}

/* splits incoming log transcript to what fits, invokes functions to trim if needed     */
static void add_wrapped_lines(log_line_t *lines, int *line_count, int max_lines,
                              const char *text, TickType_t ts, const lv_font_t *font,
                              int32_t max_width, int32_t letter_space)
{
    size_t len = strnlen(text, TEXT_BUF_SIZE);
    size_t line_start = 0;
    int32_t line_width = 0;
    size_t line_len = 0;

    if (max_width < 1) {
        max_width = 1;
    }

    for (size_t i = 0; i < len; i++) {
        uint32_t letter = (uint8_t)text[i];
        uint32_t next = (i + 1 < len) ? (uint8_t)text[i + 1] : 0;
        int32_t glyph_width = lv_font_get_glyph_width(font, letter, next);
        int32_t next_width = line_width + glyph_width;

        if (line_len > 0 && next_width > max_width) {
            while (line_start < i && text[line_start] == ' ') {
                line_start++;
            }
            size_t seg_len = i - line_start;
            if (seg_len > 0) {
                while (*line_count >= max_lines) {
                    drop_oldest_line(lines, line_count);
                }
                lines[*line_count].ts = ts;
                size_t copy_len = (seg_len > TEXT_BUF_SIZE) ? TEXT_BUF_SIZE : seg_len;
                memcpy(lines[*line_count].text, text + line_start, copy_len);
                lines[*line_count].text[copy_len] = '\0';
                (*line_count)++;
            }
            line_start = i;
            line_width = 0;
            line_len = 0;
        }

        line_width += glyph_width + letter_space;
        line_len++;
    }

    if (line_len > 0) {
        while (line_start < len && text[line_start] == ' ') {
            line_start++;
        }
        if (line_start < len) {
            size_t seg_len = len - line_start;
            while (*line_count >= max_lines) {
                drop_oldest_line(lines, line_count);
            }
            lines[*line_count].ts = ts;
            size_t copy_len = (seg_len > TEXT_BUF_SIZE) ? TEXT_BUF_SIZE : seg_len;
            memcpy(lines[*line_count].text, text + line_start, copy_len);
            lines[*line_count].text[copy_len] = '\0';
            (*line_count)++;
        }
    }
}

esp_err_t app_lcd_init(void)
{
    esp_err_t ret = ESP_OK;

    esp_log_level_set(TAG, ESP_LOG_VERBOSE);
    esp_log_level_set("lcd_panel.gc9a01", ESP_LOG_VERBOSE);
    esp_log_level_set("lcd_panel.nv3041", ESP_LOG_VERBOSE);

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
            .bits_per_pixel = LCD_BITS_PER_PIXEL,
    };
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_gc9a01(io_handle, &panel_config, &panel_handle), err, TAG, "New panel failed");
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_handle, LCD_INVERT_COLORS));

    ESP_LOGD(TAG, "Install panel IO for screen 2");
    const esp_lcd_panel_io_spi_config_t io_config_2 = {
        .dc_gpio_num = PIN_NUM_DC,
        .cs_gpio_num = PIN_NUM_CS_2,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = LCD_CMD_BITS_2,
        .lcd_param_bits = LCD_PARAM_BITS_2,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config_2, &io_handle_2), err, TAG,
                      "Failed to install panel IO (screen 2)");

    const esp_lcd_panel_dev_config_t panel_config_2 = {
        .reset_gpio_num = PIN_NUM_RST_2,
        #if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 0, 0)
            .color_space = ESP_LCD_COLOR_SPACE_BGR,
        #elif ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(6, 0, 0)
            .rgb_endian = LCD_RGB_ENDIAN_BGR,
        #else
            .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        #endif
            .bits_per_pixel = LCD_BITS_PER_PIXEL_2,
    };
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_nv3041(io_handle_2, &panel_config_2, &panel_handle_2), err, TAG,
                      "New panel failed (screen 2)");
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle_2));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle_2));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle_2, true));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_handle_2, LCD_INVERT_COLORS));

    screen1_fill_color(0x0000);
    screen2_fill_color(0x0000);

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
        if (panel_handle_2) {
            esp_lcd_panel_del(panel_handle_2);
            panel_handle_2 = NULL;
        }
        if (io_handle_2) {
            esp_lcd_panel_io_del(io_handle_2);
            io_handle_2 = NULL;
        }
        spi_bus_free(LCD_HOST);

    return ret;
}

esp_err_t app_lcd_deinit(void)
{
    esp_err_t ret = ESP_OK;
    if (panel_handle_2) {
        ESP_RETURN_ON_ERROR(esp_lcd_panel_del(panel_handle_2), TAG, "LCD panel 2 de-initialization failed");
        panel_handle_2 = NULL;
    }
    if (io_handle_2) {
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_del(io_handle_2), TAG, "LCD panel IO 2 de-initialization failed");
        io_handle_2 = NULL;
    }
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
            .swap_xy = true,
            .mirror_x = false,
            .mirror_y = true,
        },
        .flags = {
            .buff_dma = true,
            #if LVGL_VERSION_MAJOR >= 9
            .swap_bytes = true,
            #endif
        }
    };
    lvgl_disp = lvgl_port_add_disp(&disp_cfg);

    const lvgl_port_display_cfg_t disp_cfg_2 = {
        .io_handle = io_handle_2,
        .panel_handle = panel_handle_2,
        .buffer_size = LCD_H_RES_2 * LVGL_DRAW_BUF_HEIGHT_2 * sizeof(uint16_t),
        .double_buffer = LCD_DRAW_BUF_DOUBLE_2,
        .hres = LCD_H_RES_2,
        .vres = LCD_V_RES_2,
        .monochrome = false,
        .flags = {
            .buff_dma = false,
#if LVGL_VERSION_MAJOR >= 9
            .swap_bytes = true,
#endif
        }
    };
    lvgl_disp_2 = lvgl_port_add_disp(&disp_cfg_2);
    if (!lvgl_disp_2) {
        ESP_LOGE(TAG, "LVGL disp 2 init failed");
    }

    return ESP_OK;
}

esp_err_t app_lvgl_deinit(void)
{
    if (lvgl_disp_2) {
        ESP_RETURN_ON_ERROR(lvgl_port_remove_disp(lvgl_disp_2), TAG, "LVGL disp 2 removing failed");
        lvgl_disp_2 = NULL;
    }
    ESP_RETURN_ON_ERROR(lvgl_port_remove_disp(lvgl_disp), TAG, "LVGL disp removing failed");
    ESP_RETURN_ON_ERROR(lvgl_port_deinit(), TAG, "LVGL deinit failed");

    return ESP_OK;
}

void display_task(void *arg)
{   /*
    The 240x240 screen is operator-facing, so it has RSSI indicator and REC/RDY indicators. 
    The 480x128 screen only has text.
    */
    lvgl_port_lock(0);
    /* screen one of two */
    lv_obj_t *scr = lv_scr_act();
    lv_obj_t *log_area = NULL;
    lv_obj_t *indicator_area = NULL;
    lv_obj_t *rdy_label = NULL;
    lv_obj_t *rec_dot = NULL;
    lv_obj_t *rssi_label = NULL;
    const int16_t text_x = 30;
    const int16_t text_y = 40;
    const int16_t text_w = 180;
    const int16_t text_h = 160;
    const int16_t indicator_w = 120;
    const int16_t indicator_h = 30;
    const int16_t indicator_x = text_x + (text_w - indicator_w) / 2;
    const int16_t indicator_y = text_y - indicator_h;

    /* screen two of two WIP */

    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    /* defining objects for screen 1 */
    indicator_area = lv_obj_create(scr);
    lv_obj_remove_style_all(indicator_area);
    lv_obj_set_size(indicator_area, indicator_w, indicator_h);
    lv_obj_set_pos(indicator_area, indicator_x, indicator_y);
    lv_obj_set_style_bg_opa(indicator_area, LV_OPA_TRANSP, 0);

    rdy_label = lv_label_create(indicator_area);
    lv_label_set_text(rdy_label, "RDY");
    lv_obj_set_style_text_color(rdy_label, lv_color_hex(0x2D6BFF), 0);
    lv_obj_align(rdy_label, LV_ALIGN_LEFT_MID, 0, 0);

    rec_dot = lv_obj_create(indicator_area);
    lv_obj_remove_style_all(rec_dot);
    lv_obj_set_size(rec_dot, 10, 10);
    lv_obj_set_style_radius(rec_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(rec_dot, lv_color_hex(0xFF2A2A), 0);
    lv_obj_set_style_bg_opa(rec_dot, LV_OPA_COVER, 0);
    lv_obj_align(rec_dot, LV_ALIGN_LEFT_MID, 2, 0);

    rssi_label = lv_label_create(indicator_area);
    lv_label_set_text(rssi_label, "RSSI --");
    lv_obj_set_style_text_color(rssi_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(rssi_label, LV_ALIGN_RIGHT_MID, 0, 0);

    log_area = lv_textarea_create(scr);
    lv_obj_set_size(log_area, text_w, text_h);
    lv_obj_set_pos(log_area, text_x, text_y);
    lv_textarea_set_max_length(log_area, 1024);
    lv_textarea_set_cursor_click_pos(log_area, false);
    lv_textarea_set_password_mode(log_area, false);
    lv_obj_add_state(log_area, LV_STATE_DISABLED);
    lv_obj_set_style_bg_opa(log_area, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_opa(log_area, LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_color(log_area, lv_color_hex(0x00FF00), 0);
    lv_obj_set_style_text_outline_stroke_color(log_area, lv_color_hex(0x00FF00), 0);
    lv_obj_set_style_text_outline_stroke_opa(log_area, LV_OPA_COVER, 0);
    lv_obj_set_style_text_outline_stroke_width(log_area, 1, 0);
    lv_obj_set_style_pad_all(log_area, 2, 0);
    lv_obj_set_scrollbar_mode(log_area, LV_SCROLLBAR_MODE_OFF);

    const lv_font_t *log_font = lv_obj_get_style_text_font(log_area, LV_PART_MAIN);
    const int32_t line_space = lv_obj_get_style_text_line_space(log_area, LV_PART_MAIN);
    const int32_t letter_space = lv_obj_get_style_text_letter_space(log_area, LV_PART_MAIN);
    const int32_t pad_left = lv_obj_get_style_pad_left(log_area, LV_PART_MAIN);
    const int32_t pad_right = lv_obj_get_style_pad_right(log_area, LV_PART_MAIN);
    const int32_t pad_top = lv_obj_get_style_pad_top(log_area, LV_PART_MAIN);
    const int32_t pad_bottom = lv_obj_get_style_pad_bottom(log_area, LV_PART_MAIN);
    const uint16_t line_height = lv_font_get_line_height(log_font) + line_space;
    int32_t content_height = text_h - pad_top - pad_bottom;
    int32_t content_width = text_w - pad_left - pad_right;
    int max_lines = (line_height > 0) ? (content_height / line_height) : 1;
    if (max_lines < 1) {
        max_lines = 1;
    }
    if (max_lines > DISPLAY_MAX_LINES) {
        max_lines = DISPLAY_MAX_LINES;
    }
    if (content_width < 1) {
        content_width = 1;
    }

    ESP_LOGI(TAG, "display task running");
    lvgl_port_unlock();

    static log_line_t lines[DISPLAY_MAX_LINES];
    static int line_count = 0;
    QueueHandle_t disp1_q = tcp_rx_get_disp1_q();
    TickType_t last_indicator_update = 0;
    TickType_t last_prune = 0;

    while (1) {
        text_msg_t msg;
        bool got_msg = false;
        if (disp1_q) {
            if (xQueueReceive(disp1_q, &msg, pdMS_TO_TICKS(100)) == pdTRUE) {
                got_msg = true;
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(100));
        }

        TickType_t now = xTaskGetTickCount();
        bool prune_needed = (now - last_prune) > pdMS_TO_TICKS(200);
        bool indicator_needed = (now - last_indicator_update) > pdMS_TO_TICKS(250);

        if (got_msg) {
            size_t copy_len = msg.len;
            if (copy_len > TEXT_BUF_SIZE) {
                copy_len = TEXT_BUF_SIZE;
            }
            char line_buf[TEXT_BUF_SIZE + 1];
            memcpy(line_buf, msg.payload, copy_len);
            line_buf[copy_len] = '\0';
            for (size_t i = 0; i < copy_len; i++) {
                if (line_buf[i] == '\r' || line_buf[i] == '\n') {
                    line_buf[i] = ' ';
                }
            }

            lvgl_port_lock(0);
            prune_expired_lines(lines, &line_count, now);
            add_wrapped_lines(lines, &line_count, max_lines, line_buf, now, log_font,
                              content_width, letter_space);
            rebuild_log_textarea(log_area, lines, line_count, max_lines);
            lvgl_port_unlock();
        }

        if (prune_needed) {
            bool changed = false;
            lvgl_port_lock(0);
            int before = line_count;
            prune_expired_lines(lines, &line_count, now);
            changed = (before != line_count);
            if (changed) {
                rebuild_log_textarea(log_area, lines, line_count, max_lines);
            }
            lvgl_port_unlock();
            last_prune = now;
        }

        if (indicator_needed) {
            app_gpio_state_t state = gpio_get_state();
            int8_t rssi = wifi_get_rssi();
            char rssi_buf[16];
            lvgl_port_lock(0);
            if (state == APP_GPIO_STATE_IDLE) {
                lv_label_set_text(rdy_label, "RDY");
                lv_obj_align(rdy_label, LV_ALIGN_LEFT_MID, 0, 0);
                lv_obj_clear_flag(rdy_label, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(rec_dot, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(rdy_label, LV_OBJ_FLAG_HIDDEN);
                lv_obj_clear_flag(rec_dot, LV_OBJ_FLAG_HIDDEN);
            }
            snprintf(rssi_buf, sizeof(rssi_buf), "RSSI %d", (int)rssi);
            lv_label_set_text(rssi_label, rssi_buf);
            lv_obj_align(rssi_label, LV_ALIGN_RIGHT_MID, 0, 0);
            lvgl_port_unlock();
            last_indicator_update = now;
        }
    }
}

void display_make_tasks(void)
{
    ESP_ERROR_CHECK(app_lcd_init());
    TaskHandle_t screen2_task = NULL;
    TaskHandle_t current_task = xTaskGetCurrentTaskHandle();
    xTaskCreatePinnedToCore(screen2_test_task, "screen2_test", 4096, current_task, 5, &screen2_task, 1);
    if (screen2_task) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }
    ESP_ERROR_CHECK(app_lvgl_init());
    xTaskCreatePinnedToCore(display_task, "display_task", 8192, NULL, 6, NULL, 1);
}
