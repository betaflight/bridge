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

#include "tcp_server.h"
#include "ws_serial.h"
#include "bridge.h"

#include <stdint.h>
#include <string.h>
#include <errno.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "lwip/sockets.h"
#include "lwip/netdb.h"

static const char *TAG = "tcp";

// Current connected client socket, or -1. Written by the accept task, read by
// the TX task; int access is atomic on this platform.
static volatile int s_client = -1;

bool tcp_server_client_connected(void)
{
    return s_client >= 0;
}

void tcp_server_kick(void)
{
    int fd = s_client;
    if (fd >= 0) {
        ESP_LOGI(TAG, "kicking client");
        // shutdown() makes the fd readable in the accept task's select(), which
        // then reaps it via close_client(). (A bare recv() would not wake, but
        // select() does.)
        shutdown(fd, SHUT_RDWR);
    }
}

// Send FC->Configurator bytes to the connected TCP client. Called by the single
// net TX pump (ws_serial.c) only while the TCP client owns the bridge.
void tcp_server_send(const uint8_t *data, size_t len)
{
    int fd = s_client;
    if (fd < 0) {
        return;
    }
    size_t off = 0;
    while (off < len) {
        int sent = send(fd, data + off, len - off, 0);
        if (sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                vTaskDelay(pdMS_TO_TICKS(2));
                continue;
            }
            ESP_LOGW(TAG, "send failed: errno %d", errno);
            return;   // accept task's select() notices the dead socket
        }
        off += sent;
    }
}

// Drop the served client, if any, releasing the bridge only while we still own
// it (a WebSocket client may have taken over). Closes the socket.
static void close_client(void)
{
    int fd = s_client;
    if (fd < 0) {
        return;
    }
    s_client = -1;
    if (bridge_client_owner() == BRIDGE_CLIENT_TCP) {
        bridge_release(BRIDGE_CLIENT_TCP);
    }
    close(fd);
}

// Adopt a freshly accepted client as the single served connection, taking the
// bridge over from any current owner (newest connection wins).
static void adopt_client(int fd)
{
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));  // low MSP latency
    s_client = fd;
    // Take the bridge over from any owner (incl. a WebSocket client). Its own
    // task notices the ownership change and drops it; we do not reach across
    // transports here, which would risk blocking the httpd worker.
    bridge_claim(BRIDGE_CLIENT_TCP);
    ESP_LOGI(TAG, "client connected");
}

static void tcp_accept_task(void *arg)
{
    int listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_fd < 0) {
        ESP_LOGE(TAG, "socket() failed: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }

    int one = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons(TCP_SERVER_PORT),
    };
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "bind() failed: errno %d", errno);
        close(listen_fd);
        vTaskDelete(NULL);
        return;
    }
    if (listen(listen_fd, 1) != 0) {
        ESP_LOGE(TAG, "listen() failed: errno %d", errno);
        close(listen_fd);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "listening on port %d", TCP_SERVER_PORT);

    // Single task, single served client. select() watches the listener and the
    // client together: a pending connection wakes us to accept it (dropping the
    // current client — newest wins), and the client fd wakes us on data or on
    // close/shutdown (so a kick via tcp_server_kick() is noticed immediately).
    // Keeping one task and closing the old socket on takeover avoids leaking
    // sockets into lwIP's small descriptor table.
    static uint8_t buf[1024];
    while (1) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(listen_fd, &rfds);
        int maxfd = listen_fd;
        int client = s_client;
        if (client >= 0) {
            FD_SET(client, &rfds);
            if (client > maxfd) {
                maxfd = client;
            }
        }

        // 200 ms timeout so we poll ownership even when idle: a WebSocket
        // client may have taken the bridge, in which case we drop ours.
        struct timeval tv = { .tv_sec = 0, .tv_usec = 200000 };
        if (select(maxfd + 1, &rfds, NULL, NULL, &tv) < 0) {
            if (errno == EINTR) {
                continue;
            }
            ESP_LOGW(TAG, "select() failed: errno %d", errno);
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        // Superseded by a client on the other transport: drop ours.
        if (client >= 0 && bridge_client_owner() != BRIDGE_CLIENT_TCP) {
            ESP_LOGI(TAG, "superseded; dropping client");
            close_client();
            continue;
        }

        // Client activity: data to forward, or a closed/kicked socket to reap.
        if (client >= 0 && FD_ISSET(client, &rfds)) {
            int n = recv(client, buf, sizeof(buf), 0);
            if (n > 0) {
                bridge_net_to_usb_push(buf, n);
            } else {
                ESP_LOGI(TAG, "client %s", n == 0 ? "closed" : "gone");
                close_client();
            }
        }

        // A new connection is pending: accept it and let it take over.
        if (FD_ISSET(listen_fd, &rfds)) {
            struct sockaddr_in src;
            socklen_t slen = sizeof(src);
            int fd = accept(listen_fd, (struct sockaddr *)&src, &slen);
            if (fd < 0) {
                ESP_LOGW(TAG, "accept() failed: errno %d", errno);
                continue;
            }
            if (s_client >= 0) {
                ESP_LOGI(TAG, "new client; dropping current TCP client");
                close_client();
            }
            adopt_client(fd);   // claims the bridge; a WS owner drops itself
        }
    }
}

void tcp_server_start(void)
{
    xTaskCreate(tcp_accept_task, "tcp_accept", 4096, NULL, 5, NULL);
}
