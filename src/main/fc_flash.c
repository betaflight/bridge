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

#include "fc_flash.h"
#include "bridge.h"
#include "dfu_host.h"
#include "fc_cli.h"
#include "hex_parser.h"
#include "usb_cdc_host.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_rom_crc.h"

static const char *TAG = "fc_flash";

// Holds roughly one HTTP slice, so the upload handler rarely blocks while the
// flasher is keeping up.
#define UPLOAD_BUF_SIZE     (16 * 1024)
#define UPLOAD_PUSH_MS      200
#define UPLOAD_STALL_MS     30000

// A Betaflight hex is two or three contiguous runs; the rest is headroom for
// custom-defaults blocks spliced in by other tooling.
#define MAX_WRITE_RUNS      32

#define BOOTLOADER_WAIT_MS  10000
// Generous: the FC has to boot, the DFU client has to notice the device left
// and close it before the port is recycled, and only then does the CDC driver
// get a chance to reopen the VCP. 20 s was marginal in practice.
#define VCP_RETURN_WAIT_MS  45000

typedef enum {
    PH_BACKUP = 0,
    PH_BOOTLOADER,
    PH_PROBE,
    PH_ERASE,
    PH_WRITE,
    PH_VERIFY,
    PH_FINISH,
    PH_RESTORE,
    PH_COUNT,
} phase_t;

typedef enum {
    ST_PENDING = 0,
    ST_ACTIVE,
    ST_DONE,
    ST_SKIPPED,
    ST_FAILED,
} pstate_t;

static const char *const k_phase_name[PH_COUNT] = {
    "backup", "bootloader", "probe", "erase", "write", "verify", "finish", "restore",
};

// One contiguous run of written flash, checked after the fact by reading it
// back. Storing a CRC per run rather than the bytes is what lets verification
// work without a copy of the image.
typedef struct {
    uint32_t address;
    uint32_t length;
    uint32_t crc;
} write_run_t;

typedef struct {
    bool backup;
    bool erase_all;
    bool verify;
    bool restore;
    uint32_t total;             // uploaded file size, for progress

    volatile bool running;
    volatile bool abort;
    volatile uint32_t consumed; // bytes of the file taken off the wire

    pstate_t state[PH_COUNT];
    char detail[PH_COUNT][64];
    phase_t phase;
    uint32_t percent;           // progress within the active phase
    char error[128];

    StreamBufferHandle_t input;

    // DFU session
    dfu_host_info_t info;
    dfu_region_t region;
    uint16_t transfer_size;
    uint8_t erased[DFU_MAX_PAGES / 8];

    // Staging: hex records are accumulated until a full transfer-size block is
    // ready or the address jumps.
    uint8_t stage[DFU_MAX_TRANSFER_SIZE];
    uint32_t stage_addr;
    uint32_t stage_len;
    uint16_t block;             // DFU wBlockNum, reset to 2 per set-address
    bool addr_valid;
    uint32_t next_addr;         // where the current run is expected to continue

    write_run_t runs[MAX_WRITE_RUNS];
    uint32_t run_count;
    uint32_t written;

    bool failed;
} job_t;

static job_t s_job;
static SemaphoreHandle_t s_job_lock;

// ------------------------------------------------------------------ helpers

static void fail(job_t *j, const char *fmt, ...)
{
    if (!j->failed) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(j->error, sizeof(j->error), fmt, ap);
        va_end(ap);
        j->failed = true;
        ESP_LOGE(TAG, "%s", j->error);
    }
}

static void phase_begin(job_t *j, phase_t p)
{
    j->phase = p;
    j->state[p] = ST_ACTIVE;
    j->percent = 0;
}

static void phase_done(job_t *j, phase_t p, const char *fmt, ...)
{
    j->state[p] = ST_DONE;
    j->percent = 100;
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(j->detail[p], sizeof(j->detail[p]), fmt, ap);
        va_end(ap);
    }
}

