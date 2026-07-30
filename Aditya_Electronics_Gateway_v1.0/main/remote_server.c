/*
 * remote_server.c  —  Aditya Electronics Gateway cloud API client
 *
 * Endpoint names below are the ones CONFIRMED working against the
 * live server (verified directly, not from the written protocol
 * doc, which uses different filenames that did not work when
 * tested: ledstatus.php/relaystatus.php/readrelaystatus.php/etc.
 * were replaced with update_led_status.php/update_relay_status.php/
 * read_relay_status.php/read_lock_status.php/read_soft_reset_status.php).
 *
 * All calls over HTTPS (esp_crt_bundle_attach validates the server's
 * real CA-signed certificate — same trust model a browser uses).
 *
 *  API 1  register.php            — status=1 ON / status=0 OFF (see config.h note)
 *  API 2  update_led_status.php   — 25 switch states, LED_CNT=ALL every 15s
 *  API 3  update_relay_status.php — 4 relay states
 *  API 5  read_relay_status.php   — poll relay cmds  {"RlyNo":n,"Status":n}
 *  API 6  read_lock_status.php    — poll lock/unlock {"LockStatus":n}
 *  API 7  read_soft_reset_status.php — poll soft reset {"SoftResetStatus":n}
 *
 *  deviceid = MAC address (AABBCCDDEEFF) — confirmed working live;
 *  see USE_DEVICE_ID in config.h.
 */

#include "remote_server.h"
#include "config.h"
#include "uart_parser.h"
#include "relay_logic.h"
#include "status_led.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_system.h"
#include "cJSON.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "remote";



#define HTTP_RESP_BUF_SZ    1024u   /* was 512 - too small for the 25-LED
                                        read-back response (~750+ bytes with
                                        all LEDNo/Status entries), causing
                                        every read_led_status.php call to
                                        arrive truncated and fail JSON
                                        parsing even though the server's
                                        response was perfectly valid. */

/* All LED_UPDATE_MS / RELAY_UPDATE_MS / LED_ALL_MS / LOCK_POLL_MS /
 * RESET_POLL_MS / LED_READ_POLL_MS / POWER_DETAILS_MS timing macros
 * now live in config.h - centralized there so they can be tuned in
 * one place without hunting through this file. */

/* Live server confirmed: "Status" capital S, string value. */
#define JSON_RELAY_STATUS_KEY   "Status"

/* Live server is inconsistent about JSON types across fields/endpoints
 * (e.g. relay Status as a string, success as a number) - accept either
 * form everywhere instead of assuming one. */
static int cjson_as_int(const cJSON *item, int fallback)
{
    if (!item) return fallback;
    if (cJSON_IsNumber(item)) return item->valueint;
    if (cJSON_IsString(item) && item->valuestring) return atoi(item->valuestring);
    return fallback;
}

/* WORKAROUND for a confirmed server-side typo, NOT a permanent design
 * choice - delete this once fixed server-side. update_led_status.php's
 * response was observed with the field literally misspelled:
 *   {"UpdateLEDStatus":[],...,"Muccess":1,...,"Message":"Valid Parameters - Data Updated"}
 * i.e. "success" typo'd as "Muccess" (one character off). Tries the
 * correct field name first; only falls back to the typo'd name if that
 * fails, and logs loudly when the fallback actually fires so it's
 * obvious once the server is fixed and this stops being needed. */
static int cjson_get_success_with_typo_fallback(cJSON *root)
{
    int ok = cjson_as_int(cJSON_GetObjectItem(root, "success"), -1);
    if (ok == -1) {
        int typo_ok = cjson_as_int(cJSON_GetObjectItem(root, "Muccess"), -1);
        if (typo_ok != -1) {
            ESP_LOGW(TAG, "Using 'Muccess' typo fallback - server should "
                          "rename this field to 'success'");
            return typo_ok;
        }
    }
    return ok;
}

/* Logs the server's "message" field, if present, whenever a call didn't
 * report success - without this, a validation failure (bad params,
 * unregistered device, etc.) looks identical to a network failure in
 * the log. */
static void log_server_message(cJSON *root)
{
    cJSON *msg = cJSON_GetObjectItem(root, "message");
    if (cJSON_IsString(msg) && msg->valuestring) {
        ESP_LOGW(TAG, "Server message: %s", msg->valuestring);
    }
}

static volatile bool s_locked = false;

