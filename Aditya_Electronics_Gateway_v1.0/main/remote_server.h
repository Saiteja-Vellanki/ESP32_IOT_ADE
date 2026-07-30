#pragma once
#include <stdbool.h>

void remote_server_start(void);
void remote_server_notify_relay_change(void);
bool remote_server_is_locked(void);

/*
 * Switch OFF Device (proper way) — per document.
 * Call before a deliberate shutdown/restart to notify the server
 * (register.php status=0 — see config.h note on the doc's
 * internal status-code contradiction).
 */
bool remote_server_notify_power_off(const char *mac);
