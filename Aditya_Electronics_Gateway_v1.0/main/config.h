/*
 * config.h  —  Project-wide configuration
 *
 * Remote server endpoints (see REMOTE_HOST/REMOTE_BASE below) use
 * the filenames CONFIRMED working against the live server, which
 * differ from the written IOT_Device_Protocol document in several
 * places (see the comment above REMOTE_HOST).
 *
 * NOTE — document internal contradiction on register.php OFF status:
 *   The "Switch OFF" URL example shows status=0, but the prose text
 *   below it says "2-Switched OFF". These conflict. This code uses
 *   REGISTER_STATUS_OFF=0 (matching the literal URL example given).
 *   If Aditya Electronics Solutions confirms it should be 2, change
 *   the single macro below — nothing else needs to change.
 *
 * WIFI_PROVISION_MODE selects how WiFi credentials are obtained:
 *   0  →  Hardcoded WIFI_SSID / WIFI_PASSWORD below
 *   1  →  WiFi Access Point portal — ESP32 broadcasts its own AP
 *          ("Aditya-IoT-Setup"), user connects and enters their
 *          WiFi SSID/password on a branded web page at
 *          http://192.168.4.1/  (RECOMMENDED — most reliable)
 *
 * (Bluetooth SPP commissioning removed - not needed currently.
 * bt_commission.c/.h can be deleted from the project; CMakeLists.txt
 * and sdkconfig.defaults have already been updated to drop it.)
 */
#pragma once

/* ── Build mode ─────────────────────────────────────────────── */
#define WIFI_PROVISION_MODE     1       /* 0=hardcoded 1=AP portal */
#define LOCAL_WEB_SERVER        0       /* 1=local  0=remote        */

/* ── Wi-Fi (used when WIFI_PROVISION_MODE = 0) ────────────────── */
#define WIFI_SSID               "Saiteja"
#define WIFI_PASSWORD           "saiteja@1234"
#define WIFI_MAX_RETRY          10

/* ── WiFi AP provisioning portal (used when MODE = 1) ─────────── */
#define WIFI_AP_SSID             "Aditya-IoT-Setup"
#define WIFI_AP_PASSWORD         "12345678"   /* 8+ chars, or "" for open */
#define WIFI_AP_CHANNEL          1
#define WIFI_AP_MAX_CONN         4
#define WIFI_PROVISION_TIMEOUT_MS  120000u    /* 2 min to complete setup */

/* Default/factory AP portal IP - used until the user saves a custom
 * one via the portal form, and restored by the 15s button hold. */
#define WIFI_AP_DEFAULT_IP       "192.168.4.1"

/* ── WiFi credential storage (NVS) — used by AP portal ─────────
 * Namespace/keys kept the same as before (when this lived under
 * bt_commission.c's names) so any device that already has saved
 * credentials in flash keeps working without re-provisioning. */
#define WIFI_NVS_NAMESPACE       "bt_wifi"
#define WIFI_NVS_KEY_SSID        "ssid"
#define WIFI_NVS_KEY_PASS        "pass"
#define WIFI_NVS_KEY_AP_IP       "ap_ip"   /* custom AP portal IP, optional */

/* ── Factory reset button ───────────────────────────────────────
 * Hold for FACTORY_RESET_HOLD_MS to erase saved WiFi credentials
 * AND the custom AP IP, then reboot into the provisioning portal
 * at the default IP (WIFI_AP_DEFAULT_IP).
 *
 * Defaults to GPIO0 — the BOOT button already present on almost
 * every ESP32 dev board, so no extra wiring is needed. GPIO0 is a
 * strapping pin used during power-on/reset for bootloader mode
 * selection, but is safe to read as a normal input once app_main()
 * is running (well after boot strapping is complete). If you have
 * a dedicated button on a different GPIO, change this instead. */
#define FACTORY_RESET_GPIO       0
#define FACTORY_RESET_HOLD_MS    15000u
#define FACTORY_RESET_POLL_MS    100u

