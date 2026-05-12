#include "serial/serial.h"
#include "wifi_mgmt_proxy.h"
#include "wifi_data.h"
#include "net_if_mgmt_proxy.h"
#include "vserial.h"

/* Forward declarations for BT/OT stub files (no separate headers) */
bool bt_proxy_acquire_buffer(uint8_t **data, size_t len);
void bt_proxy_commit_data(size_t len);
void bt_proxy_message_complete(size_t len);

bool ot_proxy_acquire_buffer(uint8_t **data, size_t len);
void ot_proxy_commit_data(size_t len);
void ot_proxy_message_complete(size_t len);

struct client_callbacks client_cb[SERIAL_TYPE_UNKNOWN] = {
    [SERIAL_TYPE_WIFI_MGMT] = {
        .acquire_buffer  = wifi_mgmt_acquire_buffer,
        .commit_data     = wifi_mgmt_commit_data,
        .message_complete = wifi_mgmt_message_complete,
    },
    [SERIAL_TYPE_WIFI_DATA] = {
        .acquire_buffer  = wifi_data_acquire_buffer,
        .commit_data     = wifi_data_commit_data,
        .message_complete = wifi_data_message_complete,
    },
    [SERIAL_TYPE_NET_IF_MGMT] = {
        .acquire_buffer  = net_if_mgmt_acquire_buffer,
        .commit_data     = net_if_mgmt_commit_data,
        .message_complete = net_if_mgmt_message_complete,
    },
    [SERIAL_TYPE_HCI] = {
        .acquire_buffer  = bt_proxy_acquire_buffer,
        .commit_data     = bt_proxy_commit_data,
        .message_complete = bt_proxy_message_complete,
    },
    [SERIAL_TYPE_THREAD] = {
        .acquire_buffer  = ot_proxy_acquire_buffer,
        .commit_data     = ot_proxy_commit_data,
        .message_complete = ot_proxy_message_complete,
    },
    [SERIAL_TYPE_NET_OFFLOADING] = {
        /* Reserved — not implemented */
        .acquire_buffer  = NULL,
        .commit_data     = NULL,
        .message_complete = NULL,
    },
    [SERIAL_TYPE_AT_CMD] = {
        /* Reserved — not implemented */
        .acquire_buffer  = NULL,
        .commit_data     = NULL,
        .message_complete = NULL,
    },
    [SERIAL_TYPE_PROTOCOL_INFO] = {
        /* Handled inline in serial layer — no client callbacks needed */
        .acquire_buffer  = NULL,
        .commit_data     = NULL,
        .message_complete = NULL,
    },
    [SERIAL_TYPE_VSERIAL] = {
        .acquire_buffer  = vserial_acquire_buffer,
        .commit_data     = vserial_commit_data,
        .message_complete = vserial_message_complete,
    },
};
