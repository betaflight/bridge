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

// Panel/touch BSP for boards that set CONFIG_BRIDGE_DISPLAY_TOUCH. display.c includes
// this by name only, so the vendor BSP it resolves to stays a board detail; the
// bsp_display_*() API it exposes is the same across ESP-BSP components.
#pragma once

#include "bsp/esp32_s3_touch_lcd_4b.h"