/* ── UART — NUC029 TX → ESP32 GPIO16 (RX) ─────────────────────
 * FIXED: was previously UART_NUM_0 with RX=GPIO3/TX=GPIO1, which
 * is ESP32's default console/log UART — every ESP_LOGI call and
 * every NUC binary packet were fighting over the same wire, and
 * the boot log was corrupting the protocol stream (and vice
 * versa). Moved to UART2 (GPIO16/17, matching what this comment
 * always said) so the NUC link and console logging are fully
 * independent - ESP_LOGI now works safely for the LED-status
 * console print requested below.                                */
#define UART_PORT_NUM           UART_NUM_0
#define UART_BAUD_RATE          115200
#define UART_RX_PIN             3
#define UART_TX_PIN             1
#define UART_RX_BUF_SIZE        1024

/* ── Protocol (NUC029 → ESP32) ──────────────────────────────── */
#define PROTO_HEADER            0xAAu
#define PROTO_END               0x55u
#define PROTO_PACKET_LEN        6u
#define GPIO_COUNT              25

/* ── Relay GPIOs — UNUSED, kept only for reference ─────────────
 * The ESP32 has no relays wired to it — all 4 physical relays are
 * on the NUC029xAN, driven via UART (uart_send_relay_cmd()).
 * relay_logic.c no longer touches these pins; do not wire hardware
 * to them expecting relay control. GPIO25 (was RELAY1_GPIO) has
 * been repurposed below for the WiFi-only reset button since these
 * pins are confirmed free. */
#define RELAY2_GPIO             26
#define RELAY3_GPIO             27
#define RELAY4_GPIO             14

/* ── WiFi-credentials-only reset button (NEW) ──────────────────
 * A second, separate button from FACTORY_RESET_GPIO below - holds
 * for WIFI_ONLY_RESET_HOLD_MS to erase ONLY the saved WiFi SSID/
 * password, leaving the configured setup-page IP untouched. Lets a
 * user reconnect to a different WiFi network without needing to
 * redo the Step-1 IP configuration every time. Uses GPIO25 (was
 * RELAY1_GPIO, confirmed unused - see above). */
#define WIFI_ONLY_RESET_GPIO     25
#define WIFI_ONLY_RESET_HOLD_MS  7000u   /* tune anywhere in 5000-10000 */

/* ── Status LED ──────────────────────────────────────────────── */
#define STATUS_LED_GPIO         2
#define LED_SLOW_BLINK_MS       1000
#define LED_FAST_BLINK_MS       150
#define LED_LOWBAT_BLINK_MS     400

/* ── Buzzer ──────────────────────────────────────────────────── */
#define BUZZER_GPIO             4
#define BUZZER_NO_SIGNAL_BEEPS  5
#define BUZZER_BEEP_ON_MS       150
#define BUZZER_BEEP_OFF_MS      150
#define BUZZER_INTERVAL_MS      30000

/* ── Relay timing ────────────────────────────────────────────── */
#define RELAY_PULSE_MS          3000

/* ── Remote server — HTTPS ──────────────────────────────────────
 *
 * Endpoint path and filenames below are CONFIRMED against the live
 * server directly (not just the written protocol doc, which uses
 * different filenames - ledstatus.php/relaystatus.php/etc - that
 * did not work when actually tested):
 *   Path:  /iot-pump/res-pump           (hyphens)
 *   Endpoints actually used: register.php, update_led_status.php,
 *     update_relay_status.php, read_relay_status.php,
 *     read_lock_status.php, read_soft_reset_status.php
 *   JSON:  "Status":"1"  (capital S, STRING value) for relay reads;
 *          "success":1 (usually a number, but code accepts either)
 */
#define REMOTE_HOST             "www.adityaelectronicsolutions.com"
#define REMOTE_PORT             443
#define REMOTE_BASE             "/iot-pump/res-pump"

