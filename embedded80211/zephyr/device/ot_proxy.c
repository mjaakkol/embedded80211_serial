#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(ot_proxy, LOG_LEVEL_DBG);

/* OpenThread Spinel frames are not yet handled; just ACK and drop. */

bool ot_proxy_acquire_buffer(uint8_t **data, size_t len)
{
    static uint8_t scratch[256];

    if (len > sizeof(scratch)) {
        LOG_WRN("ot_proxy: frame too large (%zu), dropping", len);
        return false;
    }
    *data = scratch;
    return true;
}

void ot_proxy_commit_data(size_t len)
{
    ARG_UNUSED(len);
}

void ot_proxy_message_complete(size_t len)
{
    LOG_DBG("ot_proxy: received Spinel frame (%zu bytes) — stub, dropping", len);
}
