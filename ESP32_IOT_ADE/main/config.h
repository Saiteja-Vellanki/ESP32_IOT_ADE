/*
 * config.h  —  Project-wide configuration
 *
 * BUILD MODE:
 *   LOCAL_WEB_SERVER 1  →  ESP32 hosts dashboard at http://<IP>/
 *   LOCAL_WEB_SERVER 0  →  ESP32 POSTs to adityaelectronicsolutions.com
 */
#pragma once

/* ════════════════════════════════════════════════════════════════
   BUILD MODE
   ════════════════════════════════════════════════════════════════ */
#define LOCAL_WEB_SERVER        1       /* 1=local  0=remote server  */

/* ════════════════════════════════════════════════════════════════
   Wi-Fi
   ════════════════════════════════════════════════════════════════ */
#define WIFI_SSID               "Saiteja"
#define WIFI_PASSWORD           "saiteja@1234"
#define WIFI_MAX_RETRY          10

/* ════════════════════════════════════════════════════════════════
   UART  —  NUC029 TX → ESP32 GPIO34 (RX)
             ESP32  TX → NUC029 RX  GPIO35
   ════════════════════════════════════════════════════════════════ */
#define UART_PORT_NUM           UART_NUM_2
#define UART_BAUD_RATE          115200
#define UART_RX_PIN             16          
#define UART_TX_PIN             17        
#define UART_RX_BUF_SIZE        1024

/* ════════════════════════════════════════════════════════════════
   Protocol  (NUC029 → ESP32)
   [0xAA][B0:SW01-08][B1:SW09-16][B2:SW17-24][B3:SW25][0x55]
   ════════════════════════════════════════════════════════════════ */
#define PROTO_HEADER            0xAAu
#define PROTO_END               0x55u
#define PROTO_PACKET_LEN        6u
#define GPIO_COUNT              25

/* ════════════════════════════════════════════════════════════════
   Relay GPIOs  (ESP32 → ULN2003A → Relay coil)
   ════════════════════════════════════════════════════════════════ */
#define RELAY1_GPIO             25
#define RELAY2_GPIO             26
#define RELAY3_GPIO             27
#define RELAY4_GPIO             14

/* ════════════════════════════════════════════════════════════════
   Status LED  (WiFi signal indicator — user visible)
   ════════════════════════════════════════════════════════════════ */
#define STATUS_LED_GPIO         2
#define LED_SLOW_BLINK_MS       1000    /* good signal  */
#define LED_FAST_BLINK_MS       150     /* no signal    */
#define LED_LOWBAT_BLINK_MS     400     /* low battery  */

/* ════════════════════════════════════════════════════════════════
   Buzzer  (no signal → 5 beeps every 30 seconds)
   ════════════════════════════════════════════════════════════════ */
#define BUZZER_GPIO             4
#define BUZZER_NO_SIGNAL_BEEPS  5
#define BUZZER_BEEP_ON_MS       150
#define BUZZER_BEEP_OFF_MS      150
#define BUZZER_INTERVAL_MS      30000

/* ════════════════════════════════════════════════════════════════
   Relay timing (ms)
   RL1 : pulse ON 3s then OFF
   RL2 : toggle continuous / off-3s-then-on
   RL3 : ON then after 3s triggers RL4 pulse
   RL4 : ON 3s then OFF (triggered by RL3)
   ════════════════════════════════════════════════════════════════ */
#define RELAY_PULSE_MS          3000

/* ════════════════════════════════════════════════════════════════
   Remote server  (LOCAL_WEB_SERVER = 0)
   ════════════════════════════════════════════════════════════════ */
#define REMOTE_HOST             "www.adityaelectronicsolutions.com"
#define REMOTE_PORT             443
#define REMOTE_BASE             "/iot_pump/home_pump"
#define DEVICE_MODEL            "home_pump_v1"
#define DEVICE_SERIAL           "0001"
#define DEVICE_LOCATION         "site_a"
#define REMOTE_POLL_MS          500     /* how often to poll relay status */

/* ════════════════════════════════════════════════════════════════
   Tasks
   ════════════════════════════════════════════════════════════════ */
#define UART_TASK_STACK         4096
#define UART_TASK_PRIO          10
#define UART_TASK_CORE          1
#define RELAY_TASK_STACK        3072
#define RELAY_TASK_PRIO         8
#define LED_TASK_STACK          2048
#define LED_TASK_PRIO           3
#define REMOTE_TASK_STACK       8192
#define REMOTE_TASK_PRIO        5
