#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/sys/byteorder.h>
#include <errno.h>
#include <string.h>

#include <pb_decode.h>
#include <pb_encode.h>
#include "protos/network_interface.pb.h"
#include "net_if_mgmt_proxy.h"

LOG_MODULE_REGISTER(net_if_mgmt_proxy, LOG_LEVEL_INF);

#define NET_IF_MGMT_THREAD_STACK_SIZE 2048
#define NET_IF_MGMT_THREAD_PRIORITY   7
#define DATA_COMPLETE_EVENT           BIT(0)

K_EVENT_DEFINE(net_if_mgmt_event);

static const struct device *net_if_mgmt_serial_dev;

static uint8_t net_if_mgmt_rx_buffer[512];
static uint8_t net_if_mgmt_tx_buffer[512];
static size_t  message_length;

void net_if_mgmt_proxy_set_device(const struct device *dev)
{
    net_if_mgmt_serial_dev = dev;
}

/* -------------------------------------------------------------------------- */
/* Enumeration helper                                                          */
/* -------------------------------------------------------------------------- */

struct iface_enum_ctx {
    embedded_wifi_mgmt_RequestInterfacesResponse *resp;
    embedded_wifi_mgmt_NetIfType filter_type;
    uint32_t filter_index;
};

static void iface_enum_cb(struct net_if *iface, void *user_data)
{
    struct iface_enum_ctx *ctx = (struct iface_enum_ctx *)user_data;

    if (ctx->resp->net_ifs_count >= ARRAY_SIZE(ctx->resp->net_ifs)) {
        return;
    }

    /* Zephyr index is 1-based; net_if_get_by_index works with this value. */
    int idx = net_if_get_by_iface(iface);

    if (ctx->filter_index != 0 && (uint32_t)idx != ctx->filter_index) {
        return;
    }

    embedded_wifi_mgmt_NetworkInterface *ni =
        &ctx->resp->net_ifs[ctx->resp->net_ifs_count];

    ni->index = (uint32_t)idx;
    ni->mtu   = net_if_get_mtu(iface);
    ni->admin_up   = net_if_is_admin_up(iface);
    ni->carrier_ok = net_if_is_carrier_ok(iface);
    ni->dormant    = net_if_is_dormant(iface);
    ni->oper_state = (embedded_wifi_mgmt_NetIfOperState)net_if_oper_state(iface);

    if (net_if_is_wifi(iface)) {
        ni->net_if_type = embedded_wifi_mgmt_NetIfType_NET_IF_TYPE_WIFI;
    } else {
        ni->net_if_type = embedded_wifi_mgmt_NetIfType_NET_IF_TYPE_UNKNOWN;
    }

    if (ctx->filter_type != embedded_wifi_mgmt_NetIfType_NET_IF_TYPE_UNKNOWN &&
        ni->net_if_type != ctx->filter_type) {
        return;
    }

    struct net_linkaddr *ll = net_if_get_link_addr(iface);
    if (ll && ll->len <= sizeof(ni->mac_address)) {
        memcpy(ni->mac_address, ll->addr, ll->len);
    }

    net_if_get_name(iface, ni->name, sizeof(ni->name));

    ctx->resp->net_ifs_count++;
}

/* -------------------------------------------------------------------------- */
/* Request processing                                                          */
/* -------------------------------------------------------------------------- */

