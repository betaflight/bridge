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

// TCP server that Betaflight Configurator connects to (TCP transport, MSP over a
// raw stream). One client at a time is bridged to the FC VCP; a new connection
// takes over from the current one (TCP or WebSocket).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Default listen port. Matches Betaflight SITL / Configurator's TCP default.
#ifndef TCP_SERVER_PORT
#define TCP_SERVER_PORT  5761
#endif

// Spawn the listener/accept/RX task. Returns immediately.
void tcp_server_start(void);

// True while a Configurator client is connected.
bool tcp_server_client_connected(void);

// Drop the connected client, if any. The serve loop notices and releases the
// bridge; no-op when nobody is connected.
void tcp_server_kick(void);

// Send FC bytes to the connected TCP client (no-op if none). Called by the
// single net TX pump while a TCP client owns the bridge.
void tcp_server_send(const uint8_t *data, size_t len);
