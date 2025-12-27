/*Eric Liu 2025
based on example code by Espressif Systems Co. LTD

This code constitutes a freeRTOS task for TCP connection.
shares fd with tcp rx task
fd is created here. Connected in this task. rx task uses existing
socket

Inputs: ringbuffer audio_rb
Outputs: none

Also included is a freeRTOS task for TCP rx.
Receives short strings, and enqueues them for use by Graphics task

Inputs: none
Outputs: queue text_queue

*/

#include "sdkconfig.h"
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <sys/socket.h>
#include <errno.h>
#include <netdb.h>
#include <arpa/inet.h>
#include "esp_netif.h"
#include "esp_log.h"
#include "app_gpio.h"
#include "app_audio.h"
#include "app_tcp.h"
#include "freertos/ringbuf.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#if defined(CONFIG_EXAMPLE_SOCKET_IP_INPUT_STDIN)
#include "addr_from_stdin.h"
#endif

#if defined(CONFIG_EXAMPLE_IPV4)
#define HOST_IP_ADDR CONFIG_EXAMPLE_IPV4_ADDR
#elif defined(CONFIG_EXAMPLE_SOCKET_IP_INPUT_STDIN)
#define HOST_IP_ADDR ""
#endif

#define PORT CONFIG_EXAMPLE_PORT
#define INTERMEDIARY_BUF_SIZE 3080 // 3072 + sizeof(msg_hdr_t)
#define DISP_Q_LEN 8
#define DELAYTIME 100

static const char *TAG = "TCP tx task";
static const char *TAG2 = "TCP rx task";
static int sock = 0;
static SemaphoreHandle_t sock_ready;

msg_hdr_t hdr = {
    .magic = 0xAA,
    .version = 1, //other fields set in tcp_tx_task
};

static QueueHandle_t disp1_q;
static QueueHandle_t disp2_q;

QueueHandle_t tcp_rx_get_disp1_q(void)
{
    return disp1_q;
}   
QueueHandle_t tcp_rx_get_disp2_q(void)
{
    return disp2_q;
}

static void tcp_init_queues(void)
{
    disp1_q = xQueueCreate(DISP_Q_LEN, sizeof(text_msg_t));
    assert(disp1_q);
    disp2_q = xQueueCreate(DISP_Q_LEN, sizeof(text_msg_t));
    assert(disp2_q);
    ESP_LOGI(TAG, "TCP RX display queues initialized");
}

static bool send_all(int sock, const void *buf, size_t len)
{
    const uint8_t *ptr = (const uint8_t *)buf;
    size_t total_sent = 0;
    while (total_sent < len) {
        int sent = send(sock, ptr + total_sent, len - total_sent, 0);
        if (sent < 0) {
            ESP_LOGE(TAG, "send failed: errno %d", errno);
            return false;
        }
        total_sent += sent;
    }
    return true;
}

static bool recv_all(int sock, void *buf, size_t len)
{
    uint8_t *ptr = (uint8_t *)buf;
    size_t total_received = 0;
    while (total_received < len) {
        int received = recv(sock, ptr + total_received, len - total_received, 0);
        if (received < 0) {
            ESP_LOGE(TAG, "recv failed: errno %d", errno);
            return false;
        } else if (received == 0) {
            ESP_LOGW(TAG, "Connection closed");
            return false;
        }
        total_received += received;
    }
    return true;
}

