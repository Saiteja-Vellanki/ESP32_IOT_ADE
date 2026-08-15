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
#include "wifi_ap_provision.h"
#include "factory_reset.h"

#include "nvs_flash.h"
#include "nvs.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_system.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "ntp_time.h"
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
static volatile bool s_reconnect_pending = false;

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
    s_reconnect_pending = false;
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
            if (s_retry_count >= WIFI_MAX_RETRY) {
                xEventGroupSetBits(s_wifi_eg, WIFI_FAIL_BIT);

#if WIFI_PROVISION_MODE == 1
                /* Never connected even once since boot, after
                 * WIFI_MAX_RETRY attempts - almost certainly wrong
                 * saved credentials (bad password, SSID changed,
                 * etc), not a transient outage. This only fires
                 * before the first successful connect (guarded by
                 * the WIFI_CONNECTED_BIT check above, which is never
                 * cleared once set), so a later real outage after
                 * having connected before will NOT wipe good
                 * credentials - only reconnect forever as before. */
                ESP_LOGE(TAG, "Failed to connect after %d attempts - "
                              "erasing saved credentials and restarting "
                              "into the AP portal", s_retry_count);
                nvs_handle_t h;
                if (nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
                    nvs_erase_key(h, WIFI_NVS_KEY_SSID);
                    nvs_erase_key(h, WIFI_NVS_KEY_PASS);
                    nvs_commit(h);
                    nvs_close(h);
                }
                vTaskDelay(pdMS_TO_TICKS(300));
                esp_restart();
#endif
            }
        }

        /* Do not create multiple reconnect tasks if the WiFi driver
         * emits more than one DISCONNECTED event during recovery. */
        if (!s_reconnect_pending) {
            uint32_t *delay = malloc(sizeof(uint32_t));
            if (delay) {
                *delay = s_retry_delay_ms;
                s_reconnect_pending = true;
                if (xTaskCreate(wifi_reconnect_task, "wifi_retry",
                                2048, delay, 5, NULL) != pdPASS) {
                    s_reconnect_pending = false;
                    free(delay);
                    esp_wifi_connect();
                }
            } else {
                esp_wifi_connect();
            }
        }

        /* Double delay for next retry, cap at max */
        s_retry_delay_ms *= 2u;
        if (s_retry_delay_ms > WIFI_RETRY_DELAY_MAX_MS)
            s_retry_delay_ms = WIFI_RETRY_DELAY_MAX_MS;

    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {

        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Connected! IP: " IPSTR, IP2STR(&ev->ip_info.ip));
        ntp_init();

        /* Reset retry state */
        s_retry_count    = 0;
        s_retry_delay_ms = WIFI_RETRY_DELAY_MIN_MS;
        s_reconnect_pending = false;

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
            ESP_LOGI(TAG, "Remote (HTTPS): %s%s", REMOTE_HOST, REMOTE_BASE);
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

    /* esp_netif_init() and esp_event_loop_create_default() may
     * already have been called by wifi_ap_provision_run() if AP
     * portal provisioning ran first — guard against re-init crash
     * (ESP_ERR_INVALID_STATE) exactly like wifi_ap_provision.c does. */
    esp_err_t netif_err = esp_netif_init();
    if (netif_err != ESP_OK && netif_err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(netif_err);
    }

    esp_err_t evloop_err = esp_event_loop_create_default();
    if (evloop_err != ESP_OK && evloop_err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(evloop_err);
    }

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

    /* Reduce max TX power — lowers peak current draw during the
     * connection handshake, which is when brownout resets are
     * most likely on boards with marginal power supply/decoupling.
     * 8.5dBm is still plenty for typical indoor range; raise this
     * back toward 20 (max) once hardware power supply is fixed
     * (proper capacitor + adequate power source) if range suffers. */
    esp_wifi_set_max_tx_power(34);   /* ~8.5dBm, in 0.25dBm units */

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
    ESP_LOGI(TAG, "=== Aditya Electronics Gateway ===");
    ESP_LOGI(TAG, "Firmware version: %s", CONFIG_FW_VERSION);
#if WIFI_PROVISION_MODE == 1
    ESP_LOGI(TAG, "WiFi: AP-portal | Server: %s",
             LOCAL_WEB_SERVER ? "local" : "remote");
#else
    ESP_LOGI(TAG, "WiFi: hardcoded | Server: %s",
             LOCAL_WEB_SERVER ? "local" : "remote");
#endif

    /* NVS */
    esp_err_t ret = nvs_flash_init();
    ESP_LOGI(TAG, "nvs_flash_init() returned: %s", esp_err_to_name(ret));
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition needs erase (%s) — WIPING ALL SAVED "
                      "DATA including WiFi credentials!", esp_err_to_name(ret));
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
        ESP_LOGI(TAG, "nvs_flash_init() after erase returned: %s",
                 esp_err_to_name(ret));
    }
    ESP_ERROR_CHECK(ret);

    /* Peripherals */
    uart_parser_init();
    relay_logic_init();
    status_led_init();
    buzzer_init();
    factory_reset_init();   /* watches the hold-button regardless of WiFi state */

    /* LED fast blink while connecting */
    status_led_set_mode(LED_MODE_SEARCHING);
    buzzer_set_no_signal(true);

    /* ── WiFi credentials — 2-way provisioning mode ────────────
     * Both modes check NVS first — if credentials were already
     * saved by a previous AP-portal run, they're reused directly
     * without re-provisioning.    */
