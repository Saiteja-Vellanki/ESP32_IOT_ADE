#include "web_server.h"
#include "uart_parser.h"
#include "relay_logic.h"
#include "config.h"

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char *TAG = "web_server";

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

/* ── GET / ───────────────────────────────────────────────────── */
static esp_err_t root_handler(httpd_req_t *req)
{
    size_t len = (size_t)(index_html_end - index_html_start);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_send(req, (const char *)index_html_start, (ssize_t)len);
    return ESP_OK;
}

/* ── GET /status ─────────────────────────────────────────────── */
static esp_err_t status_handler(httpd_req_t *req)
{
    uint32_t now    = (uint32_t)(esp_timer_get_time() / 1000ULL);
    uint32_t last   = uart_parser_last_packet_ms();
    uint32_t age    = (last > 0u) ? (now - last) : UINT32_MAX;
    bool     online = uart_parser_is_online();
    uint32_t pkts   = uart_parser_packet_count();
    uint8_t  rmask  = relay_logic_get_mask();

    char buf[320];
    int  pos = 0;

    pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos,
                    "{\"gpios\":[");
    for (uint8_t i = 0; i < GPIO_COUNT; i++) {
        buf[pos++] = uart_parser_get_gpio(i) ? '1' : '0';
        if (i < (GPIO_COUNT - 1u)) buf[pos++] = ',';
    }
    pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos,
                    "],\"packets\":%lu,\"age_ms\":%lu,\"online\":%s,"
                    "\"relays\":[%d,%d,%d,%d]}",
                    (unsigned long)pkts,
                    (unsigned long)(age == UINT32_MAX ? 0u : age),
                    online ? "true" : "false",
                    (rmask >> 0) & 1,
                    (rmask >> 1) & 1,
                    (rmask >> 2) & 1,
                    (rmask >> 3) & 1);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_send(req, buf, pos);
    return ESP_OK;
}

/* ── POST /relay  body: {"sw":1}  ────────────────────────────── */
/*
 * Called when user presses SW1/SW2/SW3 button on dashboard.
 * sw=1 → relay_logic_command(1)  RL1 pulse 3s
 * sw=2 → relay_logic_command(2)  RL2 toggle continuous
 * sw=3 → relay_logic_command(3)  RL3 ON → RL4 pulse
 */
static esp_err_t relay_handler(httpd_req_t *req)
{
    char body[32] = {0};
    int  recv=0;
    // int  recv = httpd_req_recv(req, body,
    //                            MIN((int)req->content_len,
    //                                (int)sizeof(body) - 1));

    // int  recv = httpd_req_recv(req, body,
    //                            (int)req->content_len,
    //                                (int)sizeof(body) - 1);
    if (recv <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    body[recv] = '\0';

    /* parse {"sw":N} */
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

#if !LOCAL_WEB_SERVER
    extern void remote_server_notify_relay_change(void);
    remote_server_notify_relay_change();
#endif

    char resp[40];
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"sw\":%d}", sw);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* ── Start ───────────────────────────────────────────────────── */
void web_server_start(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t cfg    = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable  = true;
    cfg.max_uri_handlers  = 8;

    ESP_ERROR_CHECK(httpd_start(&server, &cfg));

    static const httpd_uri_t routes[] = {
        { .uri="/",       .method=HTTP_GET,  .handler=root_handler   },
        { .uri="/status", .method=HTTP_GET,  .handler=status_handler },
        { .uri="/relay",  .method=HTTP_POST, .handler=relay_handler  },
    };
    for (int i = 0; i < 3; i++)
        httpd_register_uri_handler(server, &routes[i]);

    ESP_LOGI(TAG, "HTTP server started");
    ESP_LOGI(TAG, "  GET  /        → dashboard");
    ESP_LOGI(TAG, "  GET  /status  → JSON (gpios + relays)");
    ESP_LOGI(TAG, "  POST /relay   → {\"sw\":1/2/3}");
}
