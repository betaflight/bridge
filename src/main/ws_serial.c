#include "ws_serial.h"
#include "bridge.h"

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
static TaskHandle_t s_tx_task;

static void ws_drop(void)
{
    s_fd = -1;
    s_hd = NULL;
    bridge_release(BRIDGE_CLIENT_WS);
    bridge_reset();
}

// FC -> WebSocket client. Drains the bridge and pushes binary frames to the
// active client; a send failure means the client has gone, so we let go.
static void ws_tx_task(void *arg)
{
    static uint8_t buf[1024];
    while (1) {
        int fd = s_fd;
        if (fd < 0 || bridge_client_owner() != BRIDGE_CLIENT_WS) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        size_t n = bridge_usb_to_net_pop(buf, sizeof(buf), 100);
        if (n == 0) {
            continue;
        }
        httpd_ws_frame_t frame = {
            .final = true,
            .type = HTTPD_WS_TYPE_BINARY,
            .payload = buf,
            .len = n,
        };
        if (httpd_ws_send_frame_async(s_hd, fd, &frame) != ESP_OK) {
            ESP_LOGW(TAG, "send failed; dropping client");
            ws_drop();
        }
    }
}

static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        // WebSocket handshake. A raw-TCP client takes priority; otherwise the
        // newest WebSocket client wins (so a browser reconnect isn't locked out
        // by a stale, half-closed session).
        if (bridge_client_owner() == BRIDGE_CLIENT_TCP) {
            ESP_LOGW(TAG, "rejected: raw-TCP client active");
            return ESP_FAIL;
        }
        int new_fd = httpd_req_to_sockfd(req);
        if (s_fd >= 0 && !(s_hd == req->handle && s_fd == new_fd)) {
            httpd_sess_trigger_close(s_hd, s_fd);   // boot the previous client
        }
        bridge_try_claim(BRIDGE_CLIENT_WS);   // no-op if we already own it
        s_hd = req->handle;
        s_fd = new_fd;
        ESP_LOGI(TAG, "client connected (fd %d)", new_fd);
        return ESP_OK;
    }

    // A frame from the Configurator. First call with len 0 gets the length.
    httpd_ws_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    frame.type = HTTPD_WS_TYPE_BINARY;
    esp_err_t ret = httpd_ws_recv_frame(req, &frame, 0);
    if (ret != ESP_OK) {
        return ret;
    }
    if (frame.type == HTTPD_WS_TYPE_CLOSE) {
        ESP_LOGI(TAG, "client closed");
        ws_drop();
        return ESP_OK;
    }
    if (frame.len) {
        uint8_t *payload = malloc(frame.len);
        if (!payload) {
            return ESP_ERR_NO_MEM;
        }
        frame.payload = payload;
        ret = httpd_ws_recv_frame(req, &frame, frame.len);
        if (ret == ESP_OK &&
            (frame.type == HTTPD_WS_TYPE_BINARY || frame.type == HTTPD_WS_TYPE_TEXT)) {
            bridge_net_to_usb_push(payload, frame.len);
        }
        free(payload);
    }
    return ret;
}

void ws_serial_register(httpd_handle_t server)
{
    static const httpd_uri_t uri = {
        .uri = "/serial",
        .method = HTTP_GET,
        .handler = ws_handler,
        .is_websocket = true,
        .supported_subprotocol = "wsSerial",
    };
    httpd_register_uri_handler(server, &uri);

    if (!s_tx_task) {
        xTaskCreate(ws_tx_task, "ws_tx", 4096, NULL, 6, &s_tx_task);
    }
}
