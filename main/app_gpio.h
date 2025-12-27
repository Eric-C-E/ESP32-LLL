#pragma once

#include "driver/gpio.h"


#ifndef APP_GPIO_BUTTON1_PIN
#define APP_GPIO_BUTTON1_PIN GPIO_NUM_47
#endif

#ifndef APP_GPIO_BUTTON2_PIN
#define APP_GPIO_BUTTON2_PIN GPIO_NUM_48
#endif


/* define states */
typedef enum {
    APP_GPIO_STATE_IDLE = 0,
    APP_GPIO_STATE_TRANSLATE_LANG1,
    APP_GPIO_STATE_TRANSLATE_LANG2,
} app_gpio_state_t;


app_gpio_state_t gpio_get_state(void);
void gpio_make_tasks(void);
