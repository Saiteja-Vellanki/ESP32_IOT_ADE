/*
 * wifi_ap_provision.c  —  WiFi Access Point Provisioning Portal
 *
 * ESP32 starts as SoftAP "Aditya-IoT-Setup" and hosts a plain HTTP
 * page at http://192.168.4.1/ where the user picks their WiFi
 * network from a scan list (or types it manually) and enters the
 * password. On submit, credentials are saved to NVS and this
 * function returns so main.c can switch to STA mode.
 *
 * NEW in this revision:
 *   - "Scan for Networks" button — GET /scan returns nearby WiFi
 *     SSIDs as JSON; tapping one fills the SSID field automatically.
 *     Runs in WIFI_MODE_APSTA so scanning works while the AP is
 *     still broadcasting.
 *   - Aditya Electronics Solutions logo — recreated as inline SVG
 *     (sunburst + orange circle + "a" + brand text) rather than
 *     embedding the PNG, to keep firmware flash usage small.
 *
 * Credentials are saved to NVS under WIFI_NVS_NAMESPACE /
 * WIFI_NVS_KEY_SSID / WIFI_NVS_KEY_PASS (config.h) - same
 * namespace/keys used previously under bt_commission.c's names,
 * so devices with existing saved credentials keep working.
 */

#include "wifi_ap_provision.h"
#include "config.h"

#include "nvs_flash.h"
#include "nvs.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_system.h"
#include "esp_log.h"
#include "lwip/ip4_addr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "wifi_ap";

#define AP_DONE_BIT   BIT0
#define MAX_SCAN_AP   20

/* Embedded logo — see CMakeLists.txt EMBED_FILES */
extern const uint8_t logo_png_start[] asm("_binary_logo_png_start");
extern const uint8_t logo_png_end[]   asm("_binary_logo_png_end");

static EventGroupHandle_t s_eg;
static httpd_handle_t     s_server = NULL;
static esp_netif_t       *s_ap_netif = NULL;
static esp_netif_t       *s_sta_netif = NULL;

static char s_ssid_out[64] = {0};
static char s_pass_out[64] = {0};

/* ─────────────────────────────────────────────────────────────
 *  wifi_provision_load_nvs  —  ported from the old bt_commission.c
 *  (BT commissioning removed; AP portal is the only provisioning
 *  path now, but this NVS-load helper is shared logic that has
 *  nothing to do with BT itself, so it lives here.)
 * ───────────────────────────────────────────────────────────── */
bool wifi_provision_load_nvs(char *ssid_out, char *pass_out, size_t sz)
{
    nvs_handle_t h;
    esp_err_t open_err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &h);
    if (open_err != ESP_OK) {
        ESP_LOGW(TAG, "NVS load: nvs_open(\"%s\") failed: %s "
                      "(namespace doesn't exist yet — never saved before)",
                 WIFI_NVS_NAMESPACE, esp_err_to_name(open_err));
        return false;
    }

    size_t l = sz;
    bool ok = false;
    esp_err_t ssid_err = nvs_get_str(h, WIFI_NVS_KEY_SSID, ssid_out, &l);

    if (ssid_err != ESP_OK) {
        ESP_LOGW(TAG, "NVS load: SSID key \"%s\" not found: %s",
                 WIFI_NVS_KEY_SSID, esp_err_to_name(ssid_err));
    } else if (strlen(ssid_out) == 0) {
        ESP_LOGW(TAG, "NVS load: SSID key exists but is empty");
    } else {
        l = sz;
        esp_err_t pass_err = nvs_get_str(h, WIFI_NVS_KEY_PASS, pass_out, &l);
        if (pass_err == ESP_OK) {
            ok = true;
        } else {
            ESP_LOGW(TAG, "NVS load: PASS key \"%s\" not found: %s",
                     WIFI_NVS_KEY_PASS, esp_err_to_name(pass_err));
        }
    }

    nvs_close(h);
    if (ok) ESP_LOGI(TAG, "NVS loaded SSID=%s", ssid_out);
    return ok;
}

