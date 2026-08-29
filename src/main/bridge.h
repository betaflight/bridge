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

// Byte bridge between the USB CDC (FC side) and the TCP client (Configurator
// side). Two FreeRTOS stream buffers decouple the producer/consumer contexts so
// neither the USB driver callback nor the socket recv loop can stall the other.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

// Initialise the bridge stream buffers. Call once at startup before any of the
// push/pop helpers below.
void bridge_init(void);

// Which transport currently owns the FC byte stream. Only one client bridges
// at a time, shared across the raw-TCP server and the WebSocket endpoint (the
// stream buffers are single-consumer). A new connection on either transport
// takes over: the transports kick the current owner and bridge_claim().
typedef enum {
    BRIDGE_CLIENT_NONE = 0,
    BRIDGE_CLIENT_TCP,
    BRIDGE_CLIENT_WS,
} bridge_client_t;

// Claim the FC stream for `who`, taking it over from any current owner (newest
// client wins). The buffers are reset; the caller must bridge_release() when its
// client goes away, but only while it still owns (see bridge_client_owner()).
void bridge_claim(bridge_client_t who);

// Release a claim previously taken by `who` (no-op if `who` is not the owner).
void bridge_release(bridge_client_t who);

// The current owner, or BRIDGE_CLIENT_NONE when free.
bridge_client_t bridge_client_owner(void);

// FC -> Configurator. Called from the USB CDC RX callback context. Non-blocking;
// returns the number of bytes actually queued (may be < len if the buffer is
// full, which means the TCP side is not draining fast enough).
size_t bridge_usb_to_net_push(const uint8_t *data, size_t len);

// FC -> Configurator drain. Called by the TCP TX task. Blocks up to
// timeout_ms for at least one byte. Returns bytes written into out.
size_t bridge_usb_to_net_pop(uint8_t *out, size_t max_len, uint32_t timeout_ms);

// Configurator -> FC. Called from the TCP RX task. Non-blocking; returns bytes
// queued.
size_t bridge_net_to_usb_push(const uint8_t *data, size_t len);

// Configurator -> FC drain. Called by the USB TX task. Blocks up to timeout_ms.
size_t bridge_net_to_usb_pop(uint8_t *out, size_t max_len, uint32_t timeout_ms);

// Discard any buffered data in both directions. Called when a client connects or
// disconnects so a new session starts clean (avoids replaying stale MSP bytes).
void bridge_reset(void);

// Monotonic count of bytes moved to/from the FC (either direction). Sample it
// to drive an activity indicator: a change since the last sample means traffic.
uint32_t bridge_fc_activity(void);
