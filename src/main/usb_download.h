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

// USB flash mode: reboot into the ROM loader so the board's own USB-C, normally
// hosting the FC, can be flashed with esptool. Entered by holding BOOT for
// three seconds or from the touch UI; RESET leaves it.
#pragma once

// Watch the BOOT button for a long press.
void usb_download_start(void);

// Show a notice on the display and reboot into the ROM loader. Does not return.
void usb_download_enter(void);
