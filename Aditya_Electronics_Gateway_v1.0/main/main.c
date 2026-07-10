/*
 * main.c  —  ESP32 IoT Monitor
 *
 * WiFi behaviour:
 *   - Initial connect: tries with exponential backoff.
 *   - Mid-session disconnect: auto-reconnect forever (2s→4s→8s→16s→30s cap).
 *   - LED fast blink when disconnected, slow blink when connected.
 *   - Buzzer beeps when no signal, silent when connected.
 */

#include "config.h"
#include "uart_parser.h"
#include "web_server.h"
#include "relay_logic.h"
#include "status_led.h"
#include "remote_server.h"
#include "bt_commission.h"

#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "main";

#define WIFI_CONNECTED_BIT      BIT0
#define WIFI_FAIL_BIT           BIT1
#define WIFI_RETRY_DELAY_MIN_MS 2000u
#define WIFI_RETRY_DELAY_MAX_MS 30000u

static EventGroupHandle_t s_wifi_eg;
static char  s_ssid[64] = {0};
static char  s_pass[64] = {0};
static bool  s_server_started   = false;
static int      s_retry_count    = 0;
static uint32_t s_retry_delay_ms = WIFI_RETRY_DELAY_MIN_MS;

/* ─────────────────────────────────────────────────────────────
 *  Reconnect task — waits backoff delay then reconnects
 * ───────────────────────────────────────────────────────────── */
static void wifi_reconnect_task(void *arg)
{
    uint32_t delay_ms = *((uint32_t *)arg);
    free(arg);
    ESP_LOGW(TAG, "Reconnecting in %lums...", (unsigned long)delay_ms);
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
    esp_wifi_connect();
    vTaskDelete(NULL);
}

/* ─────────────────────────────────────────────────────────────
 *  WiFi event handler — registered once, never unregistered.
 *  Handles initial connect + all mid-session reconnects.
 * ───────────────────────────────────────────────────────────── */
