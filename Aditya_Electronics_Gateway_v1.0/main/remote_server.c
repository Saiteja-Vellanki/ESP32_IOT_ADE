/*
 * remote_server.c  —  Full IOT_Device_Protocol_20260628 implementation
 *
 *  API 1  register.php        — power ON (status=1) / OFF (status=0)
 *  API 2  ledstatus.php       — 25 switch states, LED_CNT=ALL every 15 s
 *  API 3  relaystatus.php     — 4 relay states
 *  API 5  readrelaystatus.php — poll relay cmds  (JSON response)
 *  API 6  readlockstatus.php  — poll lock/unlock (JSON response)
 *  API 7  softresetstatus.php — poll soft reset  (JSON response)
 *
 *  deviceid = MAC address (AABBCCDDEEFF)
 *  URL format: http://<host>/<base>/<endpoint>.php?deviceid=<MAC>&...
 */

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
#include "esp_system.h"
#include "cJSON.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "remote";

#define HTTP_RESP_BUF_SZ    512u
#define LED_UPDATE_MS       500u
#define RELAY_UPDATE_MS     500u
#define LED_ALL_MS          15000u   /* LED_CNT=ALL health heartbeat */
#define LOCK_POLL_MS        5000u
#define RESET_POLL_MS       5000u

static volatile bool s_locked = false;

