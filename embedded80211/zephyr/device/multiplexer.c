#include <zephyr/logging/log.h>
#include <errno.h>

#include <embedded80211/multiplexer.h>
#include "wifi_mgmt_proxy.h"
#include "wifi_data.h"
#include "net_if_mgmt_proxy.h"
#include "vserial.h"

/* Forward declarations for BT/OT files without separate headers. */
bool bt_proxy_acquire_buffer(uint8_t **data, size_t len);
void bt_proxy_commit_data(size_t len);
void bt_proxy_message_complete(size_t len);

bool ot_proxy_acquire_buffer(uint8_t **data, size_t len);
void ot_proxy_commit_data(size_t len);
void ot_proxy_message_complete(size_t len);

LOG_MODULE_REGISTER(serial_mux, LOG_LEVEL_INF);

struct mux_iface_entry {
    bool registered;
    const struct device *dev;
    struct serial_config *config;
};

static struct mux_iface_entry mux_ifaces[SERIAL_MUX_IFACE_COUNT];

static struct client_callbacks mux_callbacks[SERIAL_TYPE_UNKNOWN] = {
#if defined(CONFIG_SERIAL_PROXY_WIFI_MGMT)
    [SERIAL_TYPE_WIFI_MGMT] = {
        .acquire_buffer = wifi_mgmt_acquire_buffer,
        .commit_data = wifi_mgmt_commit_data,
        .message_complete = wifi_mgmt_message_complete,
    },
#endif
#if defined(CONFIG_SERIAL_PROXY_WIFI_DATA)
    [SERIAL_TYPE_WIFI_DATA] = {
        .acquire_buffer = wifi_data_acquire_buffer,
        .commit_data = wifi_data_commit_data,
        .message_complete = wifi_data_message_complete,
    },
#endif
#if defined(CONFIG_SERIAL_PROXY_NET_IF_MGMT)
    [SERIAL_TYPE_NET_IF_MGMT] = {
        .acquire_buffer = net_if_mgmt_acquire_buffer,
        .commit_data = net_if_mgmt_commit_data,
        .message_complete = net_if_mgmt_message_complete,
    },
#endif
#if defined(CONFIG_SERIAL_PROXY_BT)
    [SERIAL_TYPE_HCI] = {
        .acquire_buffer = bt_proxy_acquire_buffer,
        .commit_data = bt_proxy_commit_data,
        .message_complete = bt_proxy_message_complete,
    },
#endif
#if defined(CONFIG_SERIAL_PROXY_OT)
    [SERIAL_TYPE_THREAD] = {
        .acquire_buffer = ot_proxy_acquire_buffer,
        .commit_data = ot_proxy_commit_data,
        .message_complete = ot_proxy_message_complete,
    },
#endif
#if defined(CONFIG_SERIAL_PROXY_VSERIAL)
    [SERIAL_TYPE_VSERIAL] = {
        .acquire_buffer = vserial_acquire_buffer,
        .commit_data = vserial_commit_data,
        .message_complete = vserial_message_complete,
    },
#endif
};

static bool serial_mux_iface_enabled(enum serial_mux_iface_id iface_id)
{
    switch (iface_id) {
    case SERIAL_MUX_IFACE_UART:
#if defined(CONFIG_SERIAL_MUX_IFACE_UART)
        return true;
#else
        return false;
#endif
    case SERIAL_MUX_IFACE_USB:
#if defined(CONFIG_SERIAL_MUX_IFACE_USB)
        return true;
#else
        return false;
#endif
    default:
        return false;
    }
}

static enum serial_mux_iface_id serial_mux_route_for_type(enum SerialType type)
{
    switch (type) {
    case SERIAL_TYPE_WIFI_MGMT:
#if defined(CONFIG_SERIAL_MUX_WIFI_MGMT_IFACE_USB)
        return SERIAL_MUX_IFACE_USB;
#else
        return SERIAL_MUX_IFACE_UART;
#endif
    case SERIAL_TYPE_WIFI_DATA:
#if defined(CONFIG_SERIAL_MUX_WIFI_DATA_IFACE_USB)
        return SERIAL_MUX_IFACE_USB;
#else
        return SERIAL_MUX_IFACE_UART;
#endif
    case SERIAL_TYPE_NET_IF_MGMT:
#if defined(CONFIG_SERIAL_MUX_NET_IF_MGMT_IFACE_USB)
        return SERIAL_MUX_IFACE_USB;
#else
        return SERIAL_MUX_IFACE_UART;
#endif
    case SERIAL_TYPE_HCI:
#if defined(CONFIG_SERIAL_MUX_BT_IFACE_USB)
        return SERIAL_MUX_IFACE_USB;
#else
        return SERIAL_MUX_IFACE_UART;
#endif
    case SERIAL_TYPE_THREAD:
#if defined(CONFIG_SERIAL_MUX_OT_IFACE_USB)
        return SERIAL_MUX_IFACE_USB;
#else
        return SERIAL_MUX_IFACE_UART;
#endif
    case SERIAL_TYPE_VSERIAL:
#if defined(CONFIG_SERIAL_MUX_VSERIAL_IFACE_USB)
        return SERIAL_MUX_IFACE_USB;
#else
        return SERIAL_MUX_IFACE_UART;
#endif
    default:
        return SERIAL_MUX_IFACE_UART;
    }
}

int serial_mux_register_interface(enum serial_mux_iface_id iface_id,
                                  const struct device *dev,
                                  struct serial_config *config)
{
    if (iface_id >= SERIAL_MUX_IFACE_COUNT || dev == NULL || config == NULL) {
        return -EINVAL;
    }

    if (!serial_mux_iface_enabled(iface_id)) {
        LOG_WRN("Interface %d registration ignored: disabled by Kconfig", iface_id);
        return -ENOTSUP;
    }

    mux_ifaces[iface_id].registered = true;
    mux_ifaces[iface_id].dev = dev;
    mux_ifaces[iface_id].config = config;

    LOG_INF("Registered serial interface %d (%s)", iface_id, dev->name);
    return 0;
}

const struct client_callbacks *serial_mux_get_callbacks(enum SerialType type)
{
    if (type >= SERIAL_TYPE_UNKNOWN) {
        return NULL;
    }

    if (mux_callbacks[type].acquire_buffer == NULL ||
        mux_callbacks[type].commit_data == NULL ||
        mux_callbacks[type].message_complete == NULL) {
        return NULL;
    }

    return &mux_callbacks[type];
}

uint16_t serial_mux_supported_types_mask(void)
{
    uint16_t mask = 0;

    for (int i = 0; i < SERIAL_TYPE_UNKNOWN; i++) {
        if (serial_mux_get_callbacks((enum SerialType)i) != NULL) {
            mask |= (uint16_t)(1U << i);
        }
    }

    mask |= (uint16_t)(1U << SERIAL_TYPE_PROTOCOL_INFO);
    return mask;
}

int serial_mux_send(enum SerialType type, const uint8_t *payload, size_t len)
{
    enum serial_mux_iface_id iface_id = serial_mux_route_for_type(type);

    if (iface_id >= SERIAL_MUX_IFACE_COUNT || !mux_ifaces[iface_id].registered ||
        mux_ifaces[iface_id].config == NULL) {
        LOG_ERR("No serial interface registered for type %d (iface %d)", type, iface_id);
        return -ENODEV;
    }

    return serial_send_frame(mux_ifaces[iface_id].config,
                             type,
                             ERROR_TYPE_NO_ERROR,
                             false,
                             payload,
                             len);
}
