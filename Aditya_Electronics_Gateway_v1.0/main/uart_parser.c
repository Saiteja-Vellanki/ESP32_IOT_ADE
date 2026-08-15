/*
 * uart_parser.c  —  UART2 bidirectional driver
 *
 * RX Packet 1 — Switch states  [0xAA][D0][D1][D2][D3][0x55]  every 100ms
 * RX Packet 2 — Voltage data   [0xCC][BH][BL][PH][PL][PF][LB][BAT_PCT][0x66]  every 500ms
 * TX           — Relay command  [0xBB][0x01][relay 1-4][0x44]  on demand
 *
 * Bug fixed: shared didx between switch and voltage parsers caused
 * voltage data to be collected starting at wrong offset.
 * Fix: separate sw_didx and vt_didx, reset each independently.
 */

#include "uart_parser.h"
#include "config.h"

#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>

static const char *TAG = "uart";

#define SWITCH_HDR   0xAAu
#define SWITCH_END   0x55u
#define VOLT_HDR     0xCCu
#define VOLT_END     0x66u
#define RELAY_HDR    RELAY_STATUS_HEADER
#define RELAY_END    RELAY_STATUS_END

/* ── Shared data ─────────────────────────────────────────────── */
static SemaphoreHandle_t s_mutex;
static SemaphoreHandle_t s_tx_mutex;

static bool     s_gpio[GPIO_COUNT] = {false};
static uint32_t s_pkt_count        = 0u;
static uint32_t s_last_pkt_ms      = 0u;

static uint16_t s_bat_mv     = 0u;
static uint16_t s_pwr_mv     = 0u;
static uint8_t  s_power_fail = 0u;
static uint8_t  s_low_bat    = 0u;
static uint8_t  s_bat_pct    = 0u;   /* NUC-computed battery percent */
static uint8_t  s_relay_mask  = 0x02u; /* NUC power-on default: R2 ON */
static uint8_t  s_relay_lock  = 0u;
static uint32_t s_relay_seq   = 0u;

/* ── commit_switch ───────────────────────────────────────────── */
static void commit_switch(const uint8_t d[4])
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

    /* Unambiguous "NUC just reset" detection - fires once, exactly
     * when the NUC's very first post-boot packet arrives, regardless
     * of PF/timing (which aren't always reliable indicators - PF
     * might already be 0, packet gaps can vary). */
    if (d[3] & (1u << B3_NUC_BOOT_BIT)) {
        ESP_LOGW(TAG, "*** NUC BOOT DETECTED - NUC has just reset/powered up ***");
    }

    /* Throttled RX log - this packet arrives every 100ms, so log
     * only every 20th (~2s) to avoid flooding the console, plus
     * immediately on any actual state change so a real switch
     * press/release is never missed regardless of the throttle. */
    static uint8_t  s_last_raw[4] = {0xFFu, 0xFFu, 0xFFu, 0xFFu}; /* force first log */
    static uint32_t s_sw_log_count = 0;
    bool changed = (d[0] != s_last_raw[0] || d[1] != s_last_raw[1] ||
                    d[2] != s_last_raw[2] || d[3] != s_last_raw[3]);
    if (changed || (++s_sw_log_count % 20u) == 0u) {
        /* Decode active switch numbers directly instead of leaving
         * raw hex for manual bit-twiddling - this is exactly the
         * kind of thing that's easy to misread by hand (SW07/SW23
         * mistaken for something else while debugging). */
        char active[160];
        int  pos = 0;
        bool any = false;
        for (uint8_t i = 0; i < GPIO_COUNT; i++) {
            if (tmp[i]) {
                pos += snprintf(active + pos, sizeof(active) - (size_t)pos,
                                "%sSW%02u", any ? " " : "", (unsigned)(i + 1));
                any = true;
            }
        }
        if (!any) snprintf(active, sizeof(active), "none");

        bool pf = (d[3] & (1u << B3_POWERFAIL_BIT)) != 0;
        bool lb = (d[3] & (1u << B3_LOWBAT_BIT)) != 0;

        ESP_LOGI(TAG, "← NUC: switch pkt B0=0x%02X B1=0x%02X B2=0x%02X B3=0x%02X%s "
                      "| active: %s | PF=%d LB=%d",
                 d[0], d[1], d[2], d[3], changed ? " (changed)" : "",
                 active, pf, lb);
        memcpy(s_last_raw, d, 4);
    }
}