#if WIFI_PROVISION_MODE == 1
    /* ── Step 1: mandatory IP configuration (first setup / after
     * factory reset only - no-op + returns immediately once an IP
     * is already saved). Reboots on its own if it actually runs. */
    wifi_ap_ip_config_run();

    /* ── Step 2: WiFi AP portal (recommended, most reliable) ── */
    if (!wifi_provision_load_nvs(s_ssid, s_pass, sizeof(s_ssid))) {
        char ap_ip[16];
        wifi_provision_get_ap_ip(ap_ip, sizeof(ap_ip));
        ESP_LOGI(TAG, "No NVS creds — starting AP portal (%s)", WIFI_AP_SSID);
        ESP_LOGI(TAG, "Connect to WiFi \"%s\" then open http://%s/",
                 WIFI_AP_SSID, ap_ip);
        bool got = wifi_ap_provision_run(s_ssid, s_pass, sizeof(s_ssid));
        wifi_ap_provision_stop();

        /* Settle delay — the AP→STA radio transition draws a real
         * current spike (deinit AP, reinit STA, associate, DHCP).
         * On boards with marginal power supply/decoupling, this
         * spike alone can brown out the chip (confirmed via
         * rst:0x1 POWERON_RESET immediately after credential
         * submission). This delay lets supply rails/capacitors
         * settle before the next radio-heavy operation begins.
         * Root cause is the power supply, not this delay — but it
         * meaningfully reduces the chance of the reset in practice.*/
        vTaskDelay(pdMS_TO_TICKS(500));

        if (!got) {
            ESP_LOGW(TAG, "AP portal timeout — using hardcoded");
            strncpy(s_ssid, WIFI_SSID,    sizeof(s_ssid) - 1);
            strncpy(s_pass, WIFI_PASSWORD, sizeof(s_pass) - 1);
        }
    } else {
        ESP_LOGI(TAG, "NVS creds: %s", s_ssid);
    }

#else
    /* ── Mode 0: hardcoded ────────────────────────────────────── */
    strncpy(s_ssid, WIFI_SSID,    sizeof(s_ssid) - 1);
    strncpy(s_pass, WIFI_PASSWORD, sizeof(s_pass) - 1);
    ESP_LOGI(TAG, "Hardcoded SSID: %s", s_ssid);
#endif

    /* WiFi init + first connect */
    wifi_init();

    /* Heartbeat loop — organized multi-line status summary, everything
     * in one place every 10s so the console log is easy to scan
     * instead of hunting through scattered lines from other modules. */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        EventBits_t bits = xEventGroupGetBits(s_wifi_eg);

        /* Build the active-switches list, same decoding style as
         * uart_parser.c's own switch-packet log. */
        char active_sw[160];
        int  pos = 0;
        bool any_sw = false;
        for (uint8_t i = 0; i < GPIO_COUNT; i++) {
            if (uart_parser_get_gpio(i)) {
                pos += snprintf(active_sw + pos, sizeof(active_sw) - (size_t)pos,
                                "%sSW%02u", any_sw ? " " : "", (unsigned)(i + 1));
                any_sw = true;
            }
        }
        if (!any_sw) snprintf(active_sw, sizeof(active_sw), "none");

        ESP_LOGI(TAG, "================ STATUS SUMMARY ================");
        ESP_LOGI(TAG, "WiFi     : %s | retry#%d",
                 (bits & WIFI_CONNECTED_BIT) ? "UP" : "DOWN", s_retry_count);
        ESP_LOGI(TAG, "NUC UART : %s | pkts=%lu",
                 uart_parser_is_online() ? "OK" : "LOST",
                 (unsigned long)uart_parser_packet_count());
        ESP_LOGI(TAG, "Battery  : %umV (%u%%) | Power: %s | Low-batt: %s",
                 uart_parser_get_bat_mv(), uart_parser_get_bat_pct(),
                 uart_parser_is_power_fail() ? "FAIL" : "OK",
                 uart_parser_is_low_battery() ? "YES" : "no");
        uint8_t rmask = remote_server_get_report_mask();
        ESP_LOGI(TAG, "Relays   : R1=%d R2=%d R3=%d R4=%d",
                 (rmask>>0)&1, (rmask>>1)&1, (rmask>>2)&1, (rmask>>3)&1);
        ESP_LOGI(TAG, "Lock     : %s", remote_server_is_locked() ? "LOCKED" : "unlocked");
        ESP_LOGI(TAG, "Switches : %s", active_sw);
        ESP_LOGI(TAG, "==================================================");
    }
}