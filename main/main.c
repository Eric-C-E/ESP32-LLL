#include "nvs_flash.h"

#include "esp_log.h"

#include "app_runtime.h"
#include "app_tasks.h"

static const char *TAG = "app_main";

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(err);
    }

    app_runtime_init();

    ESP_LOGI(TAG, "Starting tasks");
    app_start_display_task();
}
