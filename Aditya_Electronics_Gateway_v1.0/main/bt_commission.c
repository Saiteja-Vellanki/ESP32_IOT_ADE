#include "bt_commission.h"
#include "config.h"

#ifdef CONFIG_BT_ENABLED

#include "nvs_flash.h"
#include "nvs.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_bt_device.h"
#include "esp_spp_api.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "bt";

#define BT_DONE_BIT  BIT0
#define MAX_PIN_FAILS 3

static EventGroupHandle_t s_eg;
static uint32_t  s_spp_handle    = 0;
static bool      s_connected     = false;
static bool      s_authed        = false;
static uint8_t   s_pin_fails     = 0;
static char      s_staged_ssid[64] = {0};
static char      s_staged_pass[64] = {0};
static bool      s_has_ssid      = false;
static char      s_final_ssid[64] = {0};
static char      s_final_pass[64] = {0};
static bool      s_committed     = false;

/* ── NVS ────────────────────────────────────────────────────── */
static void nvs_save(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    if (nvs_open(BT_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, BT_NVS_KEY_SSID, ssid);
    nvs_set_str(h, BT_NVS_KEY_PASS, pass);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "NVS saved SSID=%s", ssid);
}

bool bt_commission_load_nvs(char *ssid_out, char *pass_out, size_t sz)
{
    nvs_handle_t h;
    if (nvs_open(BT_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false;
    size_t l = sz;
    bool ok = false;
    if (nvs_get_str(h, BT_NVS_KEY_SSID, ssid_out, &l) == ESP_OK
        && strlen(ssid_out) > 0) {
        l = sz;
        if (nvs_get_str(h, BT_NVS_KEY_PASS, pass_out, &l) == ESP_OK)
            ok = true;
    }
    nvs_close(h);
    if (ok) ESP_LOGI(TAG, "NVS loaded SSID=%s", ssid_out);
    return ok;
}

/* ── SPP send ────────────────────────────────────────────────── */
static void spp_send(const char *msg)
{
    if (!s_connected || !s_spp_handle) return;
    esp_spp_write(s_spp_handle, (uint16_t)strlen(msg), (uint8_t *)msg);
}

/* ── Command parser ──────────────────────────────────────────── */
static void process_cmd(char *line)
{
    /* strip \r\n */
    size_t len = strlen(line);
    while (len > 0 && (line[len-1]=='\r'||line[len-1]=='\n'))
        line[--len] = '\0';
    if (!len) return;

    ESP_LOGI(TAG, "BT RX: %s", line);

    if (strncmp(line, "PIN:", 4) == 0) {
        if (strcmp(line+4, BT_AUTH_PIN) == 0) {
            s_authed = true; s_pin_fails = 0;
            spp_send("AUTH:OK\n");
        } else {
            s_pin_fails++;
            if (s_pin_fails >= MAX_PIN_FAILS) {
                spp_send("AUTH:LOCKED\n");
                vTaskDelay(pdMS_TO_TICKS(300));
                esp_spp_disconnect(s_spp_handle);
            } else {
                char r[32];
                snprintf(r, sizeof(r), "AUTH:FAIL:%d\n",
                         MAX_PIN_FAILS - s_pin_fails);
                spp_send(r);
            }
        }
        return;
    }
    if (!s_authed) { spp_send("ERR:NOT_AUTHED\n"); return; }

    if (strncmp(line, "SSID:", 5) == 0) {
        strncpy(s_staged_ssid, line+5, sizeof(s_staged_ssid)-1);
        s_has_ssid = (strlen(s_staged_ssid) > 0);
        spp_send(s_has_ssid ? "SSID:OK\n" : "ERR:EMPTY\n");
    } else if (strncmp(line, "PASS:", 5) == 0) {
        strncpy(s_staged_pass, line+5, sizeof(s_staged_pass)-1);
        spp_send("PASS:OK\n");
    } else if (strcmp(line, "COMMIT") == 0) {
        if (!s_has_ssid) { spp_send("COMMIT:FAIL:NO_SSID\n"); return; }
        nvs_save(s_staged_ssid, s_staged_pass);
        strncpy(s_final_ssid, s_staged_ssid, sizeof(s_final_ssid)-1);
        strncpy(s_final_pass, s_staged_pass, sizeof(s_final_pass)-1);
        s_committed = true;
        spp_send("COMMIT:OK\n");
        xEventGroupSetBits(s_eg, BT_DONE_BIT);
    } else if (strcmp(line, "GET:SSID") == 0) {
        char stored[64]={0}, p[64]={0}, r[80];
        if (bt_commission_load_nvs(stored, p, sizeof(stored)))
            snprintf(r, sizeof(r), "SSID:%s\n", stored);
        else
            snprintf(r, sizeof(r), "SSID:NONE\n");
        spp_send(r);
    } else if (strcmp(line, "CLEAR") == 0) {
        nvs_handle_t h;
        if (nvs_open(BT_NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
            nvs_erase_all(h); nvs_commit(h); nvs_close(h);
        }
        spp_send("CLEAR:OK\n");
    } else {
        spp_send("ERR:UNKNOWN\n");
    }
}

/* ── Line buffer ─────────────────────────────────────────────── */
static char s_lbuf[128];
static int  s_llen = 0;

static void push_data(const uint8_t *d, uint16_t len)
{
    for (uint16_t i = 0; i < len; i++) {
        char c = (char)d[i];
        if (c == '\n') {
            s_lbuf[s_llen] = '\0';
            process_cmd(s_lbuf);
            s_llen = 0;
        } else if (c != '\r' && s_llen < (int)(sizeof(s_lbuf)-1)) {
            s_lbuf[s_llen++] = c;
        }
    }
}

/* ── SPP callback ────────────────────────────────────────────── */
static void spp_cb(esp_spp_cb_event_t ev, esp_spp_cb_param_t *p)
{
    switch (ev) {
    case ESP_SPP_INIT_EVT:
        esp_spp_start_srv(ESP_SPP_SEC_AUTHENTICATE,
                          ESP_SPP_ROLE_SLAVE, 0, BT_DEVICE_NAME);
        break;
    case ESP_SPP_SRV_OPEN_EVT:
        s_spp_handle = p->srv_open.handle;
        s_connected  = true;
        s_authed     = false;
        s_pin_fails  = 0;
        s_has_ssid   = false;
        s_llen       = 0;
        ESP_LOGI(TAG, "BT connected");
        spp_send("HELLO:Aditya-IoT\nPIN:REQUIRED\n");
        break;
    case ESP_SPP_CLOSE_EVT:
        s_connected = false; s_authed = false; s_spp_handle = 0;
        ESP_LOGI(TAG, "BT disconnected");
        break;
    case ESP_SPP_DATA_IND_EVT:
        push_data(p->data_ind.data, p->data_ind.len);
        break;
    default: break;
    }
}

static void gap_cb(esp_bt_gap_cb_event_t ev, esp_bt_gap_cb_param_t *p)
{
    if (ev == ESP_BT_GAP_AUTH_CMPL_EVT)
        ESP_LOGI(TAG, "BT pair %s",
                 p->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS ? "OK":"FAIL");
}

/* ── Public API ──────────────────────────────────────────────── */
bool bt_commission_run(char *ssid_out, char *pass_out, size_t buf_sz)
{
    s_eg = xEventGroupCreate();

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT));
    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());
    ESP_ERROR_CHECK(esp_bt_gap_register_callback(gap_cb));
    ESP_ERROR_CHECK(esp_spp_register_callback(spp_cb));

    esp_spp_cfg_t spp_cfg = {
        .mode              = ESP_SPP_MODE_CB,
        .enable_l2cap_ertm = false,
        .tx_buffer_size    = 0,
    };
    ESP_ERROR_CHECK(esp_spp_enhanced_init(&spp_cfg));
    ESP_ERROR_CHECK(esp_bt_dev_set_device_name(BT_DEVICE_NAME));
    ESP_ERROR_CHECK(esp_bt_gap_set_scan_mode(
        ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE));

    esp_bt_pin_type_t pt = ESP_BT_PIN_TYPE_FIXED;
    esp_bt_pin_code_t pc;
    memcpy(pc, "1234", 4);
    esp_bt_gap_set_pin(pt, 4, pc);

    ESP_LOGI(TAG, "BT ready: %s  waiting %d s...",
             BT_DEVICE_NAME, BT_COMMISSION_TIMEOUT_MS/1000);

    EventBits_t bits = xEventGroupWaitBits(s_eg, BT_DONE_BIT,
                           pdFALSE, pdFALSE,
                           pdMS_TO_TICKS(BT_COMMISSION_TIMEOUT_MS));

    bool ok = (bits & BT_DONE_BIT) != 0;
    if (ok) {
        strncpy(ssid_out, s_final_ssid, buf_sz-1);
        strncpy(pass_out, s_final_pass, buf_sz-1);
        ssid_out[buf_sz-1] = pass_out[buf_sz-1] = '\0';
    }
    vEventGroupDelete(s_eg);
    return ok;
}

