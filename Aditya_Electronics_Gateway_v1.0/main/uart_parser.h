#pragma once
#include <stdint.h>
#include <stdbool.h>

/*
 * ESP32 → NUC029 relay command packet (4 bytes):
 *
 *  Byte 0: 0xBB  (header  — different from NUC→ESP 0xAA)
 *  Byte 1: 0x01  (CMD = relay trigger)
 *  Byte 2: 1-4   (relay number)
 *  Byte 3: 0x44  (end     — different from NUC→ESP 0x55)
 *
 *  NUC029 UART0 RX must parse this packet and trigger the relay.
 *  (NUC receiver code to be added separately.)
 */
#define CMD_PROTO_HEADER    0xBBu
#define CMD_PROTO_END       0x44u
#define CMD_RELAY_TRIGGER   0x01u
#define CMD_PACKET_LEN      4u

/* ── RX (NUC→ESP) ───────────────────────────────────────────── */
void     uart_parser_init(void);
bool     uart_parser_get_gpio(uint8_t index);   /* 0=SW01..24=SW25 */
uint32_t uart_parser_packet_count(void);
uint32_t uart_parser_last_packet_ms(void);
bool     uart_parser_is_online(void);

/* ── TX (ESP→NUC) ───────────────────────────────────────────── */
/*
 * Send a relay trigger command to NUC029.
 * relay_num: 1=RL1, 2=RL2, 3=RL3, 4=RL4
 * Thread-safe — can be called from any task.
 */
void uart_send_relay_cmd(uint8_t relay_num);