/* ─────────────────────────────────────────────────────────────
 *  Branded HTML page — Aditya Electronics Solutions
 *  Logo recreated as inline SVG: sunburst + orange circle + "a"
 * ───────────────────────────────────────────────────────────── */
static const char *PAGE_FORM =
"<!DOCTYPE html><html><head><meta charset='UTF-8'>"
"<meta name='viewport' content='width=device-width, initial-scale=1.0'>"
"<title>Aditya Electronic Solutions</title>"
"<style>"
"*{box-sizing:border-box;margin:0;padding:0}"
"body{background:#0f1117;color:#e2e8f0;font-family:'Segoe UI',system-ui,sans-serif;"
"min-height:100vh;display:flex;align-items:center;justify-content:center;padding:20px}"
".card{background:#1a1d27;border:1px solid #2e3248;border-radius:16px;"
"padding:28px 26px;max-width:400px;width:100%;box-shadow:0 8px 32px rgba(0,0,0,.4)}"
".brand{display:flex;flex-direction:column;align-items:center;margin-bottom:8px}"
".sub{font-size:12px;color:#64748b;text-align:center;margin-bottom:18px;margin-top:14px}"
"label{font-size:12px;color:#94a3b8;text-transform:uppercase;letter-spacing:.05em;"
"display:block;margin-bottom:6px;margin-top:16px}"
"input{width:100%;background:#22263a;border:1px solid #2e3248;border-radius:8px;"
"padding:12px 14px;color:#e2e8f0;font-size:15px}"
"input:focus{outline:none;border-color:#c0392b}"
"button{width:100%;border:none;border-radius:8px;padding:13px;font-size:14px;"
"font-weight:600;cursor:pointer}"
".btn-primary{background:#c0392b;color:#fff;margin-top:22px}"
".btn-primary:hover{opacity:.9}"
".btn-scan{background:#22263a;color:#e2e8f0;border:1px solid #2e3248;"
"display:flex;align-items:center;justify-content:center;gap:8px}"
".btn-scan:hover{border-color:#c0392b}"
".btn-scan.busy{opacity:.6;cursor:not-allowed}"
"#netlist{margin-top:10px;max-height:180px;overflow-y:auto;"
"border-radius:8px;display:none}"
".net-item{background:#22263a;border:1px solid #2e3248;border-radius:8px;"
"padding:11px 14px;margin-bottom:6px;display:flex;align-items:center;"
"justify-content:space-between;cursor:pointer;font-size:14px}"
".net-item:hover{border-color:#c0392b}"
".net-item .bars{font-size:11px;color:#64748b}"
".foot{font-size:11px;color:#64748b;text-align:center;margin-top:18px}"
"</style></head><body>"
"<div class='card'>"
"<div class='brand'>"
"<img src='/logo.png' alt='Aditya Electronic Solutions' style='width:100%;max-width:320px;height:auto'>"
"</div>"
"<div class='sub'>Connect your device to WiFi</div>"
"<button type='button' class='btn-scan' id='scanBtn' onclick='doScan()'>"
"&#128246; Scan for Networks</button>"
"<div id='netlist'></div>"
"<form method='POST' action='/save' id='wifiForm'>"
"<label>WiFi Network Name (SSID)</label>"
"<input type='text' name='ssid' id='ssidField' placeholder='Your WiFi name' required maxlength='63'>"
"<label>WiFi Password</label>"
"<input type='password' name='pass' id='passField' placeholder='Your WiFi password' maxlength='63'>"
"<button type='submit' class='btn-primary'>Connect Device</button>"
"</form>"
"<div class='foot'>Aditya Electronics Gateway &middot; Setup Mode</div>"
"</div>"
"<script>"
"async function doScan(){"
"const btn=document.getElementById('scanBtn');"
"const list=document.getElementById('netlist');"
"btn.classList.add('busy');btn.textContent='Scanning...';"
"try{"
"const r=await fetch('/scan');"
"const d=await r.json();"
"list.innerHTML='';"
"if(d.networks&&d.networks.length){"
"d.networks.forEach(function(n){"
"const bars=n.rssi>-55?'||||':n.rssi>-70?'|||':n.rssi>-85?'||':'|';"
"const el=document.createElement('div');"
"el.className='net-item';"
"el.innerHTML='<span>'+n.ssid+(n.secure?' &#128274;':'')+'</span><span class=\"bars\">'+bars+'</span>';"
"el.onclick=function(){"
"document.getElementById('ssidField').value=n.ssid;"
"document.getElementById('passField').focus();"
"};"
"list.appendChild(el);"
"});"
"list.style.display='block';"
"}else{"
"list.innerHTML='<div class=\"net-item\">No networks found</div>';"
"list.style.display='block';"
"}"
"}catch(e){"
"list.innerHTML='<div class=\"net-item\">Scan failed \\u2014 try again</div>';"
"list.style.display='block';"
"}"
"btn.classList.remove('busy');btn.innerHTML='&#128246; Scan for Networks';"
"}"
"</script>"
"</body></html>";

