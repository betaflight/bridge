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

// WebSocket serial endpoint. Browsers can't open raw TCP, so this exposes the
// FC byte stream over a WebSocket at /serial — reachable as ws:// on the plain
// HTTP server and wss:// on the TLS server. Transparent binary bridge, like the
// raw-TCP server; one client bridges at a time and the newest connection wins
// (see bridge.c).
#pragma once

#include <stdbool.h>
#include "esp_http_server.h"

// Register the /serial WebSocket handler on `server`. Call once per httpd
// instance — pass secure=true for the TLS server so the status page can report
// ws vs wss. The FC->client pump task is spawned on first call.
void ws_serial_register(httpd_handle_t server, bool secure);

// Close the connected WebSocket client, if any; the session close callback
// releases the bridge.
void ws_serial_kick(void);

// True when the currently-connected WebSocket client arrived over TLS (wss).
// Only meaningful while a WS client owns the bridge.
bool ws_serial_is_secure(void);