static void phase_skip(job_t *j, phase_t p, const char *why)
{
    j->state[p] = ST_SKIPPED;
    snprintf(j->detail[p], sizeof(j->detail[p]), "%s", why ? why : "");
}

static bool aborting(job_t *j)
{
    if (j->abort && !j->failed) {
        fail(j, "cancelled");
    }
    return j->failed;
}

static bool page_is_erased(const job_t *j, uint32_t page)
{
    return page < DFU_MAX_PAGES && (j->erased[page / 8] & (1u << (page % 8)));
}

static void mark_erased(job_t *j, uint32_t page)
{
    if (page < DFU_MAX_PAGES) {
        j->erased[page / 8] |= (uint8_t)(1u << (page % 8));
    }
}

// ------------------------------------------------------------------ writing

// Erase every page overlapping [address, address+len), skipping any already
// done. Erasing on demand rather than up front is what lets the hex be
// streamed; the bitmap is what keeps a second, lower record from wiping data
// already written to the same page.
static bool ensure_erased(job_t *j, uint32_t address, uint32_t len, bool *erased_any)
{
    uint32_t offset = 0;
    while (offset < len) {
        uint32_t page;
        if (!dfu_region_page_of(&j->region, address + offset, &page)) {
            fail(j, "address 0x%08" PRIx32 " is outside %s", address + offset, j->region.name);
            return false;
        }

        uint32_t page_start = 0, page_size = 0;
        if (!dfu_region_page(&j->region, page, &page_start, &page_size)) {
            fail(j, "no page %" PRIu32 " in %s", page, j->region.name);
            return false;
        }

        if (!page_is_erased(j, page)) {
            if (dfu_host_erase_page(page_start) != ESP_OK) {
                fail(j, "erase of page %" PRIu32 " at 0x%08" PRIx32 " failed", page, page_start);
                return false;
            }
            mark_erased(j, page);
            *erased_any = true;
        }

        const uint32_t consumed = page_start + page_size - (address + offset);
        offset += consumed;
    }
    return true;
}

static bool flush_stage(job_t *j)
{
    if (j->stage_len == 0) {
        return true;
    }

    bool erased_now = false;
    if (!ensure_erased(j, j->stage_addr, j->stage_len, &erased_now)) {
        return false;
    }

    const bool new_run = !j->addr_valid || j->stage_addr != j->next_addr;

    // The device derives each block's address from the pointer set earlier plus
    // the block number, so the pointer has to be re-established whenever that
    // arithmetic is interrupted: at the start of a run, and after an erase.
    // Erasing between data blocks is peculiar to streaming - Configurator
    // erases everything up front - and skipping this re-set silently writes
    // the rest of the run to the wrong place.
    if (new_run || erased_now) {
        if (dfu_host_set_address(j->stage_addr) != ESP_OK) {
            fail(j, "could not set address 0x%08" PRIx32, j->stage_addr);
            return false;
        }
        j->block = 2;
        j->addr_valid = true;
    }

    if (new_run) {
        if (j->run_count >= MAX_WRITE_RUNS) {
            fail(j, "hex has more than %d separate regions", MAX_WRITE_RUNS);
            return false;
        }
        j->runs[j->run_count++] = (write_run_t){ .address = j->stage_addr, .crc = 0 };
    }

    if (dfu_host_write_block(j->block, j->stage, (uint16_t)j->stage_len) != ESP_OK) {
        fail(j, "write failed at 0x%08" PRIx32, j->stage_addr);
        return false;
    }
    j->block++;

    write_run_t *run = &j->runs[j->run_count - 1];
    run->crc = esp_rom_crc32_le(run->crc, j->stage, j->stage_len);
    run->length += j->stage_len;

    j->next_addr = j->stage_addr + j->stage_len;
    j->written += j->stage_len;
    j->stage_len = 0;
    return true;
}

