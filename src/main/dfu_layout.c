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

#include "dfu_layout.h"

#include <stdlib.h>
#include <string.h>

#define DESC_MAX  256

// Bootloaders that misreport their own geometry. Each entry replaces the whole
// string; these are carried over from Configurator, which has had to live with
// them in the field.
static const struct {
    const char *broken;
    const char *fixed;
} k_fixups[] = {
    // Early H750 bootloaders overstated the first run and understated the rest.
    { "@External Flash /0x90000000/1001*128Kg,3*128Kg,20*128Ka",
      "@External Flash /0x90000000/998*128Kg,1*128Kg,4*128Kg,21*128Ka" },
    // AT32F43xxM / F43xxG spell the region "byte", breaking the name match.
    { "@Option byte   /0x1FFFC000/01*4096 g",
      "@Option bytes   /0x1FFFC000/01*4096 g" },
    { "@Option byte   /0x1FFFC000/01*512 g",
      "@Option bytes   /0x1FFFC000/01*512 g" },
};

// Copy `src` into `dst`, applying a whole-string fixup if one matches and
// dropping anything outside printable ASCII.
static void sanitise(const char *src, char *dst, size_t dst_len)
{
    for (size_t i = 0; i < sizeof(k_fixups) / sizeof(k_fixups[0]); i++) {
        if (strcmp(src, k_fixups[i].broken) == 0) {
            src = k_fixups[i].fixed;
            break;
        }
    }

    size_t o = 0;
    for (const char *s = src; *s && o + 1 < dst_len; s++) {
        if ((unsigned char)*s >= 0x20 && (unsigned char)*s <= 0x7E) {
            dst[o++] = *s;
        }
    }
    dst[o] = '\0';

    // GD32H7xx omits the space, which would key the region as "internalflash".
    static const size_t split = sizeof("@Internal") - 1;
    if (strncmp(dst, "@InternalFlash", sizeof("@InternalFlash") - 1) == 0 && o + 2 <= dst_len) {
        memmove(dst + split + 1, dst + split, o - split + 1);
        dst[split] = ' ';
    }
}

// "Internal Flash" -> "internal_flash". Only the first space becomes an
// underscore, matching Configurator: "@Option Bytes" keys as "option_bytes".
static void normalise_name(const char *src, char *dst, size_t dst_len)
{
    while (*src == ' ') {
        src++;
    }
    size_t end = strlen(src);
    while (end > 0 && src[end - 1] == ' ') {
        end--;
    }

    size_t o = 0;
    bool space_used = false;
    for (size_t i = 0; i < end && o + 1 < dst_len; i++) {
        char c = src[i];
        if (c == ' ') {
            if (space_used) {
                continue;
            }
            space_used = true;
            c = '_';
        } else if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        dst[o++] = c;
    }
    dst[o] = '\0';
}

// Page sizes are written as "128Kg", "0002Kg", "4096 g" - a decimal count, an
// optional K/M multiplier, then a one-character access code. The multiplier is
// the second-to-last character, and M is treated as K: Configurator has always
// multiplied both by 1024 and real descriptors rely on that reading.
static bool parse_page_size(const char *spec, uint32_t *out)
{
    char *end = NULL;
    const unsigned long digits = strtoul(spec, &end, 10);
    if (end == spec || digits == 0) {
        return false;
    }

    const size_t len = strlen(spec);
    uint32_t multiplier = 1;
    if (len >= 2) {
        const char unit = spec[len - 2];
        if (unit == 'K' || unit == 'k' || unit == 'M' || unit == 'm') {
            multiplier = 1024;
        }
    }

    *out = (uint32_t)digits * multiplier;
    return true;
}

