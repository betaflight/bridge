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

// Over-the-air firmware update via HTTP upload. Registers POST /update on the
// existing web server: the request body is the raw .bin, streamed straight into
// the inactive OTA slot. On success the boot partition is switched and the
// device reboots; rollback (see sdkconfig) reverts a bad image on next reset.
#pragma once

#include "esp_http_server.h"

// Register the /update handler on an already-started server.
void ota_register(httpd_handle_t server);

// Mark the running image valid so the bootloader keeps it (cancels rollback).
// Call once the device has come up healthy.
void ota_mark_valid(void);

// Report the running partition label (e.g. "ota_0") and whether the image is
// confirmed valid (i.e. not still pending rollback verification).
void ota_running_info(char *label, size_t label_len, bool *valid);

// Board identity of the running image (from esp_app_desc.version), e.g.
// "esp32s3-zero". An uploaded image whose board id differs is rejected.
const char *ota_board_id(void);
