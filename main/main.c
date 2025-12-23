#include "nvs_flash.h"
#include "esp_netif.h"
#include "protocol_examples_common.h"
#include "esp_log.h"
#include "esp_event.h"

#include "app_display.h"
#include "app_audio.h"
#include "app_gpio.h"
#include "app_tcp.h"
#include "app_wifi.h"

static const char *TAG = "app_main";

void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_DEBUG);
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(err);
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_LOGD(TAG, "trying to init audio, display, gpio, TCP TX tasks");

    wifi_make_tasks();
    audio_make_tasks();
    display_make_tasks();
    gpio_make_tasks();
    tcp_make_tasks();
}
