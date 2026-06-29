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

// Status LEDs, all optional and board-defined (see board.h):
//   BOARD_WIFI_LED_GPIO  - plain GPIO LED, blink cadence reflects WiFi state
//   BOARD_RGB_LED_GPIO   - WS2812 NeoPixel, colour reflects FC/OTG-USB comms
// A board that defines neither gets a no-op. Call once after the bridge, WiFi,
// TCP and USB-host subsystems are started.
#pragma once

void leds_start(void);
