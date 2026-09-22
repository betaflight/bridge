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

#include "fc_cli.h"
#include "bridge.h"

#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "fc_cli";

#define RX_BUF_SIZE        4096

// `diff all` on a busy board with CAN peripherals runs to a few tens of KB.
// The cap is a safety net, not a target: PSRAM is used when the board has it.
#define BACKUP_MAX         (64 * 1024)
#define BACKUP_CHUNK       4096

#define CLI_ENTER_WAIT_MS  1000
#define CLI_PROMPT_MS      30000
#define CLI_LINE_MS        2000

#define MSP_BOARD_INFO     4
#define MSP_REBOOT         68

#define MSP_REBOOT_FIRMWARE          0
#define MSP_REBOOT_BOOTLOADER_ROM    1
#define MSP_REBOOT_BOOTLOADER_FLASH  4

// Bit 3 of targetCapabilities in the MSP_BOARD_INFO reply; the payload is a
// 4-byte board identifier, a u16 hardware revision and a u8 board type first.
#define BOARD_INFO_CAPS_OFFSET       7
#define TARGET_HAS_FLASH_BOOTLOADER  3

static StreamBufferHandle_t s_rx;
static char *s_backup;
static size_t s_backup_len;

void fc_cli_begin(void)
{
    if (!s_rx) {
        s_rx = xStreamBufferCreate(RX_BUF_SIZE, 1);
        configASSERT(s_rx);
    }
}

void fc_cli_end(void)
{
    // The buffer is kept for the next session; only the routing stops, which
    // the pump decides from bridge ownership.
    if (s_rx) {
        uint8_t junk[64];
        while (xStreamBufferReceive(s_rx, junk, sizeof(junk), 0) > 0) {
        }
    }
}

void fc_cli_rx(const uint8_t *data, size_t len)
{
    if (s_rx) {
        xStreamBufferSend(s_rx, data, len, 0);
    }
}

static void rx_drain(void)
{
    uint8_t junk[64];
    while (s_rx && xStreamBufferReceive(s_rx, junk, sizeof(junk), 0) > 0) {
    }
}

static size_t rx_read(uint8_t *out, size_t max_len, uint32_t timeout_ms)
{
    if (!s_rx) {
        return 0;
    }
    return xStreamBufferReceive(s_rx, out, max_len, pdMS_TO_TICKS(timeout_ms));
}

