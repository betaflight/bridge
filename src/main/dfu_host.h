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

// USB Host DFU front-end: a second USB host client alongside the CDC-ACM one,
// which adopts the flight controller once it re-enumerates as a DFU device and
// speaks DFU 1.1 plus the STM32 DfuSe extensions to it over EP0.
//
// Only control transfers are used, so no interface is claimed. Every call here
// blocks; none may be made from the client event task (see dfu_host.c).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "dfu_layout.h"

// STM32 bootloaders report 2048; the spec allows up to 65535, but nothing in
// the field exceeds this and it bounds our permanently-held DMA buffer.
#define DFU_MAX_TRANSFER_SIZE  2048

// Alternate settings on interface 0, one per advertised memory region.
#define DFU_MAX_ALTS           8

// DFU 1.1 device states (bState in the GETSTATUS reply).
typedef enum {
    DFU_STATE_APP_IDLE = 0,
    DFU_STATE_APP_DETACH = 1,
    DFU_STATE_IDLE = 2,
    DFU_STATE_DNLOAD_SYNC = 3,
    DFU_STATE_DNBUSY = 4,
    DFU_STATE_DNLOAD_IDLE = 5,
    DFU_STATE_MANIFEST_SYNC = 6,
    DFU_STATE_MANIFEST = 7,
    DFU_STATE_MANIFEST_WAIT_RESET = 8,
    DFU_STATE_UPLOAD_IDLE = 9,
    DFU_STATE_ERROR = 10,
} dfu_state_t;

#define DFU_STATUS_OK          0x00
#define DFU_STATUS_ERR_VENDOR  0x0b

typedef struct {
    uint8_t status;     // bStatus
    uint8_t state;      // bState, a dfu_state_t
    uint32_t poll_ms;   // bwPollTimeout; honour it before re-reading
} dfu_status_t;

typedef struct {
    uint16_t vid;
    uint16_t pid;
    uint16_t bcd_dfu_version;   // 0x011a marks the DfuSe extensions
    uint16_t transfer_size;     // clamped to DFU_MAX_TRANSFER_SIZE
    dfu_layout_t layout;
} dfu_host_info_t;

// Register the DFU client and start its tasks. Call after usb_host_install();
// usb_cdc_host_start() does so.
void dfu_host_start(void);

// True while a DFU device is open and has been probed.
bool dfu_host_is_present(void);

// Snapshot of the attached device. False when none is present.
bool dfu_host_get_info(dfu_host_info_t *out);

// Block until a DFU device is present, or the timeout expires.
bool dfu_host_wait(uint32_t timeout_ms);

// Raw DFU class requests. `dfu_host_dnload` with len 0 is the DfuSe leave
// command; the caller must follow it with dfu_host_get_status().
esp_err_t dfu_host_get_status(dfu_status_t *out);
esp_err_t dfu_host_clr_status(void);
esp_err_t dfu_host_abort(void);
esp_err_t dfu_host_dnload(uint16_t block, const void *data, uint16_t len);
esp_err_t dfu_host_upload(uint16_t block, void *data, uint16_t len, size_t *actual);

// Drive the device back to DFU_STATE_IDLE, clearing an error or aborting an
// in-progress download/upload as required. `busy_is_stuck` additionally treats
// a lingering DNBUSY as an error worth clearing - the STM32H7 workaround, and
// wrong to do by default because it wedges other parts.
esp_err_t dfu_host_clear_state(bool busy_is_stuck);

// Write one block of firmware at the current address pointer. `block` starts
// at 2 after each set-address and increments; the device derives the address
// from it. Runs the busy/poll/idle handshake internally.
esp_err_t dfu_host_write_block(uint16_t block, const void *data, uint16_t len);

// DfuSe commands. Each runs the busy/poll/idle handshake internally.
esp_err_t dfu_host_set_address(uint32_t address);
esp_err_t dfu_host_erase_page(uint32_t address);

// Set the address pointer without treating a refusal as fatal, so the caller
// can distinguish "read protected" from a real failure. `out` receives the
// status that ended the handshake.
esp_err_t dfu_host_set_address_probe(uint32_t address, dfu_status_t *out);

// Close the DFU device without waiting for it to announce its departure.
// Closing is what lets the host stack recycle the root port, and the CDC side
// only resumes looking for a VCP once no DFU device is present - so relying on
// the disconnect event alone risks the FC never being picked up again.
void dfu_host_release(void);

// Leave DFU and start the firmware: set the address pointer, then a
// zero-length download plus the status read that triggers the exit.
esp_err_t dfu_host_leave(uint32_t address);
