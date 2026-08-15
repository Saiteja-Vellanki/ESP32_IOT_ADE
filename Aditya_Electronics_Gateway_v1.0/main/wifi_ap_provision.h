/*
 * wifi_ap_provision.h  —  WiFi Access Point Provisioning Portal
 *
 * Two MANDATORY sequential steps, each its own AP+web-server session:
 *
 *   Step 1 — IP configuration (wifi_ap_ip_config_run()). Runs at the
 *     default IP (192.168.4.1). User sets (or accepts default) the
 *     address this portal will use going forward. Submitting saves
 *     it to NVS and reboots — this step never continues into Step 2
 *     in the same session.
 *
 *   Step 2 — WiFi credentials (wifi_ap_provision_run()). Runs at
 *     whatever IP Step 1 saved. User enters their WiFi SSID/password.
 *     Submitting saves it to NVS and returns to main.c to connect.
 *
 * Step 1 only runs once (until a factory reset erases the saved IP);
 * every boot after that skips straight to Step 2 if WiFi credentials
 * aren't yet saved, or connects directly if they are.
 *
 * Credentials/IP saved to NVS under WIFI_NVS_NAMESPACE / KEY_SSID /
 * KEY_PASS / KEY_AP_IP (config.h).
 *
 * Usage in main.c:
 *   wifi_ap_ip_config_run();   // no-op + returns if already configured,
 *                              // otherwise never returns (reboots)
 *   char ssid[64], pass[64];
 *   if (!wifi_provision_load_nvs(ssid, pass, sizeof(ssid))) {
 *       bool ok = wifi_ap_provision_run(ssid, pass, sizeof(ssid));
 *       ...
 *   }
 */
#ifndef WIFI_AP_PROVISION_H
#define WIFI_AP_PROVISION_H

#include <stdbool.h>
#include <stddef.h>

/**
 * Runs the MANDATORY Step 1 IP-configuration portal, but only if
 * no setup-page IP has been configured yet (first-ever setup, or
 * after a factory reset). Blocks until the user submits an IP (or
 * a timeout saves the default), then ALWAYS restarts the device -
 * does not return in that case. If an IP is already configured,
 * returns immediately without doing anything, so it's safe to call
 * unconditionally before wifi_provision_load_nvs()/wifi_ap_provision_run().
 */
void wifi_ap_ip_config_run(void);

/** True if a setup-page IP has already been configured (Step 1 done). */
bool wifi_provision_ip_is_configured(void);

/**
 * Copy the currently configured setup-page IP into out (falls back
 * to WIFI_AP_DEFAULT_IP if none is saved yet). Useful for logging
 * the real address instead of assuming the default.
 */
void wifi_provision_get_ap_ip(char *out, size_t buf_sz);

/**
 * Load previously-saved WiFi credentials from NVS, if any exist.
 * Returns true and fills ssid_out/pass_out if found, false otherwise
 * (e.g. first boot, or after a /wifi/reset erase).
 */
bool wifi_provision_load_nvs(char *ssid_out, char *pass_out, size_t buf_sz);

/**
 * Start the AP + portal web page, block until the user submits
 * credentials or the timeout elapses.
 *
 * @param ssid_out  buffer to receive the entered SSID
 * @param pass_out  buffer to receive the entered password
 * @param buf_sz    size of each buffer
 * @return true if credentials were submitted, false on timeout
 */
bool wifi_ap_provision_run(char *ssid_out, char *pass_out, size_t buf_sz);

/**
 * Stop the AP and web server, free resources.
 * Call after wifi_ap_provision_run() returns, before connecting
 * in STA mode.
 */
void wifi_ap_provision_stop(void);

#endif /* WIFI_AP_PROVISION_H */