static const char *PAGE_SUCCESS =
"<!DOCTYPE html><html><head><meta charset='UTF-8'>"
"<meta name='viewport' content='width=device-width, initial-scale=1.0'>"
"<title>Aditya Electronic Solutions</title>"
"<style>"
"body{background:#0f1117;color:#e2e8f0;font-family:'Segoe UI',system-ui,sans-serif;"
"min-height:100vh;display:flex;align-items:center;justify-content:center;text-align:center}"
".card{background:#1a1d27;border:1px solid #2e3248;border-radius:16px;padding:32px;max-width:340px}"
".ok{width:56px;height:56px;background:#16a34a;border-radius:50%;"
"display:flex;align-items:center;justify-content:center;font-size:28px;margin:0 auto 16px;color:#fff}"
"h1{font-size:18px;margin-bottom:8px}"
"p{font-size:13px;color:#94a3b8}"
"</style></head><body>"
"<div class='card'><div class='ok'>&#10003;</div>"
"<h1>Device Connecting</h1>"
"<p>Aditya Electronics Gateway is now connecting to your WiFi network. "
"This page will stop responding shortly as the device switches networks.</p>"
"</div></body></html>";

/* ─────────────────────────────────────────────────────────────
 *  Mandatory IP-configuration step — runs BEFORE the WiFi
 *  credentials page, at the default IP, on first ever setup (or
 *  after a factory reset). Submitting here saves the portal IP
 *  and reboots; the WiFi credentials page then opens at whatever
 *  IP was just chosen, not before.
 * ───────────────────────────────────────────────────────────── */
