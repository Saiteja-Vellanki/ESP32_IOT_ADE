#include "web_server.h"
#include "uart_parser.h"
#include "relay_logic.h"
#include "config.h"

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char *TAG = "web";

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

static void get_mac_str(char *out, size_t sz)
{
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    snprintf(out, sz, "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static esp_err_t root_handler(httpd_req_t *req)
{
    size_t len = (size_t)(index_html_end - index_html_start);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_send(req, (const char *)index_html_start, (ssize_t)len);
    return ESP_OK;
}

static esp_err_t status_handler(httpd_req_t *req)
{
    uint32_t now   = (uint32_t)(esp_timer_get_time() / 1000ULL);
    uint32_t last  = uart_parser_last_packet_ms();
    uint32_t age   = (last > 0u) ? (now - last) : 0u;
    uint8_t  rmask = relay_logic_get_mask();
    char     mac[16];
    get_mac_str(mac, sizeof(mac));

    char buf[380];
    int  pos = 0;

    pos += snprintf(buf+pos, sizeof(buf)-(size_t)pos, "{\"gpios\":[");
    for (uint8_t i = 0; i < GPIO_COUNT; i++) {
        buf[pos++] = uart_parser_get_gpio(i) ? '1' : '0';
        if (i < (GPIO_COUNT-1u)) buf[pos++] = ',';
    }
    pos += snprintf(buf+pos, sizeof(buf)-(size_t)pos,
                    "],\"packets\":%lu,\"age_ms\":%lu,\"online\":%s,"
                    "\"relays\":[%d,%d,%d,%d],"
                    "\"mac\":\"%s\","
                    "\"device\":\"Aditya Electronics Gateway\"}",
                    (unsigned long)uart_parser_packet_count(),
                    (unsigned long)age,
                    uart_parser_is_online() ? "true" : "false",
                    (rmask>>0)&1,(rmask>>1)&1,
                    (rmask>>2)&1,(rmask>>3)&1,
                    mac);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_send(req, buf, pos);
    return ESP_OK;
}

static esp_err_t relay_handler(httpd_req_t *req)
{
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

void web_server_start(void)
{
    httpd_config_t cfg   = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable = true;
    cfg.max_uri_handlers = 8;
    httpd_handle_t server = NULL;
    ESP_ERROR_CHECK(httpd_start(&server, &cfg));
    static const httpd_uri_t routes[] = {
        {.uri="/",       .method=HTTP_GET,  .handler=root_handler  },
        {.uri="/status", .method=HTTP_GET,  .handler=status_handler},
        {.uri="/relay",  .method=HTTP_POST, .handler=relay_handler },
    };
    for (int i = 0; i < 3; i++)
        httpd_register_uri_handler(server, &routes[i]);
    ESP_LOGI(TAG, "HTTP server started");
}