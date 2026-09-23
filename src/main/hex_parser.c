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

#include "hex_parser.h"

#include <string.h>

#define REC_DATA        0x00
#define REC_EOF         0x01
#define REC_EXT_SEGMENT 0x02
#define REC_START_SEG   0x03
#define REC_EXT_LINEAR  0x04
#define REC_START_LIN   0x05

#define MAX_RECORD_DATA 255

static bool fail(hex_parser_t *p, const char *why)
{
    if (!p->error) {
        p->error = why;
    }
    return false;
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static bool hex_byte(const char *s, uint8_t *out)
{
    const int hi = hex_nibble(s[0]);
    const int lo = hex_nibble(s[1]);
    if (hi < 0 || lo < 0) {
        return false;
    }
    *out = (uint8_t)((hi << 4) | lo);
    return true;
}

// Decode one ':'-prefixed record and act on it. `len` excludes any line ending.
static bool parse_line(hex_parser_t *p, const char *line, size_t len)
{
    if (line[0] != ':') {
        return fail(p, "line does not start with ':'");
    }
    // ':' + count + address + type is the shortest possible record, and every
    // record is an even number of hex digits after the colon.
    if (len < 11 || ((len - 1) & 1)) {
        return fail(p, "malformed record length");
    }

    uint8_t raw[4 + MAX_RECORD_DATA + 1];
    const size_t n_bytes = (len - 1) / 2;
    if (n_bytes > sizeof(raw)) {
        return fail(p, "record too long");
    }
    for (size_t i = 0; i < n_bytes; i++) {
        if (!hex_byte(line + 1 + i * 2, &raw[i])) {
            return fail(p, "record contains a non-hex digit");
        }
    }

    const uint8_t count = raw[0];
    if (n_bytes != (size_t)count + 5) {
        return fail(p, "byte count disagrees with record length");
    }

    // The checksum covers every decoded byte including itself, so a valid
    // record sums to zero modulo 256.
    uint8_t sum = 0;
    for (size_t i = 0; i < n_bytes; i++) {
        sum = (uint8_t)(sum + raw[i]);
    }
    if (sum != 0) {
        return fail(p, "record checksum mismatch");
    }

    const uint16_t offset = (uint16_t)((raw[1] << 8) | raw[2]);
    const uint8_t type = raw[3];
    const uint8_t *payload = &raw[4];

    switch (type) {
    case REC_DATA:
        if (count == 0) {
            return true;
        }
        p->bytes_total += count;
        if (!p->on_data(p->base + offset, payload, count, p->ctx)) {
            return fail(p, "aborted while writing");
        }
        return true;

    case REC_EOF:
        p->eof = true;
        return true;

    case REC_EXT_LINEAR:
        if (count != 2) {
            return fail(p, "bad extended linear address record");
        }
        p->base = (uint32_t)payload[0] << 24 | (uint32_t)payload[1] << 16;
        return true;

    case REC_START_LIN:
        if (count != 4) {
            return fail(p, "bad start linear address record");
        }
        p->start_linear_address = (uint32_t)payload[0] << 24 | (uint32_t)payload[1] << 16 |
                                  (uint32_t)payload[2] << 8  | (uint32_t)payload[3];
        p->have_start = true;
        return true;

    case REC_EXT_SEGMENT:
    case REC_START_SEG:
        return fail(p, "segment-addressed hex is not supported");

    default:
        return fail(p, "unknown record type");
    }
}

void hex_parser_init(hex_parser_t *p, hex_data_fn on_data, void *ctx)
{
    memset(p, 0, sizeof(*p));
    p->on_data = on_data;
    p->ctx = ctx;
}

bool hex_parser_push(hex_parser_t *p, const uint8_t *data, size_t len)
{
    if (p->error) {
        return false;
    }

    for (size_t i = 0; i < len; i++) {
        const char c = (char)data[i];

        if (c != '\n' && c != '\r') {
            if (p->line_len + 1 >= sizeof(p->line)) {
                p->line_overflow = true;   // reported when the line terminates
            } else {
                p->line[p->line_len++] = c;
            }
            continue;
        }

        const size_t line_len = p->line_len;
        const bool overflowed = p->line_overflow;
        p->line_len = 0;
        p->line_overflow = false;

        if (overflowed) {
            return fail(p, "record too long");
        }
        if (line_len == 0) {
            continue;   // blank line, or the second half of a CRLF
        }
        // Tools commonly leave a trailing newline after the EOF record; any
        // real record following one means the file is not what it claims.
        if (p->eof) {
            return fail(p, "data follows the end-of-file record");
        }
        p->line[line_len] = '\0';
        if (!parse_line(p, p->line, line_len)) {
            return false;
        }
    }

    return true;
}

bool hex_parser_finish(hex_parser_t *p)
{
    if (p->error) {
        return false;
    }
    if (p->line_overflow) {
        return fail(p, "record too long");
    }
    // A file may legally end without a line terminator, and rejecting that
    // would fail the job after the whole image had already been written. A
    // record genuinely cut short still fails its own length or checksum check.
    if (p->line_len != 0) {
        const size_t line_len = p->line_len;
        p->line_len = 0;
        if (p->eof) {
            return fail(p, "data follows the end-of-file record");
        }
        p->line[line_len] = '\0';
        if (!parse_line(p, p->line, line_len)) {
            return false;
        }
    }
    if (!p->eof) {
        return fail(p, "no end-of-file record; upload is truncated");
    }
    return true;
}

const char *hex_parser_error(const hex_parser_t *p)
{
    return p->error;
}