static const char *PAGE_IP_FORM =
"<!DOCTYPE html><html><head><meta charset='UTF-8'>"
"<meta name='viewport' content='width=device-width, initial-scale=1.0'>"
"<title>Aditya Electronic Solutions</title>"
"<style>"
"*{box-sizing:border-box;margin:0;padding:0}"
"body{background:#0f1117;color:#e2e8f0;font-family:'Segoe UI',system-ui,sans-serif;"
"min-height:100vh;display:flex;align-items:center;justify-content:center;padding:20px}"
".card{background:#1a1d27;border:1px solid #2e3248;border-radius:16px;"
"padding:28px 26px;max-width:400px;width:100%;box-shadow:0 8px 32px rgba(0,0,0,.4)}"
".brand{display:flex;flex-direction:column;align-items:center;margin-bottom:8px}"
".sub{font-size:12px;color:#64748b;text-align:center;margin-bottom:18px;margin-top:14px}"
"label{font-size:12px;color:#94a3b8;text-transform:uppercase;letter-spacing:.05em;"
"display:block;margin-bottom:6px;margin-top:16px}"
"input{width:100%;background:#22263a;border:1px solid #2e3248;border-radius:8px;"
"padding:12px 14px;color:#e2e8f0;font-size:15px}"
"input:focus{outline:none;border-color:#c0392b}"
"button{width:100%;border:none;border-radius:8px;padding:13px;font-size:14px;"
"font-weight:600;cursor:pointer}"
".btn-primary{background:#c0392b;color:#fff;margin-top:22px}"
".btn-primary:hover{opacity:.9}"
".step{font-size:11px;color:#c0392b;text-align:center;text-transform:uppercase;"
"letter-spacing:.08em;margin-bottom:4px;font-weight:600}"
"</style></head><body>"
"<div class='card'>"
"<div class='step'>Step 1 of 2</div>"
"<div class='brand'>"
"<img src='/logo.png' alt='Aditya Electronic Solutions' style='width:100%;max-width:320px;height:auto'>"
"</div>"
"<div class='sub'>Set the address this setup page will use. Leave blank for the "
"default (192.168.4.1). The device will restart after this step, then reconnect "
"here at the address you choose so you can enter your WiFi details.</div>"
"<form method='POST' action='/save_ip'>"
"<label>Setup Page IP</label>"
"<input type='text' name='ap_ip' placeholder='192.168.4.1' maxlength='15'>"
"<button type='submit' class='btn-primary'>Save &amp; Restart</button>"
"</form>"
"<div class='foot' style='font-size:11px;color:#64748b;text-align:center;margin-top:18px'>"
"Aditya Electronics Gateway &middot; Setup Mode</div>"
"</div></body></html>";

static const char *PAGE_IP_SUCCESS =
"<!DOCTYPE html><html><head><meta charset='UTF-8'>"
"<meta name='viewport' content='width=device-width, initial-scale=1.0'>"
"<title>Aditya Electronic Solutions</title>"
"<style>"
"body{background:#0f1117;color:#e2e8f0;font-family:'Segoe UI',system-ui,sans-serif;"
"min-height:100vh;display:flex;align-items:center;justify-content:center;text-align:center;padding:20px}"
".card{background:#1a1d27;border:1px solid #2e3248;border-radius:16px;padding:32px;max-width:340px}"
".ok{width:56px;height:56px;background:#16a34a;border-radius:50%;"
"display:flex;align-items:center;justify-content:center;font-size:28px;margin:0 auto 16px;color:#fff}"
"h1{font-size:18px;margin-bottom:8px}"
"p{font-size:13px;color:#94a3b8}"
"</style></head><body>"
"<div class='card'><div class='ok'>&#10003;</div>"
"<h1>Setup IP Saved</h1>"
"<p>The device is restarting. Reconnect to \"" WIFI_AP_SSID "\" and open the new "
"address to enter your WiFi network details.</p>"
"</div></body></html>";

static void url_decode(char *dst, const char *src, size_t dst_sz)
{
    size_t di = 0;
    while (*src && di < dst_sz - 1) {
        if (*src == '%' && src[1] && src[2]) {
            char hex[3] = { src[1], src[2], 0 };
            dst[di++] = (char)strtol(hex, NULL, 16);
            src += 3;
        } else if (*src == '+') {
            dst[di++] = ' ';
            src++;
        } else {
            dst[di++] = *src++;
        }
    }
    dst[di] = '\0';
}

static bool extract_field(const char *body, const char *name,
                          char *out, size_t out_sz)
{
    char key[32];
    snprintf(key, sizeof(key), "%s=", name);
    const char *p = strstr(body, key);
    if (!p) return false;
    p += strlen(key);

    char raw[128] = {0};
    size_t i = 0;
    while (*p && *p != '&' && i < sizeof(raw) - 1) raw[i++] = *p++;
    raw[i] = '\0';

    url_decode(out, raw, out_sz);
    return true;
}

static bool is_valid_ipv4(const char *s)
{
    unsigned a, b, c, d;
    char extra;
    /* sscanf with a trailing %c catches trailing garbage like "1.2.3.4x" */
    if (sscanf(s, "%u.%u.%u.%u%c", &a, &b, &c, &d, &extra) != 4) return false;
    return (a <= 255u && b <= 255u && c <= 255u && d <= 255u);
}

