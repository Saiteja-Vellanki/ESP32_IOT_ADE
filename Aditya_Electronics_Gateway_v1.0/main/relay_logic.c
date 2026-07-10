#include "relay_logic.h"
#include "config.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "relay";

static const int RELAY_GPIO[4] = {
    RELAY1_GPIO, RELAY2_GPIO, RELAY3_GPIO, RELAY4_GPIO
};

static bool s_relay[4] = {false, false, false, false};

static void relay_set(uint8_t n, bool on)
{
    if (n > 3) return;
    s_relay[n] = on;
    gpio_set_level(RELAY_GPIO[n], on ? 1 : 0);
    ESP_LOGI(TAG, "RL%d → %s", n+1, on?"ON":"OFF");
}

static bool s_rl2_continuous = false;
static QueueHandle_t s_cmd_q;

static void relay_task(void *arg)
{
    uint8_t cmd;
    while (1) {
        if (xQueueReceive(s_cmd_q, &cmd, portMAX_DELAY) != pdTRUE)
            continue;
        switch (cmd) {
            case 1:
                relay_set(0, true);
                vTaskDelay(pdMS_TO_TICKS(RELAY_PULSE_MS));
                relay_set(0, false);
                break;
            case 2:
                if (!s_rl2_continuous) {
                    s_rl2_continuous = true;
                    relay_set(1, true);
                } else {
                    relay_set(1, false);
                    vTaskDelay(pdMS_TO_TICKS(RELAY_PULSE_MS));
                    relay_set(1, true);
                }
                break;
            case 3:
                relay_set(2, true);
                vTaskDelay(pdMS_TO_TICKS(RELAY_PULSE_MS));
                relay_set(3, true);
                vTaskDelay(pdMS_TO_TICKS(RELAY_PULSE_MS));
                relay_set(3, false);
                break;
            default:
                ESP_LOGW(TAG,"Unknown cmd:%d",cmd);
                break;
        }
    }
}

void relay_logic_init(void)
{
    for (int i = 0; i < 4; i++) {
        gpio_config_t io = {
            .pin_bit_mask = (1ULL << RELAY_GPIO[i]),
            .mode         = GPIO_MODE_OUTPUT,
            .pull_up_en   = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_DISABLE,
        };
        gpio_config(&io);
        gpio_set_level(RELAY_GPIO[i], 0);
    }
    ESP_LOGI(TAG,"Relays: RL1=G%d RL2=G%d RL3=G%d RL4=G%d",
             RELAY1_GPIO,RELAY2_GPIO,RELAY3_GPIO,RELAY4_GPIO);

    s_cmd_q = xQueueCreate(8, sizeof(uint8_t));
    configASSERT(s_cmd_q);
    xTaskCreate(relay_task,"relay",RELAY_TASK_STACK,
                NULL,RELAY_TASK_PRIO,NULL);
}

void relay_logic_command(uint8_t relay_num)
{
    if (relay_num < 1 || relay_num > 3) return;
    xQueueSend(s_cmd_q, &relay_num, 0);
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
