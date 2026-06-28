#include "relay_logic.h"
#include "config.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "relay";

/* ── GPIO map ────────────────────────────────────────────────── */
static const int RELAY_GPIO[4] = {
    RELAY1_GPIO, RELAY2_GPIO, RELAY3_GPIO, RELAY4_GPIO
};

/* ── Physical relay states ───────────────────────────────────── */
static bool s_relay[4] = {false, false, false, false};

static void relay_set(uint8_t n, bool on)   /* n = 0-3 */
{
    if (n > 3) return;
    s_relay[n] = on;
    gpio_set_level(RELAY_GPIO[n], on ? 1 : 0);
    ESP_LOGI(TAG, "RL%d → %s", n + 1, on ? "ON" : "OFF");
}

/* ── RL2 mode flag ───────────────────────────────────────────── */
/*
 * RL2 logic:
 *   First  command → RL2 continuous ON
 *   Second command → RL2 OFF for 3s → then back ON
 * s_rl2_continuous tracks whether RL2 should be ON continuously
 */
static bool s_rl2_continuous = false;

/* ── Command queue ───────────────────────────────────────────── */
static QueueHandle_t s_cmd_queue;

/* ── Relay task ──────────────────────────────────────────────── */
/*
 * Timers are implemented with non-blocking vTaskDelay inside
 * the task — each relay phase is handled sequentially.
 * Commands arrive via queue so nothing is missed.
 *
 * RL1:  cmd=1 → RL1 ON → delay 3s → RL1 OFF
 * RL2:  cmd=2 → if not continuous: RL2 ON (stay)
 *               if continuous: RL2 OFF → delay 3s → RL2 ON
 * RL3:  cmd=3 → RL3 ON → delay 3s → RL4 ON → delay 3s → RL4 OFF
 *               RL3 stays ON after triggering RL4
 * RL4:  (only triggered internally by RL3 sequence)
 */
static void relay_task(void *arg)
{
    uint8_t cmd;
    while (1) {
        /* Wait for a command — no timeout, block until received */
        if (xQueueReceive(s_cmd_queue, &cmd, portMAX_DELAY) != pdTRUE)
            continue;

        switch (cmd) {

            /* ── RL1: pulse ON 3s OFF ─────────────────────────── */
            case 1:
                relay_set(0, true);
                vTaskDelay(pdMS_TO_TICKS(RELAY_PULSE_MS));
                relay_set(0, false);
                break;

            /* ── RL2: toggle continuous / off-3s-on ──────────── */
            case 2:
                if (!s_rl2_continuous) {
                    /* First press — turn ON continuously */
                    s_rl2_continuous = true;
                    relay_set(1, true);
                } else {
                    /* Second press — OFF for 3s then back ON */
                    relay_set(1, false);
                    vTaskDelay(pdMS_TO_TICKS(RELAY_PULSE_MS));
                    relay_set(1, true);   /* back ON continuously */
                }
                break;

            /* ── RL3 → after 3s trigger RL4 pulse ────────────── */
            case 3:
                relay_set(2, true);                    /* RL3 ON          */
                vTaskDelay(pdMS_TO_TICKS(RELAY_PULSE_MS)); /* wait 3s     */
                relay_set(3, true);                    /* RL4 ON          */
                vTaskDelay(pdMS_TO_TICKS(RELAY_PULSE_MS)); /* RL4 ON 3s   */
                relay_set(3, false);                   /* RL4 OFF         */
                /* RL3 stays ON — per requirement                          */
                break;

            default:
                ESP_LOGW(TAG, "Unknown relay command: %d", cmd);
                break;
        }
    }
}

/* ── Public API ──────────────────────────────────────────────── */
void relay_logic_init(void)
{
    /* Configure relay GPIO pins as output, default OFF */
    for (int i = 0; i < 4; i++) {
        gpio_set_direction(RELAY_GPIO[i], GPIO_MODE_OUTPUT);
        gpio_set_level(RELAY_GPIO[i], 0);
    }
    ESP_LOGI(TAG, "Relays init: RL1=GPIO%d RL2=GPIO%d RL3=GPIO%d RL4=GPIO%d",
             RELAY1_GPIO, RELAY2_GPIO, RELAY3_GPIO, RELAY4_GPIO);

    s_cmd_queue = xQueueCreate(8, sizeof(uint8_t));
    configASSERT(s_cmd_queue);

    xTaskCreate(relay_task, "relay", RELAY_TASK_STACK,
                NULL, RELAY_TASK_PRIO, NULL);
}

void relay_logic_command(uint8_t relay_num)
{
    if (relay_num < 1 || relay_num > 3) {
        ESP_LOGW(TAG, "Only SW1/SW2/SW3 are app-controllable");
        return;
    }
    xQueueSend(s_cmd_queue, &relay_num, 0);
    ESP_LOGI(TAG, "Command queued: SW%d", relay_num);
}

bool relay_logic_get_state(uint8_t relay_num)
{
    if (relay_num < 1 || relay_num > 4) return false;
    return s_relay[relay_num - 1];
}

uint8_t relay_logic_get_mask(void)
{
    uint8_t m = 0;
    for (int i = 0; i < 4; i++)
        if (s_relay[i]) m |= (1u << i);
    return m;
}
