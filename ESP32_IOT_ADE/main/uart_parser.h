/*
 * uart_parser.h  —  UART2 packet receiver & parser
 *
 * Packet:  [ 0xAA | D0 | D1 | D2 | D3 | 0x55 ]
 *   D0 bit0=SW01 .. bit7=SW08
 *   D1 bit0=SW09 .. bit7=SW16
 *   D2 bit0=SW17 .. bit7=SW24
 *   D3 bit0=SW25,  bits1-7 unused/reserved
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

void     uart_parser_init(void);
bool     uart_parser_get_gpio(uint8_t index);   /* 0=SW01 .. 24=SW25 */
uint32_t uart_parser_packet_count(void);
uint32_t uart_parser_last_packet_ms(void);
bool     uart_parser_is_online(void);
