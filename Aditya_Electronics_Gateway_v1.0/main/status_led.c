#include "status_led.h"
#include "config.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "led";

static volatile led_mode_t s_led_mode  = LED_MODE_SEARCHING;
static volatile bool       s_no_signal = true;

static void led_task(void *arg)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << STATUS_LED_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);

    while (1) {
        uint32_t half;
        switch (s_led_mode) {
            case LED_MODE_CONNECTED:   half = LED_SLOW_BLINK_MS   / 2; break;
            case LED_MODE_LOW_BATTERY: half = LED_LOWBAT_BLINK_MS / 2; break;
            default:                   half = LED_FAST_BLINK_MS   / 2; break;
        }
        gpio_set_level(STATUS_LED_GPIO, 1);
        vTaskDelay(pdMS_TO_TICKS(half));
        gpio_set_level(STATUS_LED_GPIO, 0);
        vTaskDelay(pdMS_TO_TICKS(half));
    }
}

static void buzzer_task(void *arg)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << BUZZER_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    gpio_set_level(BUZZER_GPIO, 0);

    uint32_t last_beep = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(500));
        if (!s_no_signal) { last_beep = 0; continue; }
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000ULL);
        if (last_beep != 0 && (now - last_beep) < BUZZER_INTERVAL_MS)
            continue;
        last_beep = now;
        for (int i = 0; i < BUZZER_NO_SIGNAL_BEEPS; i++) {
            gpio_set_level(BUZZER_GPIO, 1);
            vTaskDelay(pdMS_TO_TICKS(BUZZER_BEEP_ON_MS));
            gpio_set_level(BUZZER_GPIO, 0);
            vTaskDelay(pdMS_TO_TICKS(BUZZER_BEEP_OFF_MS));
        }
        ESP_LOGW(TAG, "Buzzer: 5 beeps — no signal");
    }
}

void status_led_init(void)
{
    xTaskCreate(led_task,"led",LED_TASK_STACK,NULL,LED_TASK_PRIO,NULL);
    ESP_LOGI(TAG,"LED GPIO%d",STATUS_LED_GPIO);
}

void status_led_set_mode(led_mode_t mode) { s_led_mode = mode; }

void buzzer_init(void)
{
    xTaskCreate(buzzer_task,"buzzer",2048,NULL,3,NULL);
    ESP_LOGI(TAG,"Buzzer GPIO%d",BUZZER_GPIO);
}

void buzzer_set_no_signal(bool v) { s_no_signal = v; }
