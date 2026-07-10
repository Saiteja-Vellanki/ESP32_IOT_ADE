/*
 * uart_parser.c  —  UART2 bidirectional driver
 *
 * RX (NUC029 → ESP32):
 *   Receives 6-byte switch state packets [0xAA][D0][D1][D2][D3][0x55]
 *   Parses and stores 25 GPIO states.
 *
 * TX (ESP32 → NUC029):
 *   Sends 4-byte relay command packets [0xBB][0x01][relay_num][0x44]
 *   Called when web dashboard / Android app presses SW1/SW2/SW3.
 *   NUC029 receives this and triggers the matching relay sequence.
 */

#include "uart_parser.h"
#include "config.h"

#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "uart";

/* ── Shared RX state ─────────────────────────────────────────── */
static SemaphoreHandle_t s_mutex;
static bool     s_gpio[GPIO_COUNT] = {false};
static uint32_t s_pkt_count        = 0u;
static uint32_t s_last_pkt_ms      = 0u;

/* ── TX mutex (prevent concurrent writes) ────────────────────── */
static SemaphoreHandle_t s_tx_mutex;

/* ── RX state machine ────────────────────────────────────────── */
typedef enum { ST_WAIT_HDR=0, ST_READ_DATA, ST_WAIT_END } pstate_t;

static void commit(const uint8_t d[4])
{
    bool tmp[GPIO_COUNT];
    for (uint8_t i = 0; i < GPIO_COUNT; i++)
        tmp[i] = (bool)((d[i / 8u] >> (i % 8u)) & 0x01u);
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000ULL);
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (uint8_t i = 0; i < GPIO_COUNT; i++) s_gpio[i] = tmp[i];
    s_pkt_count++;
    s_last_pkt_ms = now;
    xSemaphoreGive(s_mutex);
}

static void uart_rx_task(void *arg)
{
    uint8_t  buf[128];
    pstate_t st      = ST_WAIT_HDR;
    uint8_t  data[4] = {0};
    uint8_t  didx    = 0u;

    while (1) {
        int len = uart_read_bytes(UART_PORT_NUM, buf,
                                  sizeof(buf), pdMS_TO_TICKS(20));
        for (int i = 0; i < len; i++) {
            uint8_t b = buf[i];
            switch (st) {
                case ST_WAIT_HDR:
                    if (b == PROTO_HEADER) { didx=0; st=ST_READ_DATA; }
                    break;
                case ST_READ_DATA:
                    data[didx++] = b;
                    if (didx >= 4u) st = ST_WAIT_END;
                    break;
                case ST_WAIT_END:
                    if (b == PROTO_END) commit(data);
                    else ESP_LOGW(TAG, "Frame err: 0x%02X", b);
                    st = ST_WAIT_HDR;
                    break;
                default: st = ST_WAIT_HDR; break;
            }
        }
    }
}

/* ─────────────────────────────────────────────────────────────
 *  uart_parser_init
 * ───────────────────────────────────────────────────────────── */
void uart_parser_init(void)
{
    s_mutex    = xSemaphoreCreateMutex();
    s_tx_mutex = xSemaphoreCreateMutex();
    configASSERT(s_mutex);
    configASSERT(s_tx_mutex);

    const uart_config_t cfg = {
        .baud_rate  = UART_BAUD_RATE,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    /* Install driver with TX buffer (256 bytes) so TX never blocks tasks */
    ESP_ERROR_CHECK(uart_driver_install(UART_PORT_NUM,
                                        UART_RX_BUF_SIZE,
                                        256,        /* TX buf size */
                                        0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_PORT_NUM, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT_NUM,
                                 UART_TX_PIN, UART_RX_PIN,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    ESP_LOGI(TAG, "UART%d RX=GPIO%d TX=GPIO%d %d baud (bidirectional)",
             UART_PORT_NUM, UART_RX_PIN, UART_TX_PIN, UART_BAUD_RATE);

    xTaskCreatePinnedToCore(uart_rx_task, "uart_rx",
                            UART_TASK_STACK, NULL,
                            UART_TASK_PRIO, NULL, UART_TASK_CORE);
}

/* ─────────────────────────────────────────────────────────────
 *  uart_send_relay_cmd  —  ESP32 → NUC029
 *
 *  Packet: [0xBB][0x01][relay_num][0x44]
 *
 *  relay_num: 1=RL1  2=RL2  3=RL3  4=RL4
 *
 *  NUC029 parses this on UART0 RX and triggers its relay
 *  state machine exactly as if SW1/SW2/SW3 was pressed locally.
 * ───────────────────────────────────────────────────────────── */
void uart_send_relay_cmd(uint8_t relay_num)
{
    if (relay_num < 1 || relay_num > 4) {
        ESP_LOGW(TAG, "uart_send_relay_cmd: invalid relay %d", relay_num);
        return;
    }

    uint8_t pkt[CMD_PACKET_LEN] = {
        CMD_PROTO_HEADER,   /* 0xBB */
        CMD_RELAY_TRIGGER,  /* 0x01 */
        relay_num,          /* 1-4  */
        CMD_PROTO_END       /* 0x44 */
    };

    xSemaphoreTake(s_tx_mutex, portMAX_DELAY);
    int written = uart_write_bytes(UART_PORT_NUM,
                                   (const char *)pkt, CMD_PACKET_LEN);
    xSemaphoreGive(s_tx_mutex);

    if (written == CMD_PACKET_LEN) {
        ESP_LOGI(TAG, "→ NUC029: relay cmd RL%d [BB 01 0%d 44]",
                 relay_num, relay_num);
    } else {
        ESP_LOGE(TAG, "UART TX failed: wrote %d/%d bytes",
                 written, CMD_PACKET_LEN);
    }
}

/* ── RX public API ───────────────────────────────────────────── */
bool uart_parser_get_gpio(uint8_t idx)
{
    if (idx >= GPIO_COUNT) return false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool v = s_gpio[idx];
    xSemaphoreGive(s_mutex);
    return v;
}

uint32_t uart_parser_packet_count(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    uint32_t v = s_pkt_count;
    xSemaphoreGive(s_mutex);
    return v;
}

uint32_t uart_parser_last_packet_ms(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    uint32_t v = s_last_pkt_ms;
    xSemaphoreGive(s_mutex);
    return v;
}

bool uart_parser_is_online(void)
{
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000ULL);
    return ((now - uart_parser_last_packet_ms()) < 1000u);
}
