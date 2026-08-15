#pragma once
#include <stdbool.h>
#include <stdint.h>

void remote_server_start(void);
bool remote_server_is_locked(void);
uint8_t remote_server_get_report_mask(void);

/* Notify a deliberate shutdown/restart to the cloud. */
bool remote_server_notify_power_off(const char *mac);
