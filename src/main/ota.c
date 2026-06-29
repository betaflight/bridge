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

#include "ota.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "esp_log.h"

static const char *TAG = "ota";

#define OTA_BUF_SIZE  1024

const char *ota_board_id(void)
{
    return esp_app_get_description()->version;
}

// The board identity is carried in the app version, optionally followed by
// "+<ver>". Two images are for the same board if the part before '+' matches.
static bool same_board(const char *a, const char *b)
{
    size_t la = strcspn(a, "+");
    size_t lb = strcspn(b, "+");
    return la == lb && strncmp(a, b, la) == 0;
}

static esp_err_t fail(httpd_req_t *req, esp_ota_handle_t handle, const char *why)
{
    if (handle) {
        esp_ota_abort(handle);
    }
    ESP_LOGE(TAG, "update failed: %s", why);
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, why);
    return ESP_FAIL;
}

static esp_err_t update_post(httpd_req_t *req)
{
    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (!target) {
        return fail(req, 0, "no OTA partition");
    }
    ESP_LOGI(TAG, "receiving %d bytes into '%s'", req->content_len, target->label);

    esp_ota_handle_t handle = 0;
    if (esp_ota_begin(target, OTA_SIZE_UNKNOWN, &handle) != ESP_OK) {
        return fail(req, 0, "ota_begin failed");
    }

    char buf[OTA_BUF_SIZE];
    int remaining = req->content_len;
    while (remaining > 0) {
        int n = httpd_req_recv(req, buf, remaining < OTA_BUF_SIZE ? remaining : OTA_BUF_SIZE);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;   // transient; keep waiting for the rest of the body
        }
        if (n <= 0) {
            return fail(req, handle, "recv error / client aborted");
        }
        if (esp_ota_write(handle, buf, n) != ESP_OK) {
            return fail(req, handle, "flash write failed");
        }
        remaining -= n;
    }

    // esp_ota_end validates the image (magic, size, and secure-boot/signature
    // when enabled); a corrupt or wrong-chip upload is rejected here.
    if (esp_ota_end(handle) != ESP_OK) {
        return fail(req, 0, "image validation failed");
    }

    // Both boards are esp32s3, so esp_ota_end can't catch a wrong-board image
    // (mismatched partition layout). Compare the board id baked into the image
    // and refuse to boot it if it doesn't match this device.
    esp_app_desc_t incoming;
    if (esp_ota_get_partition_description(target, &incoming) != ESP_OK) {
        return fail(req, 0, "cannot read image descriptor");
    }
    if (!same_board(ota_board_id(), incoming.version)) {
        char msg[112];
        snprintf(msg, sizeof(msg), "image is for '%s' but this device is '%s'",
                 incoming.version, ota_board_id());
        ESP_LOGE(TAG, "rejecting update: %s", msg);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, msg);
        return ESP_FAIL;   // leave boot partition untouched; written slot is inert
    }

    if (esp_ota_set_boot_partition(target) != ESP_OK) {
        return fail(req, 0, "set boot partition failed");
    }

    ESP_LOGI(TAG, "update OK; booting '%s'", target->label);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");

    // Let the response flush before resetting into the new image.
    vTaskDelay(pdMS_TO_TICKS(750));
    esp_restart();
    return ESP_OK;   // not reached
}

void ota_register(httpd_handle_t server)
{
    const httpd_uri_t update = {
        .uri = "/update",
        .method = HTTP_POST,
        .handler = update_post,
    };
    httpd_register_uri_handler(server, &update);
}

void ota_running_info(char *label, size_t label_len, bool *valid)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    strlcpy(label, running->label, label_len);

    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) == ESP_OK) {
        // UNDEFINED = serial-flashed image (rollback not tracked) => treat valid.
        *valid = (state == ESP_OTA_IMG_VALID || state == ESP_OTA_IMG_UNDEFINED);
    } else {
        *valid = true;
    }
}

void ota_mark_valid(void)
{
    // Only meaningful while the image is pending verification (i.e. just OTA'd);
    // a no-op otherwise. Confirms the new firmware booted so rollback is cancelled.
    esp_ota_img_states_t state;
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (esp_ota_get_state_partition(running, &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "image '%s' marked valid", running->label);
    }
}