void tcp_tx_task(void *args)
{
    /* transmit buffer */
    uint8_t *int_buf = (uint8_t *)calloc(1, INTERMEDIARY_BUF_SIZE);
    assert(int_buf);
    size_t rb_bytes = 0; //for appending onto TCP headers
    ESP_LOGI(TAG, "TCP tx buffer size %zu initialized", INTERMEDIARY_BUF_SIZE);

    /* get ringbuffer handle */
    RingbufHandle_t audio_rb = audio_get_rb();

    while (1) {
        char host_ip[] = HOST_IP_ADDR;
        int addr_family = 0;
        int ip_protocol = 0;
        
        #if defined(CONFIG_EXAMPLE_IPV4)
        struct sockaddr_in dest_addr;
        inet_pton(AF_INET, host_ip, &dest_addr.sin_addr);
        dest_addr.sin_family = AF_INET;
        dest_addr.sin_port = htons(PORT);
        addr_family = AF_INET;
        ip_protocol = IPPROTO_IP;
        #elif defined(CONFIG_EXAMPLE_SOCKET_IP_INPUT_STDIN)
        struct sockaddr_storage dest_addr = {0};
        ESP_ERROR_CHECK(get_addr_from_stdin(PORT, SOCK_STREAM, &ip_protocol, &addr_family, &dest_addr));
        #endif

        ESP_LOGI(TAG, "Socket connecting to %s:%d", host_ip, PORT);
        sock = socket(addr_family, SOCK_STREAM, ip_protocol);
        if (sock < 0) {
            ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
            vTaskDelay(pdMS_TO_TICKS(DELAYTIME));
            continue;
        }

        int err = connect(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
        if  (err != 0) {
            ESP_LOGE(TAG, "Socket unable to connect: errno %d", errno);
            shutdown(sock, 0);
            close(sock);
            sock = 0;
            vTaskDelay(pdMS_TO_TICKS(DELAYTIME));
            continue;
        }
        ESP_LOGI(TAG, "Succesfully connected");
        xSemaphoreGive(sock_ready);

        uint32_t tx_log_ctr = 0;
        while (1)
        {
            /* rely on current FSM state to decide    */
            /* state = APP_GPIO_STATE_IDLE            */
            /* DO NOT SEND PACKETS                    */
            /* state = APP_GPIO_STATE_TRANSLATE_LANGx */
            /* SEND PACKET WITH HEADER SPECIFYING     */
            if (gpio_get_state() == APP_GPIO_STATE_IDLE)
            {  
                /* read audio_rb but don't send */
                size_t rb_bytes = 0;
                uint8_t *rb_buf = (uint8_t *)xRingbufferReceiveUpTo(audio_rb, &rb_bytes, pdMS_TO_TICKS(DELAYTIME), 3072);
                if (rb_buf != NULL) {
                    ESP_LOGI(TAG, "TCP tx idle state - dumped %zu bytes from audio rb", rb_bytes);
                    vRingbufferReturnItem(audio_rb, (void *)rb_buf);
                }
            }
            else if (gpio_get_state() == APP_GPIO_STATE_TRANSLATE_LANG1)
            {
                /* message synthesis */
                uint8_t *audio = (uint8_t *)xRingbufferReceiveUpTo(audio_rb, &rb_bytes, pdMS_TO_TICKS(DELAYTIME), 3072);
                if (audio != NULL) {
                    ESP_LOGI(TAG, "TCP tx lang1 state - read %zu bytes from audio", rb_bytes);
                    hdr.msg_type = 1; //AUDIO
                    hdr.flags = 1; //LANG1
                    hdr.payload_len = htonl(rb_bytes);
                    if ((tx_log_ctr++ % 100) == 0) {
                        ESP_LOGI(TAG, "TCP tx hdr: msg_type=%d flags=%d payload_len=%d",
                                 hdr.msg_type, hdr.flags, (int)rb_bytes);
                    }
                    /* copy header */
                    memcpy(int_buf, (uint8_t *)&hdr, sizeof(msg_hdr_t));
                    /* copy audio data */
                    memcpy(int_buf + sizeof(msg_hdr_t), audio, rb_bytes);
                    vRingbufferReturnItem(audio_rb, (void *)audio);
                
                    /* send */
                    if (!send_all(sock, int_buf, sizeof(msg_hdr_t) + rb_bytes)) {
                        break;
                    }
                }
                else if (audio == NULL) {
                    ESP_LOGE(TAG, "TCP tx lang1 state - failed to read from audio rb");
                }
            }
            else if (gpio_get_state() == APP_GPIO_STATE_TRANSLATE_LANG2)
            {
                /* message synthesis */
                uint8_t *audio = (uint8_t *)xRingbufferReceiveUpTo(audio_rb, &rb_bytes, pdMS_TO_TICKS(DELAYTIME), 3072);
                if (audio != NULL) {
                    ESP_LOGI(TAG, "TCP tx lang2 state - read %zu bytes from audio", rb_bytes);
                    /* copy header */
                    hdr.msg_type = 1; //AUDIO
                    hdr.flags = 2; //LANG2
                    hdr.payload_len = htonl(rb_bytes);
                    if ((tx_log_ctr++ % 100) == 0) {
                        ESP_LOGI(TAG, "TCP tx hdr: msg_type=%d flags=%d payload_len=%d",
                                 hdr.msg_type, hdr.flags, (int)rb_bytes);
                    }
                    memcpy(int_buf, (uint8_t *)&hdr, sizeof(msg_hdr_t));
                    /* copy audio data */
                    memcpy(int_buf + sizeof(msg_hdr_t), audio, rb_bytes);
                    vRingbufferReturnItem(audio_rb, (void *)audio);
        
                    /* send */
                    if (!send_all(sock, int_buf, sizeof(msg_hdr_t) + rb_bytes)) {
                        break;
                    }
                }
                else if (audio == NULL) {
                    ESP_LOGE(TAG, "TCP tx lang2 state - failed to read from audio rb");
                }
            }
            else
            {
                ESP_LOGE(TAG, "Critical Error - State of FSM not defined!!!");
            }
        }
        ESP_LOGE(TAG, "Shutting down socket and restarting...");
        if (sock > 0) {
            shutdown(sock, 0);
            close(sock);
            sock = 0;
        }
    }
    free(int_buf);
}

void tcp_rx_task(void *args)
{
    /* reuses the same socket created with the tx task */
    uint8_t hdr_buf[sizeof(msg_hdr_t)];
    ESP_LOGI(TAG2, "TCP RX task started");
    uint32_t rx_log_ctr = 0;
    while (1) {
        xSemaphoreTake(sock_ready, portMAX_DELAY);
        ESP_LOGI(TAG2, "TCP RX task connected");

        while (1)
        {
            if(!recv_all(sock, hdr_buf, sizeof(msg_hdr_t))) {
                ESP_LOGE(TAG2, "Failed to receive message header");
                break;
            }
            msg_hdr_t *hdr = (msg_hdr_t *)hdr_buf;
            uint32_t payload_len = ntohl(hdr->payload_len);
            if ((rx_log_ctr++ % 50) == 0) {
                ESP_LOGI(TAG2, "TCP rx hdr: msg_type=%d flags=%d payload_len=%d",
                         hdr->msg_type, hdr->flags, (int)payload_len);
            }
            
            if (payload_len > TEXT_BUF_SIZE) {
                ESP_LOGE(TAG2, "Payload length %d exceeds buffer size %d", payload_len, TEXT_BUF_SIZE);
                uint8_t discard_buf[32];
                size_t bytes_to_discard = payload_len;
                while (bytes_to_discard > 0) {
                    size_t chunk_size = (bytes_to_discard > sizeof(discard_buf)) ? sizeof(discard_buf) : bytes_to_discard;
                    if (!recv_all(sock, discard_buf, chunk_size)) {
                        ESP_LOGE(TAG2, "Failed to discard excess payload");
                        break;
                    }

                }
                continue;
            }

            text_msg_t text_msg = {
                .len = payload_len
            };

            if (!recv_all(sock, text_msg.payload, payload_len)) {
                ESP_LOGE(TAG2, "Failed to receive message payload");
                break;
            }
            if ((rx_log_ctr % 50) == 0) {
                ESP_LOGI(TAG2, "TCP rx payload ok: %d bytes", (int)payload_len);
            }
            if (hdr->flags & 0x04) {
                if (xQueueSend(disp1_q, &text_msg, pdMS_TO_TICKS(DELAYTIME)) != pdTRUE) {
                    ESP_LOGW(TAG2, "Display 1 queue full, message dropped");
                }
            } else if (hdr->flags & 0x08) {
                if (xQueueSend(disp2_q, &text_msg, pdMS_TO_TICKS(DELAYTIME)) != pdTRUE) {
                    ESP_LOGW(TAG2, "Display 2 queue full, message dropped");
                }
            } else {
                ESP_LOGW(TAG2, "Unknown display flag: %d", hdr->flags);
            }
        }
        ESP_LOGI(TAG2, "TCP RX task waiting for reconnect");
    }
}

void tcp_make_tasks(void)
{
    tcp_init_queues();
    sock_ready = xSemaphoreCreateBinary();
    assert(sock_ready);
    xTaskCreatePinnedToCore(tcp_tx_task, "tcp_tx_task", 4096, NULL, 6, NULL, 0);
    xTaskCreatePinnedToCore(tcp_rx_task, "tcp_rx_task", 4096, NULL, 6, NULL, 0);
}
