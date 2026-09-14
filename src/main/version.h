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

// Firmware version — shown in the web UI tagline and stamped into the build
// artefact's filename.
//
// CalVer, as betaflight uses: YEAR.MONTH.PATCH with an optional pre-release
// suffix, on the bridge's own release clock rather than betaflight's. Bump
// these, commit, then tag with exactly the resulting string (no "v" prefix);
// the release workflow refuses to publish when the two disagree. The Makefile
// reads the same four values to name the image.
#pragma once

#define BRIDGE_VERSION_YEAR   2026
#define BRIDGE_VERSION_MONTH  6
// 0 for the initial YEAR.MONTH release, then up for each bug-fix release.
#define BRIDGE_VERSION_PATCH  0
// Pre-release marker, written with its leading dash (e.g. "-rc1"). Empty
// string for a final release.
#define BRIDGE_VERSION_SUFFIX "-alpha"

#define BRIDGE_VERSION_STR_(x) #x
#define BRIDGE_VERSION_STR(x)  BRIDGE_VERSION_STR_(x)

#define BRIDGE_VERSION                       \
    BRIDGE_VERSION_STR(BRIDGE_VERSION_YEAR)  \
    "." BRIDGE_VERSION_STR(BRIDGE_VERSION_MONTH) \
    "." BRIDGE_VERSION_STR(BRIDGE_VERSION_PATCH) \
    BRIDGE_VERSION_SUFFIX
