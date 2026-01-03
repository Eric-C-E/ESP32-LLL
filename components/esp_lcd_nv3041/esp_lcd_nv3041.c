/*
 * SPDX-FileCopyrightText: 2021-2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <sys/cdefs.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_commands.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_check.h"

#include "esp_lcd_nv3041.h"

static const char *TAG = "lcd_panel.nv3041";

static esp_err_t panel_nv3041_del(esp_lcd_panel_t *panel);
static esp_err_t panel_nv3041_reset(esp_lcd_panel_t *panel);
static esp_err_t panel_nv3041_init(esp_lcd_panel_t *panel);
static esp_err_t panel_nv3041_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start, int x_end, int y_end, const void *color_data);
static esp_err_t panel_nv3041_invert_color(esp_lcd_panel_t *panel, bool invert_color_data);
static esp_err_t panel_nv3041_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y);
static esp_err_t panel_nv3041_swap_xy(esp_lcd_panel_t *panel, bool swap_axes);
static esp_err_t panel_nv3041_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap);
static esp_err_t panel_nv3041_disp_on_off(esp_lcd_panel_t *panel, bool off);

typedef struct {
    esp_lcd_panel_t base;
    esp_lcd_panel_io_handle_t io;
    int reset_gpio_num;
    bool reset_level;
    int x_gap;
    int y_gap;
    uint8_t fb_bits_per_pixel;
    uint8_t madctl_val; // save current value of LCD_CMD_MADCTL register
    uint8_t colmod_val; // save current value of LCD_CMD_COLMOD register
    const nv3041_lcd_init_cmd_t *init_cmds;
    uint16_t init_cmds_size;
} nv3041_panel_t;

esp_err_t esp_lcd_new_panel_nv3041(const esp_lcd_panel_io_handle_t io, const esp_lcd_panel_dev_config_t *panel_dev_config, esp_lcd_panel_handle_t *ret_panel)
{
    esp_err_t ret = ESP_OK;
    nv3041_panel_t *nv3041 = NULL;
    gpio_config_t io_conf = { 0 };

    ESP_GOTO_ON_FALSE(io && panel_dev_config && ret_panel, ESP_ERR_INVALID_ARG, err, TAG, "invalid argument");
    nv3041 = (nv3041_panel_t *)calloc(1, sizeof(nv3041_panel_t));
    ESP_GOTO_ON_FALSE(nv3041, ESP_ERR_NO_MEM, err, TAG, "no mem for nv3041 panel");

    if (panel_dev_config->reset_gpio_num >= 0) {
        io_conf.mode = GPIO_MODE_OUTPUT;
        io_conf.pin_bit_mask = 1ULL << panel_dev_config->reset_gpio_num;
        ESP_GOTO_ON_ERROR(gpio_config(&io_conf), err, TAG, "configure GPIO for RST line failed");
    }

#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 0, 0)
    switch (panel_dev_config->color_space) {
    case ESP_LCD_COLOR_SPACE_RGB:
        nv3041->madctl_val = 0;
        break;
    case ESP_LCD_COLOR_SPACE_BGR:
        nv3041->madctl_val |= LCD_CMD_BGR_BIT;
        break;
    default:
        ESP_GOTO_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, err, TAG, "unsupported color space");
        break;
    }
#elif ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(6, 0, 0)
    switch (panel_dev_config->rgb_endian) {
    case LCD_RGB_ENDIAN_RGB:
        nv3041->madctl_val = 0;
        break;
    case LCD_RGB_ENDIAN_BGR:
        nv3041->madctl_val |= LCD_CMD_BGR_BIT;
        break;
    default:
        ESP_GOTO_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, err, TAG, "unsupported rgb endian");
        break;
    }
#else
    switch (panel_dev_config->rgb_endian) {
    case LCD_RGB_ELEMENT_ORDER_RGB:
        nv3041->madctl_val = 0;
        break;
    case LCD_RGB_ELEMENT_ORDER_BGR:
        nv3041->madctl_val |= LCD_CMD_BGR_BIT;
        break;
    default:
        ESP_GOTO_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, err, TAG, "unsupported rgb endian");
        break;
    }
#endif

    switch (panel_dev_config->bits_per_pixel) {
    case 12: // SCREEN DOES NOT SUPPORT RGB444
        nv3041->colmod_val = 0x33;
        nv3041->fb_bits_per_pixel = 16;
        break;
    case 16: // RGB565
        nv3041->colmod_val = 0x55;
        nv3041->fb_bits_per_pixel = 16;
        break;
    case 18: // RGB666
        nv3041->colmod_val = 0x66;
        // each color component (R/G/B) should occupy the 6 high bits of a byte, which means 3 full bytes are required for a pixel
        nv3041->fb_bits_per_pixel = 24;
        break;
    default:
        ESP_GOTO_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, err, TAG, "unsupported pixel width");
        break;
    }

    nv3041->io = io;
    nv3041->reset_gpio_num = panel_dev_config->reset_gpio_num;
    nv3041->reset_level = panel_dev_config->flags.reset_active_high;
    if (panel_dev_config->vendor_config) {
        nv3041->init_cmds = ((nv3041_vendor_config_t *)panel_dev_config->vendor_config)->init_cmds;
        nv3041->init_cmds_size = ((nv3041_vendor_config_t *)panel_dev_config->vendor_config)->init_cmds_size;
    }
    nv3041->base.del = panel_nv3041_del;
    nv3041->base.reset = panel_nv3041_reset;
    nv3041->base.init = panel_nv3041_init;
    nv3041->base.draw_bitmap = panel_nv3041_draw_bitmap;
    nv3041->base.invert_color = panel_nv3041_invert_color;
    nv3041->base.set_gap = panel_nv3041_set_gap;
    nv3041->base.mirror = panel_nv3041_mirror;
    nv3041->base.swap_xy = panel_nv3041_swap_xy;
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 0, 0)
    nv3041->base.disp_off = panel_nv3041_disp_on_off;
#else
    nv3041->base.disp_on_off = panel_nv3041_disp_on_off;
#endif
    *ret_panel = &(nv3041->base);
    ESP_LOGD(TAG, "new nv3041 panel @%p", nv3041);

    ESP_LOGI(TAG, "LCD panel create success, version: %d.%d.%d", ESP_LCD_NV3041_VER_MAJOR, ESP_LCD_NV3041_VER_MINOR,
             ESP_LCD_NV3041_VER_PATCH);

    return ESP_OK;

err:
    if (nv3041) {
        if (panel_dev_config->reset_gpio_num >= 0) {
            gpio_reset_pin(panel_dev_config->reset_gpio_num);
        }
        free(nv3041);
    }
    return ret;
}

static esp_err_t panel_nv3041_del(esp_lcd_panel_t *panel)
{
    nv3041_panel_t *nv3041 = __containerof(panel, nv3041_panel_t, base);

    if (nv3041->reset_gpio_num >= 0) {
        gpio_reset_pin(nv3041->reset_gpio_num);
    }
    ESP_LOGD(TAG, "del nv3041 panel @%p", nv3041);
    free(nv3041);
    return ESP_OK;
}

static esp_err_t panel_nv3041_reset(esp_lcd_panel_t *panel)
{
    nv3041_panel_t *nv3041 = __containerof(panel, nv3041_panel_t, base);
    esp_lcd_panel_io_handle_t io = nv3041->io;

    // perform hardware reset
    if (nv3041->reset_gpio_num >= 0) {
        gpio_set_level(nv3041->reset_gpio_num, nv3041->reset_level);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(nv3041->reset_gpio_num, !nv3041->reset_level);
        vTaskDelay(pdMS_TO_TICKS(10));
    } else { // perform software reset
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_SWRESET, NULL, 0), TAG, "send command failed");
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    return ESP_OK;
}

//modified by Eric-C-E
//Driver: NV3041A-01, 3.9 inch TFT
//Minimal: only sets what is absolutely necessary > refer datasheet for tuning
static const nv3041_lcd_init_cmd_t vendor_specific_init_default[] = {
//  {cmd, { data }, data_size, delay_ms}
    {LCD_CMD_DISPON, NULL, 0, 20},





    // Set Gamma P




    // Set Gamma N






};

static esp_err_t panel_nv3041_init(esp_lcd_panel_t *panel)
{
    nv3041_panel_t *nv3041 = __containerof(panel, nv3041_panel_t, base);
    esp_lcd_panel_io_handle_t io = nv3041->io;

    // LCD goes into sleep mode and display will be turned off after power on reset, exit sleep mode first
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_SLPOUT, NULL, 0), TAG, "send command failed");
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_MADCTL, (uint8_t[]) {
        nv3041->madctl_val,
    }, 1), TAG, "send command failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_COLMOD, (uint8_t[]) {
        nv3041->colmod_val,
    }, 1), TAG, "send command failed");

    const nv3041_lcd_init_cmd_t *init_cmds = NULL;
    uint16_t init_cmds_size = 0;
    if (nv3041->init_cmds) {
        init_cmds = nv3041->init_cmds;
        init_cmds_size = nv3041->init_cmds_size;
    } else {
        init_cmds = vendor_specific_init_default;
        init_cmds_size = sizeof(vendor_specific_init_default) / sizeof(nv3041_lcd_init_cmd_t);
    }

    bool is_cmd_overwritten = false;
    for (int i = 0; i < init_cmds_size; i++) {
        // Check if the command has been used or conflicts with the internal
        switch (init_cmds[i].cmd) {
        case LCD_CMD_MADCTL:
            is_cmd_overwritten = true;
            nv3041->madctl_val = ((uint8_t *)init_cmds[i].data)[0];
            break;
        case LCD_CMD_COLMOD:
            is_cmd_overwritten = true;
            nv3041->colmod_val = ((uint8_t *)init_cmds[i].data)[0];
            break;
        default:
            is_cmd_overwritten = false;
            break;
        }

        if (is_cmd_overwritten) {
            ESP_LOGW(TAG, "The %02Xh command has been used and will be overwritten by external initialization sequence", init_cmds[i].cmd);
        }

        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, init_cmds[i].cmd, init_cmds[i].data, init_cmds[i].data_bytes), TAG, "send command failed");
        vTaskDelay(pdMS_TO_TICKS(init_cmds[i].delay_ms));
    }
    ESP_LOGD(TAG, "send init commands success");

    return ESP_OK;
}

static esp_err_t panel_nv3041_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start, int x_end, int y_end, const void *color_data)
{
    nv3041_panel_t *nv3041 = __containerof(panel, nv3041_panel_t, base);
    assert((x_start < x_end) && (y_start < y_end) && "start position must be smaller than end position");
    esp_lcd_panel_io_handle_t io = nv3041->io;

    x_start += nv3041->x_gap;
    x_end += nv3041->x_gap;
    y_start += nv3041->y_gap;
    y_end += nv3041->y_gap;

    // define an area of frame memory where MCU can access
    esp_lcd_panel_io_tx_param(io, LCD_CMD_CASET, (uint8_t[]) {
        (x_start >> 8) & 0xFF,
        x_start & 0xFF,
        ((x_end - 1) >> 8) & 0xFF,
        (x_end - 1) & 0xFF,
    }, 4);
    esp_lcd_panel_io_tx_param(io, LCD_CMD_RASET, (uint8_t[]) {
        (y_start >> 8) & 0xFF,
        y_start & 0xFF,
        ((y_end - 1) >> 8) & 0xFF,
        (y_end - 1) & 0xFF,
    }, 4);
    // transfer frame buffer
    size_t len = (x_end - x_start) * (y_end - y_start) * nv3041->fb_bits_per_pixel / 8;
    esp_lcd_panel_io_tx_color(io, LCD_CMD_RAMWR, color_data, len);

    return ESP_OK;
}

static esp_err_t panel_nv3041_invert_color(esp_lcd_panel_t *panel, bool invert_color_data)
{
    nv3041_panel_t *nv3041 = __containerof(panel, nv3041_panel_t, base);
    esp_lcd_panel_io_handle_t io = nv3041->io;
    int command = 0;
    if (invert_color_data) {
        command = LCD_CMD_INVON;
    } else {
        command = LCD_CMD_INVOFF;
    }
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, command, NULL, 0), TAG, "send command failed");
    return ESP_OK;
}

static esp_err_t panel_nv3041_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y)
{
    nv3041_panel_t *nv3041 = __containerof(panel, nv3041_panel_t, base);
    esp_lcd_panel_io_handle_t io = nv3041->io;
    if (mirror_x) {
        nv3041->madctl_val |= LCD_CMD_MX_BIT;
    } else {
        nv3041->madctl_val &= ~LCD_CMD_MX_BIT;
    }
    if (mirror_y) {
        nv3041->madctl_val |= LCD_CMD_MY_BIT;
    } else {
        nv3041->madctl_val &= ~LCD_CMD_MY_BIT;
    }
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_MADCTL, (uint8_t[]) {
        nv3041->madctl_val
    }, 1), TAG, "send command failed");
    return ESP_OK;
}

static esp_err_t panel_nv3041_swap_xy(esp_lcd_panel_t *panel, bool swap_axes)
{
    nv3041_panel_t *nv3041 = __containerof(panel, nv3041_panel_t, base);
    esp_lcd_panel_io_handle_t io = nv3041->io;
    if (swap_axes) {
        nv3041->madctl_val |= LCD_CMD_MV_BIT;
    } else {
        nv3041->madctl_val &= ~LCD_CMD_MV_BIT;
    }
    esp_lcd_panel_io_tx_param(io, LCD_CMD_MADCTL, (uint8_t[]) {
        nv3041->madctl_val
    }, 1);
    return ESP_OK;
}

static esp_err_t panel_nv3041_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap)
{
    nv3041_panel_t *nv3041 = __containerof(panel, nv3041_panel_t, base);
    nv3041->x_gap = x_gap;
    nv3041->y_gap = y_gap;
    return ESP_OK;
}

static esp_err_t panel_nv3041_disp_on_off(esp_lcd_panel_t *panel, bool on_off)
{
    nv3041_panel_t *nv3041 = __containerof(panel, nv3041_panel_t, base);
    esp_lcd_panel_io_handle_t io = nv3041->io;
    int command = 0;

#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 0, 0)
    on_off = !on_off;
#endif

    if (on_off) {
        command = LCD_CMD_DISPON;
    } else {
        command = LCD_CMD_DISPOFF;
    }
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, command, NULL, 0), TAG, "send command failed");
    return ESP_OK;
}
