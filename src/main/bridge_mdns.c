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

#include "bridge_mdns.h"
#include "tcp_server.h"
#include "ota.h"
#include "version.h"

#include <stdio.h>

#include "esp_mac.h"
#include "esp_log.h"
#include "mdns.h"

static const char *TAG = "mdns";

static char s_hostname[32];

const char *bridge_mdns_hostname(void)
{
    return s_hostname;
}

void bridge_mdns_start(void)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_hostname, sizeof(s_hostname), "betaflight-bridge-%02x%02x%02x", mac[3], mac[4], mac[5]);

    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mdns_init failed: %s", esp_err_to_name(err));
        return;
    }
    mdns_hostname_set(s_hostname);
    mdns_instance_name_set(s_hostname);

    char tcp_port[8];
    snprintf(tcp_port, sizeof(tcp_port), "%d", TCP_SERVER_PORT);
    mdns_txt_item_t txt[] = {
        { "tcp",     tcp_port },
        { "ws",      "80" },
        { "wss",     "443" },
        { "path",    "/serial" },
        { "board",   ota_board_id() },
        { "version", BRIDGE_VERSION },
    };
    mdns_service_add(NULL, "_betaflight", "_tcp", TCP_SERVER_PORT, txt, sizeof(txt) / sizeof(txt[0]));
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);

    ESP_LOGI(TAG, "announcing %s.local (_betaflight._tcp:%d)", s_hostname, TCP_SERVER_PORT);
}
