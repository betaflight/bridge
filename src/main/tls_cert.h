// Self-signed TLS server credentials for the HTTPS/WSS endpoint.
//
// On first use an EC (P-256) self-signed certificate is generated and stored in
// NVS, then reused on every subsequent boot — so a browser only has to accept
// the certificate once, and the acceptance survives OTA updates.
#pragma once

#include "esp_err.h"
#include <stddef.h>

// Returns PEM server certificate + private key (NUL-terminated, lengths include
// the terminator, as esp_https_server expects). Generates and persists them on
// first call. The buffers are owned by this module and live for the program
// lifetime. Returns ESP_OK on success.
esp_err_t tls_cert_get(const unsigned char **cert_pem, size_t *cert_len,
                       const unsigned char **key_pem, size_t *key_len);
