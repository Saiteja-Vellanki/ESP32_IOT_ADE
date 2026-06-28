#include "remote_server.h"
#include "config.h"
#include "uart_parser.h"
#include "relay_logic.h"
#include "status_led.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "remote";

/* ── Get MAC string ──────────────────────────────────────────── */
static void get_mac(char *out, size_t sz)
{
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    snprintf(out, sz, "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/* ── HTTP GET helper ─────────────────────────────────────────── */
static bool http_get(const char *url, char *resp_buf, size_t resp_sz)
{
    esp_http_client_config_t cfg = {
        .url        = url,
        .method     = HTTP_METHOD_GET,
        .timeout_ms = 5000,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    esp_err_t err = esp_http_client_perform(c);
    int code      = esp_http_client_get_status_code(c);

    if (err == ESP_OK && resp_buf && resp_sz > 0) {
        esp_http_client_read(c, resp_buf, (int)resp_sz - 1);
    }
    esp_http_client_cleanup(c);

    bool ok = (err == ESP_OK && (code == 200 || code == 201));
    if (!ok) ESP_LOGW(TAG, "HTTP GET failed: %s  err=%s code=%d",
                      url, esp_err_to_name(err), code);
    return ok;
}

/* ════════════════════════════════════════════════════════════════
   API CALL 1 — Register device on power ON
   GET /register.php?id=<MAC>&device_slno=<SN>&model=<M>&status=1&location=<L>
   ════════════════════════════════════════════════════════════════ */
static bool api_register(const char *mac, int status)
{
    char url[256];
    snprintf(url, sizeof(url),
        "http://%s%s/register.php"
        "?id=%s&device_slno=%s&model=%s&status=%d&location=%s",
        REMOTE_HOST, REMOTE_BASE,
        mac, DEVICE_SERIAL, DEVICE_MODEL,
        status, DEVICE_LOCATION);

    ESP_LOGI(TAG, "REGISTER → %s", url);
    return http_get(url, NULL, 0);
}

/* ════════════════════════════════════════════════════════════════
   API CALL 2 — Update switch/LED status
   GET /ledstatus.php?id=<MAC>&L1=0&L2=1&...&L25=0
   Values: 0=OFF 1=ON 2=Blinking
   ════════════════════════════════════════════════════════════════ */
static bool api_led_status(const char *mac)
{
    char url[512];
    int  pos = snprintf(url, sizeof(url),
                        "http://%s%s/ledstatus.php?id=%s",
                        REMOTE_HOST, REMOTE_BASE, mac);

    for (int i = 0; i < GPIO_COUNT; i++) {
        int val = uart_parser_get_gpio((uint8_t)i) ? 1 : 0;
        pos += snprintf(url + pos, sizeof(url) - (size_t)pos,
                        "&L%d=%d", i + 1, val);
    }

    ESP_LOGD(TAG, "LED STATUS → %s", url);
    return http_get(url, NULL, 0);
}

/* ════════════════════════════════════════════════════════════════
   API CALL 3 — Update relay status
   GET /relaystatus.php?id=<MAC>&R1=1&R2=0&R3=0&R4=0
   ════════════════════════════════════════════════════════════════ */
static bool api_relay_status(const char *mac)
{
    uint8_t mask = relay_logic_get_mask();
    char url[256];
    snprintf(url, sizeof(url),
        "http://%s%s/relaystatus.php"
        "?id=%s&R1=%d&R2=%d&R3=%d&R4=%d",
        REMOTE_HOST, REMOTE_BASE, mac,
        (mask >> 0) & 1,
        (mask >> 1) & 1,
        (mask >> 2) & 1,
        (mask >> 3) & 1);

    ESP_LOGD(TAG, "RELAY STATUS → %s", url);
    return http_get(url, NULL, 0);
}

/* ════════════════════════════════════════════════════════════════
   API CALL 4 — Power OFF notification
   GET /register.php?id=<MAC>&...&status=2...
   ════════════════════════════════════════════════════════════════ */
static bool api_power_off(const char *mac)
{
    return api_register(mac, 2);   /* status=2 = switched OFF */
}

/* ════════════════════════════════════════════════════════════════
   API CALL 5 — Poll server for relay commands from Android app
   GET /readrelaystatus.php?id=<MAC>&device_slno=<SN>
   Response: R1=1&R2=0&R3=0&R4=0
   ════════════════════════════════════════════════════════════════ */
static void api_read_relay_status(const char *mac)
{
    char url[256];
    snprintf(url, sizeof(url),
        "http://%s%s/readrelaystatus.php?id=%s&device_slno=%s",
        REMOTE_HOST, REMOTE_BASE, mac, DEVICE_SERIAL);

    char resp[64] = {0};
    if (!http_get(url, resp, sizeof(resp))) return;

    ESP_LOGI(TAG, "readrelaystatus response: %s", resp);

    /*
     * Parse: R1=1&R2=0&R3=1&R4=0
     * For each relay that is now commanded ON by the server,
     * compare with current state and send command if changed.
     */
    for (int i = 1; i <= 4; i++) {
        char key[4];
        snprintf(key, sizeof(key), "R%d=", i);
        char *p = strstr(resp, key);
        if (!p) continue;
        p += strlen(key);
        int server_state = atoi(p);
        bool cur_state   = relay_logic_get_state((uint8_t)i);

        /* If server says ON and relay is currently OFF → command it */
        if (server_state == 1 && !cur_state) {
            ESP_LOGI(TAG, "Server commanded RL%d ON", i);
            relay_logic_command((uint8_t)i);
        }
    }
}

/* ════════════════════════════════════════════════════════════════
   Background task
   ════════════════════════════════════════════════════════════════ */
static void remote_task(void *arg)
{
    char     mac[16];
    get_mac(mac, sizeof(mac));

    bool     registered     = false;
    uint32_t last_led_upd   = 0;
    uint32_t last_relay_upd = 0;
    uint32_t last_poll      = 0;

    ESP_LOGI(TAG, "Remote task started — MAC: %s → %s%s",
             mac, REMOTE_HOST, REMOTE_BASE);

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(200));

        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000ULL);

        /* ── Register on first connect ── */
        if (!registered) {
            if (api_register(mac, 1)) {
                registered = true;
                ESP_LOGI(TAG, "Device registered successfully");
                status_led_set_mode(LED_MODE_CONNECTED);
                buzzer_set_no_signal(false);
            } else {
                status_led_set_mode(LED_MODE_SEARCHING);
                buzzer_set_no_signal(true);
                vTaskDelay(pdMS_TO_TICKS(5000));
                continue;
            }
        }

        /* ── Update switch/LED status every 500ms ── */
        if ((now - last_led_upd) >= 500u) {
            api_led_status(mac);
            last_led_upd = now;
        }

        /* ── Update relay status every 500ms ── */
        if ((now - last_relay_upd) >= 500u) {
            api_relay_status(mac);
            last_relay_upd = now;
        }

        /* ── Poll server for relay commands every 500ms ── */
        if ((now - last_poll) >= (uint32_t)REMOTE_POLL_MS) {
            api_read_relay_status(mac);
            last_poll = now;
        }
    }
}

void remote_server_start(void)
{
    xTaskCreate(remote_task, "remote", REMOTE_TASK_STACK,
                NULL, REMOTE_TASK_PRIO, NULL);
}

void remote_server_notify_relay_change(void)
{
    /* Relay state changed locally — will be picked up on next 500ms cycle */
    ESP_LOGD(TAG, "Relay change notified");
}
