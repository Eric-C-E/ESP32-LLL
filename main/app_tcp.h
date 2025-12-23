#pragma once

#include <stdint.h>
#include "freertos/queue.h"

/* the following is simple enough to not need macros */

typedef struct __attribute__((packed)) {
    uint8_t magic; 
    uint8_t version;
    uint8_t msg_type;     // AUDIO = 1, TEXT = 2, CONTROL = 3
    uint8_t flags;        // LANG1 = 1, LANG2 = 2, SCREEN1 = 4, SCREEN2 = 8
    uint32_t payload_len; // bytes after header
} msg_hdr_t;

QueueHandle_t tcp_rx_get_disp1_q(void);
QueueHandle_t tcp_rx_get_disp2_q(void);

void tcp_make_tasks();