/* ── MAC as deviceid ─────────────────────────────────────────── */
static void get_mac(char *out, size_t sz)
{
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    snprintf(out, sz, "%02X%02X%02X%02X%02X%02X",
             mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
}

/* ── HTTP GET with proper open/read/close (no MIN needed) ────── */
static bool http_get(const char *url, char *resp_buf, size_t resp_sz)
{
    esp_http_client_config_t cfg = {
        .url        = url,
        .method     = HTTP_METHOD_GET,
        .timeout_ms = 8000,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return false;

    esp_err_t err = esp_http_client_open(c, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(c);
        return false;
    }

    int content_len = esp_http_client_fetch_headers(c);

    if (resp_buf && resp_sz > 0) {
        /* explicit ternary — no MIN() macro needed */
        int to_read = (content_len > 0 &&
                       (size_t)content_len < resp_sz - 1u)
                      ? content_len
                      : (int)(resp_sz - 1u);
        int n = esp_http_client_read(c, resp_buf, to_read);
        resp_buf[(n > 0) ? n : 0] = '\0';
    }

    int  code = esp_http_client_get_status_code(c);
    esp_http_client_close(c);
    esp_http_client_cleanup(c);

    bool ok = (err == ESP_OK && (code == 200 || code == 201));
    if (!ok) ESP_LOGW(TAG, "HTTP fail code=%d %s", code, url);
    return ok;
}

/* ── URL base builder ────────────────────────────────────────── */
static int url_base(char *buf, size_t sz,
                    const char *ep, const char *mac)
{
    return snprintf(buf, sz,
                    "http://%s%s/%s?deviceid=%s",
                    REMOTE_HOST, REMOTE_BASE, ep, mac);
}

/* ═══════════════════════════════════════════════════════════════
   API 1 — Register
   register.php?deviceid=<MAC>&device_slno=<SN>&model=<M>
              &status=<1|0>&location=<L>
   ═══════════════════════════════════════════════════════════════ */
static bool api_register(const char *mac, int status)
{
    char url[320];
    snprintf(url, sizeof(url),
             "http://%s%s/register.php"
             "?deviceid=%s&device_slno=%s&model=%s&status=%d&location=%s",
             REMOTE_HOST, REMOTE_BASE,
             mac, DEVICE_SERIAL, DEVICE_MODEL,
             status, DEVICE_LOCATION);
    ESP_LOGI(TAG, "REGISTER status=%d", status);
    return http_get(url, NULL, 0);
}

/* ═══════════════════════════════════════════════════════════════
   API 2 — LED status
   ledstatus.php?deviceid=<MAC>[&LED_CNT=ALL]&L1=x&...&L25=x
   LED_CNT=ALL every 15 s = health heartbeat
   ═══════════════════════════════════════════════════════════════ */
static bool api_led_status(const char *mac, bool send_all)
{
    char url[640];
    int  pos = url_base(url, sizeof(url), "ledstatus.php", mac);

    if (send_all)
        pos += snprintf(url+pos, sizeof(url)-(size_t)pos, "&LED_CNT=ALL");

    for (int i = 0; i < GPIO_COUNT; i++) {
        int val = uart_parser_get_gpio((uint8_t)i) ? 1 : 0;
        pos += snprintf(url+pos, sizeof(url)-(size_t)pos,
                        "&L%d=%d", i+1, val);
    }
    return http_get(url, NULL, 0);
}

/* ═══════════════════════════════════════════════════════════════
   API 3 — Relay status
   relaystatus.php?deviceid=<MAC>&R1=x&R2=x&R3=x&R4=x
   ═══════════════════════════════════════════════════════════════ */
static bool api_relay_status(const char *mac)
{
    uint8_t m = relay_logic_get_mask();
    char url[320];
    int pos = url_base(url, sizeof(url), "relaystatus.php", mac);
    snprintf(url+pos, sizeof(url)-(size_t)pos,
             "&R1=%d&R2=%d&R3=%d&R4=%d",
             (m>>0)&1, (m>>1)&1, (m>>2)&1, (m>>3)&1);
    return http_get(url, NULL, 0);
}

/* ═══════════════════════════════════════════════════════════════
   API 5 — Read relay commands from server
   Response JSON:
   {"RelayStatus":[{"RlyNo":1,"status":1},...], "success":1}
   ═══════════════════════════════════════════════════════════════ */
static void api_read_relay(const char *mac)
{
    char url[256];
    url_base(url, sizeof(url), "readrelaystatus.php", mac);

    char resp[HTTP_RESP_BUF_SZ] = {0};
    if (!http_get(url, resp, sizeof(resp))) return;

    cJSON *root = cJSON_Parse(resp);
    if (!root) { ESP_LOGW(TAG,"relay JSON parse fail"); return; }

    cJSON *ok  = cJSON_GetObjectItem(root, "success");
    cJSON *arr = cJSON_GetObjectItem(root, "RelayStatus");

    if (ok && ok->valueint == 1 && cJSON_IsArray(arr)) {
        cJSON *item;
        cJSON_ArrayForEach(item, arr) {
            cJSON *rno = cJSON_GetObjectItem(item, "RlyNo");
            cJSON *st  = cJSON_GetObjectItem(item, "status");
            if (!rno || !st) continue;
            int n   = rno->valueint;   /* 1-4 */
            int srv = st->valueint;    /* 0/1 */
            if (n < 1 || n > 4) continue;
            bool cur = relay_logic_get_state((uint8_t)n);
            if (srv == 1 && !cur) {
                ESP_LOGI(TAG, "Server: RL%d ON", n);
                relay_logic_command((uint8_t)n);
            }
        }
    }
    cJSON_Delete(root);
}

/* ═══════════════════════════════════════════════════════════════
   API 6 — Lock/unlock
   Response: {"LockStatus":1,"success":1}
   ═══════════════════════════════════════════════════════════════ */
static void api_read_lock(const char *mac)
{
    char url[256];
    url_base(url, sizeof(url), "readlockstatus.php", mac);

    char resp[128] = {0};
    if (!http_get(url, resp, sizeof(resp))) return;

    cJSON *root = cJSON_Parse(resp);
    if (!root) return;

    cJSON *ok   = cJSON_GetObjectItem(root, "success");
    cJSON *lock = cJSON_GetObjectItem(root, "LockStatus");

    if (ok && ok->valueint == 1 && lock) {
        bool new_lock = (lock->valueint == 1);
        if (new_lock != s_locked) {
            s_locked = new_lock;
            ESP_LOGI(TAG, "Device %s", s_locked ? "LOCKED" : "UNLOCKED");
        }
    }
    cJSON_Delete(root);
}

/* ═══════════════════════════════════════════════════════════════
   API 7 — Soft reset
   Response: {"SoftResetStatus":1,"success":1}
   ═══════════════════════════════════════════════════════════════ */
static void api_read_soft_reset(const char *mac)
{
    char url[256];
    url_base(url, sizeof(url), "softresetstatus.php", mac);

    char resp[128] = {0};
    if (!http_get(url, resp, sizeof(resp))) return;

    cJSON *root = cJSON_Parse(resp);
    if (!root) return;

    cJSON *ok  = cJSON_GetObjectItem(root, "success");
    cJSON *rst = cJSON_GetObjectItem(root, "SoftResetStatus");

    if (ok && ok->valueint == 1 && rst && rst->valueint == 1) {
        ESP_LOGW(TAG, "Soft reset by server — restarting...");
        cJSON_Delete(root);
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    }
    cJSON_Delete(root);
}

/* ── Background task ─────────────────────────────────────────── */
static void remote_task(void *arg)
{
    char mac[16];
    get_mac(mac, sizeof(mac));
    ESP_LOGI(TAG, "DeviceID (MAC): %s  server: http://%s%s",
             mac, REMOTE_HOST, REMOTE_BASE);

    bool     registered    = false;
    uint32_t last_led      = 0u;
    uint32_t last_relay    = 0u;
    uint32_t last_poll     = 0u;
    uint32_t last_all      = 0u;
    uint32_t last_lock     = 0u;
    uint32_t last_rst      = 0u;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(200));
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000ULL);

        /* API 1 — Register on boot */
        if (!registered) {
            if (api_register(mac, 1)) {
                registered = true;
                status_led_set_mode(LED_MODE_CONNECTED);
                buzzer_set_no_signal(false);
                ESP_LOGI(TAG, "Registered OK");
            } else {
                status_led_set_mode(LED_MODE_SEARCHING);
                buzzer_set_no_signal(true);
                vTaskDelay(pdMS_TO_TICKS(5000));
                continue;
            }
        }

        /* API 2 — LED status every 500 ms, LED_CNT=ALL every 15 s */
        if ((now - last_led) >= LED_UPDATE_MS) {
            bool all = ((now - last_all) >= LED_ALL_MS);
            if (!s_locked) api_led_status(mac, all);
            if (all) { last_all = now;
                       ESP_LOGI(TAG,"LED_CNT=ALL sent"); }
            last_led = now;
        }

        /* API 3 — Relay status every 500 ms */
        if ((now - last_relay) >= RELAY_UPDATE_MS) {
            api_relay_status(mac);
            last_relay = now;
        }

        /* API 5 — Poll relay commands */
        if ((now - last_poll) >= (uint32_t)REMOTE_POLL_MS) {
            if (!s_locked) api_read_relay(mac);
            last_poll = now;
        }

        /* API 6 — Poll lock status every 5 s */
        if ((now - last_lock) >= LOCK_POLL_MS) {
            api_read_lock(mac);
            last_lock = now;
        }

        /* API 7 — Poll soft reset every 5 s */
        if ((now - last_rst) >= RESET_POLL_MS) {
            api_read_soft_reset(mac);
            last_rst = now;
        }
    }
}

void remote_server_start(void)
{
    xTaskCreate(remote_task, "remote", REMOTE_TASK_STACK,
                NULL, REMOTE_TASK_PRIO, NULL);
}

void remote_server_notify_relay_change(void) {}

bool remote_server_is_locked(void) { return s_locked; }
