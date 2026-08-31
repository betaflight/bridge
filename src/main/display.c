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

#include "display.h"
#include "sdkconfig.h"

#if CONFIG_BRIDGE_DISPLAY

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"

#include "bsp/esp32_s3_touch_lcd_4b.h"
#include "lvgl.h"

#include "wifi.h"
#include "usb_cdc_host.h"
#include "tcp_server.h"
#include "bridge.h"
#include "ws_serial.h"
#include "ota.h"
#include "version.h"

static const char *TAG = "display";

// Palette lifted from the web UI (http_status.c PAGE css).
#define COL_BG      0x0f1115
#define COL_CARD    0x181b20
#define COL_BORDER  0x262b32
#define COL_TEXT    0xe6e8ea
#define COL_KEY     0x8b9199
#define COL_UP      0x46c66d
#define COL_DOWN    0x6b7178
#define COL_WARN    0xFFBB00
#define COL_ACCENT  0xFFBB00
#define COL_MONO    0x7fc7ff

#define REFRESH_MS      500
#define DISPLAY_MAX_APS 20

// Status tab value labels, updated by the refresh timer.
static lv_obj_t *s_val_fc;
static lv_obj_t *s_val_cfg;
static lv_obj_t *s_val_sta;
static lv_obj_t *s_val_rssi;
static lv_obj_t *s_val_ip;
static lv_obj_t *s_val_url;
static lv_obj_t *s_val_gw;
static lv_obj_t *s_val_mask;
static lv_obj_t *s_val_ap;
static lv_obj_t *s_val_slot;

// WiFi tab.
static lv_obj_t *s_ap_list;
static lv_obj_t *s_spinner;
static lv_obj_t *s_modal;         // join dialog overlay (NULL when closed)

static SemaphoreHandle_t s_scan_req;
static wifi_scan_ap_t s_aps[DISPLAY_MAX_APS];
static int s_ap_count;

static void set_value(lv_obj_t *label, const char *text, uint32_t colour)
{
    if (strcmp(lv_label_get_text(label), text) != 0) {
        lv_label_set_text(label, text);
    }
    lv_obj_set_style_text_color(label, lv_color_hex(colour), 0);
}

static void refresh_cb(lv_timer_t *timer)
{
    (void)timer;
    char buf[64];

    uint16_t vid = 0, pid = 0;
    bool usb = usb_cdc_host_status(&vid, &pid);
    if (usb) {
        snprintf(buf, sizeof(buf), "connected %04x:%04x", vid, pid);
        set_value(s_val_fc, buf, COL_UP);
    } else {
        set_value(s_val_fc, "waiting...", COL_DOWN);
    }

    bridge_client_t owner = bridge_client_owner();
    if (owner == BRIDGE_CLIENT_TCP) {
        snprintf(buf, sizeof(buf), "connected TCP :%d", TCP_SERVER_PORT);
        set_value(s_val_cfg, buf, COL_UP);
    } else if (owner == BRIDGE_CLIENT_WS) {
        snprintf(buf, sizeof(buf), "connected WebSocket (%s)", ws_serial_is_secure() ? "wss" : "ws");
        set_value(s_val_cfg, buf, COL_UP);
    } else {
        set_value(s_val_cfg, "none", COL_DOWN);
    }

    wifi_status_t w;
    wifi_sta_status(&w);
    if (w.state == WIFI_STA_CONNECTED) {
        set_value(s_val_sta, w.ssid, COL_UP);
    } else if (w.state == WIFI_STA_CONNECTING) {
        snprintf(buf, sizeof(buf), "connecting to %s...", w.ssid);
        set_value(s_val_sta, buf, COL_WARN);
    } else if (w.state == WIFI_STA_FAILED) {
        snprintf(buf, sizeof(buf), "failed: %s", w.ssid);
        set_value(s_val_sta, buf, COL_WARN);
    } else {
        set_value(s_val_sta, "none", COL_DOWN);
    }

    if (w.state == WIFI_STA_CONNECTED && w.rssi) {
        // Same quality thresholds as the web UI.
        const char *q = w.rssi >= -60 ? "good" : w.rssi >= -72 ? "fair" : "weak";
        snprintf(buf, sizeof(buf), "%s %d dBm", q, w.rssi);
        set_value(s_val_rssi, buf, w.rssi >= -60 ? COL_UP : COL_WARN);
    } else {
        set_value(s_val_rssi, "-", COL_DOWN);
    }

    set_value(s_val_ip, w.ip[0] ? w.ip : "-", w.ip[0] ? COL_MONO : COL_DOWN);
    if (w.ip[0]) {
        snprintf(buf, sizeof(buf), "http://%s", w.ip);
        set_value(s_val_url, buf, COL_MONO);
    } else if (w.ap_active) {
        set_value(s_val_url, "http://" WIFI_AP_IP, COL_MONO);
    } else {
        set_value(s_val_url, "-", COL_DOWN);
    }
    set_value(s_val_gw, w.gw[0] ? w.gw : "-", w.gw[0] ? COL_MONO : COL_DOWN);
    set_value(s_val_mask, w.netmask[0] ? w.netmask : "-", w.netmask[0] ? COL_MONO : COL_DOWN);

    if (w.ap_active) {
        set_value(s_val_ap, "broadcasting (setup mode)", COL_WARN);
    } else {
        set_value(s_val_ap, "off", COL_DOWN);
    }

    char slot[16];
    bool valid = true;
    ota_running_info(slot, sizeof(slot), &valid);
    snprintf(buf, sizeof(buf), "%s %s", slot, valid ? "valid" : "pending verify");
    set_value(s_val_slot, buf, valid ? COL_UP : COL_WARN);
}

