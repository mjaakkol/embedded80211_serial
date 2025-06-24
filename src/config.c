#include "serial/serial.h"
#include "wifi_mgmt_proxy.h"
#include "wifi_data.h"

struct client_callbacks client_cb[SERIAL_TYPE_UNKNOWN] = {
    [SERIAL_TYPE_WIFI_MGMT] = {
        .acquire_buffer = wifi_mgmt_acquire_buffer, // Set to your buffer acquisition function
        .commit_data = wifi_mgmt_commit_data, // Set to your commit data function
        .message_complete = wifi_mgmt_message_complete, // Set to your message complete function
    },
    [SERIAL_TYPE_WIFI_DATA] = {
        .acquire_buffer = wifi_data_acquire_buffer,
        .commit_data = wifi_data_commit_data,
        .message_complete = wifi_data_message_complete,
    },
    [SERIAL_TYPE_HCI] = {
        .acquire_buffer = NULL,
        .commit_data = NULL,
        .message_complete = NULL,
    },
    [SERIAL_TYPE_THREAD] = {
        .acquire_buffer = NULL,
        .commit_data = NULL,
        .message_complete = NULL,
    },
    [SERIAL_TYPE_AT_CMD] = {
        .acquire_buffer = NULL,
        .commit_data = NULL,
        .message_complete = NULL,
    },
};