void bt_commission_stop(void)
{
    if (s_connected && s_spp_handle)
        esp_spp_disconnect(s_spp_handle);
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_spp_deinit();
    esp_bluedroid_disable();
    esp_bluedroid_deinit();
    esp_bt_controller_disable();
    esp_bt_controller_deinit();
    esp_bt_mem_release(ESP_BT_MODE_CLASSIC_BT);
    ESP_LOGI(TAG, "BT stopped — RAM freed");
}

#endif /* CONFIG_BT_ENABLED */

/* ── Stubs when BT stack is disabled in sdkconfig ───────────────
 * Prevents linker errors when BT_COMMISSIONING=1 but
 * CONFIG_BT_ENABLED=n (e.g. during initial build before sdkconfig
 * is generated from sdkconfig.defaults).
 * These are never called at runtime when BT_COMMISSIONING=0.      */
#ifndef CONFIG_BT_ENABLED
bool bt_commission_load_nvs(char *ssid_out, char *pass_out, size_t buf_sz)
{
    (void)ssid_out; (void)pass_out; (void)buf_sz;
    return false;
}
bool bt_commission_run(char *ssid_out, char *pass_out, size_t buf_sz)
{
    (void)ssid_out; (void)pass_out; (void)buf_sz;
    return false;
}
void bt_commission_stop(void) {}
#endif