bool dfu_layout_parse_region(const char *desc, uint8_t alt, dfu_region_t *out)
{
    char buf[DESC_MAX];
    sanitise(desc, buf, sizeof(buf));

    if (buf[0] != '@') {
        return false;
    }

    // Split on '/' and keep the first three fields. G474 reports a second
    // option-byte bank as a fourth and fifth field; Configurator discards them
    // and so do we.
    char *name = buf + 1;
    char *addr_field = strchr(name, '/');
    if (!addr_field) {
        return false;
    }
    *addr_field++ = '\0';
    char *sector_field = strchr(addr_field, '/');
    if (!sector_field) {
        return false;
    }
    *sector_field++ = '\0';
    char *extra = strchr(sector_field, '/');
    if (extra) {
        *extra = '\0';
    }

    char *end = NULL;
    const unsigned long start = strtoul(addr_field, &end, 0);
    if (end == addr_field) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    normalise_name(name, out->name, sizeof(out->name));
    if (out->name[0] == '\0') {
        return false;
    }
    out->alt = alt;
    out->start_address = (uint32_t)start;

    uint32_t offset = 0;
    for (char *entry = sector_field; entry; ) {
        char *comma = strchr(entry, ',');
        if (comma) {
            *comma = '\0';
        }

        char *star = strchr(entry, '*');
        if (!star) {
            return false;
        }
        *star++ = '\0';

        const unsigned long pages = strtoul(entry, &end, 10);
        if (end == entry || pages == 0) {
            return false;
        }
        uint32_t page_size = 0;
        if (!parse_page_size(star, &page_size)) {
            return false;
        }
        if (out->sector_count >= DFU_MAX_SECTORS) {
            return false;
        }

        out->sectors[out->sector_count++] = (dfu_sector_t){
            .start_address = out->start_address + offset,
            .page_size = page_size,
            .num_pages = (uint32_t)pages,
        };
        offset += (uint32_t)pages * page_size;

        entry = comma ? comma + 1 : NULL;
    }

    if (out->sector_count == 0) {
        return false;
    }
    out->total_size = offset;
    return true;
}

void dfu_layout_add(dfu_layout_t *layout, const dfu_region_t *region)
{
    if (layout->region_count >= DFU_MAX_REGIONS) {
        return;
    }
    // Alternate settings repeat a region when a device exposes it more than
    // once; the first wins, as it does in Configurator.
    if (dfu_layout_find(layout, region->name)) {
        return;
    }
    layout->regions[layout->region_count++] = *region;
}

const dfu_region_t *dfu_layout_find(const dfu_layout_t *layout, const char *name)
{
    for (uint8_t i = 0; i < layout->region_count; i++) {
        if (strcmp(layout->regions[i].name, name) == 0) {
            return &layout->regions[i];
        }
    }
    return NULL;
}

const dfu_region_t *dfu_layout_program_region(const dfu_layout_t *layout)
{
    const dfu_region_t *r = dfu_layout_find(layout, "internal_flash");
    return r ? r : dfu_layout_find(layout, "external_flash");
}

uint32_t dfu_region_page_count(const dfu_region_t *region)
{
    uint32_t n = 0;
    for (uint8_t i = 0; i < region->sector_count; i++) {
        n += region->sectors[i].num_pages;
    }
    return n;
}

bool dfu_region_page(const dfu_region_t *region, uint32_t index,
                     uint32_t *start_address, uint32_t *page_size)
{
    for (uint8_t i = 0; i < region->sector_count; i++) {
        const dfu_sector_t *s = &region->sectors[i];
        if (index < s->num_pages) {
            if (start_address) {
                *start_address = s->start_address + index * s->page_size;
            }
            if (page_size) {
                *page_size = s->page_size;
            }
            return true;
        }
        index -= s->num_pages;
    }
    return false;
}

bool dfu_region_page_of(const dfu_region_t *region, uint32_t address, uint32_t *index)
{
    uint32_t base = 0;
    for (uint8_t i = 0; i < region->sector_count; i++) {
        const dfu_sector_t *s = &region->sectors[i];
        const uint32_t span = s->num_pages * s->page_size;
        if (address >= s->start_address && address - s->start_address < span) {
            if (index) {
                *index = base + (address - s->start_address) / s->page_size;
            }
            return true;
        }
        base += s->num_pages;
    }
    return false;
}

bool dfu_region_covers(const dfu_region_t *region, uint32_t address, uint32_t len)
{
    if (len == 0) {
        return true;
    }
    if (address < region->start_address) {
        return false;
    }
    const uint32_t offset = address - region->start_address;
    // Guards the wrap that a hex claiming a huge length would otherwise cause.
    return offset <= region->total_size && len <= region->total_size - offset;
}