/* ── deviceid: fixed test ID (confirmed) or MAC (document spec) ─ */
static void get_device_id(char *out, size_t sz)
{
#if USE_DEVICE_ID
    snprintf(out, sz, "%s", DEVICE_ID);
#else
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    snprintf(out, sz, "%02X%02X%02X%02X%02X%02X",
             mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
#endif
}

/* ── HTTPS GET — validates server cert via ESP-IDF cert bundle ── */
static bool http_get(const char *url, char *resp_buf, size_t resp_sz)
{
    esp_http_client_config_t cfg = {
        .url               = url,
        .method            = HTTP_METHOD_GET,
        .timeout_ms        = 8000,
        .crt_bundle_attach = esp_crt_bundle_attach,
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
        size_t max_read = resp_sz - 1u;
        if (content_len > 0 && (size_t)content_len < max_read) {
            max_read = (size_t)content_len;
        }
        size_t total = 0;
        while (total < max_read) {
            int n = esp_http_client_read(c, resp_buf + total, max_read - total);
            if (n <= 0) break;   /* EOF or error - stop, keep what we have */
            total += (size_t)n;
        }
        resp_buf[total] = '\0';
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
                    "https://%s%s/%s?deviceid=%s",
                    REMOTE_HOST, REMOTE_BASE, ep, mac);
}

/* Appends a millisecond-since-boot timestamp as a throwaway query
 * param, e.g. "&_ts=123456". Every read_*.php call currently uses
 * the exact same URL every time (just ?deviceid=MAC, nothing else
 * varies) - if any caching layer sits in front of the server (WAF/
 * CDN/reverse proxy), an identical URL is exactly what it would key
 * a cached response on, which would make the server *look* like it
 * never updates even if the underlying data actually changed. This
 * makes every read request's URL unique so nothing can cache it.
 * The server is expected to ignore an unrecognized parameter. */
static void append_cache_buster(char *url, size_t sz)
{
    size_t len = strlen(url);
    uint32_t ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    snprintf(url + len, sz - len, "&_ts=%lu", (unsigned long)ms);
}

/* ═══════════════════════════════════════════════════════════════
   API 1 — Register  (Switch ON / Switch OFF)
   register.php?deviceid=<MAC>&device_slno=<SN>&model=<M>
              &status=<1|0>&location=<L>
   ═══════════════════════════════════════════════════════════════ */
static bool api_register(const char *mac, int status)
{
    char url[320];
    snprintf(url, sizeof(url),
             "https://%s%s/register.php"
             "?deviceid=%s&device_slno=%s&model=%s&status=%d&location=%s",
             REMOTE_HOST, REMOTE_BASE,
             mac, DEVICE_SERIAL, DEVICE_MODEL,
             status, DEVICE_LOCATION);
    append_cache_buster(url, sizeof(url));
    ESP_LOGI(TAG, "REGISTER status=%d", status);

    char resp[HTTP_RESP_BUF_SZ] = {0};
    bool http_ok = http_get(url, resp, sizeof(resp));
    if (!http_ok) {
        ESP_LOGW(TAG, "REGISTER: HTTP request failed");
        return false;
    }

    cJSON *root = cJSON_Parse(resp);
    if (!root) {
        ESP_LOGW(TAG, "register JSON parse fail, raw body: %s", resp);
        return false;
    }
    int ok = cjson_as_int(cJSON_GetObjectItem(root, "success"), -1);
    if (ok != 1) {
        ESP_LOGW(TAG, "REGISTER NOT accepted (success=%d), raw body: %s", ok, resp);
        log_server_message(root);
    } else {
        ESP_LOGI(TAG, "REGISTER: OK");
    }
    cJSON_Delete(root);
    return (ok == 1);
}

/* Switch OFF Device (proper way) — per document */
bool remote_server_notify_power_off(const char *mac)
{
    return api_register(mac, REGISTER_STATUS_OFF);
}

/* ═══════════════════════════════════════════════════════════════
   API 2 — Update Status of LEDs
   update_led_status.php?deviceid=<MAC>[&LED_CNT=ALL]&L1=x&...&L25=x
   0=Off 1=On 2=Blinking.  LED_CNT=ALL every 15s (health check).
   ═══════════════════════════════════════════════════════════════ */
static bool api_led_status(const char *mac, bool send_all)
{
    char url[640];
    int  pos = url_base(url, sizeof(url), "update_led_status.php", mac);

    /* TEMPORARY: LED_CNT=ALL only makes sense on the full 25-value
     * write - a partial write (TEST_LED_SEND_COUNT < GPIO_COUNT)
     * never includes it, matching the doc's plain partial-update
     * form ("&L1=x&L2=x..." with no LED_CNT at all). */
    if (send_all && TEST_LED_SEND_COUNT >= GPIO_COUNT)
        pos += snprintf(url+pos, sizeof(url)-(size_t)pos, "&LED_CNT=ALL");

    for (int i = 0; i < TEST_LED_SEND_COUNT; i++) {
#if TEST_FORCE_ALL_LED_ON
        int val = 1;   /* TEMPORARY - see TEST_FORCE_ALL_LED_ON above */
#else
        int val = uart_parser_get_gpio((uint8_t)i) ? 1 : 0;
#endif
        pos += snprintf(url+pos, sizeof(url)-(size_t)pos,
                        "&L%d=%d", i+1, val);
    }
    append_cache_buster(url, sizeof(url));

    char resp[HTTP_RESP_BUF_SZ] = {0};
    if (!http_get(url, resp, sizeof(resp))) {
        ESP_LOGW(TAG, "LED write: HTTP request failed");
        return false;
    }

    cJSON *root = cJSON_Parse(resp);
    if (!root) {
        ESP_LOGW(TAG, "LED write JSON parse fail, raw body: %s", resp);
        return false;
    }
    int ok = cjson_get_success_with_typo_fallback(root);
    if (ok != 1) {
        ESP_LOGW(TAG, "LED write NOT accepted (success=%d), raw body: %s", ok, resp);
        log_server_message(root);
    } else {
        /* Throttled "still alive" summary - this call happens every
         * 500ms, so logging every single success would flood the
         * console; the LED_CNT=ALL heartbeat already gets its own
         * explicit log at the call site every 15s regardless. */
        static uint32_t s_led_write_count = 0;
        if ((++s_led_write_count % 10u) == 0u) {
            ESP_LOGI(TAG, "LED write poll OK (%d/%d slots)",
                     TEST_LED_SEND_COUNT, GPIO_COUNT);
        }
    }
    cJSON_Delete(root);
    return (ok == 1);
}

/* ═══════════════════════════════════════════════════════════════
   API 3 — Update Status of Relays
   update_relay_status.php?deviceid=<MAC>&R1=x&R2=x&R3=x&R4=x    0=Off 1=On
   ═══════════════════════════════════════════════════════════════ */
static bool api_relay_status(const char *mac)
{
    uint8_t m = relay_logic_get_mask();
#if TEST_FORCE_ALL_RELAYS_ON
    /* TEMPORARY - see TEST_FORCE_ALL_RELAYS_ON in config.h. Takes
     * priority over TEST_FORCE_RELAY_ACTIVE below. */
    m = 0x0Fu;   /* all 4 bits set: R1=R2=R3=R4=1 */
#elif TEST_FORCE_RELAY_ACTIVE
    /* TEMPORARY - see TEST_FORCE_RELAY_ACTIVE in config.h */
    m |= (uint8_t)(1u << (TEST_ACTIVE_RELAY_NUM - 1));
#endif
    char url[320];
    int pos = url_base(url, sizeof(url), "update_relay_status.php", mac);
    snprintf(url+pos, sizeof(url)-(size_t)pos,
             "&R1=%d&R2=%d&R3=%d&R4=%d",
             (m>>0)&1, (m>>1)&1, (m>>2)&1, (m>>3)&1);
    append_cache_buster(url, sizeof(url));

    char resp[HTTP_RESP_BUF_SZ] = {0};
    if (!http_get(url, resp, sizeof(resp))) {
        ESP_LOGW(TAG, "relay status write: HTTP request failed");
        return false;
    }

    cJSON *root = cJSON_Parse(resp);
    if (!root) {
        ESP_LOGW(TAG, "relay status write JSON parse fail, raw body: %s", resp);
        return false;
    }
    int ok = cjson_as_int(cJSON_GetObjectItem(root, "success"), -1);
    if (ok != 1) {
        ESP_LOGW(TAG, "relay status write NOT accepted (success=%d), raw body: %s", ok, resp);
        log_server_message(root);
    } else {
        /* Throttled - this call happens every 500ms too. */
        static uint32_t s_relay_write_count = 0;
        if ((++s_relay_write_count % 10u) == 0u) {
            ESP_LOGI(TAG, "relay status write poll OK: R1=%d R2=%d R3=%d R4=%d",
                     (m>>0)&1, (m>>1)&1, (m>>2)&1, (m>>3)&1);
        }
    }
    cJSON_Delete(root);
    return (ok == 1);
}

/* ═══════════════════════════════════════════════════════════════
   API 5 — Check for any changes in Relay status (Changed by User)
   read_relay_status.php?deviceid=<MAC>

   Response: {"RelayStatus":[{"RlyNo":1,"Status":"1"},...],"success":1}
   Confirmed live: "Status" capital S, STRING value (see config.h).
   ═══════════════════════════════════════════════════════════════ */
static void api_read_relay(const char *mac)
{
    char url[256];
    url_base(url, sizeof(url), "read_relay_status.php", mac);
    append_cache_buster(url, sizeof(url));

    char resp[HTTP_RESP_BUF_SZ] = {0};
    if (!http_get(url, resp, sizeof(resp))) {
        ESP_LOGW(TAG, "read_relay_status: HTTP request failed");
        return;
    }

    cJSON *root = cJSON_Parse(resp);
    if (!root) { ESP_LOGW(TAG,"relay JSON parse fail, raw body: %s", resp); return; }

    int   ok  = cjson_as_int(cJSON_GetObjectItem(root, "success"), -1);
    cJSON *arr = cJSON_GetObjectItem(root, "RelayStatus");

    if (ok != 1) {
        ESP_LOGW(TAG, "read_relay_status NOT accepted (success=%d), raw body: %s", ok, resp);
        log_server_message(root);
    } else if (cJSON_IsArray(arr)) {
        /* Raw values as the server actually sent them, before any
         * edge-detection filtering - -1 means "server didn't report
         * this relay number in this response". Logged every call
         * (not throttled) so exactly what the server said is always
         * visible, separate from the post-filter tracked state. */
        int raw[4] = { -1, -1, -1, -1 };

        cJSON *item;
        cJSON_ArrayForEach(item, arr) {
            cJSON *rno = cJSON_GetObjectItem(item, "RlyNo");
            cJSON *st  = cJSON_GetObjectItem(item, JSON_RELAY_STATUS_KEY);
            if (!rno || !st) continue;

            int n = cjson_as_int(rno, -1);   /* relay number, doc shows up to 10 */
            if (n < 1 || n > 4) continue;    /* we only control RL1-RL4 */

            /* Live server sends "1"/"0" as STRING — handle both
             * string and integer forms safely.                    */
            int srv = cjson_as_int(st, -1);
            raw[n - 1] = srv;

            bool cur = relay_logic_get_state((uint8_t)n);
            if (srv == 1 && !cur) {
                ESP_LOGI(TAG, "Server: RL%d ON", n);
                relay_logic_command((uint8_t)n);
                uart_send_relay_cmd((uint8_t)n);
                /* Confirm the new status back to the server right
                 * away via update_relay_status.php, rather than
                 * waiting for the next periodic 500ms write cycle -
                 * this is the same endpoint api_relay_status() already
                 * writes to on a timer, just triggered immediately on
                 * an actual state change too.
                 *
                 * Safe to call immediately (not stale) only because
                 * RELAY_TASK_PRIO > REMOTE_TASK_PRIO in config.h:
                 * relay_logic_command() above queues the change, and
                 * because the relay-tracking task has higher priority,
                 * FreeRTOS preempts this task and runs track_set()
                 * to completion before returning control here. If
                 * that priority ordering is ever changed, this call
                 * could read stale (pre-update) tracked state. */
                api_relay_status(mac);
            }
        }

        ESP_LOGI(TAG, "read_relay_status raw: R1=%d R2=%d R3=%d R4=%d",
                 raw[0], raw[1], raw[2], raw[3]);

        /* Throttled "still alive" summary of the TRACKED (post-
         * edge-detection) state - without this, a poll that returns
         * success with everything OFF (the common case) produces
         * zero log output for the tracked side, making it look like
         * relay polling isn't happening at all. Logged every ~10
         * calls (~5s at the normal 500ms poll rate) rather than
         * every single call, to avoid flooding the console. */
        static uint32_t s_relay_poll_count = 0;
        if ((++s_relay_poll_count % 10u) == 0u) {
            ESP_LOGI(TAG, "Relay poll OK (tracked): R1=%d R2=%d R3=%d R4=%d",
                     relay_logic_get_state(1), relay_logic_get_state(2),
                     relay_logic_get_state(3), relay_logic_get_state(4));
        }
    }
    cJSON_Delete(root);
}

/* ═══════════════════════════════════════════════════════════════
   API 6 — Lock/Unlock Device (Changed by User)
   read_lock_status.php?deviceid=<MAC>
   Response: {"LockStatus":1,"success":1}
   ═══════════════════════════════════════════════════════════════ */
static void api_read_lock(const char *mac)
{
    char url[256];
    url_base(url, sizeof(url), "read_lock_status.php", mac);
    append_cache_buster(url, sizeof(url));

    char resp[384] = {0};   /* was 128 - too small for the server's actual
                                nested-array response shape, caused silent
                                truncation (missing closing brace) */
    if (!http_get(url, resp, sizeof(resp))) {
        ESP_LOGW(TAG, "read_lock_status: HTTP request failed");
        return;
    }

    cJSON *root = cJSON_Parse(resp);
    if (!root) {
        ESP_LOGW(TAG, "read_lock_status JSON parse fail, raw body: %s", resp);
        return;
    }

    int ok   = cjson_as_int(cJSON_GetObjectItem(root, "success"), -1);
    int lock = -1;

    if (ok == 1) {
        /* Live server nests LockStatus differently from the doc's flat
         * example. Actual observed structure:
         *   {"LockStatus":[{"LockStatus":"0","SoftResetStatus":"1"}],...}
         * i.e. the top-level "LockStatus" key holds an ARRAY containing
         * one object with the real LockStatus (and, redundantly,
         * SoftResetStatus) nested inside - not a plain integer as the
         * doc shows. Handle both shapes: try the nested array first,
         * fall back to a flat field in case the server ever matches
         * the doc. */
        cJSON *ls_field = cJSON_GetObjectItem(root, "LockStatus");
        if (cJSON_IsArray(ls_field)) {
            cJSON *first = cJSON_GetArrayItem(ls_field, 0);
            if (first) {
                lock = cjson_as_int(cJSON_GetObjectItem(first, "LockStatus"), -1);
            }
        } else {
            lock = cjson_as_int(ls_field, -1);
        }
    }

    if (ok != 1) {
        ESP_LOGW(TAG, "read_lock_status NOT accepted (success=%d), raw body: %s", ok, resp);
        log_server_message(root);
    } else {
        if (lock == 1 || lock == 2) {
            /* Doc: 1=App wants to Lock, 2=App wants to Unlock, 0=No Change.
             * Only 1/2 are real transitions - 0 must be left alone, not
             * treated as "unlock", or the device would unlock itself on
             * every poll cycle once locked (previous bug: anything other
             * than a literal 1 was mapped to unlock, including 0). */
            bool new_lock = (lock == 1);
            if (new_lock != s_locked) {
                s_locked = new_lock;
                ESP_LOGI(TAG, "Device %s", s_locked ? "LOCKED" : "UNLOCKED");
                /* Relay lock: force RL2 persistently OFF while locked,
                 * restore to normal ON when unlocked - distinct from the
                 * normal timed RL2 toggle, see uart_parser.h. */
                uart_send_relay_lock_cmd(s_locked);
            }
        }
        ESP_LOGI(TAG, "read_lock_status poll OK (LockStatus=%d, currently %s)",
                 lock, s_locked ? "LOCKED" : "unlocked");
    }
    cJSON_Delete(root);
}

/* ═══════════════════════════════════════════════════════════════
   API 7 — Soft Reset Device (Changed by User)
   read_soft_reset_status.php?deviceid=<MAC>
   Response: {"SoftResetStatus":1,"success":1}
   ═══════════════════════════════════════════════════════════════ */
/* ═══════════════════════════════════════════════════════════════
   API 5 — Update Soft Reset Status (was missing - only the READ
   side, API 9, was implemented; the server never got told the
   reset actually happened).
   update_soft_reset_status.php?deviceid=<MAC>&SR=<0|1>&SF=<0|1>
   Doc has an internal inconsistency: the URL example uses SR, but
   its own parameter table calls the field SF. Send both so this
   works regardless of which the live PHP actually reads (same fix
   applied on the NUC-side test project earlier).
   SR/SF: 0 = Soft Reset (in progress), 1 = Soft Reset Done
   ═══════════════════════════════════════════════════════════════ */
#if ENABLE_SOFT_RESET_FEATURE
static bool api_update_soft_reset_status(const char *mac, int sf)
{
    char url[192];
    snprintf(url, sizeof(url),
             "https://%s%s/update_soft_reset_status.php?deviceid=%s&SR=%d&SF=%d",
             REMOTE_HOST, REMOTE_BASE, mac, sf, sf);
    append_cache_buster(url, sizeof(url));

    char resp[256] = {0};
    if (!http_get(url, resp, sizeof(resp))) {
        ESP_LOGW(TAG, "update_soft_reset_status: HTTP request failed");
        return false;
    }

    cJSON *root = cJSON_Parse(resp);
    if (!root) {
        ESP_LOGW(TAG, "update_soft_reset_status JSON parse fail, raw body: %s", resp);
        return false;
    }
    int ok = cjson_as_int(cJSON_GetObjectItem(root, "success"), -1);
    if (ok != 1) {
        ESP_LOGW(TAG, "update_soft_reset_status NOT accepted (success=%d), raw body: %s", ok, resp);
        log_server_message(root);
    } else {
        ESP_LOGI(TAG, "update_soft_reset_status: OK (SF=%d)", sf);
    }
    cJSON_Delete(root);
    return (ok == 1);
}

static void api_read_soft_reset(const char *mac)
{
    char url[256];
    url_base(url, sizeof(url), "read_soft_reset_status.php", mac);
    append_cache_buster(url, sizeof(url));

    char resp[384] = {0};   /* was 128 - same truncation bug as
                                read_lock_status.php, see below */
    if (!http_get(url, resp, sizeof(resp))) {
        ESP_LOGW(TAG, "read_soft_reset_status: HTTP request failed");
        return;
    }

    cJSON *root = cJSON_Parse(resp);
    if (!root) {
        ESP_LOGW(TAG, "read_soft_reset_status JSON parse fail, raw body: %s", resp);
        return;
    }

    int ok  = cjson_as_int(cJSON_GetObjectItem(root, "success"), -1);
    int rst = -1;

    if (ok == 1) {
        /* Same nested-array shape as read_lock_status.php:
         *   {"SoftResetStatus":[{"LockStatus":"0","SoftResetStatus":"1"}],...}
         * i.e. the top-level "SoftResetStatus" key holds an ARRAY
         * containing one object with the real SoftResetStatus (and,
         * redundantly, LockStatus) nested inside - not a plain
         * integer as the doc shows. Handle both shapes. */
        cJSON *rst_field = cJSON_GetObjectItem(root, "SoftResetStatus");
        if (cJSON_IsArray(rst_field)) {
            cJSON *first = cJSON_GetArrayItem(rst_field, 0);
            if (first) {
                rst = cjson_as_int(cJSON_GetObjectItem(first, "SoftResetStatus"), -1);
            }
        } else {
            rst = cjson_as_int(rst_field, -1);
        }
    }

    if (ok != 1) {
        ESP_LOGW(TAG, "read_soft_reset_status NOT accepted (success=%d), raw body: %s", ok, resp);
        log_server_message(root);
        cJSON_Delete(root);
        return;
    }
    ESP_LOGI(TAG, "read_soft_reset_status poll OK (SoftResetStatus=%d)", rst);
    cJSON_Delete(root);

    if (rst == 1) {
        ESP_LOGW(TAG, "Soft reset requested by server - waiting %lus before "
                      "acknowledging (SOFT_RESET_PRE_ACK_DELAY_MS)...",
                 (unsigned long)(SOFT_RESET_PRE_ACK_DELAY_MS / 1000u));
        vTaskDelay(pdMS_TO_TICKS(SOFT_RESET_PRE_ACK_DELAY_MS));

        api_update_soft_reset_status(mac, 0);     /* SF=0: acknowledge/clear -
                                                       so the NEXT poll (after
                                                       we're back up) doesn't
                                                       see SoftResetStatus=1
                                                       again and reset-loop */

        ESP_LOGW(TAG, "Acknowledged - resetting NUC first, then ESP32...");
        uart_send_nuc_reset_cmd();                /* NUC resets first */
        vTaskDelay(pdMS_TO_TICKS(200));           /* give it time to receive + reset */
        vTaskDelay(pdMS_TO_TICKS(SOFT_RESET_RESTART_DELAY_MS));
        esp_restart();   /* never returns */
    }
}
#endif /* ENABLE_SOFT_RESET_FEATURE */

/* ═══════════════════════════════════════════════════════════════
   Read LED status back from the server — this is the data we
   already WRITE via api_led_status()/update_led_status.php; this
   reads it back (server's stored copy) and prints it to the
   console, mainly to confirm what the server actually has on
   record vs. what we last sent.
   read_led_status.php?deviceid=<MAC>
   Response: {"LEDStatus":[{"LEDNo":1,"Status":"1"},...],"success":1}
   ═══════════════════════════════════════════════════════════════ */
static void api_read_led_status(const char *mac)
{
    char url[256];
    url_base(url, sizeof(url), "read_led_status.php", mac);
    append_cache_buster(url, sizeof(url));

    char resp[HTTP_RESP_BUF_SZ] = {0};
    if (!http_get(url, resp, sizeof(resp))) {
        ESP_LOGW(TAG, "read_led_status: HTTP request failed");
        return;
    }

    cJSON *root = cJSON_Parse(resp);
    if (!root) { ESP_LOGW(TAG, "LED read JSON parse fail, raw body: %s", resp); return; }

    int ok = cjson_as_int(cJSON_GetObjectItem(root, "success"), -1);
    cJSON *arr = cJSON_GetObjectItem(root, "LEDStatus");

    if (ok != 1) {
        ESP_LOGW(TAG, "read_led_status NOT accepted (success=%d), raw body: %s", ok, resp);
        log_server_message(root);
        cJSON_Delete(root);
        return;
    }
    if (!cJSON_IsArray(arr)) {
        ESP_LOGW(TAG, "read_led_status: LEDStatus field missing or not an array");
        cJSON_Delete(root);
        return;
    }

    /* Build one compact line: "L1=0 L2=1 L3=0 ..." rather than 25
     * separate log lines, so it's actually readable in the monitor. */
    char line[GPIO_COUNT * 7 + 1];
    line[0] = '\0';
    int pos = 0;
    cJSON *item;
    cJSON_ArrayForEach(item, arr) {
        int no = cjson_as_int(cJSON_GetObjectItem(item, "LEDNo"), -1);
        int st = cjson_as_int(cJSON_GetObjectItem(item, "Status"), -1);
        if (no < 1 || no > GPIO_COUNT) continue;
        pos += snprintf(line + pos, sizeof(line) - (size_t)pos,
                        "L%d=%d ", no, st);
        if (pos >= (int)sizeof(line) - 8) break;
    }
    ESP_LOGI(TAG, "Server LED status (read back): %s", line);

    cJSON_Delete(root);
}

/* ═══════════════════════════════════════════════════════════════
   API 10 — Poll All Inputs Status  (NEW - protocol rev 20260729)
   read_input_status.php?deviceid=<MAC>

   Consolidates relay/lock/soft-reset status into one response
   instead of three separate calls. Unusual schema, implemented
   exactly as documented:
     {"InputStatus":[
        {"RlyNo":1,"Status":"0"}, ... {"RlyNo":4,"Status":"0"},
        {"Lock":0,"Status":"0"},
        {"SoftReset":0,"Status":"0"}
     ], "DeviceID":"...","Success":1,"DateTime":"..."}
   The "Lock":0 and "SoftReset":0 values themselves are explicitly
   "no significance" per the doc - the real value is that SAME
   object's "Status" field, not the Lock/SoftReset field. Easy to
   misread since "Status" means something different in each of the
   three entry types (relay level, lock command, reset command).

   PURELY DIAGNOSTIC for now - logs what it reads for cross-checking
   against the existing separate read_relay_status.php/read_lock_status.php/
   read_soft_reset_status.php polls, but does not act on the values
   (doesn't trigger relay commands, doesn't lock, doesn't restart).
   Tell me if you want this to fully replace the three separate polls
   instead of just running alongside them for verification.
   ═══════════════════════════════════════════════════════════════ */
static void api_read_input_status(const char *mac)
{
    char url[256];
    url_base(url, sizeof(url), "read_input_status.php", mac);
    append_cache_buster(url, sizeof(url));

    char resp[512] = {0};
    if (!http_get(url, resp, sizeof(resp))) {
        ESP_LOGW(TAG, "read_input_status: HTTP request failed");
        return;
    }

    cJSON *root = cJSON_Parse(resp);
    if (!root) {
        ESP_LOGW(TAG, "read_input_status JSON parse fail, raw body: %s", resp);
        return;
    }

    int ok  = cjson_as_int(cJSON_GetObjectItem(root, "success"), -1);
    cJSON *arr = cJSON_GetObjectItem(root, "InputStatus");

    if (ok != 1) {
        ESP_LOGW(TAG, "read_input_status NOT accepted (success=%d), raw body: %s", ok, resp);
        log_server_message(root);
        cJSON_Delete(root);
        return;
    }
    if (!cJSON_IsArray(arr)) {
        ESP_LOGW(TAG, "read_input_status: InputStatus field missing or not an array");
        cJSON_Delete(root);
        return;
    }

    int relay_raw[4]  = { -1, -1, -1, -1 };
    int lock_val      = -1;
    int softreset_val = -1;

    cJSON *item;
    cJSON_ArrayForEach(item, arr) {
        cJSON *rno    = cJSON_GetObjectItem(item, "RlyNo");
        cJSON *lock   = cJSON_GetObjectItem(item, "Lock");
        cJSON *sreset = cJSON_GetObjectItem(item, "SoftReset");
        cJSON *status = cJSON_GetObjectItem(item, "Status");

        if (rno) {
            int n = cjson_as_int(rno, -1);
            if (n >= 1 && n <= 4) {
                relay_raw[n - 1] = cjson_as_int(status, -1);
            }
        } else if (lock) {
            /* "Lock" field value itself is "no significance" per the
             * doc - the real lock command lives in "Status" here. */
            lock_val = cjson_as_int(status, -1);
        } else if (sreset) {
            /* Same pattern for soft reset. */
            softreset_val = cjson_as_int(status, -1);
        }
    }

    ESP_LOGI(TAG, "read_input_status raw: R1=%d R2=%d R3=%d R4=%d Lock=%d SoftReset=%d",
             relay_raw[0], relay_raw[1], relay_raw[2], relay_raw[3],
             lock_val, softreset_val);

    cJSON_Delete(root);
}

/* ═══════════════════════════════════════════════════════════════
   API 6A — Power Details Notification  (new in protocol rev 20260727)
   update_power_details.php?deviceid=<MAC>&pwr_status=<0|1>
                            &batt_volt=<float>&batt_status=<0-3>

   pwr_status: 0=Power Failed, 1=Power On
   batt_volt:  float, VOLTS (not mV) — doc explicitly allows exponential
               or negative, but we only ever send a plain decimal since
               that's all a real battery reading can be.
   batt_status: 0=Unknown, 1=Healthy, 2=Low Battery, 3=Battery Failed

   batt_status mapping (derived from existing UART-sourced data, no new
   NUC packet needed):
     bat_mv == 0 (ADC timeout, see battery_adc.c)   -> 0 Unknown
     bat_mv <= BATTERY_CUTOFF_MV (hard cutoff)      -> 3 Battery Failed
     low_battery flag set (power_monitor.c, with
       hysteresis - see that file)                  -> 2 Low Battery
     otherwise                                       -> 1 Healthy
   ═══════════════════════════════════════════════════════════════ */
#if !TEST_HARDCODE_POWER_DATA
static int battery_status_code(uint16_t bat_mv, bool low_battery)
{
    if (bat_mv == 0u) return 0;                  /* Unknown - no reading */
    if (bat_mv <= BATTERY_CUTOFF_MV) return 3;   /* Battery Failed        */
    if (low_battery) return 2;                   /* Low Battery           */
    return 1;                                    /* Healthy               */
}
#endif

static bool api_power_details(const char *mac)
{
#if TEST_HARDCODE_POWER_DATA
    /* TEMPORARY - see TEST_HARDCODE_POWER_DATA in config.h */
    int   pwr_status  = TEST_HARDCODE_PWR_STATUS;
    float batt_volt   = TEST_HARDCODE_BATT_VOLT;
    int   batt_status = TEST_HARDCODE_BATT_STATUS;
#else
    uint16_t bat_mv      = uart_parser_get_bat_mv();
    bool     power_fail  = uart_parser_is_power_fail();
    bool     low_battery = uart_parser_is_low_battery();

    int   pwr_status  = power_fail ? 0 : 1;
    float batt_volt   = bat_mv / 1000.0f;   /* mV -> V, doc wants volts */
    int   batt_status = battery_status_code(bat_mv, low_battery);
#endif

    char url[256];
    snprintf(url, sizeof(url),
             "https://%s%s/update_power_details.php"
             "?deviceid=%s&pwr_status=%d&batt_volt=%.2f&batt_status=%d",
             REMOTE_HOST, REMOTE_BASE, mac, pwr_status, batt_volt, batt_status);
    append_cache_buster(url, sizeof(url));

    char resp[HTTP_RESP_BUF_SZ] = {0};
    if (!http_get(url, resp, sizeof(resp))) {
        ESP_LOGW(TAG, "power details: HTTP request failed");
        return false;
    }

    cJSON *root = cJSON_Parse(resp);
    if (!root) {
        ESP_LOGW(TAG, "power details JSON parse fail, raw body: %s", resp);
        return false;
    }
    int ok = cjson_as_int(cJSON_GetObjectItem(root, "success"), -1);
    if (ok != 1) {
        ESP_LOGW(TAG, "power details NOT accepted (success=%d), raw body: %s", ok, resp);
        log_server_message(root);
    } else {
        ESP_LOGI(TAG, "power details update: OK (pwr_status=%d batt_volt=%.2f batt_status=%d)",
                 pwr_status, batt_volt, batt_status);
    }
    cJSON_Delete(root);
    return (ok == 1);
}

static void remote_task(void *arg)
{
    char mac[16];
    get_device_id(mac, sizeof(mac));
    ESP_LOGI(TAG, "DeviceID (MAC): %s  server: https://%s%s",
             mac, REMOTE_HOST, REMOTE_BASE);

    bool     registered = false;
    uint32_t last_led   = 0u;
    uint32_t last_relay = 0u;
    uint32_t last_poll  = 0u;
    uint32_t last_all   = 0u;
    uint32_t last_lock  = 0u;
    uint32_t last_rst   = 0u;
    uint32_t last_led_read = 0u;
    uint32_t last_input_status = 0u;
    uint32_t last_power = 0u;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(200));
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000ULL);

        /* Immediate relay report on ANY tracked state change - this
         * covers not just the initial ON command (already reported
         * immediately from api_read_relay()) but also the automatic
         * 3-second revert (RL1 auto-OFF, RL2 auto-back-ON, RL4 auto-
         * trigger-then-OFF), which previously only got reported on
         * the next periodic 500ms poll. Checked every 200ms tick
         * here rather than waiting on it, so it never blocks/delays
         * anything else in this loop. */
        if (registered &&
            xSemaphoreTake(relay_logic_get_change_semaphore(), 0) == pdTRUE) {
            api_relay_status(mac);
        }

        /* API 1 — Register on boot (Switch ON) */
        if (!registered) {
            if (api_register(mac, REGISTER_STATUS_ON)) {
                registered = true;
                status_led_set_mode(LED_MODE_CONNECTED);
                buzzer_set_no_signal(false);
                ESP_LOGI(TAG, "Registered OK");
#if ENABLE_SOFT_RESET_FEATURE
                /* Ack any pending soft-reset as done - if this boot
                 * wasn't a soft-reset reboot at all, this is a no-op
                 * as far as real device state goes, just tells the
                 * server "nothing in progress" which is trivially true. */
                api_update_soft_reset_status(mac, 1);
#endif
            } else {
                status_led_set_mode(LED_MODE_SEARCHING);
                buzzer_set_no_signal(true);
                vTaskDelay(pdMS_TO_TICKS(5000));
                continue;
            }
        }

        /* API 2 — LED status every 500ms, LED_CNT=ALL every 15s
         * (TEMPORARY: TEST_LED_SEND_COUNT may reduce this to a
         * partial write - see the macro comment near the top) */
        if ((now - last_led) >= LED_UPDATE_MS) {
            bool all = ((now - last_all) >= LED_ALL_MS);
            if (!s_locked) {
                bool ok = api_led_status(mac, all);
                if (all) {
                    last_all = now;
                    ESP_LOGI(TAG, "LED update (%d/%d slots%s): %s",
                             TEST_LED_SEND_COUNT, GPIO_COUNT,
                             TEST_LED_SEND_COUNT >= GPIO_COUNT ? "+LED_CNT=ALL" : "",
                             ok ? "OK" : "FAILED");
                }
            } else if (all) {
                last_all = now;
                ESP_LOGW(TAG, "LED update skipped — device locked");
            }
            last_led = now;
        }

        /* API 3 — Relay status every 500ms */
        if ((now - last_relay) >= RELAY_UPDATE_MS) {
            api_relay_status(mac);
            last_relay = now;
        }

        /* API 5 — Poll relay commands */
        if ((now - last_poll) >= (uint32_t)REMOTE_POLL_MS) {
            if (!s_locked) api_read_relay(mac);
            last_poll = now;
        }

        /* API 6 — Poll lock status every 5s */
        if ((now - last_lock) >= LOCK_POLL_MS) {
            api_read_lock(mac);
            last_lock = now;
        }

        /* Read LED status back from server every 5s — prints what
         * the server has on record to the console. */
        if ((now - last_led_read) >= LED_READ_POLL_MS) {
            api_read_led_status(mac);
            last_led_read = now;
        }

        /* API 10 — consolidated relay/lock/soft-reset read every 5s
         * (new endpoint, protocol rev 20260729) - diagnostic/cross-
         * verification only, see the function's own comment. */
        if ((now - last_input_status) >= INPUT_STATUS_POLL_MS) {
            api_read_input_status(mac);
            last_input_status = now;
        }

        /* API 6A — Power/battery details every 5s (new endpoint,
         * protocol rev 20260727) */
        if ((now - last_power) >= POWER_DETAILS_MS) {
            api_power_details(mac);
            last_power = now;
        }

        /* API 7 — Poll soft reset every 5s (feature disabled for now
         * - see ENABLE_SOFT_RESET_FEATURE in config.h) */
#if ENABLE_SOFT_RESET_FEATURE
        if ((now - last_rst) >= RESET_POLL_MS) {
            api_read_soft_reset(mac);
            last_rst = now;
        }
#else
        (void)last_rst;
#endif
    }
}

void remote_server_start(void)
{
    xTaskCreate(remote_task, "remote", REMOTE_TASK_STACK,
                NULL, REMOTE_TASK_PRIO, NULL);
}

void remote_server_notify_relay_change(void) {}

bool remote_server_is_locked(void) { return s_locked; }