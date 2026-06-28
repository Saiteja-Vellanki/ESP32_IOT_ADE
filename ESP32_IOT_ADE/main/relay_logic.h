/*
 * relay_logic.h  —  Relay state machine
 *
 * RL1 : SW1 command → ON 3s → OFF
 * RL2 : SW2 command → continuous ON  /  SW2 again → OFF 3s → ON
 * RL3 : SW3 command → ON, after 3s triggers RL4
 * RL4 : triggered by RL3 → ON 3s → OFF
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

/* Call once at startup */
void relay_logic_init(void);

/*
 * Command a relay from app/web (relay_num 1-4).
 * This is called by web_server or remote_server when a user
 * presses SW1/SW2/SW3 in the Android app or dashboard.
 */
void relay_logic_command(uint8_t relay_num);

/* Read current physical state of relay (1=ON 0=OFF) */
bool relay_logic_get_state(uint8_t relay_num);   /* relay_num 1-4 */

/* Get all 4 relay states as bitmask (bit0=RL1 .. bit3=RL4) */
uint8_t relay_logic_get_mask(void);
