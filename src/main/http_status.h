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

// Web UI on port 80. Serves a live status page (USB/TCP/WiFi state) and lets the
// user scan for and join a WiFi network; credentials are stored for future use.
//   GET  /        HTML page (status + WiFi join form)
//   GET  /status  JSON snapshot of USB, TCP and WiFi state
//   GET  /scan    JSON list of nearby networks
//   POST /wifi    form-encoded ssid/pass; saves and joins live (empty = forget)
#pragma once

// Start the HTTP server. Call after WiFi is up.
void http_status_start(void);
