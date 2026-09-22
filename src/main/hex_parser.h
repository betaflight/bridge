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

// Streaming Intel HEX reader. Firmware images are flashed as they arrive over
// HTTP - the 4 MB boards have no spare partition and no PSRAM to hold a whole
// image - so this parser keeps only the current line and never sees the file as
// a whole. Feed it arbitrary byte runs; it calls back with absolute addresses.
//
// Records handled: 00 data, 01 EOF, 04 extended linear address, 05 start linear
// address. Types 02/03 (segment addressing) are rejected rather than ignored;
// no Betaflight target emits them and silently dropping them would flash to the
// wrong place.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Longest legal record: ':' + count + address + type + 255 data bytes +
// checksum, plus the line ending and a NUL.
#define HEX_MAX_LINE  600

// Called for each data record, with the extended-linear base already applied.
// Records arrive in file order; nothing guarantees they ascend, so a consumer
// that batches must compare `address` against where the previous record ended.
// Return false to abort the parse.
typedef bool (*hex_data_fn)(uint32_t address, const uint8_t *data, size_t len, void *ctx);

typedef struct {
    hex_data_fn on_data;
    void *ctx;

    uint32_t base;                  // from the last type-04 record
    uint32_t start_linear_address;  // from a type-05 record, if any
    bool have_start;

    uint64_t bytes_total;           // payload bytes seen, for reporting
    bool eof;                       // a type-01 record has been seen
    const char *error;              // NULL while healthy

    char line[HEX_MAX_LINE];
    size_t line_len;
    bool line_overflow;
} hex_parser_t;

// Prepare `p`. `on_data` may not be NULL.
void hex_parser_init(hex_parser_t *p, hex_data_fn on_data, void *ctx);

// Feed the next `len` bytes of the file. Returns false once the parse has
// failed; the reason is then available from hex_parser_error(). Bytes after the
// EOF record are ignored, which tolerates the trailing newline most tools emit.
bool hex_parser_push(hex_parser_t *p, const uint8_t *data, size_t len);

// Call once the upload is complete. Fails if no EOF record was seen, which is
// how a truncated upload is caught.
bool hex_parser_finish(hex_parser_t *p);

// Why the parse failed, or NULL if it has not.
const char *hex_parser_error(const hex_parser_t *p);
