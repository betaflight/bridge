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

#include "dfu_host.h"

#include <inttypes.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"

#include "usb/usb_host.h"
#include "usb/usb_helpers.h"

static const char *TAG = "dfu_host";

// DFU class requests (DFU 1.1 section 3). DETACH and GETSTATE are unused: the
// FC is rebooted into the bootloader over MSP, not by detaching a runtime
// interface, and GETSTATUS reports the state anyway.
#define DFU_REQ_DNLOAD     0x01
#define DFU_REQ_UPLOAD     0x02
#define DFU_REQ_GETSTATUS  0x03
#define DFU_REQ_CLRSTATUS  0x04
#define DFU_REQ_ABORT      0x06

#define DFU_REQTYPE_OUT    0x21   // host->device, class, interface
#define DFU_REQTYPE_IN     0xa1   // device->host, class, interface

// DfuSe command bytes, sent as the payload of a wValue==0 download.
#define DFUSE_CMD_SET_ADDRESS  0x21
#define DFUSE_CMD_ERASE_PAGE   0x41

#define DFU_INTERFACE          0     // STM32 bootloaders put DFU on interface 0
#define DFU_CLASS              0xfe
#define DFU_SUBCLASS           0x01
#define DFU_FUNC_DESC_TYPE     0x21
#define DFU_FUNC_DESC_MIN_LEN  9

#define DFU_XFER_TIMEOUT_MS    5000
#define DFU_CLEAR_MAX_TRIES    100

// The manifestation phase after a leave command can legitimately report a long
// poll timeout; cap the wait so a misbehaving device cannot hang the job.
#define DFU_MAX_POLL_MS        5000

#define EVT_ADOPT  0
#define EVT_DROP   1

typedef struct {
    uint8_t kind;
    uint8_t addr;
    usb_device_handle_t hdl;
} dfu_evt_t;

static usb_host_client_handle_t s_client;
static usb_transfer_t *s_xfer;
static SemaphoreHandle_t s_done;      // given by the transfer completion callback
static SemaphoreHandle_t s_lock;      // serialises s_xfer, and gates the close
static SemaphoreHandle_t s_present_sig;
static QueueHandle_t s_evt_q;

static usb_device_handle_t s_dev;
static uint8_t s_mps0 = 64;
static volatile bool s_present;
static volatile bool s_gone;
// Set when a transfer times out. The URB is still in flight and EP0 has no
// public cancel, so the transfer object can never be reused: the session is
// dead until the device detaches and the stack flushes EP0 for us.
static volatile bool s_wedged;

static dfu_host_info_t s_info;

bool dfu_host_is_present(void)
{
    return s_present;
}

bool dfu_host_get_info(dfu_host_info_t *out)
{
    if (!s_present) {
        return false;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return false;
    }
    const bool ok = s_present;
    if (ok && out) {
        *out = s_info;
    }
    xSemaphoreGive(s_lock);
    return ok;
}

bool dfu_host_wait(uint32_t timeout_ms)
{
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (!s_present) {
        const TickType_t now = xTaskGetTickCount();
        if (now >= deadline) {
            return false;
        }
        xSemaphoreTake(s_present_sig, deadline - now);
    }
    return true;
}

// ---------------------------------------------------------------- transfers

// Runs in the client event task; must do nothing that can block.
static void xfer_done(usb_transfer_t *xfer)
{
    (void)xfer;
    xSemaphoreGive(s_done);
}

