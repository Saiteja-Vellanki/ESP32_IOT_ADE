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
#include "freertos/semphr.h"
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
#include <time.h>
#include <sys/time.h>

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
static volatile bool s_registered = false;   /* shared with relay_report_task */

/* Only one HTTPS/TLS connection may be active at a time. Creating several
 * mbedTLS contexts concurrently exhausts the ESP32 internal heap and causes
 * -0x7F00/-0x4290 allocation failures. */
static SemaphoreHandle_t s_http_mutex = NULL;

/* R3's WRITE-BACK value only - a direct echo of whatever Status the
 * server most recently sent for RL3 (0 or 1), independent of the
 * toggle-based tracked state that decides WHEN to actually trigger
 * the NUC (relay_logic_get_state(3)/relay_logic_command(3) below are
 * unchanged - this is purely about what gets reported back). R1/R2/R4
 * still report their real tracked state via relay_logic_get_mask(). */
static int s_r3_echo = 0;

/* Tracks what we last actually REQUESTED for R3 (-1 = nothing yet),
 * separate from relay_logic_get_state(3) (what's currently tracked).
 * Needed because the R3->R4 cascade takes a full 6 seconds to run
 * (3s delay + 3s on) and relay_track_task processes one command at
 * a time - if the server holds a value across multiple 500ms polls
 * while the cascade is still mid-flight, comparing against the
 * tracked state alone would re-queue the SAME toggle request
 * repeatedly (since the tracked state hasn't caught up yet), and
 * since it's a toggle, those duplicates fire back-to-back once the
 * cascade frees up - flipping the state multiple extra times and
 * landing on an unpredictable final value. Comparing against what
 * we last REQUESTED instead ensures exactly one toggle is queued
 * per actual desired-state change, no matter how long the cascade
 * takes to catch up. */
static int s_r3_last_requested = -1;
static bool s_r3_command_pending = false;

/* R1/R2 are pulse commands, not persistent ON/OFF states.
 * R1: server Status=1 -> ON for 3s -> OFF.
 * R2: server Status=0 -> OFF for 3s -> ON.
 * Holding the same server value must not retrigger the pulse. */
static int s_r1_last_requested = -1;
static int s_r2_last_requested = -1;

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

/* ── HTTPS GET — one persistent client, serialized by one mutex ──
 *
 * The original working code created and destroyed an esp_http_client (and
 * therefore the TLS context) for every request. That is expensive on ESP32
 * and caused latency and, under contention, mbedTLS allocation failures.
 *
 * Keep one client alive and reuse the connection where possible. Only one
 * task may use it at a time, preserving the original single-HTTPS-context
 * safety rule. */
typedef struct {
    char   *buf;
    size_t  cap;
    size_t  len;
} http_resp_ctx_t;

static esp_http_client_handle_t s_http_client = NULL;
static http_resp_ctx_t s_http_resp_ctx = {0};

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    if (!evt) {
        return ESP_OK;
    }

    if (evt->event_id == HTTP_EVENT_ON_DATA &&
        evt->data != NULL &&
        evt->data_len > 0 &&
        s_http_resp_ctx.buf != NULL &&
        s_http_resp_ctx.cap > 0) {

        size_t room = (s_http_resp_ctx.cap - 1u) - s_http_resp_ctx.len;
        size_t copy_len = ((size_t)evt->data_len < room)
                        ? (size_t)evt->data_len
                        : room;

        if (copy_len > 0u) {
            memcpy(s_http_resp_ctx.buf + s_http_resp_ctx.len,
                   evt->data,
                   copy_len);
            s_http_resp_ctx.len += copy_len;
            s_http_resp_ctx.buf[s_http_resp_ctx.len] = '\0';
        }
    }

    return ESP_OK;
}

static bool http_client_init_once(void)
{
    if (s_http_client != NULL) {
        return true;
    }

    esp_http_client_config_t cfg = {
        .url               = "https://" REMOTE_HOST,
        .method            = HTTP_METHOD_GET,
        .timeout_ms        = 2500,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler     = http_event_handler,
        .keep_alive_enable = true,
        .keep_alive_idle   = 5,
        .keep_alive_interval = 5,
        .keep_alive_count  = 3,
    };

    s_http_client = esp_http_client_init(&cfg);
    if (s_http_client == NULL) {
        ESP_LOGE(TAG, "persistent HTTP client init failed");
        return false;
    }

    return true;
}