static void json_escape(char *dst, const char *src, size_t dst_sz)
{
    size_t di = 0;
    while (*src && di < dst_sz - 2) {
        if (*src == '"' || *src == '\\') dst[di++] = '\\';
        dst[di++] = *src++;
    }
    dst[di] = '\0';
}

static esp_err_t root_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, PAGE_FORM, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* ── GET /logo.png — serve embedded brand logo ────────────────── */
static esp_err_t logo_handler(httpd_req_t *req)
{
    size_t len = (size_t)(logo_png_end - logo_png_start);
    httpd_resp_set_type(req, "image/png");
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=3600");
    httpd_resp_send(req, (const char *)logo_png_start, (ssize_t)len);
    return ESP_OK;
}

/* ─────────────────────────────────────────────────────────────
 *  GET /scan — scan for nearby WiFi networks, return JSON
 *  Response: {"networks":[{"ssid":"Home","rssi":-45,"secure":true}]}
 * ───────────────────────────────────────────────────────────── */
static esp_err_t scan_handler(httpd_req_t *req)
{
    wifi_scan_config_t scan_cfg = {
        .ssid        = NULL,
        .bssid       = NULL,
        .channel     = 0,
        .show_hidden = false,
    };

    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Scan failed: %s", esp_err_to_name(err));
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"networks\":[]}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    uint16_t num = MAX_SCAN_AP;
    wifi_ap_record_t records[MAX_SCAN_AP];
    esp_wifi_scan_get_ap_records(&num, records);

    char buf[2048];
    int pos = snprintf(buf, sizeof(buf), "{\"networks\":[");

    char seen[MAX_SCAN_AP][33];
    memset(seen, 0, sizeof(seen));
    int seen_count = 0;

    for (int i = 0; i < num && pos < (int)sizeof(buf) - 100; i++) {
        if (records[i].ssid[0] == '\0') continue;

        bool dup = false;
        for (int j = 0; j < seen_count; j++) {
            if (strcmp(seen[j], (char *)records[i].ssid) == 0) {
                dup = true;
                break;
            }
        }
        if (dup) continue;
        if (seen_count < MAX_SCAN_AP) {
            strncpy(seen[seen_count], (char *)records[i].ssid, 32);
            seen_count++;
        }

        char esc_ssid[80];
        json_escape(esc_ssid, (char *)records[i].ssid, sizeof(esc_ssid));

        bool secure = (records[i].authmode != WIFI_AUTH_OPEN);

        pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos,
                        "%s{\"ssid\":\"%s\",\"rssi\":%d,\"secure\":%s}",
                        (seen_count > 1) ? "," : "",
                        esc_ssid, records[i].rssi,
                        secure ? "true" : "false");
    }

    pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, "]}");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, pos);
    return ESP_OK;
}

static esp_err_t save_handler(httpd_req_t *req)
{
    char body[256] = {0};
    int to_read = ((int)req->content_len < (int)(sizeof(body) - 1))
                  ? (int)req->content_len
                  : (int)(sizeof(body) - 1);
    int recv = httpd_req_recv(req, body, to_read);
    if (recv <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    body[recv] = '\0';

    char ssid[64] = {0};
    char pass[64] = {0};

    if (!extract_field(body, "ssid", ssid, sizeof(ssid)) ||
        strlen(ssid) == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing SSID");
        return ESP_FAIL;
    }
    extract_field(body, "pass", pass, sizeof(pass));

    ESP_LOGI(TAG, "Received SSID: %s", ssid);

    nvs_handle_t h;
    if (nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, WIFI_NVS_KEY_SSID, ssid);
        nvs_set_str(h, WIFI_NVS_KEY_PASS, pass);
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGI(TAG, "Credentials saved to NVS");
    }

    strncpy(s_ssid_out, ssid, sizeof(s_ssid_out) - 1);
    strncpy(s_pass_out, pass, sizeof(s_pass_out) - 1);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, PAGE_SUCCESS, HTTPD_RESP_USE_STRLEN);

    xEventGroupSetBits(s_eg, AP_DONE_BIT);
    return ESP_OK;
}