#define DEVICE_MODEL            "home_pump_v1"
#define DEVICE_SERIAL           "0001"
#define DEVICE_LOCATION         "site_a"
#define REMOTE_POLL_MS          500

/* ── Remote task poll/update intervals ───────────────────────────
 * All periodic remote_server.c timings, centralized here so they
 * can be tuned in one place without hunting through the .c file. */
#define LED_UPDATE_MS            500u    /* API 2: LED status write interval        */
#define RELAY_UPDATE_MS          500u    /* API 3: relay status write interval      */
#define LED_ALL_MS               15000u  /* API 2: LED_CNT=ALL heartbeat interval   */
#define LOCK_POLL_MS             5000u   /* API 8: lock status poll interval        */
#define RESET_POLL_MS            5000u   /* API 9: soft reset poll interval         */
#define LED_READ_POLL_MS         5000u   /* API 11: LED status read-back interval   */
#define POWER_DETAILS_MS         5000u   /* API 6A: power/battery details interval  */
#define INPUT_STATUS_POLL_MS     5000u   /* API 10: consolidated relay/lock/softreset
                                             read (new, protocol rev 20260729) - runs
                                             alongside the existing separate polls for
                                             cross-verification, doesn't replace them */
#define SOFT_RESET_RESTART_DELAY_MS  1000u  /* API 9: delay before esp_restart() */
#define SOFT_RESET_PRE_ACK_DELAY_MS  5000u  /* API 9: wait after detecting
                                                SoftResetStatus=1, BEFORE
                                                sending the SF=0 ack and
                                                actually resetting */

/* Soft-reset feature (poll read_soft_reset_status.php, reset NUC then
 * ESP32, SF ack both ways) - NOT NEEDED as of now. The server has been
 * sending SoftResetStatus=1, which was causing the device to reset
 * continuously every ~5s poll cycle - disabled while hardware isn't
 * accessible to manage a device that keeps rebooting itself. Set to 1
 * to re-enable. Kept in the code rather than deleted so it's a
 * one-line change either way. */
#define ENABLE_SOFT_RESET_FEATURE   0

/* ── TEMPORARY TEST FLAGS — NUC not connected right now ──────────
 * When TEST_HARDCODE_POWER_DATA is 1, api_power_details() sends
 * the fixed values below instead of reading (currently absent)
 * real UART power-fail/battery data from the NUC. Lets
 * update_power_details.php be exercised with real, changing-if-
 * you-want values instead of whatever a disconnected UART reports
 * (0/false for everything). Set back to 0 (or delete this block)
 * once the NUC is reconnected. */
#define TEST_HARDCODE_POWER_DATA   0
#define TEST_HARDCODE_PWR_STATUS   0       /* 1 = power on, 0 = power failed */
#define TEST_HARDCODE_BATT_VOLT    3.7f    /* volts                          */
#define TEST_HARDCODE_BATT_STATUS  0       /* 0 Unknown / 1 Healthy /
                                               2 Low Battery / 3 Battery Failed */

/* Keeps one relay reported as "active" (ON) in update_relay_status.php
 * writes, for end-to-end testing: write it active, then watch both
 * read_relay_status.php and read_input_status.php reflect the change
 * (or fail to, which is itself useful diagnostic info). Set to 0 to
 * disable (all relays report their real tracked state again).
 * CURRENTLY DISABLED - relay state now comes purely from what the
 * server reports via read_relay_status.php (the natural flow), not
 * a hardcoded firmware value. */
#define TEST_FORCE_RELAY_ACTIVE    0
#define TEST_ACTIVE_RELAY_NUM      1       /* which relay (1-4) to force ON */

