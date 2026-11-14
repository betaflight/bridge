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

#include "ble_serial.h"
#include "sdkconfig.h"

#if CONFIG_BT_NIMBLE_ENABLED

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "bridge.h"

#define TAG "ble_serial"

#define ATT_HEADER_LEN 3
#define TX_RETRY_MS 2
#define TX_RETRY_LIMIT 100

// Connection interval in 1.25 ms units, supervision timeout in 10 ms units.
#define CONN_ITVL_MIN 12
#define CONN_ITVL_MAX 24
#define CONN_SUPERVISION_TIMEOUT 400

// Nordic UART service. Clients disagree on which characteristic carries which
// direction, so both accept writes and both notify: FC data goes out on
// whichever one the client subscribed to.
static const ble_uuid128_t NUS_SVC = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93, 0xf3, 0xa3, 0xb5, 0x01, 0x00, 0x40, 0x6e);
static const ble_uuid128_t NUS_RX = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93, 0xf3, 0xa3, 0xb5, 0x02, 0x00, 0x40, 0x6e);
static const ble_uuid128_t NUS_TX = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93, 0xf3, 0xa3, 0xb5, 0x03, 0x00, 0x40, 0x6e);

static uint16_t s_rx_handle;
static uint16_t s_tx_handle;

static char s_name[32];
static uint8_t s_own_addr_type;

static volatile uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
// Bumped on every connect and disconnect, so a send that started for one client
// never reaches, or drops, the next.
static volatile uint32_t s_conn_gen;
static volatile bool s_rx_subscribed;
static volatile bool s_tx_subscribed;
static volatile bool s_claimed;
static volatile bool s_terminating;
static volatile bool s_advertising;

static void advertise(void);

static void terminate(void)
{
    uint16_t conn = s_conn;
    if (conn != BLE_HS_CONN_HANDLE_NONE && !s_terminating) {
        s_terminating = true;
        ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
    }
}

// The bridge is claimed once per connection, on the first subscribe or write.
// After that a takeover by another transport is final: ble_serial_poll() drops
// the link rather than letting a late write claim it back.
static bool claim(void)
{
    if (s_claimed) {
        return bridge_client_owner() == BRIDGE_CLIENT_BLE;
    }
    s_claimed = true;
    if (!bridge_claim_unless_flashing(BRIDGE_CLIENT_BLE)) {
        ESP_LOGW(TAG, "refusing client: flashing the FC");
        terminate();
        return false;
    }
    ESP_LOGI(TAG, "client owns the bridge");
    return true;
}

static int nus_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    if (!claim()) {
        return 0;
    }
    uint8_t buf[BLE_ATT_ATTR_MAX_LEN];
    uint16_t len = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &len) != 0) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    bridge_net_to_usb_push(BRIDGE_CLIENT_BLE, buf, len);
    return 0;
}

static const struct ble_gatt_svc_def s_services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &NUS_SVC.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                .uuid = &NUS_RX.u,
                .access_cb = nus_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_rx_handle,
            },
            {
                .uuid = &NUS_TX.u,
                .access_cb = nus_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_tx_handle,
            },
            {0},
        },
    },
    {0},
};

// MSP is request/response, so each round trip costs a couple of connection
// intervals; centrals tend to default to 30-50 ms. 15 ms is the shortest iOS
// accepts.
static void request_fast_interval(uint16_t conn)
{
    const struct ble_gap_upd_params params = {
        .itvl_min = CONN_ITVL_MIN,
        .itvl_max = CONN_ITVL_MAX,
        .latency = 0,
        .supervision_timeout = CONN_SUPERVISION_TIMEOUT,
    };
    int rc = ble_gap_update_params(conn, &params);
    if (rc != 0) {
        ESP_LOGW(TAG, "connection parameter update failed: %d", rc);
    }
}

static void on_disconnect(void)
{
    s_conn_gen++;
    if (s_claimed && bridge_client_owner() == BRIDGE_CLIENT_BLE) {
        bridge_release(BRIDGE_CLIENT_BLE);
    }
    s_conn = BLE_HS_CONN_HANDLE_NONE;
    s_rx_subscribed = false;
    s_tx_subscribed = false;
    s_claimed = false;
    s_terminating = false;
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status != 0) {
            advertise();
            break;
        }
        s_conn_gen++;
        s_conn = event->connect.conn_handle;
        s_advertising = false;
        ESP_LOGI(TAG, "client connected");
        request_fast_interval(event->connect.conn_handle);
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "client disconnected (reason 0x%x)", event->disconnect.reason);
        on_disconnect();
        advertise();
        break;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_rx_handle) {
            s_rx_subscribed = event->subscribe.cur_notify;
        } else if (event->subscribe.attr_handle == s_tx_handle) {
            s_tx_subscribed = event->subscribe.cur_notify;
        } else {
            break;
        }
        if (s_rx_subscribed || s_tx_subscribed) {
            claim();
        } else if (s_claimed && bridge_client_owner() == BRIDGE_CLIENT_BLE) {
            bridge_release(BRIDGE_CLIENT_BLE);
        }
        break;

    case BLE_GAP_EVENT_CONN_UPDATE:
        if (event->conn_update.status == 0) {
            struct ble_gap_conn_desc desc;
            if (ble_gap_conn_find(event->conn_update.conn_handle, &desc) == 0) {
                ESP_LOGI(TAG, "connection interval %u.%02u ms", desc.conn_itvl * 125 / 100,
                         desc.conn_itvl * 125 % 100);
            }
        }
        break;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU %d", event->mtu.value);
        break;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        break;

    default:
        break;
    }
    return 0;
}