// Accumulate hex records into transfer-size blocks. Records are small and need
// not be contiguous, so a jump closes the current block early.
static bool on_hex_data(uint32_t address, const uint8_t *data, size_t len, void *ctx)
{
    job_t *j = ctx;

    while (len) {
        if (aborting(j)) {
            return false;
        }
        if (j->stage_len && address != j->stage_addr + j->stage_len) {
            if (!flush_stage(j)) {
                return false;
            }
        }
        if (j->stage_len == 0) {
            j->stage_addr = address;
        }

        size_t n = j->transfer_size - j->stage_len;
        if (n > len) {
            n = len;
        }
        memcpy(j->stage + j->stage_len, data, n);
        j->stage_len += n;
        address += n;
        data += n;
        len -= n;

        if (j->stage_len == j->transfer_size && !flush_stage(j)) {
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------- verifying

// Reading back also has to re-set the pointer per run, for the same reason.
static bool verify_runs(job_t *j)
{
    uint8_t buf[DFU_MAX_TRANSFER_SIZE];
    uint32_t verified = 0;
    uint32_t total = 0;
    for (uint32_t i = 0; i < j->run_count; i++) {
        total += j->runs[i].length;
    }

    for (uint32_t i = 0; i < j->run_count; i++) {
        const write_run_t *run = &j->runs[i];

        // Reading needs the device idle, and the abort that gets it there has
        // to come after the address pointer is set, not before.
        if (dfu_host_clear_state(false) != ESP_OK ||
            dfu_host_set_address(run->address) != ESP_OK ||
            dfu_host_clear_state(false) != ESP_OK) {
            fail(j, "could not prepare to read back 0x%08" PRIx32, run->address);
            return false;
        }

        uint32_t crc = 0;
        uint32_t done = 0;
        uint16_t block = 2;
        while (done < run->length) {
            if (aborting(j)) {
                return false;
            }
            size_t want = run->length - done;
            if (want > j->transfer_size) {
                want = j->transfer_size;
            }
            size_t got = 0;
            if (dfu_host_upload(block, buf, (uint16_t)want, &got) != ESP_OK || got != want) {
                fail(j, "read back failed at 0x%08" PRIx32, run->address + done);
                return false;
            }
            crc = esp_rom_crc32_le(crc, buf, got);
            done += got;
            block++;

            verified += got;
            j->percent = total ? verified * 100 / total : 100;
            snprintf(j->detail[PH_VERIFY], sizeof(j->detail[PH_VERIFY]),
                     "0x%08" PRIx32 " · %" PRIu32 " of %" PRIu32 " KiB",
                     run->address + done, verified / 1024, total / 1024);
        }

        if (crc != run->crc) {
            fail(j, "verify failed: 0x%08" PRIx32 "..0x%08" PRIx32 " does not match",
                 run->address, run->address + run->length - 1);
            return false;
        }
    }

    return true;
}

// ------------------------------------------------------------------- phases

static bool do_backup(job_t *j)
{
    if (!j->backup) {
        phase_skip(j, PH_BACKUP, "not requested");
        return true;
    }
    if (dfu_host_is_present()) {
        phase_skip(j, PH_BACKUP, "FC already in bootloader");
        return true;
    }
    if (!usb_cdc_host_is_connected()) {
        fail(j, "no FC connected");
        return false;
    }

    phase_begin(j, PH_BACKUP);

    // One retry: a FC still finishing its boot swallows the '#' that opens the
    // CLI, which looks identical to a board that will never answer. Cheap to
    // ask twice, and the alternative is refusing to flash a healthy board.
    fc_cli_enter();
    if (fc_cli_backup() != ESP_OK) {
        ESP_LOGW(TAG, "no CLI on the first try; retrying");
        vTaskDelay(pdMS_TO_TICKS(3000));
        fc_cli_enter();
        if (fc_cli_backup() != ESP_OK) {
            fail(j, "could not save the configuration; flash aborted to avoid losing it");
            return false;
        }
    }

    size_t len = 0;
    fc_cli_backup_text(&len);
    phase_done(j, PH_BACKUP, "%u bytes", (unsigned)len);
    return true;
}

static bool do_bootloader(job_t *j)
{
    if (dfu_host_is_present()) {
        phase_skip(j, PH_BOOTLOADER, "already in bootloader");
        return true;
    }

    phase_begin(j, PH_BOOTLOADER);

    // The CLI route first: after a backup the FC is already at the prompt, and
    // MSP frames sent to a CLI are just noise. `bl` also lets the firmware pick
    // which of its bootloaders to enter.
    fc_cli_enter();
    fc_reboot_via_cli();

    if (!dfu_host_wait(BOOTLOADER_WAIT_MS)) {
        // An FC that never reached the CLI still answers MSP.
        ESP_LOGW(TAG, "no DFU device after 'bl'; trying MSP");
        fc_reboot_via_msp();
        if (!dfu_host_wait(BOOTLOADER_WAIT_MS)) {
            fail(j, "FC did not enter DFU mode; it may need the boot pins or a power cycle");
            return false;
        }
    }

    dfu_host_info_t info;
    if (dfu_host_get_info(&info)) {
        phase_done(j, PH_BOOTLOADER, "%04x:%04x", info.vid, info.pid);
    } else {
        phase_done(j, PH_BOOTLOADER, "in DFU mode");
    }
    return true;
}

static bool do_probe(job_t *j)
{
    phase_begin(j, PH_PROBE);

    if (!dfu_host_get_info(&j->info)) {
        fail(j, "DFU device went away");
        return false;
    }

    const dfu_region_t *region = dfu_layout_program_region(&j->info.layout);
    if (!region) {
        fail(j, "bootloader did not report a flash memory layout");
        return false;
    }
    j->region = *region;
    j->transfer_size = j->info.transfer_size;

    if (dfu_region_page_count(&j->region) > DFU_MAX_PAGES) {
        fail(j, "%s has more than %d pages", j->region.name, DFU_MAX_PAGES);
        return false;
    }

    // Read protection shows up as the bootloader refusing to point at the
    // option bytes. Say so plainly: clearing it mass-erases the chip, which is
    // the user's call to make, not ours to do silently mid-flash.
    const dfu_region_t *opt = dfu_layout_find(&j->info.layout, "option_bytes");
    if (opt) {
        dfu_status_t st;
        if (dfu_host_clear_state(false) == ESP_OK &&
            dfu_host_set_address_probe(opt->start_address, &st) == ESP_OK &&
            st.state == DFU_STATE_ERROR && st.status == DFU_STATUS_ERR_VENDOR) {
            fail(j, "flash is read protected; clear it with a full chip erase first");
            return false;
        }
    }

    if (dfu_host_clear_state(false) != ESP_OK) {
        fail(j, "bootloader would not return to idle");
        return false;
    }

    phase_done(j, PH_PROBE, "%04x:%04x · %s · %" PRIu32 " KiB",
               j->info.vid, j->info.pid, j->region.name, j->region.total_size / 1024);
    return true;
}

static bool do_erase_all(job_t *j)
{
    if (!j->erase_all) {
        // The pages the image touches are erased as they are reached; see
        // ensure_erased().
        phase_skip(j, PH_ERASE, "erasing as needed");
        return true;
    }

    phase_begin(j, PH_ERASE);
    const uint32_t pages = dfu_region_page_count(&j->region);
    for (uint32_t i = 0; i < pages; i++) {
        if (aborting(j)) {
            return false;
        }
        uint32_t start = 0;
        if (!dfu_region_page(&j->region, i, &start, NULL)) {
            break;
        }
        if (dfu_host_erase_page(start) != ESP_OK) {
            fail(j, "erase of page %" PRIu32 " at 0x%08" PRIx32 " failed", i, start);
            return false;
        }
        mark_erased(j, i);
        j->percent = (i + 1) * 100 / pages;
        snprintf(j->detail[PH_ERASE], sizeof(j->detail[PH_ERASE]),
                 "%" PRIu32 " of %" PRIu32 " pages", i + 1, pages);
    }

    phase_done(j, PH_ERASE, "%" PRIu32 " pages", pages);
    return true;
}

static bool do_write(job_t *j)
{
    phase_begin(j, PH_WRITE);

    hex_parser_t parser;
    hex_parser_init(&parser, on_hex_data, j);

    uint8_t buf[1024];
    uint32_t idle_ms = 0;
    while (j->consumed < j->total) {
        if (aborting(j)) {
            return false;
        }
        const size_t n = xStreamBufferReceive(j->input, buf, sizeof(buf), pdMS_TO_TICKS(100));
        if (n == 0) {
            idle_ms += 100;
            if (idle_ms >= UPLOAD_STALL_MS) {
                fail(j, "upload stalled");
                return false;
            }
            continue;
        }
        idle_ms = 0;

        if (!hex_parser_push(&parser, buf, n)) {
            if (!j->failed) {
                fail(j, "%s", hex_parser_error(&parser));
            }
            return false;
        }
        j->consumed += n;

        j->percent = j->total ? j->consumed * 100 / j->total : 0;
        snprintf(j->detail[PH_WRITE], sizeof(j->detail[PH_WRITE]),
                 "0x%08" PRIx32 " · %" PRIu32 " KiB written",
                 j->next_addr, j->written / 1024);
    }

    if (!hex_parser_finish(&parser)) {
        fail(j, "%s", hex_parser_error(&parser));
        return false;
    }
    if (!flush_stage(j)) {
        return false;
    }
    if (j->written == 0) {
        fail(j, "hex contained no data");
        return false;
    }

    phase_done(j, PH_WRITE, "%" PRIu32 " KiB in %" PRIu32 " region(s)",
               j->written / 1024, j->run_count);
    return true;
}

static bool do_verify(job_t *j)
{
    if (!j->verify) {
        phase_skip(j, PH_VERIFY, "not requested");
        return true;
    }
    phase_begin(j, PH_VERIFY);
    if (!verify_runs(j)) {
        return false;
    }
    phase_done(j, PH_VERIFY, "%" PRIu32 " KiB matched", j->written / 1024);
    return true;
}

static bool do_finish(job_t *j)
{
    phase_begin(j, PH_FINISH);
    // Leaving is best-effort: the device detaches as it starts the firmware,
    // so the final status read usually fails and that is not an error.
    dfu_host_leave(j->run_count ? j->runs[0].address : j->region.start_address);

    // Hand the port back explicitly rather than waiting to be told the device
    // went away: until the DFU client closes it, the CDC side will not look for
    // the FC's VCP again.
    dfu_host_release();

    phase_done(j, PH_FINISH, "firmware started");
    return true;
}

static bool do_restore(job_t *j)
{
    size_t len = 0;
    if (!j->restore || !fc_cli_backup_text(&len) || len == 0) {
        phase_skip(j, PH_RESTORE, j->restore ? "no backup to restore" : "not requested");
        return true;
    }

    phase_begin(j, PH_RESTORE);

    // Wait for the freshly flashed firmware to come back as a VCP.
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(VCP_RETURN_WAIT_MS);
    while (!usb_cdc_host_is_connected() && xTaskGetTickCount() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    if (!usb_cdc_host_is_connected()) {
        fail(j, "FC did not come back after flashing; the backup is still downloadable");
        return false;
    }
    // Give the firmware a moment past enumeration before talking to it.
    vTaskDelay(pdMS_TO_TICKS(2000));

    fc_cli_enter();
    uint32_t applied = 0, skipped = 0;
    if (fc_cli_restore(&applied, &skipped) != ESP_OK) {
        fail(j, "restore failed; the backup is still downloadable");
        return false;
    }

    phase_done(j, PH_RESTORE, "%" PRIu32 " applied, %" PRIu32 " skipped", applied, skipped);
    return true;
}

// --------------------------------------------------------------------- task

static void flash_task(void *arg)
{
    job_t *j = &s_job;
    ESP_LOGI(TAG, "flash starting (%" PRIu32 " bytes, backup=%d erase_all=%d verify=%d restore=%d)",
             j->total, j->backup, j->erase_all, j->verify, j->restore);

    // Take the FC. Each transport notices it is no longer the owner and drops
    // its own client; reaching across to close them from here would deadlock an
    // httpd worker.
    bridge_claim(BRIDGE_CLIENT_FLASH);
    fc_cli_begin();

    const bool ok = do_backup(j) && do_bootloader(j) && do_probe(j) &&
                    do_erase_all(j) && do_write(j) && do_verify(j) &&
                    do_finish(j) && do_restore(j);

    if (!ok) {
        j->state[j->phase] = ST_FAILED;
        if (j->error[0] == '\0') {
            snprintf(j->error, sizeof(j->error), "failed during %s", k_phase_name[j->phase]);
        }
        ESP_LOGE(TAG, "flash failed: %s", j->error);
    } else {
        ESP_LOGI(TAG, "flash complete");
    }

    fc_cli_end();
    bridge_release(BRIDGE_CLIENT_FLASH);

    j->running = false;
    vTaskDelete(NULL);
}

// --------------------------------------------------------------------- HTTP

static void status_json(char *out, size_t out_len)
{
    const job_t *j = &s_job;
    int n = snprintf(out, out_len,
        "{\"running\":%s,\"phase\":\"%s\",\"percent\":%" PRIu32 ","
        "\"consumed\":%" PRIu32 ",\"total\":%" PRIu32 ",\"written\":%" PRIu32 ","
        "\"backup\":%s,\"error\":\"%s\",\"phases\":[",
        j->running ? "true" : "false", k_phase_name[j->phase], j->percent,
        j->consumed, j->total, j->written,
        fc_cli_backup_text(NULL) ? "true" : "false", j->error);

    for (int i = 0; i < PH_COUNT && n > 0 && (size_t)n < out_len; i++) {
        static const char *const k_state[] = {
            "pending", "active", "done", "skipped", "failed",
        };
        n += snprintf(out + n, out_len - n, "%s{\"name\":\"%s\",\"state\":\"%s\",\"detail\":\"%s\"}",
                      i ? "," : "", k_phase_name[i], k_state[j->state[i]], j->detail[i]);
    }
    if (n > 0 && (size_t)n < out_len) {
        snprintf(out + n, out_len - n, "]}");
    }
}

// esp_http_server's error enum has no 409 or 503, and the distinction matters
// to the page's JS, so set the status line directly.
static esp_err_t send_error(httpd_req_t *req, const char *status, const char *message)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, message);
    return ESP_FAIL;
}

static esp_err_t send_status(httpd_req_t *req)
{
    char body[1400];
    status_json(body, sizeof(body));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body);
}

// Pull an integer option out of a form-encoded body.
static uint32_t form_value(const char *body, const char *key, uint32_t fallback)
{
    char needle[32];
    snprintf(needle, sizeof(needle), "%s=", key);
    const char *p = strstr(body, needle);
    while (p) {
        if (p == body || p[-1] == '&') {
            return (uint32_t)strtoul(p + strlen(needle), NULL, 10);
        }
        p = strstr(p + 1, needle);
    }
    return fallback;
}

static esp_err_t start_post(httpd_req_t *req)
{
    if (xSemaphoreTake(s_job_lock, 0) != pdTRUE) {
        return send_error(req, "409 Conflict", "a flash is already running");
    }

    esp_err_t result = ESP_FAIL;

    if (s_job.running) {
        send_error(req, "409 Conflict", "a flash is already running");
        goto out;
    }

    char body[256] = {0};
    const int want = req->content_len < (int)sizeof(body) - 1
                   ? req->content_len : (int)sizeof(body) - 1;
    if (want > 0 && httpd_req_recv(req, body, want) <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "could not read options");
        goto out;
    }

    const uint32_t total = form_value(body, "size", 0);
    if (total == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing file size");
        goto out;
    }

    StreamBufferHandle_t input = s_job.input;
    if (!input) {
        input = xStreamBufferCreate(UPLOAD_BUF_SIZE, 1);
        if (!input) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
            goto out;
        }
    } else {
        uint8_t junk[128];
        while (xStreamBufferReceive(input, junk, sizeof(junk), 0) > 0) {
        }
    }

    // A previous job's backup is only discarded once a new one starts, so it
    // stays downloadable after a failure.
    fc_cli_backup_free();

    memset(&s_job, 0, sizeof(s_job));
    s_job.input = input;
    s_job.total = total;
    s_job.backup = form_value(body, "backup", 0) != 0;
    s_job.erase_all = form_value(body, "erase_all", 0) != 0;
    s_job.verify = form_value(body, "verify", 1) != 0;
    s_job.restore = form_value(body, "restore", 0) != 0;
    s_job.transfer_size = DFU_MAX_TRANSFER_SIZE;
    s_job.running = true;

    if (xTaskCreate(flash_task, "fc_flash", 8192, NULL, 5, NULL) != pdTRUE) {
        s_job.running = false;
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "could not start");
        goto out;
    }

    result = send_status(req);

