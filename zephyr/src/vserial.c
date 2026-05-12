#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "vserial.h"

LOG_MODULE_REGISTER(vserial, LOG_LEVEL_DBG);

/*
 * Virtual serial interface stub.
 *
 * Per AGENTS.md:
 *   - Baud rate, stop bits, and parity are ignored for transport behavior.
 *   - Values must be shadowed for readback compatibility.
 *   - Serial-port semantics are exposed over the common protocol.
 *
 * This stub accepts frames, shadows configuration state, and logs activity.
 * No actual byte-stream transport is performed.
 */

struct vserial_config {
    uint32_t baud_rate;
    uint8_t  stop_bits;   /* 1 or 2 */
    uint8_t  parity;      /* 0=none, 1=odd, 2=even */
};

static struct vserial_config vserial_state = {
    .baud_rate = 115200,
    .stop_bits = 1,
    .parity    = 0,
};

static uint8_t vserial_scratch[256];

bool vserial_acquire_buffer(uint8_t **data, size_t len)
{
    if (len > sizeof(vserial_scratch)) {
        LOG_WRN("vserial: frame too large (%zu), dropping", len);
        return false;
    }
    *data = vserial_scratch;
    return true;
}

void vserial_commit_data(size_t len)
{
    ARG_UNUSED(len);
}

void vserial_message_complete(size_t len)
{
    /*
     * A real implementation would parse the virtual serial frame here and
     * update vserial_state fields (baud_rate, stop_bits, parity) from the
     * payload for shadowed readback, without affecting transport behavior.
     */
    LOG_DBG("vserial: received frame (%zu bytes) — stub, shadowing not yet parsed", len);
    ARG_UNUSED(vserial_state);
}
