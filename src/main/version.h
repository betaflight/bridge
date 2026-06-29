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
// This default is the single source of truth: the Makefile reads BRIDGE_VERSION
// from here so an unversioned `make <board>` still stamps and names the image
// with it. The release workflow overrides it at build time with
// -DBRIDGE_VERSION=<tag> (see CMakeLists.txt), hence the #ifndef guard.
#pragma once

#ifndef BRIDGE_VERSION
#define BRIDGE_VERSION "2026.6.0-alpha"
#endif
