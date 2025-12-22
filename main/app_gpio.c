/* Eric Liu 2025

Monitors two GPIO inputs and publishes a debounced FSM state:
idle, translate_lang1, translate_lang2.

INPUTS: button 1, button 2
OUTPUTS: app_gpio_get_state() for other tasks

*/

#include "app_gpio.h"
#include <assert.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"

#define APP_GPIO_BUTTON_ACTIVE_LEVEL 0
#define APP_GPIO_DEBOUNCE_COUNT 3
#define APP_GPIO_POLL_MS 10

static const char *TAG = "gpio_task";

typedef enum {
    APP_GPIO_LAST_NONE = 0,
    APP_GPIO_LAST_BTN1,
    APP_GPIO_LAST_BTN2,
} app_gpio_last_t;

typedef struct {
    int stable_level;
    uint8_t stable_count;
    bool pressed;
} app_gpio_debounce_t;

static app_gpio_state_t gpio_state = APP_GPIO_STATE_IDLE;
static portMUX_TYPE gpio_state_mux = portMUX_INITIALIZER_UNLOCKED;
static app_gpio_last_t last_pressed = APP_GPIO_LAST_NONE;

static void app_gpio_init_inputs(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << APP_GPIO_BUTTON1_PIN) | (1ULL << APP_GPIO_BUTTON2_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));
}

static void app_gpio_debounce_init(app_gpio_debounce_t *db, int level)
{
    db->stable_level = level;
    db->stable_count = 0;
    db->pressed = (level == APP_GPIO_BUTTON_ACTIVE_LEVEL);
}

static void app_gpio_debounce_update(app_gpio_debounce_t *db, int level, bool *edge_pressed, bool *edge_released)
{
    *edge_pressed = false;
    *edge_released = false;

    if (level == db->stable_level) {
        db->stable_count = 0;
        return;
    }

    db->stable_count++;
    if (db->stable_count < APP_GPIO_DEBOUNCE_COUNT) {
        return;
    }

    db->stable_count = 0;
    db->stable_level = level;

    bool new_pressed = (level == APP_GPIO_BUTTON_ACTIVE_LEVEL);
    if (new_pressed != db->pressed) {
        db->pressed = new_pressed;
        if (new_pressed) {
            *edge_pressed = true;
        } else {
            *edge_released = true;
        }
    }
}

static void app_gpio_set_state(app_gpio_state_t new_state)
{
    portENTER_CRITICAL(&gpio_state_mux);
    gpio_state = new_state;
    portEXIT_CRITICAL(&gpio_state_mux);
}

app_gpio_state_t gpio_get_state(void)
{
    app_gpio_state_t state;
    portENTER_CRITICAL(&gpio_state_mux);
    state = gpio_state;
    portEXIT_CRITICAL(&gpio_state_mux);
    return state;
}

static void app_gpio_task(void *args)
{
    app_gpio_debounce_t btn1;
    app_gpio_debounce_t btn2;

    app_gpio_debounce_init(&btn1, gpio_get_level(APP_GPIO_BUTTON1_PIN));
    app_gpio_debounce_init(&btn2, gpio_get_level(APP_GPIO_BUTTON2_PIN));

    ESP_LOGI(TAG, "gpio task running");

    while (1) {
        bool btn1_edge_press = false;
        bool btn1_edge_release = false;
        bool btn2_edge_press = false;
        bool btn2_edge_release = false;

        app_gpio_debounce_update(&btn1, gpio_get_level(APP_GPIO_BUTTON1_PIN), &btn1_edge_press, &btn1_edge_release);
        app_gpio_debounce_update(&btn2, gpio_get_level(APP_GPIO_BUTTON2_PIN), &btn2_edge_press, &btn2_edge_release);

        if (btn1_edge_press) {
            last_pressed = APP_GPIO_LAST_BTN1;
        }
        if (btn2_edge_press) {
            last_pressed = APP_GPIO_LAST_BTN2;
        }
        (void)btn1_edge_release;
        (void)btn2_edge_release;

        app_gpio_state_t new_state;
        if (btn1.pressed && btn2.pressed) {
            new_state = (last_pressed == APP_GPIO_LAST_BTN2) ? APP_GPIO_STATE_TRANSLATE_LANG2 : APP_GPIO_STATE_TRANSLATE_LANG1;
        } else if (btn1.pressed) {
            new_state = APP_GPIO_STATE_TRANSLATE_LANG1;
        } else if (btn2.pressed) {
            new_state = APP_GPIO_STATE_TRANSLATE_LANG2;
        } else {
            new_state = APP_GPIO_STATE_IDLE;
        }

        if (new_state != gpio_get_state()) {
            app_gpio_set_state(new_state);
            ESP_LOGI(TAG, "state -> %d", new_state);
        }

        vTaskDelay(pdMS_TO_TICKS(APP_GPIO_POLL_MS));
    }
}

void gpio_make_tasks(void)
{
    app_gpio_init_inputs();
    xTaskCreate(app_gpio_task, "gpio_task", 2048, NULL, 9, NULL);
}