static void tx(const void *data, size_t len)
{
    const uint8_t *p = data;
    // The FC-bound buffer can fill if the FC is slow; push what fits and retry
    // rather than dropping the tail of a command.
    for (int tries = 0; len && tries < 100; tries++) {
        const size_t sent = bridge_net_to_usb_push(p, len);
        p += sent;
        len -= sent;
        if (len) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

static void tx_line(const char *line)
{
    tx(line, strlen(line));
    tx("\n", 1);
}

// ------------------------------------------------------------------- MSP

// MSP v1: '$M<' then length, command, payload, and an XOR checksum over
// everything from the length byte onwards.
static void msp_send(uint8_t cmd, const uint8_t *payload, uint8_t len)
{
    uint8_t frame[6 + 255];
    frame[0] = '$';
    frame[1] = 'M';
    frame[2] = '<';
    frame[3] = len;
    frame[4] = cmd;
    if (len && payload) {
        memcpy(&frame[5], payload, len);
    }
    uint8_t crc = len ^ cmd;
    for (uint8_t i = 0; i < len; i++) {
        crc ^= payload[i];
    }
    frame[5 + len] = crc;
    tx(frame, 6 + (size_t)len);
}

// Collect one '$M>' reply for `cmd`. Other frames are skipped, so an unrelated
// reply still in flight does not derail us.
static esp_err_t msp_recv(uint8_t cmd, uint8_t *payload, uint8_t *len, uint32_t timeout_ms)
{
    enum { S_DOLLAR, S_M, S_DIR, S_LEN, S_CMD, S_DATA, S_CRC } state = S_DOLLAR;
    uint8_t buf[256];
    uint8_t want = 0, got = 0, rx_cmd = 0, crc = 0;

    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (xTaskGetTickCount() < deadline) {
        uint8_t byte;
        if (rx_read(&byte, 1, 50) != 1) {
            continue;
        }

        switch (state) {
        case S_DOLLAR:
            state = (byte == '$') ? S_M : S_DOLLAR;
            break;
        case S_M:
            state = (byte == 'M') ? S_DIR : S_DOLLAR;
            break;
        case S_DIR:
            // '!' is the FC's "unsupported command" reply; treat it as a frame
            // so its length is consumed rather than resyncing mid-payload.
            state = (byte == '>' || byte == '!') ? S_LEN : S_DOLLAR;
            break;
        case S_LEN:
            want = byte;
            got = 0;
            crc = byte;
            state = S_CMD;
            break;
        case S_CMD:
            rx_cmd = byte;
            crc ^= byte;
            state = want ? S_DATA : S_CRC;
            break;
        case S_DATA:
            buf[got++] = byte;
            crc ^= byte;
            if (got == want) {
                state = S_CRC;
            }
            break;
        case S_CRC:
            state = S_DOLLAR;
            if (byte != crc || rx_cmd != cmd) {
                break;   // not ours, or corrupt: keep looking
            }
            if (payload && len) {
                const uint8_t n = want < *len ? want : *len;
                memcpy(payload, buf, n);
                *len = n;
            }
            return ESP_OK;
        }
    }

    return ESP_ERR_TIMEOUT;
}

esp_err_t fc_reboot_via_msp(void)
{
    rx_drain();

    // Ask what kind of bootloader the target has. If it will not say - an older
    // build, or a board already wedged - fall back to the ROM bootloader, which
    // every supported MCU has.
    uint8_t mode = MSP_REBOOT_BOOTLOADER_ROM;
    uint8_t info[64];
    uint8_t info_len = sizeof(info);
    msp_send(MSP_BOARD_INFO, NULL, 0);
    if (msp_recv(MSP_BOARD_INFO, info, &info_len, 2000) == ESP_OK &&
        info_len > BOARD_INFO_CAPS_OFFSET) {
        const uint8_t caps = info[BOARD_INFO_CAPS_OFFSET];
        if (caps & (1u << TARGET_HAS_FLASH_BOOTLOADER)) {
            mode = MSP_REBOOT_BOOTLOADER_FLASH;
        }
        ESP_LOGI(TAG, "target capabilities 0x%02x; rebooting via %s bootloader",
                 caps, mode == MSP_REBOOT_BOOTLOADER_FLASH ? "flash" : "ROM");
    } else {
        ESP_LOGW(TAG, "no board info; assuming ROM bootloader");
    }

    // The FC resets without flushing its VCP, so there is usually no reply.
    // Sending is all we can confirm; the caller waits for the DFU device.
    msp_send(MSP_REBOOT, &mode, 1);
    return ESP_OK;
}

// ------------------------------------------------------------------- CLI

esp_err_t fc_reboot_via_cli(void)
{
    rx_drain();
    // No argument: the firmware knows whether it has a flash bootloader and
    // picks accordingly, so we do not have to.
    tx_line("bl");
    return ESP_OK;
}

esp_err_t fc_cli_enter(void)
{
    rx_drain();
    // '#' alone leaves the FC treating whatever follows on the same line as a
    // comment; the newline is what actually opens the prompt.
    tx("#\n", 2);
    vTaskDelay(pdMS_TO_TICKS(CLI_ENTER_WAIT_MS));
    rx_drain();
    return ESP_OK;
}

// True once the collected text ends at a CLI prompt, which the FC writes as
// "# " with nothing after it.
//
// Trailing whitespace must not be trimmed before this test. A dump contains
// comment lines like "# feature", and reads land on arbitrary boundaries, so a
// chunk ending just after that '#' would otherwise look like a prompt and the
// backup would be silently truncated mid-config.
static bool ends_with_prompt(const char *text, size_t len)
{
    return len >= 2 && text[len - 2] == '#' && text[len - 1] == ' ';
}

// The CLI speaks plain text. A byte outside it means the FC never left MSP
// mode and we are reading a binary frame, which is worth failing on straight
// away rather than storing as a "backup".
static bool is_cli_text(uint8_t c)
{
    return c == '\t' || c == '\r' || c == '\n' || (c >= 0x20 && c <= 0x7e);
}

void fc_cli_backup_free(void)
{
    free(s_backup);
    s_backup = NULL;
    s_backup_len = 0;
}

const char *fc_cli_backup_text(size_t *len)
{
    if (len) {
        *len = s_backup_len;
    }
    return s_backup;
}

esp_err_t fc_cli_backup(void)
{
    fc_cli_backup_free();

    size_t cap = BACKUP_CHUNK;
    // PSRAM where the board has it, so a large dump does not eat the internal
    // heap the WiFi and USB stacks need.
    s_backup = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_backup) {
        s_backup = malloc(cap);
    }
    if (!s_backup) {
        return ESP_ERR_NO_MEM;
    }

    rx_drain();
    tx_line("diff all");

    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(CLI_PROMPT_MS);
    while (xTaskGetTickCount() < deadline) {
        if (s_backup_len + 512 > cap) {
            if (cap >= BACKUP_MAX) {
                ESP_LOGE(TAG, "backup exceeded %d bytes", BACKUP_MAX);
                fc_cli_backup_free();
                return ESP_ERR_NO_MEM;
            }
            const size_t want = cap * 2 > BACKUP_MAX ? BACKUP_MAX : cap * 2;
            char *grown = realloc(s_backup, want);
            if (!grown) {
                fc_cli_backup_free();
                return ESP_ERR_NO_MEM;
            }
            s_backup = grown;
            cap = want;
        }

        const size_t n = rx_read((uint8_t *)s_backup + s_backup_len,
                                 cap - s_backup_len - 1, 500);
        if (n == 0) {
            continue;
        }
        for (size_t i = 0; i < n; i++) {
            if (!is_cli_text((uint8_t)s_backup[s_backup_len + i])) {
                ESP_LOGE(TAG, "non-text in CLI reply; FC is still in MSP mode");
                fc_cli_backup_free();
                return ESP_ERR_INVALID_RESPONSE;
            }
        }
        s_backup_len += n;
        s_backup[s_backup_len] = '\0';

        if (ends_with_prompt(s_backup, s_backup_len)) {
            ESP_LOGI(TAG, "backup captured, %u bytes", (unsigned)s_backup_len);
            return ESP_OK;
        }
    }

    ESP_LOGE(TAG, "CLI prompt never returned");
    fc_cli_backup_free();
    return ESP_ERR_TIMEOUT;
}

// Wait for the prompt that marks the end of a command's output, collecting it
// so the caller can tell an accepted line from a rejected one.
static bool await_prompt(char *out, size_t out_len, uint32_t timeout_ms)
{
    size_t len = 0;
    out[0] = '\0';

    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (xTaskGetTickCount() < deadline) {
        const size_t n = rx_read((uint8_t *)out + len, out_len - len - 1, 100);
        if (n == 0) {
            continue;
        }
        len += n;
        out[len] = '\0';
        if (ends_with_prompt(out, len)) {
            return true;
        }
        if (len + 1 >= out_len) {
            return false;
        }
    }
    return false;
}

esp_err_t fc_cli_restore(uint32_t *applied, uint32_t *skipped)
{
    if (!s_backup || s_backup_len == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t ok_count = 0, bad_count = 0;
    char reply[256];

    // The dump records only what differs from defaults, so the board has to be
    // back at defaults for it to mean the same thing. nosave keeps it in RAM
    // until the final save, so one reboot covers the whole restore.
    rx_drain();
    tx_line("defaults nosave");
    await_prompt(reply, sizeof(reply), 5000);

    const char *p = s_backup;
    const char *end = s_backup + s_backup_len;
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t len = nl ? (size_t)(nl - p) : (size_t)(end - p);

        char line[256];
        size_t copy = len;
        while (copy && (p[copy - 1] == '\r' || p[copy - 1] == ' ')) {
            copy--;
        }
        if (copy >= sizeof(line)) {
            copy = sizeof(line) - 1;
        }
        memcpy(line, p, copy);
        line[copy] = '\0';
        p = nl ? nl + 1 : end;

        // Skip blanks, comments, the echoed command and anything that would
        // reboot the board out from under us.
        const char *cmd = line;
        while (*cmd == ' ') {
            cmd++;
        }
        if (*cmd == '\0' || *cmd == '#' ||
            strncmp(cmd, "diff", 4) == 0 || strncmp(cmd, "defaults", 8) == 0 ||
            strcmp(cmd, "save") == 0 || strcmp(cmd, "exit") == 0) {
            continue;
        }

        tx_line(cmd);
        if (!await_prompt(reply, sizeof(reply), CLI_LINE_MS)) {
            bad_count++;
            continue;
        }
        // The CLI answers a bad setting with its own error text rather than a
        // status code, so matching on it is the only signal available.
        if (strstr(reply, "Invalid") || strstr(reply, "ERROR") || strstr(reply, "error")) {
            ESP_LOGW(TAG, "rejected: %s", cmd);
            bad_count++;
        } else {
            ok_count++;
        }
    }

    // save writes the config and reboots the FC.
    tx_line("save");
    ESP_LOGI(TAG, "restore complete: %u applied, %u skipped",
             (unsigned)ok_count, (unsigned)bad_count);

    if (applied) {
        *applied = ok_count;
    }
    if (skipped) {
        *skipped = bad_count;
    }
    return ESP_OK;
}
