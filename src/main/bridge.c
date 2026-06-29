#include "bridge.h"

#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"

// Sized for MSP traffic: frames are small but dumps (e.g. `diff all`, blackbox
// config) can burst several KB. 8 KB each direction gives comfortable slack.
#define BRIDGE_BUF_SIZE   (8 * 1024)
// Wake a draining task as soon as a single byte is available — MSP is latency
// sensitive and we do our own batching on the read side.
#define BRIDGE_TRIGGER    1

static StreamBufferHandle_t s_usb_to_net;  // FC  -> Configurator
static StreamBufferHandle_t s_net_to_usb;  // Configurator -> FC

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
    configASSERT(s_usb_to_net);
    configASSERT(s_net_to_usb);
}

// Arbitration for the single FC stream. The claim is a tiny spinlock-guarded
// owner field; the buffer reset must run outside the critical section.
static portMUX_TYPE s_owner_mux = portMUX_INITIALIZER_UNLOCKED;
static bridge_client_t s_owner = BRIDGE_CLIENT_NONE;

bool bridge_try_claim(bridge_client_t who)
{
    bool ok = false;
    taskENTER_CRITICAL(&s_owner_mux);
    if (s_owner == BRIDGE_CLIENT_NONE) {
        s_owner = who;
        ok = true;
    }
    taskEXIT_CRITICAL(&s_owner_mux);
    if (ok) {
        bridge_reset();   // fresh session: drop any stale MSP bytes
    }
    return ok;
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
    return xStreamBufferReceive(s_usb_to_net, out, max_len, pdMS_TO_TICKS(timeout_ms));
}

size_t bridge_net_to_usb_push(const uint8_t *data, size_t len)
{
    size_t sent = xStreamBufferSend(s_net_to_usb, data, len, 0);
    s_fc_activity += sent;
    return sent;
}

size_t bridge_net_to_usb_pop(uint8_t *out, size_t max_len, uint32_t timeout_ms)
{
    return xStreamBufferReceive(s_net_to_usb, out, max_len, pdMS_TO_TICKS(timeout_ms));
}

void bridge_reset(void)
{
    if (s_usb_to_net) {
        xStreamBufferReset(s_usb_to_net);
    }
    if (s_net_to_usb) {
        xStreamBufferReset(s_net_to_usb);
    }
}
