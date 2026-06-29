/*
 * This file is part of Betaflight.
 *
 * Betaflight is free software. You can redistribute this software
 * and/or modify this software under the terms of the GNU General
 * Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later
 * version.
 *
 * Betaflight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this software.
 *
 * If not, see <http://www.gnu.org/licenses/>.
 */

// WiFi bring-up. Station-first: if credentials are stored and the network is
// reachable, the device joins it as a pure station and the SoftAP is NOT
// started. The SoftAP only comes up as a fallback — when no credentials are
// stored, or the stored network can't be reached at boot — so a phone/laptop
// can always connect to reach the web UI and (re)configure. The station
// interface is always enabled so networks can be scanned and joined live from
// the web page with no reboot. Credentials persist in NVS (namespace "wifi",
// keys "ssid"/"pass") and are re-applied on boot.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Default SoftAP identity. Override at build time with -D.
#ifndef WIFI_AP_SSID
#define WIFI_AP_SSID   "betaflight-bridge"
#endif
#ifndef WIFI_AP_PASS
#define WIFI_AP_PASS   "betaflight"   // >= 8 chars for WPA2; empty => open AP
#endif
#ifndef WIFI_AP_CHANNEL
#define WIFI_AP_CHANNEL 6
#endif

// SoftAP address; also the DHCP gateway/DNS handed to clients.
#ifndef WIFI_AP_IP
#define WIFI_AP_IP     "192.168.4.1"
#endif

// Station connection lifecycle, surfaced to the web UI.
typedef enum {
    WIFI_STA_IDLE = 0,    // no credentials configured
    WIFI_STA_CONNECTING,  // associating / waiting for an IP
    WIFI_STA_CONNECTED,   // associated and has an IP
    WIFI_STA_FAILED,      // gave up after retries
} wifi_sta_state_t;

typedef struct {
    wifi_sta_state_t state;
    bool ap_active;       // true if the fallback SoftAP is currently broadcasting
    char ssid[33];        // configured target SSID ("" if none)
    char ip[16];          // assigned IPv4 ("" unless connected)
    char gw[16];          // gateway ("" unless connected)
    char netmask[16];     // netmask ("" unless connected)
    int8_t rssi;          // connected AP signal in dBm (0 unless connected)
} wifi_status_t;

// One visible access point from a scan.
typedef struct {
    char    ssid[33];
    int8_t  rssi;         // dBm
    bool    secure;       // true unless the network is open
} wifi_scan_ap_t;

// Bring up netif/event loop and WiFi. If a network is stored, join it as a
// station (no AP); otherwise start the SoftAP + DHCP server for setup. The AP
// is also raised as a fallback if a stored network can't be reached at boot.
// Association happens asynchronously.
void wifi_start(void);

// Save station credentials to NVS and apply them live (no reboot): connects as
// STA, keeping the AP up if it is currently broadcasting. Pass NULL/empty ssid
// to clear creds, disconnect, and fall back to the setup AP.
void wifi_set_station(const char *ssid, const char *pass);

// Snapshot the current station state for display.
void wifi_sta_status(wifi_status_t *out);

// Blocking scan for nearby networks. Fills up to max entries (deduped by SSID,
// strongest first) and returns the count. Safe to call while the AP is serving
// clients; the scan briefly hops channels.
int wifi_scan(wifi_scan_ap_t *out, int max);
