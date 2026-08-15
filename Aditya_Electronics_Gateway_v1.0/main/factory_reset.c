#include "factory_reset.h"
#include "config.h"

#include "driver/gpio.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "factory_reset";

/*
 * Polls FACTORY_RESET_GPIO every FACTORY_RESET_POLL_MS. GPIO0 (the
 * default) is the BOOT button on most ESP32 dev boards: idle HIGH
 * via internal pull-up, pulled LOW while held.
 */
static void factory_reset_task(void *arg)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << FACTORY_RESET_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);

    uint32_t held_ms = 0u;
    bool     warned  = false;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(FACTORY_RESET_POLL_MS));

        int level = gpio_get_level(FACTORY_RESET_GPIO);
        if (level == 0) {   /* pressed (active LOW, pull-up idle HIGH) */
            held_ms += FACTORY_RESET_POLL_MS;

            if (!warned && held_ms >= 3000u) {
                ESP_LOGW(TAG, "Reset button held — keep holding %lus more "
                              "for factory reset (WiFi creds + portal IP)",
                         (unsigned long)((FACTORY_RESET_HOLD_MS - held_ms) / 1000u));
                warned = true;
            }

            if (held_ms >= FACTORY_RESET_HOLD_MS) {
                ESP_LOGW(TAG, "Factory reset triggered — erasing WiFi "
                              "credentials and custom AP IP");

                nvs_handle_t h;
                if (nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
                    nvs_erase_key(h, WIFI_NVS_KEY_SSID);
                    nvs_erase_key(h, WIFI_NVS_KEY_PASS);
                    nvs_erase_key(h, WIFI_NVS_KEY_AP_IP);
                    nvs_commit(h);
                    nvs_close(h);
                }

                ESP_LOGW(TAG, "Restarting into provisioning mode at "
                              "default IP %s", WIFI_AP_DEFAULT_IP);
                vTaskDelay(pdMS_TO_TICKS(300));
                esp_restart();
            }
        } else {
            held_ms = 0u;
            warned  = false;
        }
    }
}

/*
 * Second, separate button - WIFI_ONLY_RESET_GPIO. Same pattern as
 * factory_reset_task above, but only erases WIFI_NVS_KEY_SSID/PASS,
 * NOT WIFI_NVS_KEY_AP_IP - so the next boot skips Step 1 (IP is
 * still configured) and goes straight to the Step 2 WiFi-credentials
 * portal at the SAME IP as before, for reconnecting to a different
 * WiFi network without redoing IP setup.
 */
static void wifi_only_reset_task(void *arg)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << WIFI_ONLY_RESET_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);

    uint32_t held_ms = 0u;
    bool     warned  = false;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(FACTORY_RESET_POLL_MS));

        int level = gpio_get_level(WIFI_ONLY_RESET_GPIO);
        if (level == 0) {   /* pressed (active LOW, pull-up idle HIGH) */
            held_ms += FACTORY_RESET_POLL_MS;

            if (!warned && held_ms >= 2000u) {
                ESP_LOGW(TAG, "WiFi-only reset button held — keep holding "
                              "%lus more (WiFi creds only, IP stays)",
                         (unsigned long)((WIFI_ONLY_RESET_HOLD_MS - held_ms) / 1000u));
                warned = true;
            }

            if (held_ms >= WIFI_ONLY_RESET_HOLD_MS) {
                ESP_LOGW(TAG, "WiFi-only reset triggered — erasing WiFi "
                              "credentials (setup-page IP untouched)");

                nvs_handle_t h;
                if (nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
                    nvs_erase_key(h, WIFI_NVS_KEY_SSID);
                    nvs_erase_key(h, WIFI_NVS_KEY_PASS);
                    /* WIFI_NVS_KEY_AP_IP deliberately NOT erased here -
                     * that's the whole point of this being a separate,
                     * shorter-hold button from the full factory reset. */
                    nvs_commit(h);
                    nvs_close(h);
                }

                ESP_LOGW(TAG, "Restarting into WiFi setup at the same "
                              "IP as before...");
                vTaskDelay(pdMS_TO_TICKS(300));
                esp_restart();
            }
        } else {
            held_ms = 0u;
            warned  = false;
        }
    }
}

void factory_reset_init(void)
{
    xTaskCreate(factory_reset_task, "factory_reset", 2560, NULL, 4, NULL);
    ESP_LOGI(TAG, "Factory reset: hold GPIO%d for %lus to erase WiFi "
                  "credentials and reset portal IP to %s",
             FACTORY_RESET_GPIO,
             (unsigned long)(FACTORY_RESET_HOLD_MS / 1000u),
             WIFI_AP_DEFAULT_IP);

    xTaskCreate(wifi_only_reset_task, "wifi_only_reset", 2560, NULL, 4, NULL);
    ESP_LOGI(TAG, "WiFi-only reset: hold GPIO%d for %lus to erase WiFi "
                  "credentials only (setup-page IP stays as configured)",
             WIFI_ONLY_RESET_GPIO,
             (unsigned long)(WIFI_ONLY_RESET_HOLD_MS / 1000u));
}
