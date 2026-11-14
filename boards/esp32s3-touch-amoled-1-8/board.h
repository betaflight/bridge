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

// Waveshare ESP32-S3-Touch-AMOLED-1.8 (ESP32-S3R8, 16 MB flash): 1.8" 368x448
// AMOLED over QSPI with capacitive touch, driven by devices/lcd_amoled.c
// (enabled via CONFIG_BRIDGE_DISPLAY_TOUCH in this board's sdkconfig.defaults).
// Two hardware revisions ship under the same name: SH8601 panel + FT3168 touch,
// and CO5300 panel + CST816 touch. The touch chip identifies the revision.
// Panel and touch resets are on a TCA9554 I/O expander; the AXP2101 PMU is
// left at its power-on defaults, which already enable the panel and touch rails.
//
// The single USB-C carries the native ESP32-S3 USB (D- GPIO19 / D+ GPIO20),
// shared between flashing/console (USB-Serial-JTAG) and the USB-host bridge —
// the serial console drops out once host mode engages (same as esp32s3-zero).
//
// No user LEDs: status is shown on the display.
#pragma once

#include "driver/gpio.h"
#include "driver/i2c_types.h"

#define BOARD_NAME "esp32s3-touch-amoled-1-8"

// 1.8" 368x448 AMOLED on QSPI. The CO5300 revision's RAM starts 16 columns in
// from the glass; the SH8601's does not.
#define BSP_LCD_H_RES           368
#define BSP_LCD_V_RES           448
#define BSP_LCD_X_GAP_CO5300    16
#define BSP_LCD_SPI_HOST        SPI2_HOST
#define BSP_LCD_CS              GPIO_NUM_12
#define BSP_LCD_PCLK            GPIO_NUM_11
#define BSP_LCD_DATA0           GPIO_NUM_4
#define BSP_LCD_DATA1           GPIO_NUM_5
#define BSP_LCD_DATA2           GPIO_NUM_6
#define BSP_LCD_DATA3           GPIO_NUM_7

// Touch, the TCA9554 expander and the AXP2101 PMU on I2C (shared with the
// codec, IMU and RTC, which are unused).
#define BOARD_PMU_I2C_PORT      I2C_NUM_0
#define BSP_I2C_SCL             GPIO_NUM_14
#define BSP_I2C_SDA             GPIO_NUM_15
#define BSP_TOUCH_INT           GPIO_NUM_21

// TCA9554 outputs EXIO0/1/2/6 hold the panel and touch in reset while low.
#define BOARD_EXPANDER_ADDR     0x20
#define BOARD_EXPANDER_RESET_MASK 0x47
