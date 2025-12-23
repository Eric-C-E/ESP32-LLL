/* Eric Liu 2025

Allocates and initializes I2S0 for IMNP 441 Stereo format
IMNP 441 data format is Phillips MSB format, 1 CLK cycle delayed data from WS edge
24 bits per channel, up to 2 channels per I2S bus

Pins 4, 5, 6 GPIO

Task reads data into a buffer and enqueues that in turn into a ringbuffer for use in other tasks

INPUTS: none
OUTPUTS: ringbuffer audio_rb interfaces with app_tcp_tx

*/

#include <stdint.h>
#include <stdlib.h>
#include <assert.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_err.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "freertos/ringbuf.h"

/* pins */

#define PIN_NUM_BCLK    4
#define PIN_NUM_WS      5
#define PIN_NUM_DIN     6

/* buffer size */
//multiple of 2 and 3 so it's very multipurpose works with frame depth of any size
#define INTERMEDIARY_BUF_SIZE   3072
#define RINGBUFFER_SIZE         32768 

static const char *TAG = "audio_task";

static i2s_chan_handle_t rx_handle; 

static RingbufHandle_t audio_rb;

/* initialization settings deviations from example norm are stated below*/
/* picked to be valid for ESP-32 S3 (I2S0 and 1 available, using system available)*/
/* IMNP 441 compatible deviations from normal: */
/* deviation: i2s_std_clk_config_t: sample_rate_hz 16000 for low mem use + for openai-whisper*/
/* deviation: mclk_multiple I2S_MCLK_MULTIPLE_384 since i2s_std_slot_config has .data_bit_width 24*/

static const i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);

static const i2s_std_config_t std_cfg = {
    .clk_cfg  = {
        .sample_rate_hz = 16000,
        .clk_src        = I2S_CLK_SRC_DEFAULT,
        .mclk_multiple  = I2S_MCLK_MULTIPLE_384,
        .bclk_div       = 8,
    },
    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_24BIT, I2S_SLOT_MODE_STEREO),
    .gpio_cfg = {
        .mclk = I2S_GPIO_UNUSED,
        .bclk = PIN_NUM_BCLK,
        .ws   = PIN_NUM_WS,
        .dout = I2S_GPIO_UNUSED,
        .din  = PIN_NUM_DIN,
        .invert_flags = {
            .mclk_inv = false,
            .bclk_inv = false,
            .ws_inv   = false,
        },
    },
};


static void i2s_init_std(void)
{
    /* Channel configs are set for IMNP441 microphone*/
    /* Channel configs tend to be plug and play*/
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &rx_handle));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle, &std_cfg));
}

static void init_audio_rb(void)
{
    audio_rb = xRingbufferCreate(RINGBUFFER_SIZE, RINGBUF_TYPE_BYTEBUF);
    assert(audio_rb);
    ESP_LOGD(TAG, "interface audio ringbuffer initialized");
}

RingbufHandle_t audio_get_rb(void)
{
    assert(audio_rb);
    return audio_rb;
}

static void i2s_read_task(void *args)
{   
    /* init intermed buffer*/
    uint8_t *int_buf = (uint8_t *)calloc(1, INTERMEDIARY_BUF_SIZE);
    assert(int_buf);
    size_t int_bytes = 0;
    ESP_LOGI(TAG, "intermediary buffer initialized");

    /* Enable RX channel */
    ESP_ERROR_CHECK(i2s_channel_enable(rx_handle));
    ESP_LOGI(TAG, "audio task running");

    /* IMPORTANT: next bit must be very fast to avoid DMA buffer overflow data loss*/
    /* around 30 ms expected, timeout 500*/
    while(1){
        if (i2s_channel_read(rx_handle, int_buf, INTERMEDIARY_BUF_SIZE, &int_bytes, 500) == ESP_OK) {
            ESP_LOGD(TAG, "audio read task read %zu bytes", int_bytes);

            BaseType_t ok = xRingbufferSend(audio_rb, int_buf, int_bytes, pdMS_TO_TICKS(5));
            if (ok != pdTRUE) {
                ESP_LOGD(TAG, "failed ringbuffer push"); //remove logging for live
            }
        }
        else {
            ESP_LOGD(TAG, "audio read task FAILED");
        }
        /*here put vTaskDelay for testing*/ 
        vTaskDelay(30);
    }
    free(int_buf);
    vTaskDelete(NULL);
}

void audio_make_tasks(void)
{
    init_audio_rb();
    i2s_init_std();
    xTaskCreate(i2s_read_task, "i2s_read_task", 4096, NULL, 8, NULL);
}
