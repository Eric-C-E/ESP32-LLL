#pragma once

#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"

#define APP_TEXT_MAX_BYTES 256
#define APP_AUDIO_CHUNK_BYTES 640

typedef enum {
    APP_BTN_1 = 1,
    APP_BTN_2 = 2,
} app_button_id_t;

typedef enum {
    APP_BTN_DOWN = 1,
    APP_BTN_UP = 2,
} app_button_action_t;

typedef struct {
    app_button_id_t button_id;
    app_button_action_t action;
    uint32_t timestamp_ms;
} app_button_event_t;

typedef struct {
    uint8_t display_id;
    char text[APP_TEXT_MAX_BYTES];
} app_text_update_t;

typedef struct {
    uint8_t source_id;
    uint32_t seq;
    uint64_t timestamp_us;
    uint16_t pcm_len;
    uint8_t pcm[APP_AUDIO_CHUNK_BYTES];
} app_audio_chunk_t;

typedef struct {
    QueueHandle_t button_events;
    QueueHandle_t text_updates;
    QueueHandle_t audio_chunks;
    EventGroupHandle_t state_bits;
} app_runtime_t;

enum {
    APP_STATE_BTN1_ACTIVE = (1u << 0),
    APP_STATE_BTN2_ACTIVE = (1u << 1),
};

app_runtime_t *app_runtime_get(void);
void app_runtime_init(void);
