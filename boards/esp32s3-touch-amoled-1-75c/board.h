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

// Waveshare ESP32-S3-Touch-AMOLED-1.75C (ESP32-S3R8, 32 MB flash): 1.75" 466x466
// round CO5300 AMOLED over QSPI with CST9217 touch, driven by
// devices/lcd_co5300.c (enabled via CONFIG_BRIDGE_DISPLAY_TOUCH +
// CONFIG_BRIDGE_DISPLAY_ROUND in this board's sdkconfig.defaults). The AXP2101
// PMU is left at its power-on defaults, which already enable the panel and
// touch rails.
//
// The single USB-C carries the native ESP32-S3 USB (D- GPIO19 / D+ GPIO20),
// shared between flashing/console (USB-Serial-JTAG) and the USB-host bridge —
// the serial console drops out once host mode engages (same as esp32s3-zero).
//
// No user LEDs: status is shown on the display.
#pragma once

#include "driver/gpio.h"

#define BOARD_NAME "esp32s3-touch-amoled-1-75c"

// 1.75" 466x466 CO5300 AMOLED on QSPI. The controller's RAM starts 6 columns
// in from the glass.
#define BSP_LCD_H_RES           466
#define BSP_LCD_V_RES           466
#define BSP_LCD_X_GAP           6
#define BSP_LCD_SPI_HOST        SPI2_HOST
#define BSP_LCD_CS              GPIO_NUM_12
#define BSP_LCD_PCLK            GPIO_NUM_38
#define BSP_LCD_DATA0           GPIO_NUM_4
#define BSP_LCD_DATA1           GPIO_NUM_5
#define BSP_LCD_DATA2           GPIO_NUM_6
#define BSP_LCD_DATA3           GPIO_NUM_7
#define BSP_LCD_RST             GPIO_NUM_1

// CST9217 touch on I2C (shared with the codecs and IMU, which are unused).
#define BSP_I2C_SCL             GPIO_NUM_14
#define BSP_I2C_SDA             GPIO_NUM_15
#define BSP_TOUCH_RST           GPIO_NUM_2
#define BSP_TOUCH_INT           GPIO_NUM_11