/* ═══════════════════════════════════════════════════════════════
   Mandatory IP-configuration step (Step 1 of 2)
   ═══════════════════════════════════════════════════════════════ */

bool wifi_provision_ip_is_configured(void)
{
    nvs_handle_t h;
    if (nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false;

    char ip[16] = {0};
    size_t l = sizeof(ip);
    esp_err_t err = nvs_get_str(h, WIFI_NVS_KEY_AP_IP, ip, &l);
    nvs_close(h);

    return (err == ESP_OK && is_valid_ipv4(ip));
}

void wifi_provision_get_ap_ip(char *out, size_t buf_sz)
{
    nvs_handle_t h;
    if (nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        char ip[16] = {0};
        size_t l = sizeof(ip);
        if (nvs_get_str(h, WIFI_NVS_KEY_AP_IP, ip, &l) == ESP_OK &&
            is_valid_ipv4(ip)) {
            nvs_close(h);
            strncpy(out, ip, buf_sz - 1);
            out[buf_sz - 1] = '\0';
            return;
        }
        nvs_close(h);
    }
    strncpy(out, WIFI_AP_DEFAULT_IP, buf_sz - 1);
    out[buf_sz - 1] = '\0';
}

static esp_err_t ip_root_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, PAGE_IP_FORM, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t save_ip_handler(httpd_req_t *req)
{
    char body[128] = {0};
    int to_read = ((int)req->content_len < (int)(sizeof(body) - 1))
                  ? (int)req->content_len
                  : (int)(sizeof(body) - 1);
    int recv = httpd_req_recv(req, body, to_read);
    if (recv <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    body[recv] = '\0';

    char ip[16] = {0};
    extract_field(body, "ap_ip", ip, sizeof(ip));

    /* Blank or invalid input -> explicitly save the default. Either
     * way, something valid gets saved so this step is marked done
     * and never repeats (until a factory reset erases it again). */
    const char *to_save = WIFI_AP_DEFAULT_IP;
    if (strlen(ip) > 0 && is_valid_ipv4(ip)) {
        to_save = ip;
    } else if (strlen(ip) > 0) {
        ESP_LOGW(TAG, "Invalid IP \"%s\" entered - using default %s instead",
                 ip, WIFI_AP_DEFAULT_IP);
    }

    nvs_handle_t h;
    if (nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, WIFI_NVS_KEY_AP_IP, to_save);
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGI(TAG, "Setup page IP saved: %s - restarting", to_save);
    }

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, PAGE_IP_SUCCESS, HTTPD_RESP_USE_STRLEN);

    xEventGroupSetBits(s_eg, AP_DONE_BIT);
    return ESP_OK;
}

/*
 * Runs the mandatory Step 1 portal at the DEFAULT IP. Blocks until
 * the user submits (or a timeout fallback saves the default and
 * proceeds), then ALWAYS reboots - this function does not return
 * on success, matching "configure then reboot" rather than
 * continuing in the same session. Only returns (without doing
 * anything) if an IP is already configured, so callers can call
 * this unconditionally before the WiFi-credentials step.
 */
