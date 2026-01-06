/* Eric Liu 2025

Non-blocking Wi-Fi connect task with status event group + RSSI tracking.
*/

#include "app_wifi.h"

#include "protocol_examples_common.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "wifi_task";

static EventGroupHandle_t wifi_event_group;
static int8_t wifi_rssi = -127;

static void wifi_task(void *args)
{
    bool was_connected = false;

    ESP_LOGD(TAG, "WiFi initializing...");
    esp_err_t err = example_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "WiFi connect failed: %s", esp_err_to_name(err));
    }

    while (1) {
        wifi_ap_record_t ap_info;
        bool connected = (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK);

        if (connected) {
            wifi_rssi = ap_info.rssi;
            xEventGroupSetBits(wifi_event_group, WIFI_STATUS_CONNECTED);
            if (!was_connected) {
                ESP_LOGD(TAG, "WiFi connected");
            }
        } else {
            xEventGroupClearBits(wifi_event_group, WIFI_STATUS_CONNECTED);
            if (was_connected) {
                ESP_LOGD(TAG, "WiFi disconnected");
            }
            esp_wifi_connect();
        }

        was_connected = connected;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

EventGroupHandle_t wifi_get_event_group(void)
{
    return wifi_event_group;
}

int8_t wifi_get_rssi(void)
{
    return wifi_rssi;
}

void wifi_make_tasks(void)
{
    wifi_event_group = xEventGroupCreate();
    if (!wifi_event_group) {
        ESP_LOGE(TAG, "Failed to create WiFi event group");
        return;
    }
    xTaskCreatePinnedToCore(wifi_task, "wifi_task", 4096, NULL, 8, NULL, 0);
}