static void process_net_if_request(const uint8_t *buf, size_t len,
                                   uint8_t *resp_buf, size_t *resp_len_io)
{
    embedded_wifi_mgmt_NetIfRequest req = embedded_wifi_mgmt_NetIfRequest_init_zero;
    pb_istream_t in = pb_istream_from_buffer(buf, len);

    embedded_wifi_mgmt_NetIfResponse resp = embedded_wifi_mgmt_NetIfResponse_init_zero;

    if (!pb_decode(&in, embedded_wifi_mgmt_NetIfRequest_fields, &req)) {
        LOG_ERR("NetIfRequest decode failed: %s", PB_GET_ERROR(&in));
        resp.status = -EBADMSG;
        goto encode;
    }

    resp.request_id = req.request_id;

    switch (req.which_payload) {
    case embedded_wifi_mgmt_NetIfRequest_get_interfaces_tag: {
        struct iface_enum_ctx ctx = {
            .resp = &resp.payload.interfaces,
            .filter_type  = req.payload.get_interfaces.net_if_type,
            .filter_index = req.payload.get_interfaces.index,
        };
        net_if_foreach(iface_enum_cb, &ctx);
        resp.which_payload = embedded_wifi_mgmt_NetIfResponse_interfaces_tag;
        resp.status = 0;
        break;
    }

    case embedded_wifi_mgmt_NetIfRequest_set_admin_state_tag: {
        struct net_if *iface = net_if_get_by_index(req.payload.set_admin_state.index);
        if (!iface) {
            resp.status = -ENODEV;
            break;
        }
        int ret = req.payload.set_admin_state.admin_up ? net_if_up(iface)
                                                        : net_if_down(iface);
        resp.status = ret;
        break;
    }

    case embedded_wifi_mgmt_NetIfRequest_set_carrier_state_tag: {
        struct net_if *iface = net_if_get_by_index(req.payload.set_carrier_state.index);
        if (!iface) {
            resp.status = -ENODEV;
            break;
        }
        if (req.payload.set_carrier_state.carrier_on) {
            net_if_carrier_on(iface);
        } else {
            net_if_carrier_off(iface);
        }
        resp.status = 0;
        break;
    }

    case embedded_wifi_mgmt_NetIfRequest_set_dormant_state_tag: {
        struct net_if *iface = net_if_get_by_index(req.payload.set_dormant_state.index);
        if (!iface) {
            resp.status = -ENODEV;
            break;
        }
        if (req.payload.set_dormant_state.dormant) {
            net_if_dormant_on(iface);
        } else {
            net_if_dormant_off(iface);
        }
        resp.status = 0;
        break;
    }

    case embedded_wifi_mgmt_NetIfRequest_set_flag_tag: {
        struct net_if *iface = net_if_get_by_index(req.payload.set_flag.index);
        if (!iface) {
            resp.status = -ENODEV;
            break;
        }
        enum net_if_flag flag =
            (enum net_if_flag)req.payload.set_flag.flag;
        if (req.payload.set_flag.set) {
            net_if_flag_set(iface, flag);
        } else {
            net_if_flag_clear(iface, flag);
        }
        resp.status = 0;
        break;
    }

    case embedded_wifi_mgmt_NetIfRequest_get_flag_tag: {
        struct net_if *iface = net_if_get_by_index(req.payload.get_flag.index);
        if (!iface) {
            resp.status = -ENODEV;
            break;
        }
        enum net_if_flag flag = (enum net_if_flag)req.payload.get_flag.flag;
        resp.which_payload = embedded_wifi_mgmt_NetIfResponse_flag_tag;
        resp.payload.flag.flag = req.payload.get_flag.flag;
        resp.payload.flag.is_set = net_if_flag_is_set(iface, flag);
        resp.status = 0;
        break;
    }

    case embedded_wifi_mgmt_NetIfRequest_get_oper_state_tag: {
        struct net_if *iface = net_if_get_by_index(req.payload.get_oper_state.index);
        if (!iface) {
            resp.status = -ENODEV;
            break;
        }
        resp.which_payload = embedded_wifi_mgmt_NetIfResponse_oper_state_tag;
        resp.payload.oper_state.oper_state =
            (embedded_wifi_mgmt_NetIfOperState)net_if_oper_state(iface);
        resp.status = 0;
        break;
    }

    case embedded_wifi_mgmt_NetIfRequest_get_name_tag: {
        struct net_if *iface = net_if_get_by_index(req.payload.get_name.index);
        if (!iface) {
            resp.status = -ENODEV;
            break;
        }
        resp.which_payload = embedded_wifi_mgmt_NetIfResponse_name_tag;
        net_if_get_name(iface, resp.payload.name.name,
                        sizeof(resp.payload.name.name));
        resp.status = 0;
        break;
    }

    case embedded_wifi_mgmt_NetIfRequest_get_mtu_tag: {
        struct net_if *iface = net_if_get_by_index(req.payload.get_mtu.index);
        if (!iface) {
            resp.status = -ENODEV;
            break;
        }
        resp.which_payload = embedded_wifi_mgmt_NetIfResponse_mtu_tag;
        resp.payload.mtu.mtu = net_if_get_mtu(iface);
        resp.status = 0;
        break;
    }

    default:
        LOG_WRN("Unhandled NetIfRequest payload tag: %d", req.which_payload);
        resp.status = -ENOTSUP;
        break;
    }

encode: {
    pb_ostream_t out = pb_ostream_from_buffer(resp_buf, *resp_len_io);
    if (pb_encode(&out, embedded_wifi_mgmt_NetIfResponse_fields, &resp)) {
        *resp_len_io = out.bytes_written;
    } else {
        LOG_ERR("NetIfResponse encode failed: %s", PB_GET_ERROR(&out));
        *resp_len_io = 0;
    }
    }
}

