idf_component_register(
    SRCS
        "main.c"
        "app_audio.c"
        "app_display.c"
        "app_gpio.c"
        "app_wifi.c"
        "app_tcp.c"
       
    INCLUDE_DIRS
        "."
    PRIV_REQUIRES
        driver
        esp_common
        esp_lvgl_port
        freertos
        heap
        log
        nvs_flash
        esp_wifi
        esp_lcd
        esp_lcd_gc9a01
        lvgl
        esp_ringbuf
        esp_netif
        esp_lcd_nv3041
    REQUIRES esp_driver_i2s
)
