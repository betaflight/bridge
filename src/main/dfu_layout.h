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

// DfuSe memory-layout parsing. An STM32-style DFU device advertises its flash
// geometry in the interface string of each alternate setting, for example
//
//   "@Internal Flash  /0x08000000/04*016Kg,01*064Kg,07*128Kg"
//
// giving the region name, its base address, and runs of equally-sized erase
// pages. The flasher needs this to know which pages to erase and to reject a
// hex that addresses memory the device does not have.
//
// Kept apart from dfu_host.c so it can be exercised without USB hardware.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DFU_MAX_SECTORS  8    // longest real string seen has 4 runs
#define DFU_MAX_REGIONS  6    // internal/external flash, option bytes, OTP, ...
#define DFU_MAX_PAGES    2048 // bounds the flasher's erased-page bitmap

// One run of equally-sized pages.
typedef struct {
    uint32_t start_address;
    uint32_t page_size;
    uint32_t num_pages;
} dfu_sector_t;

typedef struct {
    char name[32];          // normalised key, e.g. "internal_flash"
    uint8_t alt;            // alternate setting this region was read from
    uint32_t start_address;
    uint32_t total_size;
    dfu_sector_t sectors[DFU_MAX_SECTORS];
    uint8_t sector_count;
} dfu_region_t;

typedef struct {
    dfu_region_t regions[DFU_MAX_REGIONS];
    uint8_t region_count;
} dfu_layout_t;

// Parse one interface string into `out`. Returns false if the string is not a
// layout descriptor, which is not an error: devices expose unrelated strings on
// other alternate settings and those are simply skipped.
bool dfu_layout_parse_region(const char *desc, uint8_t alt, dfu_region_t *out);

// Add a parsed region, ignoring duplicates and overflow.
void dfu_layout_add(dfu_layout_t *layout, const dfu_region_t *region);

// Look a region up by its normalised name, or NULL.
const dfu_region_t *dfu_layout_find(const dfu_layout_t *layout, const char *name);

// The region to flash: internal flash if present, else external. NULL if
// neither was advertised.
const dfu_region_t *dfu_layout_program_region(const dfu_layout_t *layout);

// Total number of erase pages in `region`.
uint32_t dfu_region_page_count(const dfu_region_t *region);

// Address and size of page `index`. False if the index is out of range.
bool dfu_region_page(const dfu_region_t *region, uint32_t index,
                     uint32_t *start_address, uint32_t *page_size);

// Index of the page containing `address`. False if the address falls outside
// the region.
bool dfu_region_page_of(const dfu_region_t *region, uint32_t address, uint32_t *index);

// True when every byte of [address, address+len) lies inside the region. Runs
// are contiguous by construction, so this is a plain range check.
bool dfu_region_covers(const dfu_region_t *region, uint32_t address, uint32_t len);
