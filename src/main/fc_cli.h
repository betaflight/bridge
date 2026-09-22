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

// Talks to the flight controller itself rather than passing bytes through:
// enough MSP to reboot it into its bootloader, and enough CLI to save the
// configuration before a flash and put it back afterwards.
//
// Only usable while the flasher owns the bridge (BRIDGE_CLIENT_FLASH); the
// FC->client pump routes the FC's replies here for the duration.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

// Start and finish a session. fc_cli_begin() must be called after claiming the
// bridge and fc_cli_end() before releasing it. The captured backup outlives the
// session so it can still be downloaded; fc_cli_backup_free() discards it.
void fc_cli_begin(void);
void fc_cli_end(void);

// Feed bytes received from the FC. Called by the single FC->client pump in
// ws_serial.c while the flasher owns the bridge; a no-op otherwise.
void fc_cli_rx(const uint8_t *data, size_t len);

// Put the FC into CLI mode. Sends "#\n" rather than a bare '#': without the
// newline the '#' runs into the next command and comments it out, which fails
// silently and looks like the FC ignoring us.
esp_err_t fc_cli_enter(void);

// Capture `diff all` into the session's backup buffer. Fails if the FC is still
// in MSP mode (detected as non-text in the reply) or if the prompt never
// returns.
esp_err_t fc_cli_backup(void);

// The captured backup, or NULL. Valid until fc_cli_backup_free().
const char *fc_cli_backup_text(size_t *len);
void fc_cli_backup_free(void);

// Replay the captured backup into the CLI, prefixed with "defaults nosave" and
// committed with "save". `applied` and `skipped` count the lines the FC
// accepted and rejected; either may be NULL.
esp_err_t fc_cli_restore(uint32_t *applied, uint32_t *skipped);

// Reboot the FC into its bootloader from the CLI. Bare `bl` lets the firmware
// pick between its flash and ROM bootloaders, which is more reliable than
// inferring the choice from MSP capability bits. Requires CLI mode, which is
// where fc_cli_backup() leaves the FC anyway.
esp_err_t fc_reboot_via_cli(void);

// The same over MSP, for an FC that never reached the CLI. Chooses the flash
// bootloader when the target reports one. The FC resets without answering, so
// a missing reply is expected rather than a failure.
esp_err_t fc_reboot_via_msp(void);