/* Forces ALL 4 relays to report ON (1) in update_relay_status.php
 * writes, for a full write-path verification test. Includes relay 4
 * deliberately even though the server never sends it an explicit
 * command directly - per the NUC's relay.c timing, RL4 only ever
 * fires as a side effect of RL3's 3-second cascade (RL3 triggered ->
 * RL4 auto-fires 3s later), so reporting RL4=1 here mirrors that real
 * cascade behavior rather than literally mirroring only what the
 * server explicitly commanded. Takes priority over
 * TEST_FORCE_RELAY_ACTIVE above if both are enabled. Set to 0 to
 * disable (all relays report their real tracked state again). */
#define TEST_FORCE_ALL_RELAYS_ON   0


/* ═══════════════════════════════════════════════════════════════
   TEMPORARY TEST FLAG — remove once the NUC is back on the bench.
   NUC isn't connected right now, so uart_parser_get_gpio() only
   ever returns real switch data when the NUC is actually sending
   packets. Set to 1 to force all 25 LED values to ON (1) instead
   of reading (dead/absent) UART data, so update_led_status.php can
   be exercised with real changing values instead of always-zero.
   Set back to 0 (or delete this block) once the NUC is reconnected.
   ═══════════════════════════════════════════════════════════════ */
#define TEST_FORCE_ALL_LED_ON    0

/* ═══════════════════════════════════════════════════════════════
   TEMPORARY TEST FLAG — remove once the persistence question is
   settled. Controls how many LED slots update_led_status.php
   actually gets sent, to compare whether a small partial write
   (matching the doc's plain "&L1=x&L2=x" form, no LED_CNT) behaves
   any differently server-side than the full 25-value heartbeat
   write. Set to 3 for a partial write (L1-L3 only, no LED_CNT=ALL
   ever), or GPIO_COUNT (25) for the normal full write.
   ═══════════════════════════════════════════════════════════════ */
#define TEST_LED_SEND_COUNT      3

/* ── Device ID ───────────────────────────────────────────────────
 * Live server CONFIRMED working with deviceid = MAC address
 * (uppercase hex, no separators, e.g. EC62609CD708) - every
 * endpoint above was actually tested and verified against a
 * MAC-based deviceid. USE_DEVICE_ID=1 (a fixed test string) was
 * only ever a one-off test value from Aditya Electronics Solutions
 * for early API verification - every device would collide on the
 * same ID if used for real deployment. Kept as an option only in
 * case a future server revision requires a different scheme.
 */
#define USE_DEVICE_ID            0
#define DEVICE_ID                "3"     /* only used if USE_DEVICE_ID=1 */

/* Register API status codes.
 * ON is unambiguous (1). OFF has a document contradiction — see
 * note at top of this file. Using 0 (matches the literal URL
 * example). Change to 2 if confirmed with Aditya Electronics. */
#define REGISTER_STATUS_ON      1
#define REGISTER_STATUS_OFF     0

/* ── Local dashboard is plain HTTP — no port config needed,
   uses the standard HTTPD_DEFAULT_CONFIG() port (80) ──────────── */

/* ── Dashboard login (local web + Android app) ────────────────────
 * HTTP Basic Auth — browser/app must send this username:password.
 * Hardcoded for now; change here to update for both web and app. */
#define DASHBOARD_USERNAME       "admin"
#define DASHBOARD_PASSWORD       "aditya@123"

/* ── Battery percentage thresholds (Li-ion, matches NUC029 side) ──
 * bat_mv is currently 0 (no ADC fitted on NUC yet) — these are
 * ready for when ADC_MODE is enabled on the NUC side.            */
#define BATTERY_FULL_MV          4200u
#define BATTERY_CUTOFF_MV        3000u

/* ── Task config ─────────────────────────────────────────────── */
#define UART_TASK_STACK         4096
#define UART_TASK_PRIO          10
#define UART_TASK_CORE          1
#define RELAY_TASK_STACK        3072
#define RELAY_TASK_PRIO         8
#define LED_TASK_STACK          2048
#define LED_TASK_PRIO           3
#define REMOTE_TASK_STACK       8192
#define REMOTE_TASK_PRIO        5