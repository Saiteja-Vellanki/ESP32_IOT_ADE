#pragma once

/*
 * factory_reset.h — hold-button resets
 *
 * TWO separate buttons/tasks:
 *
 *   1. Full factory reset — watches FACTORY_RESET_GPIO. Holding for
 *      FACTORY_RESET_HOLD_MS (15s) erases the saved WiFi credentials
 *      AND the custom AP portal IP from NVS, then reboots — next
 *      boot has no saved IP either, so it drops all the way back to
 *      Step 1 (mandatory IP configuration), same as a brand-new
 *      device.
 *
 *   2. WiFi-only reset — watches WIFI_ONLY_RESET_GPIO. Holding for
 *      WIFI_ONLY_RESET_HOLD_MS (7s) erases ONLY the saved WiFi SSID/
 *      password, leaving the configured setup-page IP untouched.
 *      Next boot skips Step 1 entirely (IP is still configured) and
 *      goes straight to the Step 2 WiFi-credentials portal at that
 *      same IP - for reconnecting to a different WiFi network
 *      without redoing IP setup.
 */
void factory_reset_init(void);
