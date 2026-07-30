/*
 * web_server.c  —  ESP32 Local Dashboard  (plain HTTP)
 *
 * NEW in this revision:
 *   - HTTP Basic Auth on every route (DASHBOARD_USERNAME/PASSWORD
 *     in config.h) — browser shows native login prompt; Android
 *     app sends the same header programmatically.
 *   - Battery percentage in /status JSON (bat_pct), computed from
 *     bat_mv using BATTERY_FULL_MV/BATTERY_CUTOFF_MV thresholds.
 *   - POST /wifi/reset — erases saved WiFi credentials from NVS
 *     and restarts into provisioning mode. Used by both the local
 *     dashboard button and the Android app.
 *
 * All existing routes/payloads otherwise unchanged.
 */
#include "web_server.h"
#include "uart_parser.h"
#include "relay_logic.h"
#include "config.h"

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
static const char *TAG = "web";

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

/* ─────────────────────────────────────────────────────────────
 *  HTTP Basic Auth check
 *
 *  Compares the "Authorization: Basic <base64>" header against
 *  a fixed expected value built from DASHBOARD_USERNAME/PASSWORD.
 *  Returns true if authorized, sends 401 challenge and returns
 *  false otherwise (caller must return ESP_OK without further
 *  processing when this returns false).
 * ───────────────────────────────────────────────────────────── */
static const char b64_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int base64_encode(const char *in, char *out, size_t out_sz)
{
    size_t in_len = strlen(in);
    size_t oi = 0;
    for (size_t i = 0; i < in_len; i += 3) {
        uint32_t n = (uint32_t)(unsigned char)in[i] << 16;
        if (i + 1 < in_len) n |= (uint32_t)(unsigned char)in[i+1] << 8;
        if (i + 2 < in_len) n |= (uint32_t)(unsigned char)in[i+2];

        if (oi + 4 >= out_sz) return -1;
        out[oi++] = b64_table[(n >> 18) & 0x3F];
        out[oi++] = b64_table[(n >> 12) & 0x3F];
        out[oi++] = (i + 1 < in_len) ? b64_table[(n >> 6) & 0x3F] : '=';
        out[oi++] = (i + 2 < in_len) ? b64_table[n & 0x3F]        : '=';
    }
    out[oi] = '\0';
    return (int)oi;
}

static bool check_auth(httpd_req_t *req)
{
    char expected_plain[128];
    snprintf(expected_plain, sizeof(expected_plain), "%s:%s",
             DASHBOARD_USERNAME, DASHBOARD_PASSWORD);

    char expected_b64[192];
    base64_encode(expected_plain, expected_b64, sizeof(expected_b64));

    char expected_hdr[220];
    snprintf(expected_hdr, sizeof(expected_hdr), "Basic %s", expected_b64);

    char actual_hdr[220] = {0};
    esp_err_t err = httpd_req_get_hdr_value_str(req, "Authorization",
                                                actual_hdr, sizeof(actual_hdr));

    bool ok = (err == ESP_OK) && (strcmp(actual_hdr, expected_hdr) == 0);

    if (!ok) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_hdr(req, "WWW-Authenticate",
                           "Basic realm=\"Aditya Electronics Gateway\"");
        httpd_resp_send(req, "Unauthorized", HTTPD_RESP_USE_STRLEN);
    }
    return ok;
}

/* ── GET / ───────────────────────────────────────────────────── */
static esp_err_t root_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;

    size_t len = (size_t)(index_html_end - index_html_start);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_send(req, (const char *)index_html_start, (ssize_t)len);
    return ESP_OK;
}

/* Battery percentage now comes directly from the NUC (uart_parser_get_bat_pct()),
 * computed there from the same thresholds via Battery_PercentFromMv() -
 * this local recalculation was removed to avoid the two ends ever
 * disagreeing. bat_mv==0 (ADC timeout on the NUC) still needs special
 * handling here since the NUC sends 0% for that case too, which would
 * otherwise look identical to a genuinely empty battery. */

/* ── GET /status ─────────────────────────────────────────────── */
static esp_err_t status_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;

    uint32_t now    = (uint32_t)(esp_timer_get_time() / 1000ULL);
    uint32_t last   = uart_parser_last_packet_ms();
    uint32_t age    = (last > 0u) ? (now - last) : 0u;
    uint8_t  rmask  = relay_logic_get_mask();
    uint16_t bat_mv = uart_parser_get_bat_mv();
    uint16_t pwr_mv = uart_parser_get_pwr_mv();
    bool     pf     = uart_parser_is_power_fail();
    bool     lb     = uart_parser_is_low_battery();
    int      bat_pct = (bat_mv == 0u) ? -1 : (int)uart_parser_get_bat_pct();

   time_t now_time;
