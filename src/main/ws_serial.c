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

#include "ws_serial.h"
#include "bridge.h"
#include "tcp_server.h"

#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "ws";

// The single active WebSocket client, identified by its server handle + socket
// fd (an fd is only meaningful within its httpd instance, so we keep both).
static httpd_handle_t s_hd;
static volatile int s_fd = -1;
static volatile bool s_secure;   // current client arrived over TLS (wss)
static TaskHandle_t s_tx_task;

// Stable non-NULL marker for the TLS server's user_ctx (a real pointer, so no
// int-to-pointer cast); its address just has to be distinct from NULL.
static char s_secure_marker;

bool ws_serial_is_secure(void)
{
    // Only meaningful while a WS client owns the bridge; gate on ownership so a
    // stale flag from a closed wss session can't read back as secure.
    return bridge_client_owner() == BRIDGE_CLIENT_WS && s_secure;
}

void ws_serial_kick(void)
{
    int fd = s_fd;
    if (fd >= 0 && s_hd) {
        ESP_LOGI(TAG, "kicking client (fd %d)", fd);
        httpd_sess_trigger_close(s_hd, fd);
    }
}

static void ws_drop(void)
{
    s_fd = -1;
    s_hd = NULL;
    s_secure = false;
    // Only release if we still own it: a newer client may have taken over, in
    // which case the bridge is theirs and this is just a late socket close.
    if (bridge_client_owner() == BRIDGE_CLIENT_WS) {
        bridge_release(BRIDGE_CLIENT_WS);
    }
}

// Per-session marker so httpd tells us when the socket goes away for any reason
// (abrupt disconnect, TLS teardown), not only on a CLOSE frame. Without this an
// unclean drop leaves a dead client owning the bridge and locks out raw TCP.
typedef struct {
    httpd_handle_t hd;
    int fd;
} ws_session_t;

static void ws_session_closed(void *ctx)
{
    ws_session_t *sess = ctx;
    if (sess->hd == s_hd && sess->fd == s_fd) {
        ESP_LOGI(TAG, "client gone (fd %d)", sess->fd);
        ws_drop();
    }
    free(sess);
}

// Set to the fd we have already asked httpd to close, so we trigger it once.
static int s_closing_fd = -1;

// Send FC bytes to the connected WebSocket client. Only called by the pump while
// a WS client owns the bridge, so s_hd/s_fd are valid.
static void ws_send(const uint8_t *data, size_t len)
{
    httpd_ws_frame_t frame = {
        .final = true,
        .type = HTTPD_WS_TYPE_BINARY,
        .payload = (uint8_t *)data,
        .len = len,
    };
    if (httpd_ws_send_frame_async(s_hd, s_fd, &frame) != ESP_OK) {
        ESP_LOGW(TAG, "send failed; dropping client");
        ws_drop();
    }
}

// Single FC -> Configurator pump. It is the only consumer of the FC->net buffer,
// so there is never a second reader to race with; it routes each frame to the
// client that currently owns the bridge (or discards it when nobody does). A WS
// client that has just been superseded is closed here so the newest connection
// wins.
static void net_tx_task(void *arg)
{
    static uint8_t buf[1024];
    while (1) {
        // Close a superseded WS client (owner moved to TCP) once.
        if (s_fd >= 0 && bridge_client_owner() != BRIDGE_CLIENT_WS && s_fd != s_closing_fd) {
            ESP_LOGI(TAG, "superseded; dropping client (fd %d)", s_fd);
            s_closing_fd = s_fd;
            httpd_sess_trigger_close(s_hd, s_fd);
        }

        size_t n = bridge_usb_to_net_pop(buf, sizeof(buf), 100);
        if (n == 0) {
            continue;
        }
        switch (bridge_client_owner()) {
        case BRIDGE_CLIENT_TCP:
            tcp_server_send(buf, n);
            break;
        case BRIDGE_CLIENT_WS:
            ws_send(buf, n);
            break;
        default:
            break;   // no owner: discard
        }
    }
}

static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        // WebSocket handshake. The newest client wins, whatever transport the
        // current one is on (so a reconnect isn't locked out by a stale,
        // half-closed session).
        int new_fd = httpd_req_to_sockfd(req);
        ws_session_t *sess = malloc(sizeof(*sess));
        if (!sess) {
            return ESP_ERR_NO_MEM;
        }
        sess->hd = req->handle;
        sess->fd = new_fd;
        req->sess_ctx = sess;
        req->free_ctx = ws_session_closed;
        // Newest client wins. Adopt this session first so the predecessor's
        // teardown (gated on the active fd/owner) can't tear us down, then drop
        // the previous owner on whichever transport it was.
        httpd_handle_t prev_hd = s_hd;
        int prev_fd = s_fd;
        s_hd = req->handle;
        s_fd = new_fd;
        s_closing_fd = -1;
        if (prev_fd >= 0 && !(prev_hd == req->handle && prev_fd == new_fd)) {
            ESP_LOGI(TAG, "new client; dropping current WebSocket client");
            httpd_sess_trigger_close(prev_hd, prev_fd);
        }
        // Take the bridge over from any owner (incl. a TCP client). Its own task
        // notices the ownership change and drops it; we do not reach across
        // transports here, which would risk blocking the httpd worker.
        bridge_claim(BRIDGE_CLIENT_WS);
        s_secure = (req->user_ctx != NULL);   // set per-server at registration
        ESP_LOGI(TAG, "client connected (fd %d, %s)", new_fd, s_secure ? "wss" : "ws");
        return ESP_OK;
    }

    // A frame from the Configurator. Frames may arrive from a superseded session
    // (one we replaced via newest-wins but that hasn't finished closing) — those
    // must not touch the promoted client's bridge, so gate on the active session.
    bool active = (req->handle == s_hd && httpd_req_to_sockfd(req) == s_fd);

    // First call with len 0 gets the length.
    httpd_ws_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    frame.type = HTTPD_WS_TYPE_BINARY;
    esp_err_t ret = httpd_ws_recv_frame(req, &frame, 0);
    if (ret != ESP_OK) {
        return ret;
    }
    if (frame.type == HTTPD_WS_TYPE_CLOSE) {
        if (active) {
            ESP_LOGI(TAG, "client closed");
            ws_drop();
        }
        return ESP_OK;
    }
    if (frame.len) {
        uint8_t *payload = malloc(frame.len);
        if (!payload) {
            return ESP_ERR_NO_MEM;
        }
        frame.payload = payload;
        ret = httpd_ws_recv_frame(req, &frame, frame.len);
        if (ret == ESP_OK && active &&
            (frame.type == HTTPD_WS_TYPE_BINARY || frame.type == HTTPD_WS_TYPE_TEXT)) {
            bridge_net_to_usb_push(payload, frame.len);
        }
        free(payload);
    }
    return ret;
}

void ws_serial_register(httpd_handle_t server, bool secure)
{
    // user_ctx carries the secure flag so the handler can tell ws from wss.
    const httpd_uri_t uri = {
        .uri = "/serial",
        .method = HTTP_GET,
        .handler = ws_handler,
        .is_websocket = true,
        .handle_ws_control_frames = true,   // deliver CLOSE so we release the claim
        .supported_subprotocol = "binary",   // what the app requests; must be echoed or the browser aborts
        .user_ctx = secure ? &s_secure_marker : NULL,
    };
    httpd_register_uri_handler(server, &uri);

    if (!s_tx_task) {
        xTaskCreate(net_tx_task, "net_tx", 4096, NULL, 6, &s_tx_task);
    }
}
