#pragma once
#include <stdbool.h>
#include <stddef.h>

/*
 * BT commissioning API.
 * Functions are always declared here so main.c can include this header
 * regardless of CONFIG_BT_ENABLED. The implementations in bt_commission.c
 * are compiled only when CONFIG_BT_ENABLED=y (guarded there).
 *
 * When BT_COMMISSIONING=0 in config.h these functions are never called,
 * so the linker will not complain about missing symbols.
 */
bool bt_commission_run(char *ssid_out, char *pass_out, size_t buf_sz);
bool bt_commission_load_nvs(char *ssid_out, char *pass_out, size_t buf_sz);
void bt_commission_stop(void);
