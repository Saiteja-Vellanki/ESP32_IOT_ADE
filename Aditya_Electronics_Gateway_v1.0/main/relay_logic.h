#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/*
 * Relay STATE TRACKER — no local GPIO control. The ESP32 has no
 * relays; the NUC029xAN owns all 4 physical relays, driven via
 * UART (uart_send_relay_cmd()). This just mirrors the NUC's known
 * timing in software for local dashboard / server-report purposes.
 * See relay_logic.c for details.
 */
void    relay_logic_init(void);
void    relay_logic_command(uint8_t relay_num);   /* 1-4 */
bool    relay_logic_get_state(uint8_t relay_num); /* 1-4 */
uint8_t relay_logic_get_mask(void);               /* bit0=RL1..   */

/*
 * Returns a semaphore that's given every time ANY tracked relay
 * state changes - including the automatic 3-second revert (RL1
 * OFF, RL2 back ON, RL4 auto-trigger-then-OFF), not just the
 * initial command. Callers (remote_server.c) should non-blockingly
 * take this in their own task/stack context and immediately report
 * the new state to the server when it fires, rather than waiting
 * for the next periodic poll. Deliberately NOT a callback - running
 * an HTTPS call directly from relay_logic's own task would need far
 * more stack than RELAY_TASK_STACK provides.
 */
SemaphoreHandle_t relay_logic_get_change_semaphore(void);
