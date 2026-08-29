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

#include "bridge.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "freertos/semphr.h"

// Sized for MSP traffic: frames are small but dumps (e.g. `diff all`, blackbox
// config) can burst several KB. 8 KB each direction gives comfortable slack.
#define BRIDGE_BUF_SIZE   (8 * 1024)
// Wake a draining task as soon as a single byte is available — MSP is latency
// sensitive and we do our own batching on the read side.
#define BRIDGE_TRIGGER    1

static StreamBufferHandle_t s_usb_to_net;  // FC  -> Configurator
static StreamBufferHandle_t s_net_to_usb;  // Configurator -> FC
// One receiver at a time per buffer: a client-takeover reset must never run
// concurrently with a pump task blocked in xStreamBufferReceive (that asserts).
static SemaphoreHandle_t s_u2n_mux;   // guards receives from s_usb_to_net
static SemaphoreHandle_t s_n2u_mux;   // guards receives from s_net_to_usb

// Total bytes moved to/from the FC in either direction. Monotonic; sampled by
// the LED indicator to detect traffic. Plain increments are fine — a missed
// update only costs one LED tick.
static volatile uint32_t s_fc_activity;

uint32_t bridge_fc_activity(void)
{
    return s_fc_activity;
}

void bridge_init(void)
{
    s_usb_to_net = xStreamBufferCreate(BRIDGE_BUF_SIZE, BRIDGE_TRIGGER);
    s_net_to_usb = xStreamBufferCreate(BRIDGE_BUF_SIZE, BRIDGE_TRIGGER);
    s_u2n_mux = xSemaphoreCreateMutex();
    s_n2u_mux = xSemaphoreCreateMutex();
    configASSERT(s_usb_to_net);
    configASSERT(s_net_to_usb);
    configASSERT(s_u2n_mux);
    configASSERT(s_n2u_mux);
}

// Arbitration for the single FC stream. The claim is a tiny spinlock-guarded
// owner field; the buffer reset must run outside the critical section.
static portMUX_TYPE s_owner_mux = portMUX_INITIALIZER_UNLOCKED;
static bridge_client_t s_owner = BRIDGE_CLIENT_NONE;

void bridge_claim(bridge_client_t who)
{
    // Newest client wins: take ownership unconditionally. The previous owner's
    // teardown is gated on still owning, so it won't release this claim.
    taskENTER_CRITICAL(&s_owner_mux);
    s_owner = who;
    taskEXIT_CRITICAL(&s_owner_mux);
    bridge_reset();   // fresh session: drop any stale MSP bytes
}

void bridge_release(bridge_client_t who)
{
    taskENTER_CRITICAL(&s_owner_mux);
    if (s_owner == who) {
        s_owner = BRIDGE_CLIENT_NONE;
    }
    taskEXIT_CRITICAL(&s_owner_mux);
}

bridge_client_t bridge_client_owner(void)
{
    return s_owner;
}

size_t bridge_usb_to_net_push(const uint8_t *data, size_t len)
{
    // Zero timeout: never block the USB driver callback. Dropped bytes here are
    // visible as a stalled/overflowing TCP client rather than a wedged FC link.
    size_t sent = xStreamBufferSend(s_usb_to_net, data, len, 0);
    s_fc_activity += sent;
    return sent;
}

size_t bridge_usb_to_net_pop(uint8_t *out, size_t max_len, uint32_t timeout_ms)
{
    xSemaphoreTake(s_u2n_mux, portMAX_DELAY);
    size_t n = xStreamBufferReceive(s_usb_to_net, out, max_len, pdMS_TO_TICKS(timeout_ms));
    xSemaphoreGive(s_u2n_mux);
    return n;
}

size_t bridge_net_to_usb_push(const uint8_t *data, size_t len)
{
    size_t sent = xStreamBufferSend(s_net_to_usb, data, len, 0);
    s_fc_activity += sent;
    return sent;
}

size_t bridge_net_to_usb_pop(uint8_t *out, size_t max_len, uint32_t timeout_ms)
{
    xSemaphoreTake(s_n2u_mux, portMAX_DELAY);
    size_t n = xStreamBufferReceive(s_net_to_usb, out, max_len, pdMS_TO_TICKS(timeout_ms));
    xSemaphoreGive(s_n2u_mux);
    return n;
}

// xStreamBufferReset() asserts if a task is blocked receiving on the buffer, so
// drain under the same mutex the pump tasks use to read it.
static void drain(StreamBufferHandle_t sb, SemaphoreHandle_t mux)
{
    uint8_t junk[64];
    xSemaphoreTake(mux, portMAX_DELAY);
    while (xStreamBufferReceive(sb, junk, sizeof(junk), 0) > 0) {
    }
    xSemaphoreGive(mux);
}

void bridge_reset(void)
{
    drain(s_usb_to_net, s_u2n_mux);
    drain(s_net_to_usb, s_n2u_mux);
}
