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

// mDNS announcement so the app can find the bridge on the LAN without knowing
// its IP. Hostname is betaflight-bridge-<mac6>.local (unique per unit). Services:
//   _betaflight._tcp  port 5761, TXT: tcp, ws, wss, path, board, version
//   _http._tcp        port 80 (generic Bonjour/Avahi browsers)
#pragma once

// Start the responder. Call once after the WiFi netifs exist; it follows
// whichever interface (STA or SoftAP) is up.
void bridge_mdns_start(void);

// Advertised hostname without the ".local" suffix, e.g. "betaflight-bridge-a1b2c3".
const char *bridge_mdns_hostname(void);