void wifi_ap_ip_config_run(void)
{
    if (wifi_provision_ip_is_configured()) {
        return;   /* already done - nothing to do */
    }

    ESP_LOGI(TAG, "No setup-page IP configured yet - mandatory Step 1");

    s_eg = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t evloop_err = esp_event_loop_create_default();
    if (evloop_err != ESP_OK && evloop_err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(evloop_err);
    }

    s_ap_netif  = esp_netif_create_default_wifi_ap();
    s_sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t ap_cfg = {
        .ap = {
            .ssid_len       = 0,
            .channel        = WIFI_AP_CHANNEL,
            .max_connection = WIFI_AP_MAX_CONN,
            .authmode       = WIFI_AUTH_WPA2_PSK,
        },
    };
    strncpy((char *)ap_cfg.ap.ssid, WIFI_AP_SSID, sizeof(ap_cfg.ap.ssid) - 1);
    strncpy((char *)ap_cfg.ap.password, WIFI_AP_PASSWORD, sizeof(ap_cfg.ap.password) - 1);
    if (strlen(WIFI_AP_PASSWORD) == 0) ap_cfg.ap.authmode = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Step 1 portal: SSID=%s -> connect and open http://%s/",
             WIFI_AP_SSID, WIFI_AP_DEFAULT_IP);

    httpd_config_t http_cfg = HTTPD_DEFAULT_CONFIG();
    http_cfg.lru_purge_enable = true;
    http_cfg.max_uri_handlers = 3;
    ESP_ERROR_CHECK(httpd_start(&s_server, &http_cfg));

    static const httpd_uri_t routes[] = {
        { .uri = "/",         .method = HTTP_GET,  .handler = ip_root_handler },
        { .uri = "/logo.png", .method = HTTP_GET,  .handler = logo_handler    },
        { .uri = "/save_ip",  .method = HTTP_POST, .handler = save_ip_handler },
    };
    for (int i = 0; i < 3; i++)
        httpd_register_uri_handler(s_server, &routes[i]);

    EventBits_t bits = xEventGroupWaitBits(s_eg, AP_DONE_BIT,
                           pdFALSE, pdFALSE,
                           pdMS_TO_TICKS(WIFI_PROVISION_TIMEOUT_MS));

    if (!(bits & AP_DONE_BIT)) {
        /* Nobody submitted - save the default so this doesn't hang
         * forever on every boot, then proceed the same as a normal
         * submission would. */
        ESP_LOGW(TAG, "Step 1 timeout - saving default IP %s", WIFI_AP_DEFAULT_IP);
        nvs_handle_t h;
        if (nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_str(h, WIFI_NVS_KEY_AP_IP, WIFI_AP_DEFAULT_IP);
            nvs_commit(h);
            nvs_close(h);
        }
    }

    vEventGroupDelete(s_eg);
    wifi_ap_provision_stop();

    ESP_LOGW(TAG, "Restarting to apply setup-page IP...");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

