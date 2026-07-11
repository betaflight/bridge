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

// Waveshare ESP32-S3-Touch-LCD-4B (N16R8): 4.0" 480x480 ST7701 RGB panel with
// GT911 touch, driven by the waveshare/esp32_s3_touch_lcd_4b BSP component
// (enabled via CONFIG_BRIDGE_DISPLAY in this board's sdkconfig.defaults).
//
// The single USB-C carries the native ESP32-S3 USB (D- GPIO19 / D+ GPIO20),
// shared between flashing/console (USB-Serial-JTAG) and the USB-host bridge —
// the serial console drops out once host mode engages (same as esp32s3-zero).
// UART0 (TX GPIO43 / RX GPIO44) is on the header for a persistent log console.
//
// No user LEDs: status is shown on the LCD.
#pragma once

#define BOARD_NAME "esp32s3-touch-lcd-4b"
