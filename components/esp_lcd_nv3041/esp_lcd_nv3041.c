/*
 * SPDX-FileCopyrightText: 2021-2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
//adapted for nv3041a by Eric-C-E
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
    switch (panel_dev_config->rgb_ele_order) {
    case LCD_RGB_ELEMENT_ORDER_RGB:
        nv3041->madctl_val = 0;
        break;
    case LCD_RGB_ELEMENT_ORDER_BGR:
        nv3041->madctl_val |= LCD_CMD_BGR_BIT;
        break;
    default:
        ESP_GOTO_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, err, TAG, "unsupported rgb element order");
        break;
    }
#endif

    switch (panel_dev_config->bits_per_pixel) {
    case 12: // SCREEN DOES NOT SUPPORT RGB444
        nv3041->colmod_val = 0x01;
        nv3041->fb_bits_per_pixel = 16;
        break;
    case 16: // RGB565
        nv3041->colmod_val = 0x01;
        nv3041->fb_bits_per_pixel = 16;
        break;
    case 18: // RGB666
        nv3041->colmod_val = 0x00;
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
        vTaskDelay(pdMS_TO_TICKS(120));
        gpio_set_level(nv3041->reset_gpio_num, !nv3041->reset_level);
        vTaskDelay(pdMS_TO_TICKS(120));
    } else { // perform software reset
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_SWRESET, NULL, 0), TAG, "send command failed");
        vTaskDelay(pdMS_TO_TICKS(120)); //spec, wait at least 120ms before sending new command
    }

    return ESP_OK;
}

//modified by Eric-C-E
//Driver: NV3041A-01, 3.9 inch TFT
//Minimal: only sets what is absolutely necessary > refer datasheet for tuning
static const nv3041_lcd_init_cmd_t vendor_specific_init_default[] = {
//  {cmd, { data }, data_size, delay_ms}
    {0xff, (uint8_t []){0xa5}, 1, 0},   //ENABLE registers, undocumented register
    {0x36, (uint8_t []){0xc0}, 1, 0},   //MADCTL 
    {0x3a, (uint8_t []){0x01}, 1, 0},   //COLMOD 
    //{0x4a, (uint8_t []){0x00, 0x7f}, 2, 0},   //SCAN_VRES vertical resolution setting max 271
    //{0x4b, (uint8_t []){0x01, 0xdf}, 2, 0},   //SCAN_HRES horizontal resolution setting max 479
    {0x41, (uint8_t []){0x03}, 1, 0},   //Bus Width
    {0x44, (uint8_t []){0x15}, 1, 0},   //vbp 
    {0x45, (uint8_t []){0x15}, 1, 0},   //vfp
    {0x7d, (uint8_t []){0x03}, 1, 0},   //vdds_trim[2:0]
    {0xc1, (uint8_t []){0xbb}, 1, 0},   //avdd_clp_en | avdd_clp[1:0] | avdd_clp_en | acvl_clp[1:0]
    {0xc2, (uint8_t []){0x05}, 1, 0},   //vgh_clp_en | vgh_clp[2:0]
    {0xc3, (uint8_t []){0x10}, 1, 0},   //vgl_clp_en | vgl_clp[2:0]
    {0xc6, (uint8_t []){0x3e}, 1, 0},   //avdd_ratio_sel | avcl_ratio_sel | vgh_ratio_sel[1:0] | vgl_ratio_sel[1:0] 
    {0xc7, (uint8_t []){0x25}, 1, 0},   //mv_clk_sel | avdd_clk_sel[1:0] | avcl_clk_sel[1:0] 
    {0xc8, (uint8_t []){0x11}, 1, 0},   //vgl_clk_sel
    {0x7a, (uint8_t []){0x5f}, 1, 0},   //usr_vgsp[6:0] 
    {0x6f, (uint8_t []){0x44}, 1, 0},   //usr_gvdd[6:0]
    {0x78, (uint8_t []){0x70}, 1, 0},   //usr_gvcl[6:0]
    {0xc9, (uint8_t []){0x00}, 1, 0},   //avdd_fd_bk_en | avcl_fd_bk_en | vgh_freq_en | avdd_freq_en | avcl_freq_en
    {0x67, (uint8_t []){0x21}, 1, 0},   //undocumented register

    //GATE_Setting
    {0x51, (uint8_t []){0x0a}, 1, 0},   //gate_st_o[7:0]
    {0x52, (uint8_t []){0x76}, 1, 0},   //gate_ed_o[7:0]
    {0x53, (uint8_t []){0x0a}, 1, 0},   //gate_st_e[7:0]
    {0x54, (uint8_t []){0x76}, 1, 0},   //gate_sd_e[7:0]

    //FSM_V-Porch
    {0x46, (uint8_t []){0x0a}, 1, 0},   //fsm_hbp_o[5:0]
    {0x47, (uint8_t []){0x2a}, 1, 0},   //fsm_hfp_o[5:0]
    {0x48, (uint8_t []){0x0a}, 1, 0},   //fsm_hbp_e[5:0]
    {0x49, (uint8_t []){0x1a}, 1, 0},   //fsm_hfp_e[5:0]

    //SRC registers
    {0x56, (uint8_t []){0x43}, 1, 0},   //src_ld_wd[1:0] | src_ld_st[5:0]
    {0x57, (uint8_t []){0x42}, 1, 0},   //pn_cs_en | src_cs_st [5:0] 
    {0x58, (uint8_t []){0x3c}, 1, 0},   //src_cs_p_wd[6:0]
    {0x59, (uint8_t []){0x64}, 1, 0},   //src_cs_n_wd[6:0]
    {0x5a, (uint8_t []){0x41}, 1, 0},   //src_pchg_st_o[6:0]
    {0x5b, (uint8_t []){0x3c}, 1, 0},   //src_pchg_wd_o[6:0]
    {0x5c, (uint8_t []){0x02}, 1, 0},   //src_pchg_st_e[6:0]
    {0x5d, (uint8_t []){0x3c}, 1, 0},   //src_pchg_wd_e[6:0]
    {0x5e, (uint8_t []){0x1f}, 1, 0},   //src_pol_sw[7:0]
    {0x60, (uint8_t []){0x80}, 1, 0},   //src_op_st_o[7:0]
    {0x61, (uint8_t []){0x3f}, 1, 0},   //src_op_st_e[7:0]
    {0x62, (uint8_t []){0x21}, 1, 0},   //src_op_ed_o[9:8] | src_op_ed_e[9:8]
    {0x63, (uint8_t []){0x07}, 1, 0},   //src_op_ed_o[7:0]
    {0x64, (uint8_t []){0xe0}, 1, 0},   //src_op_ed_e[7:0]
    {0x65, (uint8_t []){0x02}, 1, 0},   //gamma_chop_en | src_ofc_sel[2:0]

    //undocumented registers
    {0xca, (uint8_t []){0x20}, 1, 0},   //avdd_mux_st_o[7:0]
    {0xcb, (uint8_t []){0x52}, 1, 0},   //avdd_mux_ed_o[7:0]
    {0xcc, (uint8_t []){0x10}, 1, 0},   //avdd_mux_st_e[7:0]
    {0xcd, (uint8_t []){0x42}, 1, 0},   //avdd_mux_ed_e[7:0]
    {0xd0, (uint8_t []){0x20}, 1, 0},   //avcl_mux_st_o[7:0]
    {0xd1, (uint8_t []){0x52}, 1, 0},   //avcl_mux_ed_o[7:0]
    {0xd2, (uint8_t []){0x10}, 1, 0},   //avcl_mux_st_e[7:0]
    {0xd3, (uint8_t []){0x42}, 1, 0},   //avcl_mux_ed_e[7:0]
    {0xd4, (uint8_t []){0x0a}, 1, 0},   //vgh_mux_st[7:0]
    {0xd5, (uint8_t []){0x32}, 1, 0},   //vgh_mux_ed[7:0]

    //Gamma P
    {0x80, (uint8_t []){0x00, 0x07, 0x02, 0x37, 0x35,
                        0x3f, 0x11, 0x27, 0x0b, 0x14,
                        0x1a, 0x0a, 0x14, 0x17, 0x16, 
                        0x1b, 0x04, 0x0a, 0x16}, 19, 0},
    
    //Gamma N
    {0xa0, (uint8_t []){0x00, 0x06, 0x01, 0x37, 0x35, 
                        0x3f, 0x10, 0x27, 0x0b, 0x14,
                        0x1a, 0x0a, 0x08, 0x07, 0x06,
                        0x07, 0x04, 0x0a, 0x15}, 19, 0},

    //end write registers (undocumented)
    {0xFF, (uint8_t []){0x00}, 1, 0},

    //exit sleep again, wait 120 ms (min 120 ms)
    {0x11, (uint8_t []){0x00}, 1, 120},

    //dispon, wait 100 ms (CONTEMPORARY 0X28 DISPOFF)
    {0x29, (uint8_t []){0x00}, 1, 100},
    
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
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_CASET, (uint8_t[]) {
        (x_start >> 8) & 0xFF,
        x_start & 0xFF,
        ((x_end - 1) >> 8) & 0xFF,
        (x_end - 1) & 0xFF,
    }, 4), TAG, "send command failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_RASET, (uint8_t[]) {
        (y_start >> 8) & 0xFF,
        y_start & 0xFF,
        ((y_end - 1) >> 8) & 0xFF,
        (y_end - 1) & 0xFF,
    }, 4), TAG, "send command failed");
    // transfer frame buffer
    size_t len = (x_end - x_start) * (y_end - y_start) * nv3041->fb_bits_per_pixel / 8;
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_color(io, LCD_CMD_RAMWR, color_data, len), TAG, "send color failed");

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
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_MADCTL, (uint8_t[]) {
        nv3041->madctl_val
    }, 1), TAG, "send command failed");
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
