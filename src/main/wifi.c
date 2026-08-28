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

#include "wifi.h"
#include "bridge_mdns.h"

#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
#include "lwip/inet.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "wifi";

#define NVS_NAMESPACE   "wifi"
#define STA_MAX_RETRY   5

static esp_netif_t *s_ap_netif;
static esp_netif_t *s_sta_netif;

// Shared station state, written from the WiFi/IP event task and read by the
// HTTP task; guarded by a mutex.
static SemaphoreHandle_t s_lock;
static wifi_sta_state_t  s_sta_state = WIFI_STA_IDLE;
static bool              s_sta_configured;   // creds set => auto-connect on START
static bool              s_sta_started;      // STA interface is up
static bool              s_ap_active;        // SoftAP is broadcasting
static bool              s_ever_connected;   // got an IP at least once this boot
static int               s_sta_retries;
static char              s_sta_ssid[33];     // configured target
static char              s_sta_ip[16];       // assigned IP when connected
static char              s_sta_gw[16];       // gateway when connected
static char              s_sta_netmask[16];  // netmask when connected

static inline void lock(void)   { xSemaphoreTake(s_lock, portMAX_DELAY); }
static inline void unlock(void) { xSemaphoreGive(s_lock); }

static void start_ap(void);
static void configure_ap_dhcp(esp_netif_t *ap);

// Bring the fallback SoftAP up (idempotent). Used when no creds are stored or
// the stored network is unreachable, so the web UI is always reachable.
static void ensure_ap(void)
{
    lock();
    bool already = s_ap_active;
    s_ap_active = true;
    unlock();
    if (already) {
        return;
    }
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));   // add AP alongside STA
    start_ap();                                            // AP iface now valid
    configure_ap_dhcp(s_ap_netif);
    ESP_LOGI(TAG, "SoftAP '%s' enabled for configuration", WIFI_AP_SSID);
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START: {
            lock();
            s_sta_started = true;
            bool connect = s_sta_configured;
            unlock();
            if (connect) {
                esp_wifi_connect();   // join the configured network
            }
            break;
        }
        case WIFI_EVENT_STA_STOP:
            lock();
            s_sta_started = false;
            unlock();
            break;
        case WIFI_EVENT_STA_DISCONNECTED: {
            // Decide under the lock, act after releasing it.
            enum { RETRY, KEEP_TRYING, FALLBACK_AP, STOP } action;
            lock();
            if (!s_sta_configured) {
                s_sta_state = WIFI_STA_IDLE;
                action = STOP;
            } else if (s_sta_retries++ < STA_MAX_RETRY) {
                s_sta_state = WIFI_STA_CONNECTING;
                action = RETRY;
            } else if (s_ever_connected) {
                // Known network briefly dropped: keep trying, don't raise the AP.
                s_sta_retries = 0;
                s_sta_state = WIFI_STA_CONNECTING;
                s_sta_ip[0] = s_sta_gw[0] = s_sta_netmask[0] = '\0';
                action = KEEP_TRYING;
            } else {
                // Never reached this network: bring up the AP so it can be fixed.
                s_sta_state = WIFI_STA_FAILED;
                s_sta_ip[0] = s_sta_gw[0] = s_sta_netmask[0] = '\0';
                action = FALLBACK_AP;
            }
            unlock();

            switch (action) {
            case RETRY:
                ESP_LOGW(TAG, "STA disconnected, retry %d", s_sta_retries);
                esp_wifi_connect();
                break;
            case KEEP_TRYING:
                esp_wifi_connect();
                break;
            case FALLBACK_AP:
                ESP_LOGW(TAG, "network '%s' unreachable; enabling AP for setup", s_sta_ssid);
                ensure_ap();
                break;
            case STOP:
                break;
            }
            break;
        }
        case WIFI_EVENT_AP_STACONNECTED:
            ESP_LOGI(TAG, "client joined AP");
            break;
        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        lock();
        s_sta_retries = 0;
        s_ever_connected = true;
        s_sta_state = WIFI_STA_CONNECTED;
        snprintf(s_sta_ip, sizeof(s_sta_ip), IPSTR, IP2STR(&event->ip_info.ip));
        snprintf(s_sta_gw, sizeof(s_sta_gw), IPSTR, IP2STR(&event->ip_info.gw));
        snprintf(s_sta_netmask, sizeof(s_sta_netmask), IPSTR, IP2STR(&event->ip_info.netmask));
        unlock();
        ESP_LOGI(TAG, "STA got IP " IPSTR " (port 5761)", IP2STR(&event->ip_info.ip));
    }
}

