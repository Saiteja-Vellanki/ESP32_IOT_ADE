/*
 * main.c  —  ESP32 IoT Monitor
 *
 * Boot sequence:
 *   1. NVS init
 *   2. UART2 parser task  (core 1, prio 10) — GPIO34=RX GPIO35=TX
 *   3. Relay logic init   (GPIO25/26/27/14)
 *   4. Status LED + Buzzer tasks
 *   5. WiFi STA connect
 *   6. HTTP server (LOCAL) or Remote server task (REMOTE)
 */

#include "config.h"
#include "uart_parser.h"
#include "web_server.h"
#include "relay_logic.h"
#include "status_led.h"
#include "remote_server.h"

#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

static const char *TAG = "main";

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1

static EventGroupHandle_t s_wifi_eg;
static int                s_retry = 0;

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        status_led_set_mode(LED_MODE_SEARCHING);
        buzzer_set_no_signal(true);
        if (s_retry < WIFI_MAX_RETRY) {
            esp_wifi_connect();
            s_retry++;
            ESP_LOGW(TAG, "WiFi retry %d/%d", s_retry, WIFI_MAX_RETRY);
        } else {
            xEventGroupSetBits(s_wifi_eg, WIFI_FAIL_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "IP: " IPSTR, IP2STR(&ev->ip_info.ip));
        s_retry = 0;
        status_led_set_mode(LED_MODE_CONNECTED);
        buzzer_set_no_signal(false);
        xEventGroupSetBits(s_wifi_eg, WIFI_CONNECTED_BIT);
    }
}

static bool wifi_init_sta(void)
{
    s_wifi_eg = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t h1, h2;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, &h1));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, &h2));

    wifi_config_t wc = {
        .sta = {
            .ssid     = WIFI_SSID,
            .password = WIFI_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Connecting to %s ...", WIFI_SSID);
    EventBits_t bits = xEventGroupWaitBits(s_wifi_eg,
                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                           pdFALSE, pdFALSE, portMAX_DELAY);

    esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, h2);
    esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, h1);
    vEventGroupDelete(s_wifi_eg);

    return (bits & WIFI_CONNECTED_BIT) != 0;
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== ESP32 IoT Monitor v1.0 ===");
    ESP_LOGI(TAG, "Build mode: %s",
             LOCAL_WEB_SERVER ? "LOCAL WEB SERVER" : "REMOTE SERVER");
    ESP_LOGI(TAG, "UART: RX=GPIO%d TX=GPIO%d @ %d baud",
             UART_RX_PIN, UART_TX_PIN, UART_BAUD_RATE);

    /* NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* Peripherals */
    uart_parser_init();     /* GPIO34=RX, GPIO35=TX            */
    relay_logic_init();     /* RL1-4 GPIO init + task          */
    status_led_init();      /* LED blink task                  */
    buzzer_init();          /* Buzzer 5-beep task              */

    /* WiFi */
    if (!wifi_init_sta()) {
        ESP_LOGE(TAG, "WiFi failed — running in UART-only mode");
        ESP_LOGE(TAG, "Check WIFI_SSID/WIFI_PASSWORD in config.h");
        while (1) vTaskDelay(pdMS_TO_TICKS(10000));
    }

    /* Web / Remote */
#if LOCAL_WEB_SERVER
    web_server_start();
    ESP_LOGI(TAG, "Local dashboard ready at http://<IP>/");
#else
    remote_server_start();
    ESP_LOGI(TAG, "Remote mode: posting to %s", REMOTE_HOST);
#endif

    /* Periodic log */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        ESP_LOGI(TAG, "Uptime pkts=%lu online=%s relays=0x%X",
                 (unsigned long)uart_parser_packet_count(),
                 uart_parser_is_online() ? "YES" : "NO",
                 relay_logic_get_mask());
    }
}