static bool http_get_timed(const char *url,
                           char *resp_buf,
                           size_t resp_sz,
                           uint32_t mutex_wait_ms)
{
    if (s_http_mutex == NULL) {
        ESP_LOGE(TAG, "HTTP mutex not initialized");
        return false;
    }

    if (xSemaphoreTake(s_http_mutex, pdMS_TO_TICKS(mutex_wait_ms)) != pdTRUE) {
        ESP_LOGD(TAG, "HTTP busy, skipping: %s", url);
        return false;
    }

    bool result = false;

    s_http_resp_ctx.buf = resp_buf;
    s_http_resp_ctx.cap = resp_sz;
    s_http_resp_ctx.len = 0u;

    if (resp_buf != NULL && resp_sz > 0u) {
        resp_buf[0] = '\0';
    }

    if (!http_client_init_once()) {
        goto out;
    }

    if (esp_http_client_set_url(s_http_client, url) != ESP_OK) {
        ESP_LOGW(TAG, "HTTP set URL failed: %s", url);
        goto out;
    }

    (void)esp_http_client_set_method(s_http_client, HTTP_METHOD_GET);

    esp_err_t err = esp_http_client_perform(s_http_client);
    int code = esp_http_client_get_status_code(s_http_client);

    result = (err == ESP_OK && (code == 200 || code == 201));

    if (!result) {
        ESP_LOGW(TAG, "HTTP fail err=%s code=%d %s",
                 esp_err_to_name(err), code, url);

        /*
         * Do not destroy the persistent client here unless the connection
         * itself failed. Closing the current keep-alive connection allows
         * the next request to reconnect while retaining the client object.
         */
        if (err != ESP_OK) {
            (void)esp_http_client_close(s_http_client);
        }
    }

out:
    s_http_resp_ctx.buf = NULL;
    s_http_resp_ctx.cap = 0u;
    s_http_resp_ctx.len = 0u;

    xSemaphoreGive(s_http_mutex);
    return result;
}

static bool http_get(const char *url, char *resp_buf, size_t resp_sz)
{
    return http_get_timed(url, resp_buf, resp_sz, 3500u);
}

/* ── URL base builder ────────────────────────────────────────── */
static int url_base(char *buf, size_t sz,
                    const char *ep, const char *mac)
{
    return snprintf(buf, sz,
                    "https://%s%s/%s?deviceid=%s",
                    REMOTE_HOST, REMOTE_BASE, ep, mac);
}

/* Appends a real wall-clock timestamp as a throwaway query param,
 * format rtc=DDMMYYHHMMSSmmm (day/month/2-digit-year/hour/min/sec,
 * then milliseconds). Every read_*.php AND write/update_*.php call
 * uses this - previously the exact same URL was sent every time
 * (just ?deviceid=MAC, nothing else varies on a read; a write with
 * unchanged state also produces an identical URL), which is exactly
 * what a caching layer (WAF/CDN/reverse proxy) would key a cached
 * response on, making the server *look* like it never updates even
 * if the underlying data actually changed. This makes every request
 * URL unique so nothing can cache it. The server is expected to
 * ignore an unrecognized parameter.
 *
 * Uses real wall-clock time (NTP-synced - ntp_init() runs before
 * remote_server_start() in main.c) rather than boot-relative
 * milliseconds, so the timestamp directly correlates to a real date/
 * time in server-side logs - useful for anyone on the server side
 * correlating requests. Still guarantees a unique URL either way
 * (millisecond resolution), which is what actually matters for
 * defeating caching - the real-time format is a readability bonus
 * on top of that, not a requirement for it.
 *
 * Falls back to boot-relative milliseconds if NTP hasn't synced yet
 * (shouldn't normally happen given the init order above, but keeps
 * this guaranteed-unique even in that edge case, just without a
 * meaningful real date attached). */