// ---------------------------------------------------------------- WiFi tab

static void scan_busy(bool busy)
{
    if (busy) {
        lv_obj_remove_flag(s_spinner, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_spinner, LV_OBJ_FLAG_HIDDEN);
    }
}

static void modal_close(void)
{
    if (s_modal) {
        lv_obj_delete(s_modal);
        s_modal = NULL;
    }
}

static void join_selected(lv_event_t *e)
{
    lv_obj_t *ta = lv_event_get_user_data(e);
    const char *ssid = lv_obj_get_user_data(ta);
    wifi_set_station(ssid, lv_textarea_get_text(ta));
    modal_close();
}

static void modal_cancel(lv_event_t *e)
{
    (void)e;
    modal_close();
}

// Full-screen password prompt for a secured network: SSID title, password
// box, Join/Cancel, and a keyboard docked to the bottom half.
static void modal_open(const wifi_scan_ap_t *ap)
{
    modal_close();

    s_modal = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_modal, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_modal, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_border_width(s_modal, 0, 0);
    lv_obj_set_style_radius(s_modal, 0, 0);
    lv_obj_set_style_pad_all(s_modal, 12, 0);
    lv_obj_remove_flag(s_modal, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(s_modal);
    lv_label_set_text_fmt(title, "Join %s", ap->ssid);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(title, LV_PCT(100));
    lv_obj_set_style_text_color(title, lv_color_hex(COL_ACCENT), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 4);

    lv_obj_t *ta = lv_textarea_create(s_modal);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_password_mode(ta, true);
    lv_textarea_set_placeholder_text(ta, "Password");
    lv_obj_set_width(ta, LV_PCT(100));
    lv_obj_align(ta, LV_ALIGN_TOP_LEFT, 0, 36);
    // The SSID rides along on the textarea so the join callback has both.
    static char ssid_buf[33];
    strlcpy(ssid_buf, ap->ssid, sizeof(ssid_buf));
    lv_obj_set_user_data(ta, ssid_buf);

    lv_obj_t *join = lv_button_create(s_modal);
    lv_obj_align(join, LV_ALIGN_TOP_LEFT, 0, 92);
    lv_obj_set_style_bg_color(join, lv_color_hex(COL_ACCENT), 0);
    lv_obj_t *jl = lv_label_create(join);
    lv_label_set_text(jl, "Join");
    lv_obj_set_style_text_color(jl, lv_color_hex(0x15140e), 0);
    lv_obj_add_event_cb(join, join_selected, LV_EVENT_CLICKED, ta);

    lv_obj_t *cancel = lv_button_create(s_modal);
    lv_obj_align(cancel, LV_ALIGN_TOP_LEFT, 110, 92);
    lv_obj_set_style_bg_color(cancel, lv_color_hex(0x222831), 0);
    lv_obj_t *cl = lv_label_create(cancel);
    lv_label_set_text(cl, "Cancel");
    lv_obj_add_event_cb(cancel, modal_cancel, LV_EVENT_CLICKED, NULL);

    lv_obj_t *kb = lv_keyboard_create(s_modal);
    lv_keyboard_set_textarea(kb, ta);
    lv_obj_set_size(kb, LV_PCT(100), LV_PCT(45));
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_event_cb(kb, join_selected, LV_EVENT_READY, ta);   // keyboard tick
    lv_obj_add_event_cb(kb, modal_cancel, LV_EVENT_CANCEL, NULL); // keyboard cross
}

static void ap_clicked(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= s_ap_count) {
        return;
    }
    if (s_aps[idx].secure) {
        modal_open(&s_aps[idx]);
    } else {
        wifi_set_station(s_aps[idx].ssid, "");
    }
}

