#pragma once
#include <stdbool.h>

void remote_server_start(void);
void remote_server_notify_relay_change(void);
bool remote_server_is_locked(void);