out:
    xSemaphoreGive(s_job_lock);
    return result;
}

static esp_err_t data_post(httpd_req_t *req)
{
    job_t *j = &s_job;
    if (!j->running) {
        return send_error(req, "409 Conflict", "no flash is running");
    }

    char buf[1024];
    int remaining = req->content_len;
    while (remaining > 0) {
        const int want = remaining < (int)sizeof(buf) ? remaining : (int)sizeof(buf);
        const int n = httpd_req_recv(req, buf, want);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (n <= 0) {
            return ESP_FAIL;   // client went away; the job's stall timer ends it
        }
        remaining -= n;

        // Push in bounded waits so this handler never holds the single httpd
        // task long enough to stall a /dfu/status poll.
        size_t offset = 0;
        while (offset < (size_t)n) {
            if (!j->running || j->failed) {
                return send_error(req, "409 Conflict",
                                  j->error[0] ? j->error : "flash stopped");
            }
            offset += xStreamBufferSend(j->input, buf + offset, n - offset,
                                        pdMS_TO_TICKS(UPLOAD_PUSH_MS));
        }
    }

    return send_status(req);
}

static esp_err_t status_get(httpd_req_t *req)
{
    return send_status(req);
}

static esp_err_t backup_get(httpd_req_t *req)
{
    size_t len = 0;
    const char *text = fc_cli_backup_text(&len);
    if (!text || len == 0) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no backup captured");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Content-Disposition",
                       "attachment; filename=\"betaflight-backup.txt\"");
    return httpd_resp_send(req, text, len);
}

static esp_err_t abort_post(httpd_req_t *req)
{
    s_job.abort = true;
    return send_status(req);
}

void fc_flash_register(httpd_handle_t server)
{
    if (!s_job_lock) {
        s_job_lock = xSemaphoreCreateMutex();
        configASSERT(s_job_lock);
    }

    const httpd_uri_t routes[] = {
        { .uri = "/dfu/start",  .method = HTTP_POST, .handler = start_post  },
        { .uri = "/dfu/data",   .method = HTTP_POST, .handler = data_post   },
        { .uri = "/dfu/status", .method = HTTP_GET,  .handler = status_get  },
        { .uri = "/dfu/backup", .method = HTTP_GET,  .handler = backup_get  },
        { .uri = "/dfu/abort",  .method = HTTP_POST, .handler = abort_post  },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        httpd_register_uri_handler(server, &routes[i]);
    }
}
