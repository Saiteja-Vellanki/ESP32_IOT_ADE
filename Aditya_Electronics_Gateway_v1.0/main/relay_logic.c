/*
 * relay_logic.c — ESP32 relay state cache.
 *
 * The NUC029xAN owns the physical relays and all timing. The ESP32
 * must never run a second relay timer because that creates two sources
 * of truth and caused R2/R3 synchronization errors.
 *
 * The state in this module is therefore only the latest physical state
 * received from the NUC relay-feedback packet. Commands are sent by
 * uart_parser.c; this module never drives hardware.
 */
#include "relay_logic.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "relay";

static bool s_relay[4] = { false, true, false, false };
static SemaphoreHandle_t s_mutex;
static SemaphoreHandle_t s_change_sem;

void relay_logic_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    s_change_sem = xSemaphoreCreateBinary();
    configASSERT(s_mutex);
    configASSERT(s_change_sem);

    ESP_LOGI(TAG, "Relay cache init: NUC owns R1-R4 and all timing");
}

static void set_mask_locked(uint8_t mask)
{
    for (uint8_t i = 0; i < 4u; ++i)
        s_relay[i] = ((mask >> i) & 1u) != 0u;
}

void relay_logic_sync_mask(uint8_t mask)
{
    bool changed = false;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (uint8_t i = 0; i < 4u; ++i) {
        bool v = ((mask >> i) & 1u) != 0u;
        if (s_relay[i] != v) changed = true;
        s_relay[i] = v;
    }
    xSemaphoreGive(s_mutex);

    if (changed) {
        ESP_LOGI(TAG, "NUC physical relays -> R1=%d R2=%d R3=%d R4=%d",
                 (mask >> 0) & 1, (mask >> 1) & 1,
                 (mask >> 2) & 1, (mask >> 3) & 1);
        xSemaphoreGive(s_change_sem);
    }
}

/* Compatibility API: commands are deliberately handled by UART only.
 * Keeping these functions avoids breaking older callers, but they no
 * longer create a second software relay timer. */
void relay_logic_command(uint8_t relay_num)
{
    (void)relay_num;
}

void relay_logic_set_r3(bool turn_on)
{
    (void)turn_on;
}

bool relay_logic_get_state(uint8_t relay_num)
{
    if (relay_num < 1u || relay_num > 4u) return false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool v = s_relay[relay_num - 1u];
    xSemaphoreGive(s_mutex);
    return v;
}

uint8_t relay_logic_get_mask(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    uint8_t m = 0u;
    for (uint8_t i = 0; i < 4u; ++i)
        if (s_relay[i]) m |= (uint8_t)(1u << i);
    xSemaphoreGive(s_mutex);
    return m;
}

SemaphoreHandle_t relay_logic_get_change_semaphore(void)
{
    return s_change_sem;
}
