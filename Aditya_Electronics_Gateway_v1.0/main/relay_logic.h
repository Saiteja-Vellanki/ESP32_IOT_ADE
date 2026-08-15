#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

void relay_logic_init(void);

void relay_logic_command(uint8_t relay_num);

bool relay_logic_get_state(uint8_t relay_num);

uint8_t relay_logic_get_mask(void);

void relay_logic_set_r3(bool turn_on);

/* Synchronize the cache from the NUC physical relay feedback packet. */
void relay_logic_sync_mask(uint8_t mask);

SemaphoreHandle_t relay_logic_get_change_semaphore(void);