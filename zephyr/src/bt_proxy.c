#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(bt_proxy, LOG_LEVEL_DBG);

/* HCI frames are not yet handled; just ACK and drop. */

bool bt_proxy_acquire_buffer(uint8_t **data, size_t len)
{
    /* Discard buffer — use a static scratch area. */
    static uint8_t scratch[256];

    if (len > sizeof(scratch)) {
        LOG_WRN("bt_proxy: frame too large (%zu), dropping", len);
        return false;
    }
    *data = scratch;
    return true;
}

void bt_proxy_commit_data(size_t len)
{
    ARG_UNUSED(len);
}

void bt_proxy_message_complete(size_t len)
{
    LOG_DBG("bt_proxy: received HCI frame (%zu bytes) — stub, dropping", len);
}