static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {

        esp_wifi_connect();
        ESP_LOGI(TAG, "Connecting to %s...", s_ssid);

    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {

        wifi_event_sta_disconnected_t *disc =
            (wifi_event_sta_disconnected_t *)data;

        s_retry_count++;
        ESP_LOGW(TAG, "Disconnected (reason=%d) retry #%d in %lums",
                 disc->reason, s_retry_count,
                 (unsigned long)s_retry_delay_ms);

        /* LED fast blink, buzzer on */
        status_led_set_mode(LED_MODE_SEARCHING);
        buzzer_set_no_signal(true);

        /* Signal startup failure if still in initial connect phase */
        if (!(xEventGroupGetBits(s_wifi_eg) & WIFI_CONNECTED_BIT)) {
            if (s_retry_count >= WIFI_MAX_RETRY)
                xEventGroupSetBits(s_wifi_eg, WIFI_FAIL_BIT);
        }

        /* Spawn reconnect task with current backoff delay */
        uint32_t *delay = malloc(sizeof(uint32_t));
        if (delay) {
            *delay = s_retry_delay_ms;
            xTaskCreate(wifi_reconnect_task, "wifi_retry",
                        2048, delay, 5, NULL);
        } else {
            esp_wifi_connect();   /* malloc failed — retry immediately */
        }

        /* Double delay for next retry, cap at max */
        s_retry_delay_ms *= 2u;
        if (s_retry_delay_ms > WIFI_RETRY_DELAY_MAX_MS)
            s_retry_delay_ms = WIFI_RETRY_DELAY_MAX_MS;

    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {

        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Connected! IP: " IPSTR, IP2STR(&ev->ip_info.ip));

        /* Reset retry state */
        s_retry_count    = 0;
        s_retry_delay_ms = WIFI_RETRY_DELAY_MIN_MS;

        /* LED slow blink, buzzer off */
        status_led_set_mode(LED_MODE_CONNECTED);
        buzzer_set_no_signal(false);

        /* Update event bits */
        xEventGroupClearBits(s_wifi_eg, WIFI_FAIL_BIT);
        xEventGroupSetBits(s_wifi_eg,   WIFI_CONNECTED_BIT);

        /* Start server only on FIRST successful connect */
        if (!s_server_started) {
            s_server_started = true;
#if LOCAL_WEB_SERVER
            web_server_start();
            ESP_LOGI(TAG, "Dashboard: http://" IPSTR "/",
                     IP2STR(&ev->ip_info.ip));
#else
            remote_server_start();
            ESP_LOGI(TAG, "Remote: %s%s", REMOTE_HOST, REMOTE_BASE);
#endif
        }
    }
}

/* ─────────────────────────────────────────────────────────────
 *  wifi_init — init stack, register handler, start, wait
 * ───────────────────────────────────────────────────────────── */
static void wifi_init(void)
{
    s_wifi_eg = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    /* Register permanently — handles all future reconnects */
    ESP_ERROR_CHECK(esp_event_handler_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(
        IP_EVENT,   IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL));

    wifi_config_t wc = {0};
    strncpy((char *)wc.sta.ssid,     s_ssid, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, s_pass, sizeof(wc.sta.password) - 1);
    wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* Wait up to 30s for first connect — app continues either way */
    xEventGroupWaitBits(s_wifi_eg,
                        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                        pdFALSE, pdFALSE,
                        pdMS_TO_TICKS(30000));
}

/* ─────────────────────────────────────────────────────────────
 *  app_main
 * ───────────────────────────────────────────────────────────── */
void app_main(void)
{
    ESP_LOGI(TAG, "=== Aditya Electronics Gateway v1.0 ===");
    ESP_LOGI(TAG, "WiFi: %s | Server: %s",
             BT_COMMISSIONING ? "BT-commission" : "hardcoded",
             LOCAL_WEB_SERVER  ? "local"          : "remote");

    /* NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* Peripherals */
    uart_parser_init();
    relay_logic_init();
    status_led_init();
    buzzer_init();

    /* LED fast blink while connecting */
    status_led_set_mode(LED_MODE_SEARCHING);
    buzzer_set_no_signal(true);

    /* WiFi credentials */
#if BT_COMMISSIONING && defined(CONFIG_BT_ENABLED)
    if (!bt_commission_load_nvs(s_ssid, s_pass, sizeof(s_ssid))) {
        ESP_LOGI(TAG, "No NVS creds — starting BT (%s)", BT_DEVICE_NAME);
        bool got = bt_commission_run(s_ssid, s_pass, sizeof(s_ssid));
        if (!got) {
            ESP_LOGW(TAG, "BT timeout — using hardcoded");
            strncpy(s_ssid, WIFI_SSID,    sizeof(s_ssid) - 1);
            strncpy(s_pass, WIFI_PASSWORD, sizeof(s_pass) - 1);
        }
    } else {
        ESP_LOGI(TAG, "NVS creds: %s", s_ssid);
    }
    bt_commission_stop();
#else
    strncpy(s_ssid, WIFI_SSID,    sizeof(s_ssid) - 1);
    strncpy(s_pass, WIFI_PASSWORD, sizeof(s_pass) - 1);
    ESP_LOGI(TAG, "Hardcoded SSID: %s", s_ssid);
#endif

    /* WiFi init + first connect */
    wifi_init();

    /* Heartbeat loop */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        EventBits_t bits = xEventGroupGetBits(s_wifi_eg);
        ESP_LOGI(TAG, "WiFi:%s pkts=%lu uart:%s relays=0x%X retry#%d",
                 (bits & WIFI_CONNECTED_BIT) ? "UP" : "DOWN",
                 (unsigned long)uart_parser_packet_count(),
                 uart_parser_is_online() ? "OK" : "LOST",
                 relay_logic_get_mask(),
                 s_retry_count);
    }
}