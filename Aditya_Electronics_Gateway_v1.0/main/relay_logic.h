#pragma once
#include <stdint.h>
#include <stdbool.h>

void    relay_logic_init(void);
void    relay_logic_command(uint8_t relay_num);   /* 1-3 from app */
bool    relay_logic_get_state(uint8_t relay_num); /* 1-4          */
uint8_t relay_logic_get_mask(void);               /* bit0=RL1..   */