bool wifi_ap_provision_run(char *ssid_out, char *pass_out, size_t buf_sz)
{
    s_eg = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t evloop_err = esp_event_loop_create_default();
    if (evloop_err != ESP_OK && evloop_err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(evloop_err);
    }

    s_ap_netif  = esp_netif_create_default_wifi_ap();
    s_sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t ap_cfg = {
        .ap = {
            .ssid_len       = 0,
            .channel        = WIFI_AP_CHANNEL,
            .max_connection = WIFI_AP_MAX_CONN,
            .authmode       = WIFI_AUTH_WPA2_PSK,
        },
    };
    strncpy((char *)ap_cfg.ap.ssid, WIFI_AP_SSID,
            sizeof(ap_cfg.ap.ssid) - 1);
    strncpy((char *)ap_cfg.ap.password, WIFI_AP_PASSWORD,
            sizeof(ap_cfg.ap.password) - 1);

    if (strlen(WIFI_AP_PASSWORD) == 0) {
        ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
    }

    /* AP+STA mode — AP broadcasts the portal, STA (unconnected)
     * allows esp_wifi_scan_start() to work for the /scan endpoint */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* Apply a previously-saved custom portal IP, if one exists.
     * MUST happen after esp_wifi_start(), not before - the AP
     * interface (and its DHCP server) don't actually exist until
     * the interface comes up here, so calling esp_netif_dhcps_stop()/
     * esp_netif_set_ip_info() any earlier has nothing to act on and
     * silently doesn't take effect (this was the bug behind the
     * portal only ever coming up at the default 192.168.4.1). */
    {
        char saved_ip[16] = {0};
        bool have_custom_ip = false;

        nvs_handle_t h;
        if (nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
            size_t l = sizeof(saved_ip);
            if (nvs_get_str(h, WIFI_NVS_KEY_AP_IP, saved_ip, &l) == ESP_OK &&
                is_valid_ipv4(saved_ip)) {
                have_custom_ip = true;
            }
            nvs_close(h);
        }

        if (have_custom_ip) {
            unsigned a, b, c, d;
            sscanf(saved_ip, "%u.%u.%u.%u", &a, &b, &c, &d);

            esp_netif_ip_info_t ip_info;
            IP4_ADDR(&ip_info.ip,      a, b, c, d);
            IP4_ADDR(&ip_info.gw,      a, b, c, d);
            IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);

            esp_err_t stop_err = esp_netif_dhcps_stop(s_ap_netif);
            esp_err_t set_err  = esp_netif_set_ip_info(s_ap_netif, &ip_info);
            esp_err_t start_err = esp_netif_dhcps_start(s_ap_netif);

            if (set_err == ESP_OK) {
                ESP_LOGI(TAG, "Applied saved custom portal IP: %s", saved_ip);
            } else {
                ESP_LOGW(TAG, "Failed to apply saved IP %s (dhcps_stop=%s set_ip=%s dhcps_start=%s) - using default",
                         saved_ip, esp_err_to_name(stop_err),
                         esp_err_to_name(set_err), esp_err_to_name(start_err));
            }
        } else {
            ESP_LOGI(TAG, "No custom portal IP saved — using default %s",
                     WIFI_AP_DEFAULT_IP);
        }
    }

    {
        esp_netif_ip_info_t actual_ip;
        esp_netif_get_ip_info(s_ap_netif, &actual_ip);
        ESP_LOGI(TAG, "AP started: SSID=%s  ->  connect and open http://" IPSTR "/",
                 WIFI_AP_SSID, IP2STR(&actual_ip.ip));
    }

    httpd_config_t http_cfg = HTTPD_DEFAULT_CONFIG();
    http_cfg.lru_purge_enable = true;
    http_cfg.max_uri_handlers = 5;
    http_cfg.stack_size       = 8192;
    ESP_ERROR_CHECK(httpd_start(&s_server, &http_cfg));

    static const httpd_uri_t routes[] = {
        { .uri = "/",         .method = HTTP_GET,  .handler = root_handler },
        { .uri = "/logo.png", .method = HTTP_GET,  .handler = logo_handler },
        { .uri = "/scan",     .method = HTTP_GET,  .handler = scan_handler },
        { .uri = "/save",     .method = HTTP_POST, .handler = save_handler },
    };
    for (int i = 0; i < 4; i++)
        httpd_register_uri_handler(s_server, &routes[i]);

    ESP_LOGI(TAG, "Waiting up to %d s for WiFi credentials...",
             WIFI_PROVISION_TIMEOUT_MS / 1000);

    EventBits_t bits = xEventGroupWaitBits(s_eg, AP_DONE_BIT,
                           pdFALSE, pdFALSE,
                           pdMS_TO_TICKS(WIFI_PROVISION_TIMEOUT_MS));

    bool ok = (bits & AP_DONE_BIT) != 0;
    if (ok) {
        strncpy(ssid_out, s_ssid_out, buf_sz - 1);
        strncpy(pass_out, s_pass_out, buf_sz - 1);
        ssid_out[buf_sz - 1] = pass_out[buf_sz - 1] = '\0';
        vTaskDelay(pdMS_TO_TICKS(500));
    } else {
        ESP_LOGW(TAG, "AP provisioning timeout - no credentials received");
    }

    vEventGroupDelete(s_eg);
    return ok;
}

void wifi_ap_provision_stop(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }

    esp_wifi_stop();
    esp_wifi_deinit();

    if (s_ap_netif) {
        esp_netif_destroy_default_wifi(s_ap_netif);
        s_ap_netif = NULL;
    }
    if (s_sta_netif) {
        esp_netif_destroy_default_wifi(s_sta_netif);
        s_sta_netif = NULL;
    }

    ESP_LOGI(TAG, "AP provisioning stopped");
}