// Returns true and fills ssid/pass (NUL-terminated) if station creds exist.
static bool load_sta_creds(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    bool ok = nvs_get_str(h, "ssid", ssid, &ssid_len) == ESP_OK &&
              nvs_get_str(h, "pass", pass, &pass_len) == ESP_OK &&
              ssid[0] != '\0';
    nvs_close(h);
    return ok;
}

static void save_sta_creds(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    ESP_ERROR_CHECK(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h));
    if (ssid && ssid[0]) {
        ESP_ERROR_CHECK(nvs_set_str(h, "ssid", ssid));
        ESP_ERROR_CHECK(nvs_set_str(h, "pass", pass ? pass : ""));
    } else {
        nvs_erase_key(h, "ssid");
        nvs_erase_key(h, "pass");
    }
    ESP_ERROR_CHECK(nvs_commit(h));
    nvs_close(h);
}

// Push credentials into the STA config and (re)start association. Called both
// at boot (creds from NVS) and live (creds from the web form).
static void apply_sta_config(const char *ssid, const char *pass)
{
    wifi_config_t sta = {0};
    strlcpy((char *)sta.sta.ssid, ssid, sizeof(sta.sta.ssid));
    strlcpy((char *)sta.sta.password, pass ? pass : "", sizeof(sta.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));

    lock();
    strlcpy(s_sta_ssid, ssid, sizeof(s_sta_ssid));
    s_sta_configured = true;
    s_sta_retries = 0;
    s_sta_state = WIFI_STA_CONNECTING;
    s_sta_ip[0] = '\0';
    bool started = s_sta_started;
    unlock();

    // If the interface is already up (live reconfigure) kick a reconnect now;
    // otherwise WIFI_EVENT_STA_START will connect once it comes up.
    if (started) {
        esp_wifi_disconnect();
        esp_wifi_connect();
    }
    ESP_LOGI(TAG, "joining station network '%s'", ssid);
}