static void append_cache_buster(char *url, size_t sz)
{
    size_t len = strlen(url);

    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm timeinfo;
    localtime_r(&tv.tv_sec, &timeinfo);

    if (timeinfo.tm_year > (2024 - 1900)) {
        /* NTP-synced real wall-clock time */
        int ms = (int)(tv.tv_usec / 1000);
        snprintf(url + len, sz - len, "&rtc=%02d%02d%02d%02d%02d%02d%03d",
                 timeinfo.tm_mday, timeinfo.tm_mon + 1,
                 (timeinfo.tm_year % 100),
                 timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec, ms);
    } else {
        /* NTP not synced yet - fall back to boot-relative ms, still
         * guaranteed unique per call even without real time. */
        uint32_t ms_since_boot = (uint32_t)(esp_timer_get_time() / 1000ULL);
        snprintf(url + len, sz - len, "&rtc=%lu", (unsigned long)ms_since_boot);
    }
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
static bool api_led_status(const char *mac, bool send_all, const uint8_t *values_override)
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
int val;
        if (values_override) {
            val = values_override[i];
        } else {
#if TEST_FORCE_ALL_LED_ON
            val = 1;   /* TEMPORARY - see TEST_FORCE_ALL_LED_ON above */
#else
            val = uart_parser_get_gpio((uint8_t)i) ? 1 : 0;
#endif
        }
        pos += snprintf(url+pos, sizeof(url)-(size_t)pos,
                        "&L%d=%d", i+1, val);
    }
    append_cache_buster(url, sizeof(url));

    char resp[HTTP_RESP_BUF_SZ] = {0};
    if (!http_get_timed(url, resp, sizeof(resp), 1000u)) {
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
uint8_t remote_server_get_report_mask(void)
{
    uint8_t m = relay_logic_get_mask();

    /* Once the NUC has sent physical DD feedback, that is authoritative.
     * This prevents post-reset/reconnect drift and makes R3/R4 reporting
     * follow the real NUC outputs. */
    uint8_t physical_mask = 0u;
    bool physical_locked = false;
    uint32_t physical_seq = 0u;
    uart_parser_get_relay_snapshot(&physical_mask,
                                   &physical_locked,
                                   &physical_seq);
    if (physical_seq != 0u) {
        m = physical_mask;
        s_locked = physical_locked;
    } else if (s_locked) {
        m &= ~0x02u;
    }
#if TEST_FORCE_ALL_RELAYS_ON
    /* TEMPORARY - see TEST_FORCE_ALL_RELAYS_ON in config.h. Takes
     * priority over TEST_FORCE_RELAY_ACTIVE below. */
    m = 0x0Fu;   /* all 4 bits set: R1=R2=R3=R4=1 */
#elif TEST_FORCE_RELAY_ACTIVE
    /* TEMPORARY - see TEST_FORCE_RELAY_ACTIVE in config.h */
    m |= (uint8_t)(1u << (TEST_ACTIVE_RELAY_NUM - 1));
#endif
    return m;
}

static bool api_relay_status(const char *mac)
{
    uint8_t m = remote_server_get_report_mask();
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
    if (!http_get_timed(url, resp, sizeof(resp), 50u)) {
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
            if (n < 1 || n > 4) continue;    /* we only track RL1-RL4 */

            /* Live server sends "1"/"0" as STRING — handle both
             * string and integer forms safely.                    */
            int srv = cjson_as_int(st, -1);
            raw[n - 1] = srv;   /* still recorded/logged even for R4,
                                    for diagnostic visibility - just
                                    never acted on below */

            if (n == 4) {
                /* RL4 has no independent server command. It is driven
                 * only by the NUC when RL3 is turned ON. */
                continue;
            }

            if (n == 3 && (srv == 0 || srv == 1)) {
                /* Direct echo, every poll, no delay - matches exactly
                 * what the server last sent for R3, immediately.
                 * Independent of the trigger logic below - the echo
                 * always mirrors the raw server value as-is. */
                s_r3_echo = srv;
            }

            /* Relay command semantics:
             *
             * RL1 is a one-shot: Status=1 means "run RL1".
             * RL1 then goes OFF automatically after 3 seconds.
             *
             * RL2 is a one-shot with inverted command meaning:
             * Status=0 means "turn RL2 OFF for 3 seconds".
             * RL2 then returns to its normal ON state automatically.
             *
             * RL3 is a persistent state: Status=1 -> ON, Status=0 -> OFF.
             * RL4 is intentionally ignored here; it is generated only by
             * the NUC's RL3 cascade.
             *
             * R1/R2 use last-requested values so a server value held for
             * multiple polls produces exactly one physical pulse. */
            bool want_on = (srv == 1);
            bool should_trigger = false;

            if (n == 1) {
                should_trigger = (srv == 1 && srv != s_r1_last_requested);
                if (srv == 0 || srv == 1) {
                    s_r1_last_requested = srv;
                }
            } else if (n == 2) {
                /* R2 command is Status=0: OFF for 3s, then back ON.
                 * While locked, consume the command without triggering.
                 * Unlock explicitly restores R2 ON. */
                should_trigger = (srv == 0 && srv != s_r2_last_requested
                                  && !s_locked);
                if (srv == 0 || srv == 1) {
                    s_r2_last_requested = srv;
                }
            } else { /* n == 3 */
                /*
                 * R3 is a TOGGLE command in the NUC. Never use the ESP
                 * rebooted software mirror as the authority.
                 * Wait for the first real DD feedback from the NUC.
                 */
                uint8_t physical_mask = 0u;
                bool physical_locked = false;
                uint32_t physical_seq = 0u;
                uart_parser_get_relay_snapshot(&physical_mask,
                                               &physical_locked,
                                               &physical_seq);
                (void)physical_locked;

                if (physical_seq != 0u) {
                    bool physical_r3 = (physical_mask & 0x04u) != 0u;
                    should_trigger = (srv == 0 || srv == 1)
                                   && (srv != (physical_r3 ? 1 : 0))
                                   && !s_r3_command_pending;
                    if (should_trigger) {
                        s_r3_command_pending = true;
                    }
                } else {
                    /* NUC physical state is not known yet. Never infer a
                     * toggle after soft reset; wait for DD feedback. */
                    should_trigger = false;
                }

                if (srv == 0 || srv == 1) {
                    s_r3_last_requested = srv;
                }
            }

            if (should_trigger) {
                ESP_LOGI(TAG, "Server command: RL%d Status=%d", n, srv);
                if (n == 3) {
                    relay_logic_set_r3(want_on);
                } else {
                    relay_logic_command((uint8_t)n);
                }
                uart_send_relay_cmd((uint8_t)n);

                /*
                 * Do not perform a second HTTPS write here. The NUC
                 * physical DD feedback is the authoritative report path.
                 * Removing this extra request keeps R3/R4 command latency low.
                 */
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
            uint8_t rmask = remote_server_get_report_mask();
            ESP_LOGI(TAG, "Relay poll OK (tracked): R1=%d R2=%d R3=%d R4=%d",
                     (rmask>>0)&1, (rmask>>1)&1, (rmask>>2)&1, (rmask>>3)&1);
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
        /* Live server behavior: LockStatus=1 means locked, and 0 means
         * the device is unlocked. Some older server responses/documentation
         * used 2 as an explicit unlock command, so accept that too.
         * Critically, 0 only causes an action when we are currently locked,
         * so a normal unlocked poll cannot repeatedly transmit unlock. */
        if (lock == 1 || lock == 2 || (lock == 0 && s_locked)) {
            bool new_lock = (lock == 1);
            if (new_lock != s_locked) {
                s_locked = new_lock;
                ESP_LOGI(TAG, "Device %s", s_locked ? "LOCKED" : "UNLOCKED");
                uart_send_relay_lock_cmd(s_locked);

                /* A lock release forces the physical R2 back to its normal
                 * ON state at the NUC. If the server is still presenting the
                 * normal R2=0 one-shot command, allow that command to be
                 * consumed again after the unlock instead of treating the
                 * pre-unlock 0 as already consumed while locked. */
                if (!s_locked) {
                    /* Unlock itself restores R2 to ON.  If the server still
                     * shows the old R2=0 command, do NOT execute that old
                     * command immediately after unlock (which would create
                     * an unwanted OFF->ON pulse).  Require a fresh server
                     * transition 1->0 before generating the next R2 pulse. */
                    s_r2_last_requested = 0;
                }

                api_relay_status(mac);
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
#if 0   /* disabled - no longer polled, see remote_task() for why */
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
#endif /* disabled api_read_input_status */

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
    if (bat_mv == 0u) return 3;                  /* Unknown - no reading */
    if (bat_mv <= BATTERY_CUTOFF_MV) return 2;   /* Battery Failed        */
    // if (low_battery) return 2;                   /* Low Battery           */
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
    uint32_t last_relay = 0u;   /* unused now - relay writes are sequenced
                                    directly off the read poll instead of
                                    an independent timer, see below */
    (void)last_relay;
    uint32_t last_poll  = 0u;   /* unused now - relay command polling
                                    moved to its own dedicated task */
    (void)last_poll;
    uint32_t last_lock  = 0u;
    uint32_t last_rst   = 0u;
    uint32_t last_input_status = 0u;   /* unused now - see the removed
                                           API 10 poll block below */
    (void)last_input_status;
    uint32_t last_power = 0u;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(200));
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000ULL);

        /* Immediate relay reporting on any tracked state change is
         * now handled by the dedicated relay_report_task (see
         * remote_server_start()) instead of a non-blocking check
         * here - that task blocks indefinitely on the change
         * semaphore rather than checking it once per (potentially
         * slow, due to this loop's other sequential HTTP calls)
         * iteration. Only one consumer may take from this semaphore
         * - having a second check here too would race against the
         * dedicated task over which one actually handles a given
         * change. */

        /* API 1 — Register on boot (Switch ON) */
        if (!registered) {
            if (api_register(mac, REGISTER_STATUS_ON)) {
                registered = true;
                s_registered = true;   /* shared with relay_report_task */
                status_led_set_mode(LED_MODE_CONNECTED);
                buzzer_set_no_signal(false);
                ESP_LOGI(TAG, "Registered OK");
#if ENABLE_SOFT_RESET_FEATURE
                /* Ack any pending soft-reset as done - if this boot
                 * wasn't a soft-reset reboot at all, this is a no-op
                 * as far as real device state goes, just tells the
                 * server "nothing in progress" which is trivially true. */
                // api_update_soft_reset_status(mac, 1);
#endif
            } else {
                status_led_set_mode(LED_MODE_SEARCHING);
                buzzer_set_no_signal(true);
                vTaskDelay(pdMS_TO_TICKS(5000));
                continue;
            }
        }

        /* API 3 (relay status write) — REMOVED as an independent
         * timer. Previously ran on its own separate RELAY_UPDATE_MS
         * (500ms) cadence, completely independent of the read poll
         * below - this meant the write could fire with whatever
         * local state existed AT THAT MOMENT, regardless of whether
         * a fresh read had just come in, occasionally sending stale
         * data right around when the server had just sent something
         * new. Every real relay change already triggers an immediate
         * write (see api_read_relay()'s own edge-detection logic,
         * and the tracked-state-change semaphore above) - the write
         * now only ever happens as a direct, sequenced response to
         * actually processing new data, never on its own independent
         * schedule. */

        /* API 5 (relay command poll) — MOVED to its own dedicated
         * task (relay_poll_task, see below), for the same reason
         * relay reporting was split out earlier: this loop also
         * runs several other sequential HTTPS calls (LED write,
         * lock poll, power details, etc.), each taking real network
         * time. Relay command responsiveness is the most latency-
         * sensitive thing this device does, so it shouldn't have to
         * wait in line behind a slow LED or power-details call. */

        /* Lock polling runs in its own dedicated task so it never waits
         * behind LED/power/other HTTPS work in this task. */
        (void)last_lock;

        /* API 10 (read_input_status.php) polling REMOVED - it was
         * purely diagnostic, never drove any actual behavior, and
         * repeatedly showed different values than read_relay_status.php
         * (the endpoint that actually matters) for the same relay at
         * the same time - a server-side inconsistency we can't fix
         * from firmware, and one that kept causing confusion in
         * testing since it looked like "the server said X" when it
         * was really just a second endpoint disagreeing with the
         * first. Re-enable by uncommenting below if needed again for
         * server-side diagnostic purposes.
         * if ((now - last_input_status) >= INPUT_STATUS_POLL_MS) {
         *     api_read_input_status(mac);
         *     last_input_status = now;
         * }
         */

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

/* Dedicated LED reporting task.
 *
 * The old implementation performed the LED HTTPS write inside remote_task(),
 * which also performs lock, power, soft-reset and other HTTPS operations. A
 * slow network transaction could therefore postpone an LED change by seconds.
 * This task samples the already-parsed NUC LED/switch snapshot frequently and
 * writes the complete 25-LED state as one request whenever any LED changes.
 * It also marks an LED as 2 (BLINK) once it has toggled rapidly, and returns
 * to the stable 0/1 value after the signal has been quiet for 1 second. */
static void led_report_task(void *arg)
{
    (void)arg;

    char mac[16];
    get_device_id(mac, sizeof(mac));

    bool previous[GPIO_COUNT] = {0};
    bool have_previous = false;
    bool blinking[GPIO_COUNT] = {0};
    uint32_t last_change_ms[GPIO_COUNT] = {0};

    for (;;) {
        bool gpio[GPIO_COUNT] = {0};
        uint8_t values[GPIO_COUNT] = {0};
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000ULL);
        bool changed = false;

        /*
         * Read the latest 25 input/LED states from the NUC.
         * No periodic LED HTTP update is performed.
         */
        uart_parser_get_gpio_snapshot(gpio, GPIO_COUNT);

        if (!have_previous) {
            /*
             * Startup only establishes the baseline.
             * Do NOT send an LED packet just because the ESP booted.
             */
            memcpy(previous, gpio, sizeof(previous));
            have_previous = true;

            for (int i = 0; i < GPIO_COUNT; ++i) {
                last_change_ms[i] = now;
            }
        } else {
            for (int i = 0; i < GPIO_COUNT; ++i) {
                if (gpio[i] != previous[i]) {
                    uint32_t dt = now - last_change_ms[i];

                    /*
                     * Any LED/input transition inside 500 ms is reported
                     * as BLINK (2). This is preserved from the previous
                     * behavior.
                     */
                    if (dt <= 500u) {
                        blinking[i] = true;
                    }

                    last_change_ms[i] = now;
                    previous[i] = gpio[i];
                    changed = true;
                } else if (blinking[i] &&
                           (now - last_change_ms[i]) >= 1000u) {
                    /*
                     * Signal stopped toggling: return to normal 0/1 state.
                     * This is also a real state change and therefore causes
                     * one complete 25-LED update.
                     */
                    blinking[i] = false;
                    changed = true;
                }
            }
        }

        /*
         * Build the COMPLETE 25-LED packet every time, but transmit it only
         * when at least one LED/input state actually changed.
         *
         * send_all=true guarantees LED_CNT=ALL and all 25 L1..L25 values
         * are sent in ONE HTTPS request.
         */
        for (int i = 0; i < GPIO_COUNT; ++i) {
            values[i] = blinking[i] ? 2u : (gpio[i] ? 1u : 0u);
        }

        if (s_registered && changed) {
            bool ok = api_led_status(mac, true, values);

            if (ok) {
                ESP_LOGI(TAG,
                         "LED state changed -> all %d LEDs sent in ONE packet: OK",
                         GPIO_COUNT);
            } else {
                ESP_LOGW(TAG,
                         "LED state changed -> all %d LEDs packet FAILED",
                         GPIO_COUNT);
            }
        }

        /*
         * Fast local sampling. This is NOT an HTTP polling interval.
         * Server traffic occurs only on a detected state transition.
         */
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

/* Dedicated task purely for reporting relay changes immediately -
 * decoupled entirely from remote_task()'s other polling work below.
 * Previously this was a non-blocking check done once per main-loop
 * iteration, but that loop also runs several sequential, blocking
 * HTTPS calls (register, LED, lock poll, power details, etc.) each
 * taking real time - so the check wasn't truly "every 200ms", it
 * was "every 200ms plus however long that iteration's other HTTP
 * calls took", which could stretch to several seconds. If a relay
 * change (e.g. RL2's 3-second auto-restore) happened during that
 * stretch, it sat waiting until the loop finally got back around to
 * checking - and if another relay change happened to occur in the
 * meantime, both got reported together, making it look like "RL2
 * only updates when something else changes". This task blocks
 * indefinitely on the semaphore instead, so it reports the instant
 * a change happens, regardless of what the rest of the polling
 * loop is doing. */
static void relay_report_task(void *arg)
{
    char mac[16];
    get_device_id(mac, sizeof(mac));
    ESP_LOGI(TAG, "relay_report_task started, mac=%s", mac);

    /* 0xFF is not a valid 4-bit relay mask (only bits 0-3 ever used),
     * so this guarantees the very first real mask always looks
     * "changed" and gets reported once at startup. */
    uint8_t last_sent = 0xFFu;

    for (;;) {
        if (!s_registered) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        /* First publish the current state (also covers boot), then wait
         * for a real state change. This removes the old fixed 1-second
         * reporting delay without losing the initial report. */
        uint8_t physical_mask = 0u;
        bool physical_locked = false;
        uint32_t physical_seq = 0u;
        uart_parser_get_relay_snapshot(&physical_mask,
                                       &physical_locked,
                                       &physical_seq);
        (void)physical_locked;

        if (physical_seq != 0u) {
            bool physical_r3 = (physical_mask & 0x04u) != 0u;
            if (s_r3_command_pending &&
                s_r3_last_requested >= 0 &&
                physical_r3 == (s_r3_last_requested != 0)) {
                s_r3_command_pending = false;
            }
        }

        uint8_t current = remote_server_get_report_mask();
        if (current != last_sent) {
            ESP_LOGI(TAG, "Relay state changed -> R1=%d R2=%d R3=%d R4=%d",
                     (current>>0)&1, (current>>1)&1, (current>>2)&1, (current>>3)&1);
            bool write_ok = api_relay_status(mac);
            ESP_LOGI(TAG, "relay_report_task write result: %s",
                     write_ok ? "OK" : "FAILED");
            if (write_ok) {
                last_sent = current;
            }
            /* If the write failed, last_sent remains unchanged, so the
             * current state is retried on the next notification/timeout. */
        }

        /* Block until another relay state change occurs. If a change
         * happens between the state read above and this wait, the binary
         * semaphore remains given and the next loop handles it. */
        (void)xSemaphoreTake(relay_logic_get_change_semaphore(),
                             pdMS_TO_TICKS(100));
    }
}

/* Dedicated task purely for polling the server for relay commands
 * (read_relay_status.php) - decoupled from remote_task()'s other,
 * slower sequential polls (LED write, lock poll, power details,
 * etc.). Relay command responsiveness is the most latency-sensitive
 * thing this device does - it shouldn't have to wait behind an LED
 * write or a power-details call that happens to be in progress at
 * the same moment. */
static void relay_poll_task(void *arg)
{
    char mac[16];
    get_device_id(mac, sizeof(mac));
    ESP_LOGI(TAG, "relay_poll_task started, mac=%s", mac);

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(100));
        if (!s_registered) continue;
        /* R2 lock affects only R2. Keep polling so RL1 and RL3 commands
         * remain available while R2 is locked. api_read_relay() suppresses
         * only the R2 pulse when s_locked is true. */
        api_read_relay(mac);
    }
}

static void lock_poll_task(void *arg)
{
    char mac[16];
    get_device_id(mac, sizeof(mac));
    ESP_LOGI(TAG, "lock_poll_task started, mac=%s", mac);

    for (;;) {
        if (s_registered) {
            api_read_lock(mac);
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void remote_server_start(void)
{
    s_http_mutex = xSemaphoreCreateMutex();
    configASSERT(s_http_mutex);

    xTaskCreate(remote_task, "remote", REMOTE_TASK_STACK,
                NULL, REMOTE_TASK_PRIO, NULL);
    xTaskCreate(led_report_task, "led_report", 6144,
                NULL, LED_TASK_PRIO, NULL);
    xTaskCreate(relay_report_task, "relay_report", 6144,
                NULL, REMOTE_TASK_PRIO, NULL);
    xTaskCreate(relay_poll_task, "relay_poll", 8192,
                NULL, REMOTE_TASK_PRIO + 1, NULL);
    xTaskCreate(lock_poll_task, "lock_poll", 6144,
                NULL, REMOTE_TASK_PRIO, NULL);
}

void remote_server_notify_relay_change(void) {}

bool remote_server_is_locked(void) { return s_locked; }