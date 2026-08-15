#pragma once
#include <stdint.h>
#include <stdbool.h>

/* ── Relay command packet: ESP32 → NUC029 ────────────────────── */
#define CMD_PROTO_HEADER        0xBBu
#define CMD_RELAY_TRIGGER       0x01u
#define CMD_PROTO_END           0x44u
#define CMD_PACKET_LEN          4u

/* ── Relay LOCK packet: ESP32 → NUC029  (NEW) ─────────────────
 * [0xBB][0x02][1=lock/0=unlock][0x44] — a distinct instruction
 * from a normal relay trigger: forces RL2 to a persistent OFF
 * (no timer, no auto-restore) until explicitly unlocked. See
 * the NUC-side system_config.h/relay.h for the full rationale. */
#define CMD_RELAY_LOCK          0x02u
#define RELAY_LOCK_ENGAGE       1u
#define RELAY_LOCK_RELEASE      0u
#define RELAY_LOCK_NUM          2u   /* which relay lock applies to (RL2) */

/* ── NUC reset packet: ESP32 → NUC029 ─────────────────────────
 * [0xBB][0x03][0x00][0x44] - tells the NUC to reset immediately.
 * Sent before the ESP32 resets itself on a server-requested soft
 * reset, so the NUC reboots first, not simultaneously. */
#define CMD_NUC_RESET            0x03u

/* ── Switch state packet: NUC029 → ESP32 ────────────────────── */
#define PROTO_HEADER            0xAAu
#define PROTO_END               0x55u
#define PROTO_PACKET_LEN        6u

/* B3 bit layout - mirrors system_config.h on the NUC side exactly */
#define B3_SW25_BIT              0u      /* bit0 = SW25 state        */
#define B3_POWERFAIL_BIT         1u      /* bit1 = 1 if power fail   */
#define B3_LOWBAT_BIT            2u      /* bit2 = 1 if low battery  */
#define B3_NUC_BOOT_BIT          3u      /* bit3 = 1 on the NUC's very
                                             first switch packet after
                                             boot only - unambiguous
                                             "NUC just reset" marker */

/* ── Voltage data packet: NUC029 → ESP32 ────────────────────── */
#define VOLT_HEADER             0xCCu
#define VOLT_END                0x66u
#define VOLT_PACKET_LEN         9u

/* Physical relay feedback from NUC029xAN */
#define RELAY_STATUS_HEADER     0xDDu
#define RELAY_STATUS_END        0x77u
#define RELAY_STATUS_PACKET_LEN 7u   /* was 8 - NUC now also sends bat_pct */

/* ── Init ────────────────────────────────────────────────────── */
void uart_parser_init(void);

/* ── Switch state API ────────────────────────────────────────── */
bool     uart_parser_get_gpio(uint8_t index);
uint32_t uart_parser_packet_count(void);
uint32_t uart_parser_last_packet_ms(void);
bool     uart_parser_is_online(void);

/* ── Voltage data API ────────────────────────────────────────── */
uint16_t uart_parser_get_bat_mv(void);
uint16_t uart_parser_get_pwr_mv(void);
bool     uart_parser_is_power_fail(void);
bool     uart_parser_is_low_battery(void);
uint8_t  uart_parser_get_bat_pct(void);   /* NUC-computed, 0-100 */

/* ── TX: relay command to NUC029 ─────────────────────────────── */
void uart_send_relay_cmd(uint8_t relay_num);

/* ── TX: relay lock command to NUC029 ─────────────────────────── */
void uart_send_relay_lock_cmd(bool locked);

/* ── TX: reset command to NUC029 ─────────────────────────────── */
void uart_send_nuc_reset_cmd(void);
/* Physical relay feedback from NUC: bit0=R1..bit3=R4. */
uint8_t uart_parser_get_relay_mask(void);
bool uart_parser_is_relay_locked(void);
void uart_parser_get_relay_snapshot(uint8_t *mask, bool *locked, uint32_t *seq);

/* Telemetry snapshots used by remote_server.c. */
void uart_parser_get_gpio_snapshot(bool *out, uint8_t count);
void uart_parser_get_power_snapshot(uint16_t *bat_mv,
                                    uint16_t *pwr_status,
                                    bool *power_fail,
                                    bool *low_battery,
                                    uint8_t *bat_pct);

