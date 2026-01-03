#ifndef APP_DISPLAY_H
#define APP_DISPLAY_H

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t app_lcd_init(void);
esp_err_t app_lcd_deinit(void);
esp_err_t app_lvgl_init(void);
esp_err_t app_lvgl_deinit(void);
//void app_main_display(void);
void display_task(void *arg);
void display_make_tasks(void);
//void check_leak(size_t start_free, size_t end_free, const char *type);

#ifdef __cplusplus
}
#endif

#endif /* APP_DISPLAY_H */