/* -------------------------------------------------------------------------- */
/* Thread                                                                      */
/* -------------------------------------------------------------------------- */

static void net_if_mgmt_thread_entry(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

    while (true) {
        uint32_t ev = k_event_wait(&net_if_mgmt_event, DATA_COMPLETE_EVENT,
                                   true, K_FOREVER);

        if (!(ev & DATA_COMPLETE_EVENT)) {
            continue;
        }

        LOG_INF("net_if_mgmt: processing request, length=%zu", message_length);

        size_t resp_len = sizeof(net_if_mgmt_tx_buffer);
        process_net_if_request(net_if_mgmt_rx_buffer, message_length,
                               net_if_mgmt_tx_buffer, &resp_len);

        if (net_if_mgmt_serial_dev != NULL && resp_len > 0) {
            extern uint8_t serial_get_tx_sequence_number(void);
            extern void serial_advance_tx_sequence_number(void);

            struct SerialHeader hdr = {
                .type = SERIAL_TYPE_NET_IF_MGMT,
                .no_ack = false,
                .host = true,
                .error = ERROR_TYPE_NO_ERROR,
                .reserved = 0,
                .sequence_number = serial_get_tx_sequence_number(),
                .length = sys_cpu_to_be16((uint16_t)resp_len),
            };
            serial_data_tx(net_if_mgmt_serial_dev, (const uint8_t *)&hdr, sizeof(hdr));
            serial_data_tx(net_if_mgmt_serial_dev, net_if_mgmt_tx_buffer, resp_len);
            serial_advance_tx_sequence_number();
        }

        message_length = 0;
    }
}

K_THREAD_DEFINE(net_if_mgmt_thread_id,
                NET_IF_MGMT_THREAD_STACK_SIZE,
                net_if_mgmt_thread_entry,
                NULL, NULL, NULL,
                NET_IF_MGMT_THREAD_PRIORITY,
                0, 0);

/* -------------------------------------------------------------------------- */
/* Client callbacks (called from serial layer)                                */
/* -------------------------------------------------------------------------- */

bool net_if_mgmt_acquire_buffer(uint8_t **data, size_t len)
{
    if (len > (sizeof(net_if_mgmt_rx_buffer) - message_length)) {
        LOG_WRN("net_if_mgmt: no buffer space for %zu bytes", len);
        return false;
    }
    *data = &net_if_mgmt_rx_buffer[message_length];
    return true;
}

void net_if_mgmt_commit_data(size_t len)
{
    message_length += len;
}

void net_if_mgmt_message_complete(size_t len)
{
    ARG_UNUSED(len);
    k_event_set(&net_if_mgmt_event, DATA_COMPLETE_EVENT);
}
