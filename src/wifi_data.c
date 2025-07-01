#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_pkt.h>
#include <zephyr/net/wifi_mgmt.h>

#include "wifi_data.h"

LOG_MODULE_REGISTER(wifi_data, LOG_LEVEL_INF);

#define WIFI_MGMT_EVENTS (NET_EVENT_WIFI_CONNECT_RESULT |		\
				NET_EVENT_WIFI_DISCONNECT_RESULT | \
                NET_EVENT_WIFI_AP_STA_DISCONNECTED | \
                NET_EVENT_WIFI_AP_STA_CONNECTED | \
            	NET_EVENT_WIFI_CMD_DISCONNECT_COMPLETE)

static struct net_if_dev* wifi_dev;
static struct net_mgmt_event_callback wifi_mgmt_cb;
static size_t message_length;
static uint8_t wifi_data_tx_buffer[1600];


static void wifi_mgmt_event_handler(struct net_mgmt_event_callback *cb,
				     uint32_t mgmt_event, struct net_if *iface)
{
	switch (mgmt_event) {
	case NET_EVENT_WIFI_CONNECT_RESULT:
		// Open data gates
		break;
	case NET_EVENT_WIFI_DISCONNECT_RESULT:
		// Stop data transmission
		break;
    case NET_EVENT_WIFI_AP_STA_DISCONNECTED:
        LOG_INF("A station disconnected from the AP");
        break;
    case NET_EVENT_WIFI_AP_STA_CONNECTED:
        LOG_INF("A station connected to the AP");
        break;
    case NET_EVENT_WIFI_CMD_DISCONNECT_COMPLETE:
        LOG_INF("Wi-Fi disconnect command completed");
        // Handle disconnect completion, e.g., reset state or notify user
        break;
	default:
        LOG_WRN("Unhandled Wi-Fi management event: %u", mgmt_event);
		break;
	}
}

static void wifi_iface_enumeration_cb(struct net_if *iface, void *user_data)
{
    if (net_if_is_wifi(iface)) {
        LOG_INF("Found Wi-Fi interface: %p", iface);
        // store only the first one - implement later how to narrow is down to a specific device.
        // multi-device support comes even later.

        if (wifi_dev == NULL) {
            wifi_dev = iface->if_dev;
            LOG_INF("Storing Wi-Fi device: %p", wifi_dev);

            /* Register link layer status callback for this interface */
            net_mgmt_init_event_callback(&wifi_mgmt_cb, wifi_mgmt_event_handler, WIFI_MGMT_EVENTS);
        	net_mgmt_add_event_callback(&wifi_mgmt_cb);
        } else {
            LOG_WRN("Multiple Wi-Fi interfaces found, only the first will be used.");
        }
    }
}

bool wifi_data_acquire_buffer(uint8_t **data, size_t len)
{
    // Check if the ring buffer has enough space
    if (len > (sizeof(wifi_data_tx_buffer) - message_length)) {
        LOG_WRN("Not enough space in buffer for %zu bytes, Abort", len);
        // TODO: This is severe error condition and should happen. Reseting things would be an appropriate action.
        return false;
    }
    return true; // Indicate that the buffer is available
}


void wifi_data_commit_data(size_t len)
{
    message_length += len; // Update the total message length
}

void wifi_data_message_complete(size_t len)
{
    struct net_if *iface = net_if_lookup_by_dev(wifi_dev->dev);
    if (iface == NULL) {
        LOG_ERR("Failed to get network interface for Wi-Fi device");
        return;
    }

    struct net_pkt *pkt = net_pkt_alloc_with_buffer(iface, len, AF_UNSPEC, 0, K_FOREVER);

    if (pkt == NULL) {
        LOG_ERR("Failed to acquire packet & buffer of size %zu", len);
    }

    message_length += len; // Update the total message length

    net_pkt_write(pkt, &wifi_data_tx_buffer[4], message_length);

    message_length = 0; // Reset after sending

    if (net_send_data(pkt) < 0) {
        LOG_ERR("Failed to send Wi-Fi data packet");
        net_pkt_unref(pkt);
    }
}



static int init_wifi_data()
{
    net_if_foreach(wifi_iface_enumeration_cb, NULL);

    return 0;
}

SYS_INIT(init_wifi_data, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);