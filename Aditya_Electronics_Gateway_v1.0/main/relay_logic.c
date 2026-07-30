/*
 * relay_logic.c  —  Relay STATE TRACKER (no local GPIO control)
 *
 * The ESP32 has no relays. The NUC029xAN owns all 4 physical relays
 * and their timing behavior (see relay.c on the NUC side); the ESP32
 * only sends trigger commands to the NUC over UART
 * (uart_send_relay_cmd() in uart_parser.c/remote_server.c).
 *
 * This file used to also toggle 4 local GPIO pins as if relays were
 * wired directly to the ESP32 — they weren't connected to anything,
 * so that GPIO driving has been removed. What's kept is a state
 * TRACKER: it mirrors the NUC's known timing behavior in software
 * only (no hardware access) so that:
 *   - remote_server.c can edge-detect (only send a UART trigger on
 *     a real 0→1 transition from the server, not on every 500ms poll
 *     while the app's Status stays at 1)
 *   - web_server.c's local dashboard and main.c's heartbeat log have
 *     something meaningful to show for "relays=0x%X"
 *
 * This is a best-effort mirror, not ground truth — the NUC is the
 * only thing that actually knows real relay state. If the NUC and
 * ESP32 ever need to agree exactly (e.g. after a NUC-side manual
 * SW1/SW2/SW3 press, or after an ESP32 reboot mid-timer), a real
 * feedback packet from NUC → ESP32 reporting actual relay state
 * would be needed — the current UART protocol doesn't have one.
 *
 * Timing mirrors system_config.h on the NUC side:
 *   RL1: ON RL1_ON_MS → OFF
 *   RL2: default ON; toggle → OFF RL2_OFF_MS → back ON
 *   RL3: latch ON; RL3_TO_RL4_DELAY_MS later → RL4 triggers
 *   RL4: ON RL4_ON_MS → OFF (auto via RL3, or direct command)
 */

#include "relay_logic.h"
#include "config.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"

static const char *TAG = "relay";

static bool s_relay[4] = {false, false, false, false};
static SemaphoreHandle_t s_mutex;
static SemaphoreHandle_t s_change_sem;   /* given on every actual state change */

static void track_set(uint8_t idx, bool on)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool changed = (s_relay[idx] != on);
    s_relay[idx] = on;
    xSemaphoreGive(s_mutex);
    ESP_LOGI(TAG, "RL%d tracked → %s (NUC drives the real relay)",
             idx + 1, on ? "ON" : "OFF");
    if (changed) {
        /* Signal immediately - covers both the initial command AND
         * the automatic 3-second revert (RL1 auto-OFF, RL2 auto-
         * back-ON, RL4 auto-trigger-then-OFF), not just the former.
         * xSemaphoreGive is safe to call repeatedly even if the
         * previous signal hasn't been consumed yet (binary semaphore
         * just stays "given" - the consumer only needs to know
         * "something changed since I last checked", not how many
         * times). */
        xSemaphoreGive(s_change_sem);
    }
}

static bool s_rl2_continuous = false;
static QueueHandle_t s_cmd_q;

/* Mirrors the NUC's relay.c timing purely for local state tracking -
 * no GPIO access here, see file header. */
static void relay_track_task(void *arg)
{
    uint8_t cmd;
    while (1) {
        if (xQueueReceive(s_cmd_q, &cmd, portMAX_DELAY) != pdTRUE)
            continue;
        switch (cmd) {
            case 1:
                track_set(0, true);
                vTaskDelay(pdMS_TO_TICKS(RELAY_PULSE_MS));
                track_set(0, false);
                break;
            case 2:
                if (!s_rl2_continuous) {
                    s_rl2_continuous = true;
                    track_set(1, true);
                } else {
                    track_set(1, false);
                    vTaskDelay(pdMS_TO_TICKS(RELAY_PULSE_MS));
                    track_set(1, true);
                }
                break;
            case 3:
                track_set(2, true);
                vTaskDelay(pdMS_TO_TICKS(RELAY_PULSE_MS));
                track_set(3, true);
                vTaskDelay(pdMS_TO_TICKS(RELAY_PULSE_MS));
                track_set(3, false);
                break;
            case 4:
                /* Direct RL4 trigger (NUC's RELAY_Command supports this
                 * independent of RL3) */
                track_set(3, true);
                vTaskDelay(pdMS_TO_TICKS(RELAY_PULSE_MS));
                track_set(3, false);
                break;
            default:
                ESP_LOGW(TAG,"Unknown cmd:%d",cmd);
                break;
        }
    }
}

void relay_logic_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    configASSERT(s_mutex);
    s_change_sem = xSemaphoreCreateBinary();
    configASSERT(s_change_sem);

    ESP_LOGI(TAG, "Relay state tracker init (no local GPIO - "
                  "NUC owns the physical relays, driven via UART)");

    s_cmd_q = xQueueCreate(8, sizeof(uint8_t));
    configASSERT(s_cmd_q);
    xTaskCreate(relay_track_task,"relay_track",RELAY_TASK_STACK,
                NULL,RELAY_TASK_PRIO,NULL);
}

SemaphoreHandle_t relay_logic_get_change_semaphore(void)
{
    return s_change_sem;
}

void relay_logic_command(uint8_t relay_num)
{
    if (relay_num < 1 || relay_num > 4) return;
    xQueueSend(s_cmd_q, &relay_num, 0);
}

bool relay_logic_get_state(uint8_t relay_num)
{
    if (relay_num < 1 || relay_num > 4) return false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool v = s_relay[relay_num - 1];
    xSemaphoreGive(s_mutex);
    return v;
}

uint8_t relay_logic_get_mask(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    uint8_t m = 0;
    for (int i = 0; i < 4; i++)
        if (s_relay[i]) m |= (1u << i);
    xSemaphoreGive(s_mutex);
    return m;
}
