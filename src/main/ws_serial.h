// WebSocket serial endpoint. Browsers can't open raw TCP, so this exposes the
// FC byte stream over a WebSocket at /serial — reachable as ws:// on the plain
// HTTP server and wss:// on the TLS server. Transparent binary bridge, like the
// raw-TCP server; only one Configurator client bridges at a time (see bridge.c).
#pragma once

#include "esp_http_server.h"

// Register the /serial WebSocket handler on `server`. Call once per httpd
// instance (HTTP and HTTPS). The FC->client pump task is spawned on first call.
void ws_serial_register(httpd_handle_t server);
