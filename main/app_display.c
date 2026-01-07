/* 2025 Eric Liu
This program defines a freeRTOS task to drive two SPI displays. The displays are multiplexed with their CS pins. 
There is one SPI channel defined on SPI2, which drives LCD1 and LCD2 with their own io handles, panel handles, and LVGL objects.
In use is esp_lcd component for GC9A01 (primary disp) and NV3041A (secondary disp)


Inputs: Queue
Outputs: None
*/


#include "app_display.h"
#include "app_tcp.h"
#include "app_wifi.h"
#include "app_gpio.h"
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
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lvgl_port.h"

#if LV_FONT_MONTSERRAT_28
LV_FONT_DECLARE(lv_font_montserrat_28);
#endif

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
#define LCD_DRAW_BUF_DOUBLE_2 true
#define LCD_CMD_BITS_2 8
#define LCD_PARAM_BITS_2 8
#define LCD_BITS_PER_PIXEL_2 16
#define LCD_DRAW_BUF_HEIGHT_2 4
#define SCREEN2_SWAP_BYTES true
#define SCREEN2_LVGL_DMA true
#define SCREEN2_TEST_MODE 2
#define SCREEN2_TEST_SINGLE 2
#define SCREEN2_TEST_OBSERVE_MS 10000
#define SCREEN2_EARLY_PANEL_TEST 0
#define SCREEN2_PANEL_HEIGHT 272
#define SCREEN2_SCAN_STEP 16
#define SCREEN2_SCAN_BAND_HEIGHT 16
#define SCREEN2_SCAN_HOLD_MS 1000
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

static void screen2_fill_color(uint16_t color);

static int32_t min_i32(int32_t a, int32_t b)
{
    return (a < b) ? a : b;
}

static void create_logo_rect(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h)
{
    if (w <= 0 || h <= 0) {
        return;
    }
    lv_obj_t *rect = lv_obj_create(parent);
    lv_obj_remove_style_all(rect);
    lv_obj_set_pos(rect, x, y);
    lv_obj_set_size(rect, w, h);
    lv_obj_set_style_bg_color(rect, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(rect, LV_OPA_COVER, 0);
}

static void create_logo_rect_rotated_ccw(lv_obj_t *parent,
                                         int32_t origin_x,
                                         int32_t origin_y,
                                         int32_t logo_w,
                                         int32_t logo_h,
                                         int32_t x,
                                         int32_t y,
                                         int32_t w,
                                         int32_t h)
{
    int32_t rx = y;
    int32_t ry = logo_w - (x + w);
    int32_t rw = h;
    int32_t rh = w;
    create_logo_rect(parent, origin_x + rx, origin_y + ry, rw, rh);
}

static void show_boot_logo(lv_display_t *disp, int32_t disp_w, int32_t disp_h, int rotate_ccw)
{
    if (!disp || disp_w <= 0 || disp_h <= 0) {
        return;
    }
    lv_display_t *prev = lv_display_get_default();
    lv_display_set_default(disp);
    lv_obj_t *scr = lv_scr_act();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    int32_t base = min_i32(disp_w, disp_h);
    int32_t logo_w = (base * 60) / 100;
    int32_t logo_h = (disp_h * 70) / 100;
    if (logo_w < 24) {
        logo_w = 24;
    }
    if (logo_h < 24) {
        logo_h = 24;
    }
    int32_t gap = logo_h / 12;
    int32_t letter_h = (logo_h - (gap * 2)) / 3;
    if (letter_h < 8) {
        letter_h = 8;
        gap = 2;
        logo_h = (letter_h * 3) + (gap * 2);
    }
    int32_t thickness = letter_h / 4;
    if (thickness < 3) {
        thickness = 3;
    }
    if (thickness > letter_h) {
        thickness = letter_h;
    }

    int32_t draw_w = rotate_ccw ? logo_h : logo_w;
    int32_t draw_h = rotate_ccw ? logo_w : logo_h;
    int32_t logo_x = (disp_w - draw_w) / 2;
    int32_t logo_y = (disp_h - draw_h) / 2;

    for (int i = 0; i < 3; i++) {
        int32_t top = logo_y + i * (letter_h + gap);
        if (rotate_ccw) {
            int32_t local_top = i * (letter_h + gap);
            create_logo_rect_rotated_ccw(scr, logo_x, logo_y, logo_w, logo_h,
                                         0, local_top, thickness, letter_h);
            create_logo_rect_rotated_ccw(scr, logo_x, logo_y, logo_w, logo_h,
                                         0, local_top + letter_h - thickness, logo_w, thickness);
        } else {
            create_logo_rect(scr, logo_x, top, thickness, letter_h);
            create_logo_rect(scr, logo_x, top + letter_h - thickness, logo_w, thickness);
        }
    }

    lv_display_set_default(prev);
}

static void clear_display(lv_display_t *disp)
{
    if (!disp) {
        return;
    }
    lv_display_t *prev = lv_display_get_default();
    lv_display_set_default(disp);
    lv_obj_clean(lv_scr_act());
    lv_display_set_default(prev);
}

static void screen2_panel_color_bars(void)
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
        ESP_LOGE(TAG, "screen2_panel_color_bars skipped: panel_handle_2=%p", panel_handle_2);
        return;
    }
    ESP_LOGE(TAG, "screen2_panel_color_bars: handle=%p", panel_handle_2);
    const size_t bar_count = sizeof(bars) / sizeof(bars[0]);
    for (int x = 0; x < LCD_H_RES_2; x++) {
        size_t idx = (size_t)(x * bar_count) / (size_t)LCD_H_RES_2;
        if (idx >= bar_count) {
            idx = bar_count - 1;
        }
        line[x] = bars[idx];
    }
    for (int y = 0; y < LCD_V_RES_2; y++) {
        esp_lcd_panel_draw_bitmap(panel_handle_2, 0, y, LCD_H_RES_2, y + 1, line);
    }
}

