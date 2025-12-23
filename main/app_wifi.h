#pragma once

#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#define WIFI_STATUS_CONNECTED (1U << 0)

EventGroupHandle_t wifi_get_event_group(void);
int8_t wifi_get_rssi(void);
void wifi_make_tasks(void);
