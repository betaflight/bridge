#include "tls_cert.h"

#include <stdlib.h>
#include <string.h>

#include "nvs.h"
#include "esp_log.h"

#include "mbedtls/pk.h"
#include "mbedtls/ecp.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"

static const char *TAG = "tls";

#define NVS_NS    "tls"
#define KEY_CERT  "cert"
#define KEY_KEY   "key"

// Generic identity; the cert is self-signed and accepted manually, so the
// CN/validity are cosmetic. Wide validity avoids the device's lack of an RTC at
// generation time tripping a not-yet-valid error.
#define CERT_NAME      "CN=betaflight-bridge"
#define CERT_NOT_BEFORE "20240101000000"
#define CERT_NOT_AFTER  "20440101000000"

static unsigned char *s_cert;
static unsigned char *s_key;
static size_t s_cert_len;
static size_t s_key_len;

static esp_err_t nvs_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return ESP_FAIL;
    }
    size_t cl = 0, kl = 0;
    esp_err_t ret = ESP_FAIL;
    if (nvs_get_blob(h, KEY_CERT, NULL, &cl) == ESP_OK &&
        nvs_get_blob(h, KEY_KEY, NULL, &kl) == ESP_OK && cl && kl) {
        unsigned char *cert = malloc(cl);
        unsigned char *key = malloc(kl);
        if (cert && key &&
            nvs_get_blob(h, KEY_CERT, cert, &cl) == ESP_OK &&
            nvs_get_blob(h, KEY_KEY, key, &kl) == ESP_OK) {
            s_cert = cert; s_cert_len = cl;
            s_key = key;   s_key_len = kl;
            ret = ESP_OK;
        } else {
            free(cert);
            free(key);
        }
    }
    nvs_close(h);
    return ret;
}

static esp_err_t nvs_store(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return ESP_FAIL;
    }
    esp_err_t ret = nvs_set_blob(h, KEY_CERT, s_cert, s_cert_len);
    if (ret == ESP_OK) {
        ret = nvs_set_blob(h, KEY_KEY, s_key, s_key_len);
    }
    if (ret == ESP_OK) {
        ret = nvs_commit(h);
    }
    nvs_close(h);
    return ret;
}

static esp_err_t generate(void)
{
    mbedtls_pk_context key;
    mbedtls_x509write_cert crt;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_pk_init(&key);
    mbedtls_x509write_crt_init(&crt);
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&drbg);

    // Heap, not stack: these are large and app_main's stack is shared.
    unsigned char *keybuf = malloc(1800);
    unsigned char *crtbuf = malloc(2048);
    esp_err_t result = ESP_FAIL;
    int ret = 0;
    const char *pers = "betaflight-bridge-tls";
    unsigned char serial[] = { 0x01 };

    if (!keybuf || !crtbuf) {
        goto done;
    }

    if ((ret = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
                                     (const unsigned char *)pers, strlen(pers))) != 0 ||
        (ret = mbedtls_pk_setup(&key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY))) != 0 ||
        (ret = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(key),
                                   mbedtls_ctr_drbg_random, &drbg)) != 0 ||
        (ret = mbedtls_pk_write_key_pem(&key, keybuf, 1800)) != 0) {
        goto done;
    }
    size_t klen = strlen((char *)keybuf) + 1;

    mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&crt, &key);
    mbedtls_x509write_crt_set_issuer_key(&crt, &key);
    mbedtls_x509write_crt_set_basic_constraints(&crt, 0, -1);
    if ((ret = mbedtls_x509write_crt_set_serial_raw(&crt, serial, sizeof(serial))) != 0 ||
        (ret = mbedtls_x509write_crt_set_subject_name(&crt, CERT_NAME)) != 0 ||
        (ret = mbedtls_x509write_crt_set_issuer_name(&crt, CERT_NAME)) != 0 ||
        (ret = mbedtls_x509write_crt_set_validity(&crt, CERT_NOT_BEFORE, CERT_NOT_AFTER)) != 0 ||
        (ret = mbedtls_x509write_crt_pem(&crt, crtbuf, 2048,
                                         mbedtls_ctr_drbg_random, &drbg)) != 0) {
        goto done;
    }
    size_t clen = strlen((char *)crtbuf) + 1;

    s_key = malloc(klen);
    s_cert = malloc(clen);
    if (!s_key || !s_cert) {
        free(s_key);  s_key = NULL;
        free(s_cert); s_cert = NULL;
        goto done;
    }
    memcpy(s_key, keybuf, klen);   s_key_len = klen;
    memcpy(s_cert, crtbuf, clen);  s_cert_len = clen;
    result = ESP_OK;

done:
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "cert generation failed: -0x%04x", (unsigned)-ret);
    }
    free(keybuf);
    free(crtbuf);
    mbedtls_x509write_crt_free(&crt);
    mbedtls_pk_free(&key);
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&entropy);
    return result;
}

esp_err_t tls_cert_get(const unsigned char **cert_pem, size_t *cert_len,
                       const unsigned char **key_pem, size_t *key_len)
{
    if (!s_cert) {
        if (nvs_load() == ESP_OK) {
            ESP_LOGI(TAG, "loaded TLS cert from NVS");
        } else {
            ESP_LOGI(TAG, "generating self-signed TLS cert (first boot)…");
            if (generate() != ESP_OK) {
                return ESP_FAIL;
            }
            if (nvs_store() != ESP_OK) {
                ESP_LOGW(TAG, "could not persist TLS cert; will regenerate next boot");
            }
        }
    }
    *cert_pem = s_cert; *cert_len = s_cert_len;
    *key_pem = s_key;   *key_len = s_key_len;
    return ESP_OK;
}
