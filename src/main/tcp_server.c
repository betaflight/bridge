#include "tcp_server.h"
#include "bridge.h"

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

// Drains FC->Configurator bytes from the bridge and writes them to the client.
static void tcp_tx_task(void *arg)
{
    static uint8_t buf[1024];
    while (1) {
        int fd = s_client;
        if (fd < 0) {
            // No TCP client: don't drain, or we'd steal bytes destined for a
            // WebSocket client that owns the stream instead.
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        size_t n = bridge_usb_to_net_pop(buf, sizeof(buf), 100);
        if (n == 0) {
            continue;
        }
        size_t off = 0;
        while (off < n) {
            int sent = send(fd, buf + off, n - off, 0);
            if (sent < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    vTaskDelay(pdMS_TO_TICKS(2));
                    continue;
                }
                ESP_LOGW(TAG, "send failed: errno %d", errno);
                break;  // accept task will notice on its next recv
            }
            off += sent;
        }
    }
}

static void serve_client(int fd)
{
    // One Configurator at a time, shared with the WebSocket endpoint.
    if (!bridge_try_claim(BRIDGE_CLIENT_TCP)) {
        ESP_LOGW(TAG, "client rejected: bridge busy");
        close(fd);
        return;
    }

    // Low latency: push MSP frames out immediately rather than coalescing.
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    s_client = fd;   // bridge_try_claim already reset the buffers
    ESP_LOGI(TAG, "client connected");

    uint8_t buf[1024];
    while (1) {
        int n = recv(fd, buf, sizeof(buf), 0);
        if (n > 0) {
            bridge_net_to_usb_push(buf, n);
        } else if (n == 0) {
            ESP_LOGI(TAG, "client closed");
            break;
        } else {
            if (errno == EINTR) {
                continue;
            }
            ESP_LOGW(TAG, "recv failed: errno %d", errno);
            break;
        }
    }

    s_client = -1;
    bridge_release(BRIDGE_CLIENT_TCP);
    bridge_reset();
    close(fd);
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

    while (1) {
        struct sockaddr_in src;
        socklen_t slen = sizeof(src);
        int fd = accept(listen_fd, (struct sockaddr *)&src, &slen);
        if (fd < 0) {
            ESP_LOGW(TAG, "accept() failed: errno %d", errno);
            continue;
        }
        serve_client(fd);  // blocks until this client disconnects
    }
}

void tcp_server_start(void)
{
    xTaskCreate(tcp_tx_task, "tcp_tx", 4096, NULL, 6, NULL);
    xTaskCreate(tcp_accept_task, "tcp_accept", 4096, NULL, 5, NULL);
}
