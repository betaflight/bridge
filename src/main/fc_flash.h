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

// Flashes the flight controller from the web UI: saves its configuration over
// CLI, reboots it into its bootloader, writes an Intel HEX over DFU, verifies
// it, and puts the configuration back.
//
//   POST /dfu/start   begin a job; form-encoded options
//   POST /dfu/data    the next slice of the hex; returns the progress snapshot
//   GET  /dfu/status  progress snapshot
//   GET  /dfu/backup  the captured `diff all`, as text
//   POST /dfu/abort   cancel the running job
//
// The hex is never held whole: the 4 MB boards have neither a spare partition
// nor PSRAM for it, so slices are parsed and flashed as they arrive and the
// browser's upload is paced by TCP backpressure.
#pragma once

#include "esp_http_server.h"

// Register the /dfu/* routes on `server`. Called for both the HTTP and HTTPS
// instances; the job itself is global, so whichever server starts it owns it.
void fc_flash_register(httpd_handle_t server);