/* ── commit_voltage ──────────────────────────────────────────── */
static void commit_voltage(const uint8_t d[7])
{
    uint16_t bat = ((uint16_t)d[0] << 8u) | d[1];
    /* NOTE: this field is named s_pwr_mv / "pwr" for historical
     * reasons (originally reserved for a future mains-voltage sense
     * line), but it now carries a clean power-status flag (0/1) from
     * the NUC, not millivolts - 1=power OK, 0=power fail. Kept as a
     * uint16_t on the wire and in the getter API for compatibility
     * with anything already parsing this field, but treat the value
     * as boolean, not a voltage. */
    uint16_t pwr_status = ((uint16_t)d[2] << 8u) | d[3];
    uint8_t  pf  = d[4];
    uint8_t  lb  = d[5];
    uint8_t  pct = d[6];

    /* Sanity-check before accepting. This protocol has no checksum -
     * only fixed header/end-marker bytes for framing - so a
     * corrupted or noise byte stream CAN occasionally produce
     * something that looks like a validly-framed packet purely by
     * chance, especially when the NUC has no power at all (its UART
     * TX line is then floating/undefined rather than actively
     * driven, which can easily produce byte patterns that happen to
     * match 0xCC...0x66 framing). Confirmed case: batt_volt=7.53V
     * reached the server while the NUC had no power - physically
     * impossible for this battery/divider, and nearly 2x anything
     * ever seen from a real reading. Reject anything outside sane
     * bounds instead of accepting it as real data. */
    if (bat > 5000u || pct > 100u || pf > 1u || lb > 1u || pwr_status > 1u) {
        ESP_LOGW(TAG, "VOLT pkt REJECTED - implausible values (BAT=%u PWR=%u "
                      "PF=%u LB=%u PCT=%u%%), likely corrupted/noise - "
                      "possibly NUC unpowered (no checksum in this protocol "
                      "to catch this more reliably)",
                 bat, pwr_status, pf, lb, pct);
        return;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_bat_mv     = bat;
    s_pwr_mv     = pwr_status;
    s_power_fail = pf;
    s_low_bat    = lb;
    s_bat_pct    = pct;
    xSemaphoreGive(s_mutex);

    /* Always visible in monitor — helps debug */
    ESP_LOGI(TAG, "← NUC: VOLT pkt BAT=%umV (%u%%) PWR_STATUS=%u PF=%d LB=%d",
             bat, pct, pwr_status, pf, lb);
}

/* ── commit_relay_status ─────────────────────────────────────── */
static void commit_relay_status(const uint8_t d[5])
{
    uint8_t mask = 0u;
    if (d[0]) mask |= 0x01u;
    if (d[1]) mask |= 0x02u;
    if (d[2]) mask |= 0x04u;
    if (d[3]) mask |= 0x08u;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool changed = (mask != s_relay_mask) || (d[4] != s_relay_lock);
    s_relay_mask = mask;
    s_relay_lock = d[4] ? 1u : 0u;
    s_relay_seq++;
    s_last_pkt_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    xSemaphoreGive(s_mutex);

    if (changed) {
        ESP_LOGI(TAG, "← NUC: RELAY pkt R1=%u R2=%u R3=%u R4=%u LOCK=%u",
                 d[0], d[1], d[2], d[3], d[4]);
    }
}

/* ── RX task ─────────────────────────────────────────────────── */
typedef enum {
    ST_IDLE=0, ST_SW_DATA, ST_SW_END, ST_VT_DATA, ST_VT_END, ST_RL_DATA, ST_RL_END
} rx_state_t;

static void uart_rx_task(void *arg)
{
    uint8_t    buf[128];
    rx_state_t st = ST_IDLE;
    uint8_t    sw_data[4] = {0};
    uint8_t    vt_data[7] = {0};
    uint8_t    rl_data[5] = {0};
    uint8_t    sw_didx = 0u;
    uint8_t    vt_didx = 0u;
    uint8_t    rl_didx = 0u;

    while (1) {
        int len = uart_read_bytes(UART_PORT_NUM, buf,
                                  sizeof(buf), pdMS_TO_TICKS(20));
        for (int i = 0; i < len; i++) {
            uint8_t b = buf[i];
            switch (st) {

            case ST_IDLE:
                if      (b == SWITCH_HDR) { sw_didx=0; st=ST_SW_DATA; }
                else if (b == VOLT_HDR)   { vt_didx=0; st=ST_VT_DATA; }
                else if (b == RELAY_HDR)  { rl_didx=0; st=ST_RL_DATA; }
                break;

            case ST_SW_DATA:
                sw_data[sw_didx++] = b;
                if (sw_didx >= 4u) st = ST_SW_END;
                break;

            case ST_SW_END:
                if (b == SWITCH_END) commit_switch(sw_data);
                else ESP_LOGW(TAG, "SW framing err: 0x%02X", b);
                st = ST_IDLE;
                break;

            case ST_VT_DATA:
                vt_data[vt_didx++] = b;
                if (vt_didx >= 7u) st = ST_VT_END;
                break;

            case ST_VT_END:
                if (b == VOLT_END) commit_voltage(vt_data);
                else ESP_LOGW(TAG, "VT framing err: 0x%02X", b);
                st = ST_IDLE;
                break;

            case ST_RL_DATA:
                rl_data[rl_didx++] = b;
                if (rl_didx >= 5u) st = ST_RL_END;
                break;

            case ST_RL_END:
                if (b == RELAY_END) commit_relay_status(rl_data);
                else ESP_LOGW(TAG, "RL framing err: 0x%02X", b);
                st = ST_IDLE;
                break;

            default:
                st = ST_IDLE;
                break;
            }
        }
    }
}

/* ── uart_parser_init ────────────────────────────────────────── */
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
    ESP_ERROR_CHECK(uart_driver_install(UART_PORT_NUM,
                                        UART_RX_BUF_SIZE, 256, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_PORT_NUM, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT_NUM,
                                 UART_TX_PIN, UART_RX_PIN,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_LOGI(TAG, "UART%d RX=GPIO%d TX=GPIO%d %dbaud",
             UART_PORT_NUM, UART_RX_PIN, UART_TX_PIN, UART_BAUD_RATE);

    xTaskCreatePinnedToCore(uart_rx_task, "uart_rx",
                            UART_TASK_STACK, NULL,
                            UART_TASK_PRIO, NULL, UART_TASK_CORE);
}

/* ── uart_send_relay_cmd ─────────────────────────────────────── */
void uart_send_relay_cmd(uint8_t relay_num)
{
    if (relay_num < 1 || relay_num > 4) return;
    uint8_t pkt[CMD_PACKET_LEN] = {
        CMD_PROTO_HEADER,
        CMD_RELAY_TRIGGER,
        relay_num,
        CMD_PROTO_END
    };
    xSemaphoreTake(s_tx_mutex, portMAX_DELAY);
    uart_write_bytes(UART_PORT_NUM, (const char *)pkt, CMD_PACKET_LEN);
    xSemaphoreGive(s_tx_mutex);
    ESP_LOGI(TAG, "→ NUC: relay cmd RL%d", relay_num);
}

void uart_send_relay_lock_cmd(bool locked)
{
    uint8_t pkt[CMD_PACKET_LEN] = {
        CMD_PROTO_HEADER,
        CMD_RELAY_LOCK,
        locked ? RELAY_LOCK_ENGAGE : RELAY_LOCK_RELEASE,
        CMD_PROTO_END
    };
    xSemaphoreTake(s_tx_mutex, portMAX_DELAY);
    uart_write_bytes(UART_PORT_NUM, (const char *)pkt, CMD_PACKET_LEN);
    xSemaphoreGive(s_tx_mutex);
    ESP_LOGI(TAG, "→ NUC: relay lock RL%d %s", RELAY_LOCK_NUM, locked ? "ENGAGE" : "RELEASE");
}

void uart_send_nuc_reset_cmd(void)
{
    uint8_t pkt[CMD_PACKET_LEN] = {
        CMD_PROTO_HEADER,
        CMD_NUC_RESET,
        0x00u,   /* unused param, always 0 */
        CMD_PROTO_END
    };
    xSemaphoreTake(s_tx_mutex, portMAX_DELAY);
    uart_write_bytes(UART_PORT_NUM, (const char *)pkt, CMD_PACKET_LEN);
    xSemaphoreGive(s_tx_mutex);
    ESP_LOGW(TAG, "→ NUC: RESET (server-requested soft reset - NUC resets first)");
}

/* ── Switch API ──────────────────────────────────────────────── */
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

/* ── Voltage API ─────────────────────────────────────────────── */
uint16_t uart_parser_get_bat_mv(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    uint16_t v = s_bat_mv;
    xSemaphoreGive(s_mutex);
    return v;
}
uint16_t uart_parser_get_pwr_mv(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    uint16_t v = s_pwr_mv;
    xSemaphoreGive(s_mutex);
    return v;
}
bool uart_parser_is_power_fail(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    uint8_t v = s_power_fail;
    xSemaphoreGive(s_mutex);
    return (v == 0x01u);
}
bool uart_parser_is_low_battery(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    uint8_t v = s_low_bat;
    xSemaphoreGive(s_mutex);
    return (v == 0x01u);
}
uint8_t uart_parser_get_bat_pct(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    uint8_t v = s_bat_pct;
    xSemaphoreGive(s_mutex);
    return v;
}
void uart_parser_get_gpio_snapshot(bool *out, uint8_t count)
{
    if (!out || count == 0u) return;
    if (count > GPIO_COUNT) count = GPIO_COUNT;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memcpy(out, s_gpio, count * sizeof(bool));
    xSemaphoreGive(s_mutex);
}

void uart_parser_get_power_snapshot(uint16_t *bat_mv, uint16_t *pwr_status,
                                    bool *power_fail, bool *low_battery,
                                    uint8_t *bat_pct)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (bat_mv) *bat_mv = s_bat_mv;
    if (pwr_status) *pwr_status = s_pwr_mv;
    if (power_fail) *power_fail = (s_power_fail == 1u);
    if (low_battery) *low_battery = (s_low_bat == 1u);
    if (bat_pct) *bat_pct = s_bat_pct;
    xSemaphoreGive(s_mutex);
}


uint8_t uart_parser_get_relay_mask(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    uint8_t v = s_relay_mask;
    xSemaphoreGive(s_mutex);
    return v;
}

bool uart_parser_is_relay_locked(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool v = (s_relay_lock != 0u);
    xSemaphoreGive(s_mutex);
    return v;
}

void uart_parser_get_relay_snapshot(uint8_t *mask, bool *locked, uint32_t *seq)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (mask) *mask = s_relay_mask;
    if (locked) *locked = (s_relay_lock != 0u);
    if (seq) *seq = s_relay_seq;
    xSemaphoreGive(s_mutex);
}