// One control transfer, start to finish. Never call from dfu_evt_task: the
// completion callback is dispatched by usb_host_client_handle_events(), so
// waiting for it there deadlocks.
static esp_err_t ctrl_xfer(uint8_t req_type, uint8_t request, uint16_t value,
                           uint16_t index, void *data, uint16_t len, size_t *actual)
{
    const bool is_in = (req_type & USB_BM_REQUEST_TYPE_DIR_IN) != 0;
    esp_err_t err;

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(DFU_XFER_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    if (s_dev == NULL || s_gone || s_wedged) {
        err = ESP_ERR_INVALID_STATE;
        goto out;
    }
    if (sizeof(usb_setup_packet_t) + len > s_xfer->data_buffer_size) {
        err = ESP_ERR_INVALID_SIZE;
        goto out;
    }

    usb_setup_packet_t *setup = (usb_setup_packet_t *)s_xfer->data_buffer;
    setup->bmRequestType = req_type;
    setup->bRequest = request;
    setup->wValue = value;
    setup->wIndex = index;
    setup->wLength = len;

    if (!is_in && len && data) {
        memcpy(s_xfer->data_buffer + sizeof(usb_setup_packet_t), data, len);
    }

    s_xfer->device_handle = s_dev;
    s_xfer->bEndpointAddress = 0;
    s_xfer->flags = 0;
    s_xfer->callback = xfer_done;
    s_xfer->context = NULL;
    // IN transfers must be rounded up to a multiple of the control endpoint's
    // packet size; OUT transfers must match wLength exactly (usbh.c:258).
    s_xfer->num_bytes = is_in
        ? sizeof(usb_setup_packet_t) + usb_round_up_to_mps(len, s_mps0)
        : sizeof(usb_setup_packet_t) + len;

    xSemaphoreTake(s_done, 0);   // discard a stale completion, if any

    err = usb_host_transfer_submit_control(s_client, s_xfer);
    if (err != ESP_OK) {
        goto out;
    }

    if (xSemaphoreTake(s_done, pdMS_TO_TICKS(DFU_XFER_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "control transfer timed out (req 0x%02x); session is dead", request);
        s_wedged = true;
        err = ESP_ERR_TIMEOUT;
        goto out;
    }

    if (s_xfer->status != USB_TRANSFER_STATUS_COMPLETED) {
        err = s_xfer->status == USB_TRANSFER_STATUS_STALL     ? ESP_ERR_NOT_SUPPORTED
            : s_xfer->status == USB_TRANSFER_STATUS_NO_DEVICE ? ESP_ERR_INVALID_STATE
            : ESP_FAIL;
        goto out;
    }

    // actual_num_bytes counts the setup packet as well as the data stage.
    size_t got = (size_t)s_xfer->actual_num_bytes - sizeof(usb_setup_packet_t);
    if (got > len) {
        got = len;
    }
    if (is_in && data && len) {
        memcpy(data, s_xfer->data_buffer + sizeof(usb_setup_packet_t), got);
    }
    if (actual) {
        *actual = got;
    }
    err = ESP_OK;

out:
    xSemaphoreGive(s_lock);
    return err;
}

// ------------------------------------------------------------ DFU requests

esp_err_t dfu_host_set_alt(uint8_t alt)
{
    return ctrl_xfer(USB_BM_REQUEST_TYPE_DIR_OUT | USB_BM_REQUEST_TYPE_TYPE_STANDARD |
                         USB_BM_REQUEST_TYPE_RECIP_INTERFACE,
                     USB_B_REQUEST_SET_INTERFACE, alt, DFU_INTERFACE, NULL, 0, NULL);
}

esp_err_t dfu_host_get_status(dfu_status_t *out)
{
    uint8_t buf[6] = {0};
    size_t got = 0;
    const esp_err_t err = ctrl_xfer(DFU_REQTYPE_IN, DFU_REQ_GETSTATUS, 0, DFU_INTERFACE,
                                    buf, sizeof(buf), &got);
    if (err != ESP_OK) {
        return err;
    }
    if (got != sizeof(buf)) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (out) {
        out->status = buf[0];
        // bwPollTimeout is a 24-bit little-endian millisecond count.
        out->poll_ms = (uint32_t)buf[1] | (uint32_t)buf[2] << 8 | (uint32_t)buf[3] << 16;
        out->state = buf[4];
    }
    return ESP_OK;
}

esp_err_t dfu_host_clr_status(void)
{
    return ctrl_xfer(DFU_REQTYPE_OUT, DFU_REQ_CLRSTATUS, 0, DFU_INTERFACE, NULL, 0, NULL);
}

esp_err_t dfu_host_abort(void)
{
    return ctrl_xfer(DFU_REQTYPE_OUT, DFU_REQ_ABORT, 0, DFU_INTERFACE, NULL, 0, NULL);
}

esp_err_t dfu_host_dnload(uint16_t block, const void *data, uint16_t len)
{
    return ctrl_xfer(DFU_REQTYPE_OUT, DFU_REQ_DNLOAD, block, DFU_INTERFACE,
                     (void *)data, len, NULL);
}

esp_err_t dfu_host_upload(uint16_t block, void *data, uint16_t len, size_t *actual)
{
    return ctrl_xfer(DFU_REQTYPE_IN, DFU_REQ_UPLOAD, block, DFU_INTERFACE,
                     data, len, actual);
}

static void poll_delay(uint32_t ms)
{
    if (ms > DFU_MAX_POLL_MS) {
        ms = DFU_MAX_POLL_MS;
    }
    // Round up: a sub-tick poll timeout would otherwise not delay at all, and
    // the device genuinely needs the time it asked for.
    vTaskDelay(pdMS_TO_TICKS(ms) + 1);
}

esp_err_t dfu_host_clear_state(bool busy_is_stuck)
{
    for (int i = 0; i < DFU_CLEAR_MAX_TRIES; i++) {
        dfu_status_t st;
        const esp_err_t err = dfu_host_get_status(&st);
        if (err != ESP_OK) {
            return err;
        }

        switch (st.state) {
        case DFU_STATE_IDLE:
            return ESP_OK;

        case DFU_STATE_DNLOAD_IDLE:
        case DFU_STATE_UPLOAD_IDLE:
            // An abort, not a clear: CLRSTATUS outside an error state is
            // refused by conforming bootloaders (the STM32C5 ROM stalls it).
            poll_delay(st.poll_ms);
            if (dfu_host_abort() != ESP_OK) {
                return ESP_FAIL;
            }
            break;

        case DFU_STATE_ERROR:
            poll_delay(st.poll_ms);
            if (dfu_host_clr_status() != ESP_OK) {
                return ESP_FAIL;
            }
            break;

        case DFU_STATE_DNBUSY:
            poll_delay(st.poll_ms);
            // Normally busy just means "ask again"; only the H7 unwedge treats
            // it as an error state worth clearing.
            if (busy_is_stuck && dfu_host_clr_status() != ESP_OK) {
                return ESP_FAIL;
            }
            break;

        default:
            poll_delay(st.poll_ms);
            break;
        }
    }

    ESP_LOGE(TAG, "device never returned to idle");
    return ESP_ERR_TIMEOUT;
}

// A DfuSe command is a download with wValue 0, acknowledged by a status read
// that reports DNBUSY, then - after the timeout the device asked for - a second
// read that must report DNLOAD_IDLE.
static esp_err_t dfuse_command(const uint8_t *cmd, uint16_t len, dfu_status_t *probe)
{
    esp_err_t err = dfu_host_dnload(0, cmd, len);
    if (err != ESP_OK) {
        return err;
    }

    dfu_status_t st;
    err = dfu_host_get_status(&st);
    if (err != ESP_OK) {
        return err;
    }
    if (st.state != DFU_STATE_DNBUSY) {
        if (probe) {
            *probe = st;
            return ESP_OK;
        }
        ESP_LOGE(TAG, "command 0x%02x not accepted (state %u status %u)",
                 cmd[0], st.state, st.status);
        return ESP_FAIL;
    }

    poll_delay(st.poll_ms);

    err = dfu_host_get_status(&st);
    if (err != ESP_OK) {
        return err;
    }
    if (probe) {
        *probe = st;
        return ESP_OK;
    }

    if (st.state == DFU_STATE_DNLOAD_IDLE) {
        return ESP_OK;
    }

    // An H743 Rev.V can sit in DNBUSY past its own poll timeout. Two clear
    // requests unwedge it: the first answers with an error, the second with OK.
    if (st.state == DFU_STATE_DNBUSY) {
        ESP_LOGW(TAG, "device stuck busy after 0x%02x; unwedging", cmd[0]);
        if (dfu_host_clear_state(true) != ESP_OK) {
            return ESP_FAIL;
        }
        return ESP_OK;
    }

    ESP_LOGE(TAG, "command 0x%02x failed (state %u status %u)",
             cmd[0], st.state, st.status);
    return ESP_FAIL;
}

esp_err_t dfu_host_write_block(uint16_t block, const void *data, uint16_t len)
{
    esp_err_t err = dfu_host_dnload(block, data, len);
    if (err != ESP_OK) {
        return err;
    }

    dfu_status_t st;
    err = dfu_host_get_status(&st);
    if (err != ESP_OK) {
        return err;
    }
    if (st.state != DFU_STATE_DNBUSY) {
        ESP_LOGE(TAG, "block %u not accepted (state %u status %u)", block, st.state, st.status);
        return ESP_FAIL;
    }

    poll_delay(st.poll_ms);

    err = dfu_host_get_status(&st);
    if (err != ESP_OK) {
        return err;
    }
    if (st.state != DFU_STATE_DNLOAD_IDLE) {
        ESP_LOGE(TAG, "block %u failed (state %u status %u)", block, st.state, st.status);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void put_le32(uint8_t *dst, uint32_t v)
{
    dst[0] = (uint8_t)v;
    dst[1] = (uint8_t)(v >> 8);
    dst[2] = (uint8_t)(v >> 16);
    dst[3] = (uint8_t)(v >> 24);
}

esp_err_t dfu_host_set_address(uint32_t address)
{
    uint8_t cmd[5] = { DFUSE_CMD_SET_ADDRESS };
    put_le32(&cmd[1], address);
    return dfuse_command(cmd, sizeof(cmd), NULL);
}

esp_err_t dfu_host_set_address_probe(uint32_t address, dfu_status_t *out)
{
    uint8_t cmd[5] = { DFUSE_CMD_SET_ADDRESS };
    put_le32(&cmd[1], address);
    return dfuse_command(cmd, sizeof(cmd), out);
}

esp_err_t dfu_host_erase_page(uint32_t address)
{
    uint8_t cmd[5] = { DFUSE_CMD_ERASE_PAGE };
    put_le32(&cmd[1], address);
    return dfuse_command(cmd, sizeof(cmd), NULL);
}

void dfu_host_release(void)
{
    usb_device_handle_t hdl;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    hdl = s_dev;
    s_dev = NULL;
    s_present = false;
    s_gone = false;
    s_wedged = false;
    memset(&s_info, 0, sizeof(s_info));
    xSemaphoreGive(s_lock);

    if (hdl) {
        usb_host_device_close(s_client, hdl);
        ESP_LOGI(TAG, "released DFU device");
    }
}

esp_err_t dfu_host_leave(uint32_t address)
{
    esp_err_t err = dfu_host_clear_state(false);
    if (err != ESP_OK) {
        return err;
    }
    err = dfu_host_set_address(address);
    if (err != ESP_OK) {
        return err;
    }
    // A zero-length download followed by a status read is what starts the
    // firmware; the device detaches during the read, so errors are expected.
    err = dfu_host_dnload(0, NULL, 0);
    if (err != ESP_OK) {
        return err;
    }
    dfu_host_get_status(NULL);
    return ESP_OK;
}

// ---------------------------------------------------------- device probing

// Fetch string descriptor `index` as ASCII. DfuSe layout strings are plain
// ASCII, so anything wider is replaced rather than decoded.
static esp_err_t read_string(uint8_t index, uint16_t langid, char *out, size_t out_len)
{
    out[0] = '\0';
    if (index == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    // Ask for the header first: the length is only known from the descriptor.
    uint8_t head[2] = {0};
    size_t got = 0;
    esp_err_t err = ctrl_xfer(USB_BM_REQUEST_TYPE_DIR_IN | USB_BM_REQUEST_TYPE_TYPE_STANDARD |
                                  USB_BM_REQUEST_TYPE_RECIP_DEVICE,
                              USB_B_REQUEST_GET_DESCRIPTOR,
                              (USB_W_VALUE_DT_STRING << 8) | index, langid,
                              head, sizeof(head), &got);
    if (err != ESP_OK || got < 2 || head[1] != USB_W_VALUE_DT_STRING || head[0] < 2) {
        return err == ESP_OK ? ESP_ERR_INVALID_RESPONSE : err;
    }

    uint8_t buf[255];
    const uint8_t want = head[0];
    err = ctrl_xfer(USB_BM_REQUEST_TYPE_DIR_IN | USB_BM_REQUEST_TYPE_TYPE_STANDARD |
                        USB_BM_REQUEST_TYPE_RECIP_DEVICE,
                    USB_B_REQUEST_GET_DESCRIPTOR,
                    (USB_W_VALUE_DT_STRING << 8) | index, langid,
                    buf, want, &got);
    if (err != ESP_OK) {
        return err;
    }
    if (got < 2 || got > want) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    size_t o = 0;
    for (size_t i = 2; i + 1 < got && o + 1 < out_len; i += 2) {
        const uint16_t c = (uint16_t)buf[i] | (uint16_t)buf[i + 1] << 8;
        out[o++] = (c && c < 0x80) ? (char)c : '?';
    }
    out[o] = '\0';
    return ESP_OK;
}

// Read the memory layout from the interface string of every DFU alternate
// setting, and the transfer size from the DFU functional descriptor. Both come
// out of the configuration descriptor cached at enumeration; the functional
// descriptor is deliberately not requested on its own, because the STM32C5 ROM
// bootloader hangs forever on a standalone GET_DESCRIPTOR for it.
static bool probe_device(void)
{
    const usb_device_desc_t *dev_desc = NULL;
    if (usb_host_get_device_descriptor(s_dev, &dev_desc) != ESP_OK) {
        return false;
    }
    const usb_config_desc_t *cfg = NULL;
    if (usb_host_get_active_config_descriptor(s_dev, &cfg) != ESP_OK) {
        return false;
    }

    memset(&s_info, 0, sizeof(s_info));
    s_info.vid = dev_desc->idVendor;
    s_info.pid = dev_desc->idProduct;
    s_info.transfer_size = DFU_MAX_TRANSFER_SIZE;

    uint8_t alts[DFU_MAX_ALTS];
    uint8_t alt_strings[DFU_MAX_ALTS];
    size_t alt_count = 0;

    int offset = 0;
    const usb_standard_desc_t *d = (const usb_standard_desc_t *)cfg;
    while ((d = usb_parse_next_descriptor_of_type(d, cfg->wTotalLength,
                                                  USB_B_DESCRIPTOR_TYPE_INTERFACE,
                                                  &offset)) != NULL) {
        const usb_intf_desc_t *intf = (const usb_intf_desc_t *)d;
        if (intf->bInterfaceNumber != DFU_INTERFACE ||
            intf->bInterfaceClass != DFU_CLASS ||
            intf->bInterfaceSubClass != DFU_SUBCLASS) {
            continue;
        }
        if (alt_count < sizeof(alts)) {
            alts[alt_count] = intf->bAlternateSetting;
            alt_strings[alt_count] = intf->iInterface;
            alt_count++;
        }
    }

    if (alt_count == 0) {
        return false;   // not a DFU device; some other peripheral was plugged in
    }

    offset = 0;
    d = usb_parse_next_descriptor_of_type((const usb_standard_desc_t *)cfg,
                                          cfg->wTotalLength, DFU_FUNC_DESC_TYPE, &offset);
    if (d && d->bLength >= DFU_FUNC_DESC_MIN_LEN) {
        const uint8_t *p = (const uint8_t *)d;
        const uint16_t size = (uint16_t)p[5] | (uint16_t)p[6] << 8;
        s_info.bcd_dfu_version = (uint16_t)p[7] | (uint16_t)p[8] << 8;
        if (size > 0 && size < s_info.transfer_size) {
            s_info.transfer_size = size;
        }
    }

    // LANGID table, so the interface strings can be asked for in a language
    // the device actually has. Fall back to US English if it will not say.
    uint16_t langid = 0x0409;
    {
        uint8_t lang[4] = {0};
        size_t got = 0;
        if (ctrl_xfer(USB_BM_REQUEST_TYPE_DIR_IN | USB_BM_REQUEST_TYPE_TYPE_STANDARD |
                          USB_BM_REQUEST_TYPE_RECIP_DEVICE,
                      USB_B_REQUEST_GET_DESCRIPTOR,
                      (USB_W_VALUE_DT_STRING << 8) | 0, 0,
                      lang, sizeof(lang), &got) == ESP_OK && got >= 4) {
            langid = (uint16_t)lang[2] | (uint16_t)lang[3] << 8;
        }
    }

    for (size_t i = 0; i < alt_count; i++) {
        char desc[256];
        if (read_string(alt_strings[i], langid, desc, sizeof(desc)) != ESP_OK) {
            continue;
        }
        dfu_region_t region;
        if (dfu_layout_parse_region(desc, alts[i], &region)) {
            dfu_layout_add(&s_info.layout, &region);
        } else {
            ESP_LOGD(TAG, "alt %u: '%s' is not a memory layout", alts[i], desc);
        }
    }

    ESP_LOGI(TAG, "DFU device %04x:%04x, transfer size %u, %u region(s)",
             s_info.vid, s_info.pid, s_info.transfer_size, s_info.layout.region_count);
    for (uint8_t i = 0; i < s_info.layout.region_count; i++) {
        const dfu_region_t *r = &s_info.layout.regions[i];
        ESP_LOGI(TAG, "  %s @ 0x%08" PRIx32 ", %" PRIu32 " KiB, %" PRIu32 " page(s)",
                 r->name, r->start_address, r->total_size / 1024, dfu_region_page_count(r));
    }
    return true;
}

// ------------------------------------------------------------ client plumbing

// Runs in the client event task. Must not block, open, close or transfer.
static void client_event_cb(const usb_host_client_event_msg_t *msg, void *arg)
{
    (void)arg;
    dfu_evt_t evt = {0};

    switch (msg->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV:
        evt.kind = EVT_ADOPT;
        evt.addr = msg->new_dev.address;
        xQueueSend(s_evt_q, &evt, 0);
        break;

    case USB_HOST_CLIENT_EVENT_DEV_GONE:
        if (msg->dev_gone.dev_hdl != s_dev) {
            break;
        }
        // Stop new transfers immediately; the close happens on the worker task,
        // which is the only place we can prove none is in flight.
        s_present = false;
        s_gone = true;
        evt.kind = EVT_DROP;
        evt.hdl = msg->dev_gone.dev_hdl;
        xQueueSend(s_evt_q, &evt, 0);
        break;

    default:
        break;
    }
}

// Pumps client events and dispatches transfer completions. Does nothing else:
// anything that blocks on a transfer must run on dfu_ctl_task instead.
static void dfu_evt_task(void *arg)
{
    (void)arg;
    while (1) {
        usb_host_client_handle_events(s_client, portMAX_DELAY);
    }
}

static void adopt(uint8_t addr)
{
    if (s_dev != NULL) {
        return;   // already bridging a DFU device; ignore anything else
    }

    usb_device_handle_t hdl = NULL;
    if (usb_host_device_open(s_client, addr, &hdl) != ESP_OK) {
        return;
    }

    usb_device_info_t info;
    if (usb_host_device_info(hdl, &info) == ESP_OK && info.bMaxPacketSize0) {
        s_mps0 = info.bMaxPacketSize0;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_dev = hdl;
    s_gone = false;
    s_wedged = false;
    xSemaphoreGive(s_lock);

    if (!probe_device()) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_dev = NULL;
        xSemaphoreGive(s_lock);
        usb_host_device_close(s_client, hdl);
        return;
    }

    s_present = true;
    xSemaphoreGive(s_present_sig);
}

static void drop(usb_device_handle_t hdl)
{
    // Taking the lock is what proves no control transfer is in flight;
    // usbh_dev_close() asserts on that. Closing is also what lets the root port
    // be recycled, so the FC can enumerate again - skipping it wedges USB.
    xSemaphoreTake(s_lock, portMAX_DELAY);
    // dfu_host_release() may have closed it already, in which case there is
    // nothing left to do and closing again would be a double free.
    const bool ours = (hdl != NULL && s_dev == hdl);
    if (ours) {
        s_dev = NULL;
        s_gone = false;
        s_wedged = false;
        memset(&s_info, 0, sizeof(s_info));
    }
    xSemaphoreGive(s_lock);

    if (ours) {
        usb_host_device_close(s_client, hdl);
        ESP_LOGI(TAG, "DFU device gone");
    }
}

// Owns every blocking USB operation: adopting a device (which probes it over
// EP0) and closing one.
static void dfu_ctl_task(void *arg)
{
    (void)arg;
    dfu_evt_t evt;
    while (1) {
        if (xQueueReceive(s_evt_q, &evt, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (evt.kind == EVT_ADOPT) {
            adopt(evt.addr);
        } else {
            drop(evt.hdl);
        }
    }
}

void dfu_host_start(void)
{
    s_done = xSemaphoreCreateBinary();
    s_lock = xSemaphoreCreateMutex();
    s_present_sig = xSemaphoreCreateBinary();
    s_evt_q = xQueueCreate(8, sizeof(dfu_evt_t));
    configASSERT(s_done && s_lock && s_present_sig && s_evt_q);

    // One transfer, reused for the life of the app. The buffer is allocated
    // DMA-capable and cache-aligned by the host library, so we copy into it
    // rather than handing it a buffer of our own.
    ESP_ERROR_CHECK(usb_host_transfer_alloc(sizeof(usb_setup_packet_t) + DFU_MAX_TRANSFER_SIZE,
                                            0, &s_xfer));

    const usb_host_client_config_t cfg = {
        .is_synchronous = false,
        .max_num_event_msg = 5,
        .async = {
            .client_event_callback = client_event_cb,
            .callback_arg = NULL,
        },
    };
    ESP_ERROR_CHECK(usb_host_client_register(&cfg, &s_client));

    xTaskCreate(dfu_evt_task, "dfu_evt", 4096, NULL, 9, NULL);
    xTaskCreate(dfu_ctl_task, "dfu_ctl", 5120, NULL, 6, NULL);

    // A device attached before the client registered produces no NEW_DEV event,
    // so sweep what is already enumerated. adopt() ignores duplicates.
    uint8_t addrs[10];
    int count = 0;
    if (usb_host_device_addr_list_fill(sizeof(addrs), addrs, &count) == ESP_OK) {
        for (int i = 0; i < count; i++) {
            const dfu_evt_t evt = { .kind = EVT_ADOPT, .addr = addrs[i] };
            xQueueSend(s_evt_q, &evt, 0);
        }
    }

    ESP_LOGI(TAG, "DFU host client started");
}