static void start_ap(void)
{
    wifi_config_t ap = {
        .ap = {
            .channel = WIFI_AP_CHANNEL,
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    strlcpy((char *)ap.ap.ssid, WIFI_AP_SSID, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = strlen(WIFI_AP_SSID);
    strlcpy((char *)ap.ap.password, WIFI_AP_PASS, sizeof(ap.ap.password));
    if (strlen(WIFI_AP_PASS) == 0) {
        ap.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_LOGI(TAG, "SoftAP '%s' up at %s", WIFI_AP_SSID, WIFI_AP_IP);
}

#define WARN_ON_ERR(expr) do { \
    esp_err_t _e = (expr); \
    if (_e != ESP_OK) ESP_LOGW(TAG, "%s -> %s", #expr, esp_err_to_name(_e)); \
} while (0)

// Pin the AP subnet and (re)assert the built-in DHCP server so every client
// gets a 192.168.4.x lease with this device as gateway and DNS. IDF starts a
// DHCP server on the default AP netif automatically; we configure it explicitly
// so the addressing is intentional and easy to change.
static void configure_ap_dhcp(esp_netif_t *ap)
{
    esp_netif_ip_info_t ip = {0};
    ip.ip.addr      = ipaddr_addr(WIFI_AP_IP);
    ip.gw.addr      = ipaddr_addr(WIFI_AP_IP);
    ip.netmask.addr = ipaddr_addr("255.255.255.0");

    // DHCP server must be stopped to change the interface address.
    WARN_ON_ERR(esp_netif_dhcps_stop(ap));   // may already be stopped pre-start
    ESP_ERROR_CHECK(esp_netif_set_ip_info(ap, &ip));

    // Hand the device itself out as DNS so a captive name could resolve later.
    esp_netif_dns_info_t dns = {0};
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    dns.ip.u_addr.ip4.addr = ipaddr_addr(WIFI_AP_IP);
    uint8_t offer_dns = 0x02;   // DHCPS_OFFER_DNS
    WARN_ON_ERR(esp_netif_dhcps_option(ap, ESP_NETIF_OP_SET,
                ESP_NETIF_DOMAIN_NAME_SERVER, &offer_dns, sizeof(offer_dns)));
    WARN_ON_ERR(esp_netif_set_dns_info(ap, ESP_NETIF_DNS_MAIN, &dns));

    WARN_ON_ERR(esp_netif_dhcps_start(ap));
    ESP_LOGI(TAG, "AP DHCP server serving leases; gateway/DNS %s", WIFI_AP_IP);
}

void wifi_start(void)
{
    s_lock = xSemaphoreCreateMutex();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &on_wifi_event, NULL, NULL));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    // Modem sleep (the default) drops multicast between DTIM beacons, so mDNS
    // queries go unanswered for seconds at a time and browsers expire the
    // bridge. The board is USB powered; keep the radio awake.
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    // Both interfaces exist up front: the STA must be present for scanning and
    // live joins, and the AP netif must exist so the fallback can raise it. The
    // AP only *broadcasts* once the mode includes it (see ensure_ap()).
    s_ap_netif  = esp_netif_create_default_wifi_ap();
    s_sta_netif = esp_netif_create_default_wifi_sta();
    bridge_mdns_start();

    char ssid[33] = {0};
    char pass[65] = {0};
    bool have_creds = load_sta_creds(ssid, sizeof(ssid), pass, sizeof(pass));

    if (have_creds) {
        // Station-first: join the stored network, no AP. If it proves
        // unreachable, the disconnect handler raises the AP as a fallback.
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        wifi_config_t sta = {0};
        strlcpy((char *)sta.sta.ssid, ssid, sizeof(sta.sta.ssid));
        strlcpy((char *)sta.sta.password, pass, sizeof(sta.sta.password));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
        strlcpy(s_sta_ssid, ssid, sizeof(s_sta_ssid));
        s_sta_configured = true;
        s_sta_state = WIFI_STA_CONNECTING;
        ESP_ERROR_CHECK(esp_wifi_start());
        ESP_LOGI(TAG, "joining stored network '%s' (AP stays down unless unreachable)", ssid);
    } else {
        // No stored network: come up in AP+STA so the user can scan and join.
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
        start_ap();
        configure_ap_dhcp(s_ap_netif);
        s_ap_active = true;
        ESP_ERROR_CHECK(esp_wifi_start());
        ESP_LOGI(TAG, "no stored network; SoftAP '%s' up for setup", WIFI_AP_SSID);
    }
}

void wifi_set_station(const char *ssid, const char *pass)
{
    save_sta_creds(ssid, pass);

    if (ssid && ssid[0]) {
        apply_sta_config(ssid, pass);
    } else {
        // Clear: stop trying to associate and make sure the AP is up so the
        // device stays reachable for reconfiguration.
        lock();
        s_sta_configured = false;
        s_sta_state = WIFI_STA_IDLE;
        s_sta_ssid[0] = s_sta_ip[0] = s_sta_gw[0] = s_sta_netmask[0] = '\0';
        unlock();
        esp_wifi_disconnect();
        ensure_ap();
        ESP_LOGI(TAG, "station creds cleared; AP available for setup");
    }
}

void wifi_sta_status(wifi_status_t *out)
{
    lock();
    out->state = s_sta_state;
    out->ap_active = s_ap_active;
    strlcpy(out->ssid, s_sta_ssid, sizeof(out->ssid));
    strlcpy(out->ip, s_sta_ip, sizeof(out->ip));
    strlcpy(out->gw, s_sta_gw, sizeof(out->gw));
    strlcpy(out->netmask, s_sta_netmask, sizeof(out->netmask));
    unlock();

    // Live signal strength of the associated AP. Queried outside the lock; only
    // valid while connected, otherwise reported as 0.
    out->rssi = 0;
    if (out->state == WIFI_STA_CONNECTED) {
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            out->rssi = ap.rssi;
        }
    }
}

int wifi_scan(wifi_scan_ap_t *out, int max)
{
    if (max <= 0) {
        return 0;
    }

    wifi_scan_config_t sc = { .show_hidden = false };
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) {   // blocking
        ESP_LOGW(TAG, "scan failed to start");
        return 0;
    }

    uint16_t found = 0;
    esp_wifi_scan_get_ap_num(&found);
    if (found == 0) {
        return 0;
    }

    wifi_ap_record_t *recs = calloc(found, sizeof(*recs));
    if (!recs) {
        esp_wifi_clear_ap_list();
        return 0;
    }
    esp_wifi_scan_get_ap_records(&found, recs);   // frees the internal list

    int count = 0;
    for (int i = 0; i < found && count < max; i++) {
        const char *ssid = (const char *)recs[i].ssid;
        if (ssid[0] == '\0') {
            continue;   // hidden
        }
        bool dup = false;
        for (int j = 0; j < count; j++) {
            if (strcmp(out[j].ssid, ssid) == 0) { dup = true; break; }
        }
        if (dup) {
            continue;   // keep the strongest (records are RSSI-sorted)
        }
        strlcpy(out[count].ssid, ssid, sizeof(out[count].ssid));
        out[count].rssi = recs[i].rssi;
        out[count].secure = recs[i].authmode != WIFI_AUTH_OPEN;
        count++;
    }
    free(recs);
    return count;
}
