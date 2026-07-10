/*
 * config.h  —  Project-wide configuration
 *
 * BT_COMMISSIONING  1  →  WiFi credentials via Bluetooth (Android app)
 * BT_COMMISSIONING  0  →  Hardcoded WIFI_SSID / WIFI_PASSWORD
 *
 * LOCAL_WEB_SERVER  1  →  ESP32 hosts dashboard at http://<IP>/
 * LOCAL_WEB_SERVER  0  →  ESP32 POSTs to adityaelectronicsolutions.com
 */
#pragma once

/* ── Build mode ─────────────────────────────────────────────── */
#define BT_COMMISSIONING        1       /* 1=BT  0=hardcoded        */
#define LOCAL_WEB_SERVER        1       /* 1=local  0=remote        */

/* ── Wi-Fi (used when BT_COMMISSIONING = 0) ─────────────────── */
#define WIFI_SSID               "Saiteja"
#define WIFI_PASSWORD           "saiteja@1234"
#define WIFI_MAX_RETRY          10

/* ── Bluetooth commissioning ─────────────────────────────────── */
#define BT_DEVICE_NAME          "ADE_IoT_GW"
#define BT_AUTH_PIN             "123456"        /* app must send this first */
#define BT_NVS_NAMESPACE        "bt_wifi"
#define BT_NVS_KEY_SSID         "ssid"
#define BT_NVS_KEY_PASS         "pass"
#define BT_COMMISSION_TIMEOUT_MS  60000u        /* 60 s wait for app        */

/* ── UART — NUC029 TX → ESP32 GPIO16 (RX) ───────────────────── */
#define UART_PORT_NUM           UART_NUM_2
#define UART_BAUD_RATE          115200
#define UART_RX_PIN             16
#define UART_TX_PIN             17
#define UART_RX_BUF_SIZE        1024

/* ── Protocol (NUC029 → ESP32) ──────────────────────────────── */
#define PROTO_HEADER            0xAAu
#define PROTO_END               0x55u
#define PROTO_PACKET_LEN        6u
#define GPIO_COUNT              25

/* ── Relay GPIOs ─────────────────────────────────────────────── */
#define RELAY1_GPIO             25
#define RELAY2_GPIO             26
#define RELAY3_GPIO             27
#define RELAY4_GPIO             14

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

/* ── Remote server ───────────────────────────────────────────── */
#define REMOTE_HOST             "www.adityaelectronicsolutions.com"
#define REMOTE_PORT             80
#define REMOTE_BASE             "/iot_pump/home_pump"
#define DEVICE_MODEL            "home_pump_v1"
#define DEVICE_SERIAL           "0001"
#define DEVICE_LOCATION         "site_a"
#define REMOTE_POLL_MS          500

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
#define BT_TASK_STACK           4096
#define BT_TASK_PRIO            6