// Rebuild the AP list from s_aps. Caller holds the display lock.
static void scan_list_rebuild(void)
{
    lv_obj_clean(s_ap_list);
    if (s_ap_count == 0) {
        lv_obj_t *none = lv_list_add_text(s_ap_list, "no networks found");
        lv_obj_set_style_text_color(none, lv_color_hex(COL_DOWN), 0);
        return;
    }
    for (int i = 0; i < s_ap_count; i++) {
        char item[64];
        snprintf(item, sizeof(item), "%s  %s%d dBm",
                 s_aps[i].ssid, s_aps[i].secure ? LV_SYMBOL_EYE_CLOSE " " : "", s_aps[i].rssi);
        lv_obj_t *btn = lv_list_add_button(s_ap_list, LV_SYMBOL_WIFI, item);
        lv_obj_set_style_bg_color(btn, lv_color_hex(COL_CARD), 0);
        lv_obj_set_style_text_color(btn, lv_color_hex(COL_TEXT), 0);
        lv_obj_add_event_cb(btn, ap_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
}

// wifi_scan() blocks for seconds — run it off the LVGL task and repopulate
// the list under the display lock. One automatic scan shortly after boot,
// then one per Rescan press. Scans land in a staging buffer: s_aps is only
// written under the display lock, where ap_clicked() also reads it (LVGL
// event callbacks run under the same mutex).
static void scan_task(void *arg)
{
    (void)arg;
    static wifi_scan_ap_t aps[DISPLAY_MAX_APS];
    vTaskDelay(pdMS_TO_TICKS(2000));
    for (;;) {
        int n = wifi_scan(aps, DISPLAY_MAX_APS);
        bsp_display_lock(0);
        s_ap_count = n;
        memcpy(s_aps, aps, sizeof(aps));
        scan_list_rebuild();
        scan_busy(false);
        bsp_display_unlock();
        xSemaphoreTake(s_scan_req, portMAX_DELAY);
    }
}

static void rescan_clicked(lv_event_t *e)
{
    (void)e;
    scan_busy(true);
    xSemaphoreGive(s_scan_req);
}

static void forget_clicked(lv_event_t *e)
{
    (void)e;
    wifi_set_station("", "");
}

// ------------------------------------------------------------- UI assembly

static lv_obj_t *add_row(lv_obj_t *parent, const char *key)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_bottom(row, 8, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *k = lv_label_create(row);
    lv_label_set_text(k, key);
    lv_obj_set_width(k, LV_PCT(42));
    lv_obj_set_style_text_color(k, lv_color_hex(COL_KEY), 0);
    lv_obj_align(k, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *v = lv_label_create(row);
    lv_label_set_text(v, "...");
    lv_label_set_long_mode(v, LV_LABEL_LONG_DOT);
    lv_obj_set_width(v, LV_PCT(56));
    lv_obj_set_style_text_color(v, lv_color_hex(COL_TEXT), 0);
    lv_obj_align(v, LV_ALIGN_TOP_RIGHT, 0, 0);
    return v;
}

static void build_status_tab(lv_obj_t *tab)
{
    lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(tab, 14, 0);
    lv_obj_set_style_pad_row(tab, 2, 0);

    s_val_fc   = add_row(tab, "FC (USB VCP)");
    s_val_cfg  = add_row(tab, "Configurator");
    s_val_sta  = add_row(tab, "WiFi network");
    s_val_rssi = add_row(tab, "Signal");
    s_val_ip   = add_row(tab, "IP address");
    s_val_url  = add_row(tab, "Web UI");
    s_val_gw   = add_row(tab, "Gateway");
    s_val_mask = add_row(tab, "Netmask");
    s_val_ap   = add_row(tab, "Access point");
    lv_obj_t *board = add_row(tab, "Board");
    set_value(board, ota_board_id(), COL_MONO);
    s_val_slot = add_row(tab, "Firmware slot");

    lv_obj_t *ver = lv_label_create(tab);
    lv_label_set_text(ver, "betaflight-bridge " BRIDGE_VERSION);
    lv_obj_set_style_text_color(ver, lv_color_hex(COL_KEY), 0);

    lv_timer_create(refresh_cb, REFRESH_MS, NULL);
    refresh_cb(NULL);
}

static void build_wifi_tab(lv_obj_t *tab)
{
    lv_obj_set_style_pad_all(tab, 14, 0);

    lv_obj_t *rescan = lv_button_create(tab);
    lv_obj_align(rescan, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(rescan, lv_color_hex(COL_ACCENT), 0);
    lv_obj_t *rl = lv_label_create(rescan);
    lv_label_set_text(rl, LV_SYMBOL_REFRESH " Rescan");
    lv_obj_set_style_text_color(rl, lv_color_hex(0x15140e), 0);
    lv_obj_add_event_cb(rescan, rescan_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *forget = lv_button_create(tab);
    lv_obj_align(forget, LV_ALIGN_TOP_LEFT, 130, 0);
    lv_obj_set_style_bg_color(forget, lv_color_hex(0x222831), 0);
    lv_obj_t *fl = lv_label_create(forget);
    lv_label_set_text(fl, LV_SYMBOL_TRASH " Forget");
    lv_obj_add_event_cb(forget, forget_clicked, LV_EVENT_CLICKED, NULL);

    s_spinner = lv_spinner_create(tab);
    lv_obj_set_size(s_spinner, 32, 32);
    lv_obj_align(s_spinner, LV_ALIGN_TOP_RIGHT, 0, 2);

    s_ap_list = lv_list_create(tab);
    lv_obj_set_size(s_ap_list, LV_PCT(100), LV_PCT(84));
    lv_obj_align(s_ap_list, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(s_ap_list, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_border_color(s_ap_list, lv_color_hex(COL_BORDER), 0);
    lv_obj_t *hint = lv_list_add_text(s_ap_list, "scanning...");
    lv_obj_set_style_text_color(hint, lv_color_hex(COL_KEY), 0);
}

static void build_ui(void)
{
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(COL_BG), 0);

    lv_obj_t *tv = lv_tabview_create(screen);
    lv_tabview_set_tab_bar_size(tv, 52);
    lv_obj_set_style_bg_color(tv, lv_color_hex(COL_BG), 0);

    lv_obj_t *bar = lv_tabview_get_tab_bar(tv);
    lv_obj_set_style_bg_color(bar, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_text_color(bar, lv_color_hex(COL_TEXT), 0);

    lv_obj_t *tab_status = lv_tabview_add_tab(tv, "Status");
    lv_obj_t *tab_wifi   = lv_tabview_add_tab(tv, "WiFi");
    lv_obj_set_style_bg_color(tab_status, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_color(tab_wifi, lv_color_hex(COL_BG), 0);

    build_status_tab(tab_status);
    build_wifi_tab(tab_wifi);
}

void display_start(void)
{
    if (bsp_display_start() == NULL) {
        ESP_LOGE(TAG, "display init failed");
        return;
    }

    bsp_display_lock(0);
    lv_theme_t *theme = lv_theme_default_init(lv_display_get_default(),
                                              lv_color_hex(COL_ACCENT),
                                              lv_color_hex(COL_CARD),
                                              true, &lv_font_montserrat_16);
    lv_display_set_theme(lv_display_get_default(), theme);
    build_ui();
    bsp_display_unlock();

    s_scan_req = xSemaphoreCreateBinary();
    configASSERT(s_scan_req);
    BaseType_t ok = xTaskCreate(scan_task, "wifi_scan", 4096, NULL, 3, NULL);
    configASSERT(ok == pdTRUE);
    // Light the panel only after the first UI is in the framebuffer.
    bsp_display_backlight_on();

    ESP_LOGI(TAG, "display ready");
}

#elif CONFIG_BRIDGE_HGLRC_DISPLAY

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "lvgl.h"

#include "lcd_st7789.h"
#include "hglrc_logo.h"
#include "adc_voltage.h"
#include "wifi.h"
#include "usb_cdc_host.h"
#include "tcp_server.h"
#include "bridge.h"
#include "ws_serial.h"

LV_FONT_DECLARE(ui_font_size14);
LV_FONT_DECLARE(ui_font_size24);

static const char *TAG = "hglrc_display";

#define COL_BG      0x0f1115
#define COL_CARD    0x181b20
#define COL_BORDER  0x262b32
#define COL_TEXT    0xe6e8ea
#define COL_KEY     0x8b9199
#define COL_UP      0x46c66d
#define COL_DOWN    0x6b7178
#define COL_WARN    0xFFBB00
#define COL_ACCENT  0xFFBB00
#define COL_MONO    0x7fc7ff

#define REFRESH_MS 500

static lv_obj_t *s_compact_fc;
static lv_obj_t *s_compact_client;
static lv_obj_t *s_compact_wifi;
static lv_obj_t *s_compact_address[3];
static lv_obj_t *s_compact_address_key;
static lv_obj_t *s_compact_status;
static lv_obj_t *s_compact_voltage;
static lv_obj_t *s_compact_qr;
static lv_obj_t *s_compact_qr_hint;
static char s_compact_qr_url[64];

static void set_value(lv_obj_t *label, const char *text, uint32_t colour)
{
    if (strcmp(lv_label_get_text(label), text) != 0) {
        lv_label_set_text(label, text);
    }
    lv_obj_set_style_text_color(label, lv_color_hex(colour), 0);
}

static lv_obj_t *compact_value(lv_obj_t *parent, const char *key, int y)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_pos(label, 76, y);
    lv_obj_set_size(label, 98, 19);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(label, &ui_font_size14, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(COL_TEXT), 0);

    lv_obj_t *caption = lv_label_create(parent);
    lv_label_set_text(caption, key);
    lv_obj_set_pos(caption, 10, y);
    lv_obj_set_size(caption, 62, 19);
    lv_obj_set_style_text_font(caption, &ui_font_size14, 0);
    lv_obj_set_style_text_color(caption, lv_color_hex(COL_KEY), 0);
    if (strcmp(key, "IP") == 0) {
        s_compact_address_key = caption;
    }
    return label;
}

static lv_obj_t *compact_address_value(lv_obj_t *parent, int y)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_pos(label, 76, y);
    lv_obj_set_size(label, 108, 16);
    lv_label_set_long_mode(label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_font(label, &ui_font_size14, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(COL_TEXT), 0);
    return label;
}

static void compact_card(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_size(card, w, h);
    lv_obj_set_style_bg_color(card, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_border_color(card, lv_color_hex(COL_BORDER), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, 8, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
}

static void compact_refresh_cb(lv_timer_t *timer)
{
    (void)timer;
    char buf[64];

    uint16_t vid = 0;
    uint16_t pid = 0;
    if (usb_cdc_host_status(&vid, &pid)) {
        set_value(s_compact_fc, "CONNECTED", COL_UP);
    } else {
        set_value(s_compact_fc, "WAITING", COL_DOWN);
    }

    bridge_client_t owner = bridge_client_owner();
    if (owner == BRIDGE_CLIENT_TCP) {
        set_value(s_compact_client, "TCP", COL_UP);
    } else if (owner == BRIDGE_CLIENT_WS) {
        set_value(s_compact_client, ws_serial_is_secure() ? "WSS" : "WS", COL_UP);
    } else {
        set_value(s_compact_client, "NONE", COL_DOWN);
    }

    uint32_t voltage_mv = adc_voltage_get_mv();
    if (voltage_mv > 0) {
        uint32_t voltage_centi_v = (voltage_mv + 5) / 10;
        snprintf(buf, sizeof(buf), "%lu.%02luV",
                 (unsigned long)(voltage_centi_v / 100),
                 (unsigned long)(voltage_centi_v % 100));
        set_value(s_compact_voltage, buf, COL_MONO);
    } else {
        set_value(s_compact_voltage, "--.--V", COL_DOWN);
    }

    wifi_status_t w;
    wifi_sta_status(&w);

    if (w.ap_active) {
        set_value(s_compact_status, "SETUP", COL_WARN);
        lv_obj_set_style_bg_color(s_compact_status, lv_color_hex(0x5A4300), 0);
    } else if (w.state == WIFI_STA_CONNECTED) {
        set_value(s_compact_status, "ONLINE", COL_UP);
        lv_obj_set_style_bg_color(s_compact_status, lv_color_hex(0x164B2A), 0);
    } else {
        set_value(s_compact_status, "OFFLINE", COL_DOWN);
        lv_obj_set_style_bg_color(s_compact_status, lv_color_hex(0x2A2E34), 0);
    }

    if (w.state == WIFI_STA_CONNECTED) {
        set_value(s_compact_wifi, w.ssid, COL_UP);
    } else if (w.state == WIFI_STA_CONNECTING) {
        set_value(s_compact_wifi, "CONNECTING", COL_WARN);
    } else if (w.ap_active) {
        set_value(s_compact_wifi, "SETUP AP", COL_WARN);
    } else {
        set_value(s_compact_wifi, "OFFLINE", COL_DOWN);
    }

    if (w.state == WIFI_STA_CONNECTED && w.ip[0]) {
        lv_label_set_text(s_compact_address_key, "PORT");
        snprintf(buf, sizeof(buf), "tcp://%s:%d", w.ip, TCP_SERVER_PORT);
        set_value(s_compact_address[0], buf, COL_MONO);
        snprintf(buf, sizeof(buf), "ws://%s/serial", w.ip);
        set_value(s_compact_address[1], buf, COL_MONO);
        snprintf(buf, sizeof(buf), "wss://%s/serial", w.ip);
        set_value(s_compact_address[2], buf, COL_MONO);

        snprintf(buf, sizeof(buf), "http://%s/", w.ip);
        if (strcmp(s_compact_qr_url, buf) != 0) {
            strlcpy(s_compact_qr_url, buf, sizeof(s_compact_qr_url));
            lv_qrcode_set_data(s_compact_qr, s_compact_qr_url);
        }
        lv_label_set_text(s_compact_qr_hint, "scan to monitor");
        lv_obj_clear_flag(s_compact_qr, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_compact_qr_hint, LV_OBJ_FLAG_HIDDEN);
    } else {
        if (w.ap_active) {
            lv_label_set_text(s_compact_address_key, "IP");
            set_value(s_compact_address[0], WIFI_AP_IP, COL_MONO);

            snprintf(buf, sizeof(buf), "http://%s/", WIFI_AP_IP);
            if (strcmp(s_compact_qr_url, buf) != 0) {
                strlcpy(s_compact_qr_url, buf, sizeof(s_compact_qr_url));
                lv_qrcode_set_data(s_compact_qr, s_compact_qr_url);
            }
            lv_label_set_text(s_compact_qr_hint, "scan to setup");
            lv_obj_clear_flag(s_compact_qr, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(s_compact_qr_hint, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_label_set_text(s_compact_address_key, "IP");
            set_value(s_compact_address[0], "-", COL_DOWN);
            s_compact_qr_url[0] = '\0';
            lv_obj_add_flag(s_compact_qr, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_compact_qr_hint, LV_OBJ_FLAG_HIDDEN);
        }
        set_value(s_compact_address[1], "", COL_DOWN);
        set_value(s_compact_address[2], "", COL_DOWN);
    }
}

static void build_hglrc_ui(void)
{
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_pad_all(screen, 0, 0);

    lv_obj_t *header = lv_obj_create(screen);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_size(header, 320, 35);
    lv_obj_set_style_bg_color(header, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_border_side(header, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(header, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_border_width(header, 2, 0);
    lv_obj_set_style_pad_all(header, 0, 0);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *logo = lv_image_create(header);
    lv_image_set_src(logo, &hglrc_logo);
    lv_obj_set_style_image_recolor(logo, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_image_recolor_opa(logo, LV_OPA_COVER, 0);
    lv_obj_set_pos(logo, 10, 8);

    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text(title, "USB BRIDGE");
    lv_obj_set_size(title, 85, 20);
    lv_obj_set_pos(title, 91, 8);
    lv_label_set_long_mode(title, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(title, &ui_font_size14, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(COL_TEXT), 0);

    s_compact_voltage = lv_label_create(header);
    lv_label_set_text(s_compact_voltage, "--.--V");
    lv_obj_set_size(s_compact_voltage, 80, 20);
    lv_obj_set_pos(s_compact_voltage, 176, 4);
    lv_obj_set_style_text_font(s_compact_voltage, &ui_font_size24, 0);
    lv_label_set_long_mode(s_compact_voltage, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(s_compact_voltage, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_compact_voltage, lv_color_hex(COL_DOWN), 0);

    s_compact_status = lv_label_create(header);
    lv_label_set_text(s_compact_status, "OFFLINE");
    lv_obj_set_size(s_compact_status, 58, 20);
    lv_obj_set_pos(s_compact_status, 258, 7);
    lv_obj_set_style_text_font(s_compact_status, &ui_font_size14, 0);
    lv_obj_set_style_text_align(s_compact_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_compact_status, lv_color_hex(COL_DOWN), 0);
    lv_obj_set_style_bg_color(s_compact_status, lv_color_hex(0x2A2E34), 0);
    lv_obj_set_style_radius(s_compact_status, 10, 0);
    lv_obj_set_style_pad_top(s_compact_status, 3, 0);

    compact_card(screen, 8, 42, 180, 122);
    compact_card(screen, 198, 42, 114, 122);

    s_compact_fc = compact_value(screen, "FC", 48);
    s_compact_client = compact_value(screen, "CLIENT", 67);
    s_compact_wifi = compact_value(screen, "WIFI", 86);
    s_compact_address[0] = compact_value(screen, "IP", 105);
    lv_obj_set_pos(s_compact_address_key, 10, 105);
    lv_obj_set_size(s_compact_address_key, 62, 48);
    lv_obj_set_pos(s_compact_address[0], 76, 105);
    lv_obj_set_size(s_compact_address[0], 108, 16);
    lv_label_set_long_mode(s_compact_address[0], LV_LABEL_LONG_SCROLL_CIRCULAR);
    s_compact_address[1] = compact_address_value(screen, 121);
    s_compact_address[2] = compact_address_value(screen, 137);
    lv_label_set_long_mode(s_compact_wifi, LV_LABEL_LONG_SCROLL_CIRCULAR);

    s_compact_qr = lv_qrcode_create(screen);
    lv_qrcode_set_size(s_compact_qr, 84);
    lv_qrcode_set_dark_color(s_compact_qr, lv_color_black());
    lv_qrcode_set_light_color(s_compact_qr, lv_color_white());
    lv_qrcode_set_quiet_zone(s_compact_qr, false);
    lv_obj_set_pos(s_compact_qr, 213, 48);
    lv_obj_add_flag(s_compact_qr, LV_OBJ_FLAG_HIDDEN);

    s_compact_qr_hint = lv_label_create(screen);
    lv_label_set_text(s_compact_qr_hint, "scan to setup");
    lv_obj_set_pos(s_compact_qr_hint, 204, 139);
    lv_obj_set_size(s_compact_qr_hint, 102, 18);
    lv_obj_set_style_text_font(s_compact_qr_hint, &ui_font_size14, 0);
    lv_obj_set_style_text_align(s_compact_qr_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_compact_qr_hint, lv_color_hex(COL_KEY), 0);
    lv_obj_add_flag(s_compact_qr_hint, LV_OBJ_FLAG_HIDDEN);

    lv_timer_create(compact_refresh_cb, REFRESH_MS, NULL);
    compact_refresh_cb(NULL);
}

void display_start(void)
{
    if (bsp_display_start() == NULL) {
        ESP_LOGE(TAG, "display init failed");
        return;
    }

    bsp_display_lock(0);
    build_hglrc_ui();
    bsp_display_unlock();

    vTaskDelay(pdMS_TO_TICKS(200));
    bsp_display_backlight_on();
    ESP_LOGI(TAG, "display ready");
}

#else  // no display on this board

void display_start(void) { }

#endif
