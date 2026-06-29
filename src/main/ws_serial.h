// WebSocket serial endpoint. Browsers can't open raw TCP, so this exposes the
// FC byte stream over a WebSocket at /serial — reachable as ws:// on the plain
// HTTP server and wss:// on the TLS server. Transparent binary bridge, like the
// raw-TCP server; only one Configurator client bridges at a time (see bridge.c).
#pragma once

#include <stdbool.h>
#include "esp_http_server.h"

// Register the /serial WebSocket handler on `server`. Call once per httpd
// instance — pass secure=true for the TLS server so the status page can report
// ws vs wss. The FC->client pump task is spawned on first call.
void ws_serial_register(httpd_handle_t server, bool secure);

// True when the currently-connected WebSocket client arrived over TLS (wss).
// Only meaningful while a WS client owns the bridge.
bool ws_serial_is_secure(void);