// The 128-bit service UUID fills most of the advertising packet, so the name
// goes in the scan response.
static void advertise(void)
{
    struct ble_hs_adv_fields adv = {
        .flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP,
        .uuids128 = &NUS_SVC,
        .num_uuids128 = 1,
        .uuids128_is_complete = 1,
    };
    struct ble_hs_adv_fields rsp = {
        .name = (const uint8_t *)s_name,
        .name_len = strlen(s_name),
        .name_is_complete = 1,
    };
    int rc = ble_gap_adv_set_fields(&adv);
    if (rc == 0) {
        rc = ble_gap_adv_rsp_set_fields(&rsp);
    }
    const struct ble_gap_adv_params params = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
    };
    if (rc == 0) {
        rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &params, gap_event, NULL);
    }
    if (rc == 0 || rc == BLE_HS_EALREADY) {
        s_advertising = true;
    } else {
        ESP_LOGE(TAG, "advertising failed: %d", rc);
    }
}

static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &s_own_addr_type);
    ble_gap_write_sugg_def_data_len(BLE_HCI_SET_DATALEN_TX_OCTETS_MAX, BLE_HCI_SET_DATALEN_TX_TIME_MAX);
    advertise();
    ESP_LOGI(TAG, "advertising as \"%s\"", s_name);
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "host reset: %d", reason);
}

static void host_task(void *arg)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void ble_serial_start(void)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_name, sizeof(s_name), "Betaflight Bridge %02x%02x%02x", mac[3], mac[4], mac[5]);

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NimBLE init failed: %s", esp_err_to_name(err));
        return;
    }
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    int rc = ble_gatts_count_cfg(s_services);
    if (rc == 0) {
        rc = ble_gatts_add_svcs(s_services);
    }
    if (rc == 0) {
        rc = ble_svc_gap_device_name_set(s_name);
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "GATT setup failed: %d", rc);
        return;
    }
    nimble_port_freertos_init(host_task);
}

void ble_serial_send(const uint8_t *data, size_t len)
{
    const uint32_t gen = s_conn_gen;
    while (len > 0 && gen == s_conn_gen) {
        const uint16_t conn = s_conn;
        const uint16_t handle = s_tx_subscribed ? s_tx_handle : s_rx_subscribed ? s_rx_handle : 0;
        if (conn == BLE_HS_CONN_HANDLE_NONE || handle == 0) {
            return;
        }
        const uint16_t mtu = ble_att_mtu(conn);
        if (mtu <= ATT_HEADER_LEN) {
            return;
        }
        size_t chunk = mtu - ATT_HEADER_LEN;
        if (chunk > len) {
            chunk = len;
        }

        int rc = BLE_HS_ENOMEM;
        for (int tries = 0; rc == BLE_HS_ENOMEM && tries < TX_RETRY_LIMIT && gen == s_conn_gen; tries++) {
            struct os_mbuf *om = ble_hs_mbuf_from_flat(data, chunk);
            rc = om ? ble_gatts_notify_custom(conn, handle, om) : BLE_HS_ENOMEM;
            if (rc == BLE_HS_ENOMEM) {
                vTaskDelay(pdMS_TO_TICKS(TX_RETRY_MS));
            }
        }
        if (rc != 0) {
            if (gen == s_conn_gen) {
                ESP_LOGW(TAG, "notify failed: %d; dropping client", rc);
                terminate();
            }
            return;
        }
        data += chunk;
        len -= chunk;
    }
}

void ble_serial_kick(void)
{
    terminate();
}

ble_serial_state_t ble_serial_state(void)
{
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) {
        return BLE_SERIAL_CONNECTED;
    }
    return s_advertising ? BLE_SERIAL_ADVERTISING : BLE_SERIAL_OFF;
}

const char *ble_serial_name(void)
{
    return s_name;
}

void ble_serial_poll(void)
{
    if (s_conn != BLE_HS_CONN_HANDLE_NONE && s_claimed && !s_terminating &&
        bridge_client_owner() != BRIDGE_CLIENT_BLE) {
        ESP_LOGI(TAG, "superseded; dropping client");
        terminate();
    }
}

#else

void ble_serial_start(void)
{
}

void ble_serial_kick(void)
{
}

ble_serial_state_t ble_serial_state(void)
{
    return BLE_SERIAL_OFF;
}

const char *ble_serial_name(void)
{
    return "";
}

void ble_serial_send(const uint8_t *data, size_t len)
{
}

void ble_serial_poll(void)
{
}

#endif