static void screen2_panel_scan_bands(void)
{
    static uint16_t line[LCD_H_RES_2];
    if (!panel_handle_2 || LCD_H_RES_2 <= 0 || SCREEN2_PANEL_HEIGHT <= 0) {
        ESP_LOGE(TAG, "screen2_panel_scan_bands skipped: panel_handle_2=%p", panel_handle_2);
        return;
    }
    for (int x = 0; x < LCD_H_RES_2; x++) {
        line[x] = 0xFFFF;
    }
    int max_y = SCREEN2_PANEL_HEIGHT - SCREEN2_SCAN_BAND_HEIGHT;
    if (max_y < 0) {
        max_y = 0;
    }
    for (int y = 0; y <= max_y; y += SCREEN2_SCAN_STEP) {
        ESP_LOGE(TAG, "screen2 scan band y=%d", y);
        for (int by = 0; by < SCREEN2_SCAN_BAND_HEIGHT; by++) {
            int row = y + by;
            esp_lcd_panel_draw_bitmap(panel_handle_2, 0, row, LCD_H_RES_2, row + 1, line);
        }
        vTaskDelay(pdMS_TO_TICKS(SCREEN2_SCAN_HOLD_MS));
        screen2_fill_color(0x0000);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

static void screen2_lvgl_color_test(void)
{
    if (!lvgl_disp_2) {
        ESP_LOGW(TAG, "screen2 LVGL color test skipped: no lvgl_disp_2");
        return;
    }
    lv_display_t *prev = lv_display_get_default();
    lv_display_set_default(lvgl_disp_2);
    lv_obj_t *scr = lv_scr_act();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    int32_t bar_w = LCD_H_RES_2 / 3;
    int32_t bar_h = LCD_V_RES_2;
    lv_obj_t *bar_red = lv_obj_create(scr);
    lv_obj_remove_style_all(bar_red);
    lv_obj_set_pos(bar_red, 0, 0);
    lv_obj_set_size(bar_red, bar_w, bar_h);
    lv_obj_set_style_bg_color(bar_red, lv_color_hex(0xFF0000), 0);
    lv_obj_set_style_bg_opa(bar_red, LV_OPA_COVER, 0);

    lv_obj_t *bar_green = lv_obj_create(scr);
    lv_obj_remove_style_all(bar_green);
    lv_obj_set_pos(bar_green, bar_w, 0);
    lv_obj_set_size(bar_green, bar_w, bar_h);
    lv_obj_set_style_bg_color(bar_green, lv_color_hex(0x00FF00), 0);
    lv_obj_set_style_bg_opa(bar_green, LV_OPA_COVER, 0);

    lv_obj_t *bar_blue = lv_obj_create(scr);
    lv_obj_remove_style_all(bar_blue);
    lv_obj_set_pos(bar_blue, bar_w * 2, 0);
    lv_obj_set_size(bar_blue, LCD_H_RES_2 - (bar_w * 2), bar_h);
    lv_obj_set_style_bg_color(bar_blue, lv_color_hex(0x0000FF), 0);
    lv_obj_set_style_bg_opa(bar_blue, LV_OPA_COVER, 0);

    lv_refr_now(lvgl_disp_2);
    lv_display_set_default(prev);
}

static void screen2_lvgl_text_test(const char *text)
{
    if (!lvgl_disp_2) {
        ESP_LOGW(TAG, "screen2 LVGL text test skipped: no lvgl_disp_2");
        return;
    }
    lv_display_t *prev = lv_display_get_default();
    lv_display_set_default(lvgl_disp_2);
    lv_obj_t *scr = lv_scr_act();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    lv_obj_t *label = lv_label_create(scr);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_center(label);

    lv_refr_now(lvgl_disp_2);
    lv_display_set_default(prev);
}

typedef enum {
    SCREEN2_TEST_PANEL_BARS = 0,
    SCREEN2_TEST_PANEL_WHITE = 1,
    SCREEN2_TEST_LVGL_BARS = 2,
    SCREEN2_TEST_LVGL_TEXT = 3,
} screen2_test_id_t;

static void run_screen2_test(screen2_test_id_t test_id, TickType_t hold)
{
    switch (test_id) {
        case SCREEN2_TEST_PANEL_BARS:
            ESP_LOGE(TAG, "screen2 test: panel color bars");
            lvgl_port_lock(0);
            screen2_panel_color_bars();
            vTaskDelay(hold);
            lvgl_port_unlock();
            break;
        case SCREEN2_TEST_PANEL_WHITE:
            ESP_LOGE(TAG, "screen2 test: panel solid white");
            lvgl_port_lock(0);
            screen2_fill_color(0xFFFF);
            vTaskDelay(hold);
            lvgl_port_unlock();
            break;
        case SCREEN2_TEST_LVGL_BARS:
            ESP_LOGE(TAG, "screen2 test: LVGL RGB bars");
            lvgl_port_lock(0);
            screen2_lvgl_color_test();
            lvgl_port_unlock();
            vTaskDelay(hold);
            break;
        case SCREEN2_TEST_LVGL_TEXT:
            ESP_LOGE(TAG, "screen2 test: LVGL text");
            lvgl_port_lock(0);
            screen2_lvgl_text_test("LVGL TEST");
            lvgl_port_unlock();
            vTaskDelay(hold);
            break;
        default:
            break;
    }
    lvgl_port_lock(0);
    clear_display(lvgl_disp_2);
    lvgl_port_unlock();
}

static void run_screen2_tests(void)
{
#if SCREEN2_TEST_MODE
    const TickType_t hold = pdMS_TO_TICKS(SCREEN2_TEST_OBSERVE_MS);
    ESP_LOGE(TAG, "screen2 test sequence start (mode=%d single=%d)", SCREEN2_TEST_MODE, SCREEN2_TEST_SINGLE);
    ESP_LOGE(TAG, "screen2 test handles: panel=%p lvgl=%p", panel_handle_2, lvgl_disp_2);
#if SCREEN2_TEST_MODE == 2
    run_screen2_test((screen2_test_id_t)SCREEN2_TEST_SINGLE, hold);
#else
    run_screen2_test(SCREEN2_TEST_PANEL_BARS, hold);
    run_screen2_test(SCREEN2_TEST_PANEL_WHITE, hold);
    run_screen2_test(SCREEN2_TEST_LVGL_BARS, hold);
    run_screen2_test(SCREEN2_TEST_LVGL_TEXT, hold);
#endif
    ESP_LOGE(TAG, "screen2 test sequence done");
#endif
}

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

static void prune_expired_lines_with_age(log_line_t *lines, int *line_count, TickType_t now,
                                         TickType_t max_age_ticks)
{
    while (*line_count > 0 &&
           (now - lines[0].ts) > max_age_ticks) {
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
            .color_space = ESP_LCD_COLOR_SPACE_RGB,
        #elif ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(6, 0, 0)
            .rgb_endian = LCD_RGB_ENDIAN_RGB,
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
            .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        #endif
            .bits_per_pixel = LCD_BITS_PER_PIXEL_2,
    };
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_nv3041(io_handle_2, &panel_config_2, &panel_handle_2), err, TAG,
                      "New panel failed (screen 2)");
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle_2));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle_2));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle_2, true));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_handle_2, true));

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
        .buffer_size = LCD_H_RES_2 * LCD_DRAW_BUF_HEIGHT_2 * sizeof(uint16_t),
        .double_buffer = LCD_DRAW_BUF_DOUBLE_2,
        .hres = LCD_H_RES_2,
        .vres = LCD_V_RES_2,
        .monochrome = false,
        .rotation = {
            .swap_xy = false,
            .mirror_x = false,
            .mirror_y = true,
        },
        .flags = {
            .buff_dma = SCREEN2_LVGL_DMA,
#if LVGL_VERSION_MAJOR >= 9
            .swap_bytes = SCREEN2_SWAP_BYTES,
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
    ESP_LOGE(TAG, "display_task: screen2 tests begin");
    run_screen2_tests();
    ESP_LOGE(TAG, "display_task: screen2 tests end");

    lvgl_port_lock(0);
    show_boot_logo(lvgl_disp, LCD_H_RES, LCD_V_RES, true);
    show_boot_logo(lvgl_disp_2, LCD_H_RES_2, LCD_V_RES_2, true);
    lvgl_port_unlock();
    vTaskDelay(pdMS_TO_TICKS(5000));
    lvgl_port_lock(0);
    clear_display(lvgl_disp);
    clear_display(lvgl_disp_2);
    lvgl_port_unlock();

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
    lv_obj_t *scr_2 = NULL;
    lv_obj_t *log_area_2 = NULL;
    const int16_t text_margin_2 = 8;
    const int16_t text_x_2 = text_margin_2;
    const int16_t text_y_2 = text_margin_2;
    const int16_t text_w_2 = LCD_H_RES_2 - (text_margin_2 * 2);
    const int16_t text_h_2 = LCD_V_RES_2 - (text_margin_2 * 2);

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

    const lv_font_t *log_font_2 = NULL;
    int32_t content_width_2 = 1;
    int max_lines_2 = 1;
    int32_t letter_space_2 = 0;
    if (lvgl_disp_2) {
        lv_display_t *prev_disp = lv_display_get_default();
        lv_display_set_default(lvgl_disp_2);
        scr_2 = lv_scr_act();
        lv_obj_set_style_bg_color(scr_2, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(scr_2, LV_OPA_COVER, 0);

        log_area_2 = lv_textarea_create(scr_2);
        lv_obj_set_size(log_area_2, text_w_2, text_h_2);
        lv_obj_set_pos(log_area_2, text_x_2, text_y_2);
        lv_textarea_set_max_length(log_area_2, 1024);
        lv_textarea_set_cursor_click_pos(log_area_2, false);
        lv_textarea_set_password_mode(log_area_2, false);
        lv_obj_add_state(log_area_2, LV_STATE_DISABLED);
        lv_obj_set_style_bg_opa(log_area_2, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_opa(log_area_2, LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_color(log_area_2, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_outline_stroke_color(log_area_2, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_outline_stroke_opa(log_area_2, LV_OPA_COVER, 0);
        lv_obj_set_style_text_outline_stroke_width(log_area_2, 1, 0);
        lv_obj_set_style_pad_all(log_area_2, 4, 0);
        lv_obj_set_scrollbar_mode(log_area_2, LV_SCROLLBAR_MODE_OFF);
#if LV_FONT_MONTSERRAT_28
        lv_obj_set_style_text_font(log_area_2, &lv_font_montserrat_28, 0);
#endif

        log_font_2 = lv_obj_get_style_text_font(log_area_2, LV_PART_MAIN);
        const int32_t line_space_2 = lv_obj_get_style_text_line_space(log_area_2, LV_PART_MAIN);
        letter_space_2 = lv_obj_get_style_text_letter_space(log_area_2, LV_PART_MAIN);
        const int32_t pad_left_2 = lv_obj_get_style_pad_left(log_area_2, LV_PART_MAIN);
        const int32_t pad_right_2 = lv_obj_get_style_pad_right(log_area_2, LV_PART_MAIN);
        const int32_t pad_top_2 = lv_obj_get_style_pad_top(log_area_2, LV_PART_MAIN);
        const int32_t pad_bottom_2 = lv_obj_get_style_pad_bottom(log_area_2, LV_PART_MAIN);
        const uint16_t line_height_2 = lv_font_get_line_height(log_font_2) + line_space_2;
        int32_t content_height_2 = text_h_2 - pad_top_2 - pad_bottom_2;
        content_width_2 = text_w_2 - pad_left_2 - pad_right_2;
        max_lines_2 = (line_height_2 > 0) ? (content_height_2 / line_height_2) : 1;
        if (max_lines_2 < 1) {
            max_lines_2 = 1;
        }
        if (max_lines_2 > DISPLAY_MAX_LINES_2) {
            max_lines_2 = DISPLAY_MAX_LINES_2;
        }
        if (content_width_2 < 1) {
            content_width_2 = 1;
        }
        lv_display_set_default(prev_disp);
    }

    ESP_LOGI(TAG, "display task running");
    lvgl_port_unlock();

    static log_line_t lines[DISPLAY_MAX_LINES];
    static log_line_t lines_2[DISPLAY_MAX_LINES_2];
    static int line_count = 0;
    static int line_count_2 = 0;
    QueueHandle_t disp1_q = tcp_rx_get_disp1_q();
    QueueHandle_t disp2_q = tcp_rx_get_disp2_q();
    TickType_t last_indicator_update = 0;
    TickType_t last_prune = 0;
    TickType_t last_prune_2 = 0;
    const TickType_t max_age_2 = pdMS_TO_TICKS(DISPLAY_LINE_MAX_AGE_MS_2);

    while (1) {
        text_msg_t msg;
        text_msg_t msg_2;
        bool got_msg = false;
        bool got_msg_2 = false;
        if (disp1_q) {
            if (xQueueReceive(disp1_q, &msg, pdMS_TO_TICKS(100)) == pdTRUE) {
                got_msg = true;
            }
        } else if (!disp2_q) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (disp2_q) {
            TickType_t wait_ticks = disp1_q ? 0 : pdMS_TO_TICKS(100);
            if (xQueueReceive(disp2_q, &msg_2, wait_ticks) == pdTRUE) {
                got_msg_2 = true;
            }
        }

        TickType_t now = xTaskGetTickCount();
        bool prune_needed = (now - last_prune) > pdMS_TO_TICKS(200);
        bool indicator_needed = (now - last_indicator_update) > pdMS_TO_TICKS(250);
        bool prune_needed_2 = (now - last_prune_2) > pdMS_TO_TICKS(200);

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

        if (got_msg_2 && log_area_2) {
            size_t copy_len = msg_2.len;
            if (copy_len > TEXT_BUF_SIZE) {
                copy_len = TEXT_BUF_SIZE;
            }
            char line_buf[TEXT_BUF_SIZE + 1];
            memcpy(line_buf, msg_2.payload, copy_len);
            line_buf[copy_len] = '\0';
            for (size_t i = 0; i < copy_len; i++) {
                if (line_buf[i] == '\r' || line_buf[i] == '\n') {
                    line_buf[i] = ' ';
                }
            }

            lvgl_port_lock(0);
            prune_expired_lines_with_age(lines_2, &line_count_2, now, max_age_2);
            add_wrapped_lines(lines_2, &line_count_2, max_lines_2, line_buf, now, log_font_2,
                              content_width_2, letter_space_2);
            rebuild_log_textarea(log_area_2, lines_2, line_count_2, max_lines_2);
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

        if (prune_needed_2 && log_area_2) {
            bool changed = false;
            lvgl_port_lock(0);
            int before = line_count_2;
            prune_expired_lines_with_age(lines_2, &line_count_2, now, max_age_2);
            changed = (before != line_count_2);
            if (changed) {
                rebuild_log_textarea(log_area_2, lines_2, line_count_2, max_lines_2);
            }
            lvgl_port_unlock();
            last_prune_2 = now;
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
#if SCREEN2_EARLY_PANEL_TEST
    ESP_LOGE(TAG, "display_make_tasks: early screen2 scan bands");
    screen2_panel_scan_bands();
    screen2_fill_color(0x0000);
    ESP_LOGE(TAG, "display_make_tasks: early screen2 scan bands done");
#endif
    ESP_ERROR_CHECK(app_lvgl_init());
    xTaskCreatePinnedToCore(display_task, "display_task", 8192, NULL, 6, NULL, 1);
}
