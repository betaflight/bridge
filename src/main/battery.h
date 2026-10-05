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

// Battery monitor for boards with an AXP2101 PMU (CONFIG_BRIDGE_BATTERY_AXP2101):
// a background task polls the PMU and caches the result, so the display and
// web handlers never block on I2C. Elsewhere battery_get() just reports false.
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool present;       // a cell is connected
    bool charging;
    uint16_t mv;        // cell voltage
    uint8_t percent;    // fuel-gauge state of charge, 0-100
} battery_status_t;

// Start polling. Call after display_start(): the PMU shares the touch
// controller's I2C bus, which the display brings up.
void battery_start(void);

// Latest reading. False when there is no monitor on this board or it failed to
// start; out is then left untouched.
bool battery_get(battery_status_t *out);
