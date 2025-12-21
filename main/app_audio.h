#pragma once
#include "freertos/ringbuf.h"

void audio_make_tasks();

RingbufHandle_t audio_get_rb();