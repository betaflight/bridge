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

// betaflight-bridge: ESP32-S3 USB-host-to-WiFi bridge.
//
// Acts as USB host to a Betaflight flight controller's Virtual COM Port (VCP)
// and exposes that serial stream over TCP, so Betaflight Configurator (TCP
// transport, default port 5761) can connect wirelessly from a phone or laptop.
//
//   [FC USB VCP] <--USB host--> [ESP32-S3] <--WiFi/TCP:5761--> [Configurator]
//
// Data flow is a transparent byte bridge; no MSP parsing happens here.

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "bridge.h"
#include "wifi.h"
#include "tcp_server.h"
#include "usb_cdc_host.h"
#include "http_status.h"
#include "ota.h"
#include "leds.h"

static const char *TAG = "main";

void app_main(void)
{
    // NVS holds WiFi station credentials (and is required by the WiFi stack).
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    bridge_init();
    wifi_start();
    http_status_start();
    tcp_server_start();
    usb_cdc_host_start();
    leds_start();   // WiFi + FC-comms status LEDs (board-defined)

    // Everything came up: if we just booted a freshly-OTA'd image, confirm it
    // so the bootloader keeps it instead of rolling back on the next reset.
    ota_mark_valid();

    ESP_LOGI(TAG, "betaflight-bridge ready");
}
