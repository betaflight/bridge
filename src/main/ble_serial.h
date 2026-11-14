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

// Bluetooth LE serial on the Nordic UART service, so a phone can reach the FC
// without WiFi. One client at a time; it takes over the bridge when it
// subscribes, like a new TCP or WebSocket connection does.
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef enum {
    BLE_SERIAL_OFF = 0,
    BLE_SERIAL_ADVERTISING,
    BLE_SERIAL_CONNECTED,
} ble_serial_state_t;

// Bring up the BLE host and start advertising. Call after wifi_start(): the
// device name carries the WiFi station MAC suffix, as the mDNS hostname does.
void ble_serial_start(void);

// Send FC bytes to the subscribed BLE client (no-op if none). Called by the
// single net TX pump while a BLE client owns the bridge.
void ble_serial_send(const uint8_t *data, size_t len);

// Whether BLE is up, and whether a client is connected.
ble_serial_state_t ble_serial_state(void);

// The name the bridge advertises, e.g. "Betaflight Bridge a1b2c3" (empty when
// BLE is off).
const char *ble_serial_name(void);

// Drop the BLE client if another transport has taken the bridge from it.
// Called periodically by the net TX pump.
void ble_serial_poll(void);