struct tm timeinfo;
char datetime[32];

time(&now_time);

if ((now_time > 1700000000) &&
    localtime_r(&now_time, &timeinfo) != NULL)
{
    strftime(datetime,
             sizeof(datetime),
             "%d-%m-%Y %H:%M:%S",
             &timeinfo);
}
else
{
    strcpy(datetime, "Synchronizing...");
}

    uint8_t mac_bytes[6];
    char    mac[16] = {0};
    esp_wifi_get_mac(WIFI_IF_STA, mac_bytes);
    snprintf(mac, sizeof(mac), "%02X%02X%02X%02X%02X%02X",
             mac_bytes[0], mac_bytes[1], mac_bytes[2],
             mac_bytes[3], mac_bytes[4], mac_bytes[5]);

    char buf[500];
    int  pos = 0;

    pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos,
                    "{\"gpios\":[");
    for (uint8_t i = 0; i < GPIO_COUNT; i++) {
        buf[pos++] = uart_parser_get_gpio(i) ? '1' : '0';
        if (i < (GPIO_COUNT - 1u)) buf[pos++] = ',';
    }
   pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos,
    "],\"packets\":%lu"
    ",\"age_ms\":%lu"
    ",\"online\":%s"
    ",\"relays\":[%d,%d,%d,%d]"
    ",\"mac\":\"%s\""
    ",\"power_fail\":%s"
    ",\"low_battery\":%s"
    ",\"bat_mv\":%u"
    ",\"pwr_mv\":%u"
    ",\"bat_pct\":%d"
    ",\"datetime\":\"%s\""
    ",\"device\":\"Aditya Electronics Gateway\"}",

    (unsigned long)uart_parser_packet_count(),
    (unsigned long)age,
    uart_parser_is_online() ? "true" : "false",

    (rmask >> 0) & 1,
    (rmask >> 1) & 1,
    (rmask >> 2) & 1,
    (rmask >> 3) & 1,

    mac,

    pf ? "true" : "false",
    lb ? "true" : "false",

    (unsigned int)bat_mv,
    (unsigned int)pwr_mv,

    bat_pct,

    datetime
);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_send(req, buf, pos);
    return ESP_OK;
}

/* ── POST /relay  {"sw":1} ───────────────────────────────────── */
static esp_err_t relay_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;

    char body[32] = {0};
    int to_read = ((int)req->content_len < (int)(sizeof(body) - 1))
                  ? (int)req->content_len
                  : (int)(sizeof(body) - 1);
    int recv = httpd_req_recv(req, body, to_read);
    if (recv <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    body[recv] = '\0';

    char *p = strstr(body, "\"sw\"");
    if (!p) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing sw");
        return ESP_FAIL;
    }
    p += 4;
    while (*p == ' ' || *p == ':') p++;
    int sw = atoi(p);

    if (sw < 1 || sw > 3) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "sw must be 1-3");
        return ESP_FAIL;
    }

    relay_logic_command((uint8_t)sw);
    uart_send_relay_cmd((uint8_t)sw);

    char resp[40];
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"sw\":%d}", sw);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* ── Delayed restart task ─────────────────────────────────────── */
static void delayed_restart_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
    vTaskDelete(NULL);
}

/* ── POST /wifi/reset — erase WiFi creds, restart into portal ──── */
static esp_err_t wifi_reset_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;

    nvs_handle_t h;
    if (nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, WIFI_NVS_KEY_SSID);
        nvs_erase_key(h, WIFI_NVS_KEY_PASS);
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGW(TAG, "WiFi credentials erased — restarting into "
                      "provisioning mode");
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req,
        "{\"ok\":true,\"message\":\"Restarting into WiFi setup mode\"}",
        HTTPD_RESP_USE_STRLEN);

    xTaskCreate(delayed_restart_task, "wifi_reset_restart",
                2048, NULL, 5, NULL);

    return ESP_OK;
}

/* ── Start — plain HTTP server ───────────────────────────────── */
void web_server_start(void)
{
    httpd_config_t cfg   = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable = true;
    cfg.max_uri_handlers = 8;

    httpd_handle_t server = NULL;
    ESP_ERROR_CHECK(httpd_start(&server, &cfg));

    static const httpd_uri_t routes[] = {
        { .uri = "/",           .method = HTTP_GET,  .handler = root_handler       },
        { .uri = "/status",     .method = HTTP_GET,  .handler = status_handler     },
        { .uri = "/relay",      .method = HTTP_POST, .handler = relay_handler      },
        { .uri = "/wifi/reset", .method = HTTP_POST, .handler = wifi_reset_handler },
    };
    for (int i = 0; i < 4; i++)
        httpd_register_uri_handler(server, &routes[i]);

    ESP_LOGI(TAG, "HTTP server started (login required)");
}
