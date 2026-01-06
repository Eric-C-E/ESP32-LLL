# ESP LCD NV3041

NV3041 LCD controller driver for the ESP-IDF `esp_lcd` component (4 Wire SPI interface).

| LCD controller | Communication interface | Component name | Link to datasheet |
| :------------: | :---------------------: | :------------: | :---------------: |
| NV3041         | SPI                     | esp_lcd_nv3041 | [NV3041A datasheet](../../../NV_3041_A_Datasheet_V1_2_20221011_686486a221.pdf) |

## Add to project

This is a local ESP-IDF component. Add it to your project in one of these ways:

1. Copy `ESP32-LLL/components/esp_lcd_nv3041` into your project's `components/` folder.
2. Keep it elsewhere and register it via `EXTRA_COMPONENT_DIRS` in your top-level `CMakeLists.txt`:

```cmake
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/components")
```

## Example use

```c
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_nv3041.h"

ESP_LOGI(TAG, "Initialize SPI bus");
const spi_bus_config_t bus_config = NV3041_PANEL_BUS_SPI_CONFIG(EXAMPLE_PIN_NUM_LCD_PCLK,
                                                                EXAMPLE_PIN_NUM_LCD_MOSI,
                                                                EXAMPLE_LCD_H_RES * 80 * sizeof(uint16_t));
ESP_ERROR_CHECK(spi_bus_initialize(EXAMPLE_LCD_HOST, &bus_config, SPI_DMA_CH_AUTO));

ESP_LOGI(TAG, "Install panel IO");
esp_lcd_panel_io_handle_t io_handle = NULL;
const esp_lcd_panel_io_spi_config_t io_config = NV3041_PANEL_IO_SPI_CONFIG(EXAMPLE_PIN_NUM_LCD_CS,
                                                                           EXAMPLE_PIN_NUM_LCD_DC,
                                                                           example_callback,
                                                                           &example_callback_ctx);
ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)EXAMPLE_LCD_HOST,
                                        &io_config,
                                        &io_handle));

/**
 * Optional: override initialization commands.
 * The array should be declared as "static const" and positioned outside the function.
 */
// static const nv3041_lcd_init_cmd_t lcd_init_cmds[] = {
//     {cmd, { data }, data_size, delay_ms},
//     {0xff, (uint8_t []){0xa5}, 1, 0},
//     ...
// };

ESP_LOGI(TAG, "Install NV3041 panel driver");
esp_lcd_panel_handle_t panel_handle = NULL;
// nv3041_vendor_config_t vendor_config = {
//     .init_cmds = lcd_init_cmds,
//     .init_cmds_size = sizeof(lcd_init_cmds) / sizeof(nv3041_lcd_init_cmd_t),
// };
const esp_lcd_panel_dev_config_t panel_config = {
    .reset_gpio_num = EXAMPLE_PIN_NUM_LCD_RST,   // Set to -1 if not used
    .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,  // RGB element order: R-G-B
    .bits_per_pixel = 16,                        // Implemented by LCD command `3Ah` (16/18)
    // .vendor_config = &vendor_config,
};
ESP_ERROR_CHECK(esp_lcd_new_panel_nv3041(io_handle, &panel_config, &panel_handle));
ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));
```
