#include "uart_parser.h"
#include "config.h"

#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "uart_parser";

/* ── Shared state ────────────────────────────────────────────── */
static SemaphoreHandle_t s_mutex;
static bool     s_gpio[GPIO_COUNT] = {false};
static uint32_t s_pkt_count        = 0u;
static uint32_t s_last_pkt_ms      = 0u;

/* ── Parser states ───────────────────────────────────────────── */
typedef enum {
    ST_WAIT_HDR = 0,
    ST_READ_DATA,
    ST_WAIT_END
} pstate_t;

/* ── Commit one valid packet ─────────────────────────────────── */
static void commit(const uint8_t d[4])
{
    bool tmp[GPIO_COUNT];
    for (uint8_t i = 0; i < GPIO_COUNT; i++)
        tmp[i] = (bool)((d[i / 8u] >> (i % 8u)) & 0x01u);

    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000ULL);

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (uint8_t i = 0; i < GPIO_COUNT; i++)
        s_gpio[i] = tmp[i];
    s_pkt_count++;
    s_last_pkt_ms = now;
    xSemaphoreGive(s_mutex);
}

/* ── RX task ─────────────────────────────────────────────────── */
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
                    if (b == PROTO_HEADER) { didx = 0; st = ST_READ_DATA; }
                    break;
                case ST_READ_DATA:
                    data[didx++] = b;
                    if (didx >= 4u) st = ST_WAIT_END;
                    break;
                case ST_WAIT_END:
                    if (b == PROTO_END)
                        commit(data);
                    else
                        ESP_LOGW(TAG, "Framing error: expected 0x55, got 0x%02X", b);
                    st = ST_WAIT_HDR;
                    break;
                default:
                    st = ST_WAIT_HDR;
                    break;
            }
        }
    }
}

/* ── Public API ──────────────────────────────────────────────── */
void uart_parser_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    configASSERT(s_mutex);

    const uart_config_t cfg = {
        .baud_rate  = UART_BAUD_RATE,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(UART_PORT_NUM,
                                        UART_RX_BUF_SIZE, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_PORT_NUM, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT_NUM,
                                 UART_TX_PIN, UART_RX_PIN,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    ESP_LOGI(TAG, "UART%d ready — RX=GPIO%d TX=GPIO%d %d baud",
             UART_PORT_NUM, UART_RX_PIN, UART_TX_PIN, UART_BAUD_RATE);

    xTaskCreatePinnedToCore(uart_rx_task, "uart_rx",
                            UART_TASK_STACK, NULL,
                            UART_TASK_PRIO, NULL, UART_TASK_CORE);
}

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
