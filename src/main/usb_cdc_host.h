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

// USB Host CDC-ACM front-end: enumerates the flight controller's Virtual COM
// Port and pumps bytes to/from the bridge.
#pragma once

#include <stdbool.h>
#include <stdint.h>

// Start the USB host stack and the CDC connect/disconnect handling. Spawns the
// USB library event task and the net->USB TX task. Returns once the host stack
// is installed (device attach happens asynchronously thereafter).
void usb_cdc_host_start(void);

// True while a FC VCP is open and ready to carry traffic.
bool usb_cdc_host_is_connected(void);

// Connection status plus the VID/PID of the open VCP (0 if not connected).
// vid/pid may be NULL. Returns the connected state.
bool usb_cdc_host_status(uint16_t *vid, uint16_t *pid);
