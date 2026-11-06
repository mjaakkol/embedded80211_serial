#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/ethernet.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/net_pkt.h>
#include <zephyr/net/socket.h>
#include <zephyr/posix/sys/socket.h>
#include <zephyr/posix/unistd.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/net/wifi.h>
#include <zephyr/posix/poll.h>
 #include <errno.h>

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
				     uint64_t mgmt_event, struct net_if *iface)
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
        LOG_WRN("Unhandled Wi-Fi management event: %llu", mgmt_event);
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

/*
 * Raw 802.11 frame transmission (bypasses the TCP/IP stack).
 *
 * The nRF70/71 driver exposes a raw TX path: an AF_PACKET socket bound to the
 * Wi-Fi interface carries a small driver header followed by a complete 802.11
 * frame. The driver recognizes the magic number, strips the header, and
 * transmits the frame at the configured rate/queue. Reference usage:
 * nrf/samples/wifi/shell/src/wifi_raw_tx_pkt_shell.c.
 */
#define NRF_WIFI_MAGIC_NUM_RAWTX 0x12345678u

/* Legacy (802.11a/b/g) rate 6 Mbps: valid on any 2.4/5 GHz BSS. */
#define RAW_TX_DATA_RATE 6
/* TX mode: legacy. */
#define RAW_TX_MODE_LEGACY 0
/* Queue: best-effort (data). */
#define RAW_TX_QUEUE_DATA 1

#define RAW_FRAME_MIN_LEN 24 /* 802.11 header only */
#define RAW_FRAME_MAX_LEN 2346 /* 802.11 max frame minus FCS */

struct raw_tx_pkt_header {
	uint32_t magic_num;
	unsigned char data_rate;
	unsigned short packet_length;
	unsigned char tx_mode;
	unsigned char queue;
	unsigned char reserved[3];
} __packed;

static int raw_tx_sockfd = -1;
static struct sockaddr_ll raw_tx_addr;
static uint8_t raw_tx_buf[sizeof(struct raw_tx_pkt_header) + RAW_FRAME_MAX_LEN];
static uint32_t raw_tx_frame_count;
static int raw_tx_injection_ret;
static int raw_tx_setup_ret;
static int raw_tx_last_ret;
static size_t raw_tx_last_frame_len;
static int raw_tx_last_iface_wifi;

/* Create and bind the AF_PACKET socket used for raw transmission. */
static int raw_tx_socket_setup(struct net_if *iface)
{
	struct sockaddr_ll *sa = &raw_tx_addr;

	if (raw_tx_sockfd >= 0) {
		return 0; /* already set up */
	}

	raw_tx_setup_ret = 1; /* about to call socket() */
	raw_tx_sockfd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
	raw_tx_setup_ret = 2; /* socket() returned */
	if (raw_tx_sockfd < 0) {
		LOG_ERR("raw_tx: socket() failed: %d", raw_tx_sockfd);
		raw_tx_setup_ret = 100 + raw_tx_sockfd;
		return -EIO;
	}

	memset(sa, 0, sizeof(*sa));
	sa->sll_family = AF_PACKET;
	sa->sll_ifindex = net_if_get_by_iface(iface);

	raw_tx_setup_ret = 3; /* about to call bind() */
	if (bind(raw_tx_sockfd, (struct sockaddr *)sa, sizeof(*sa)) < 0) {
		LOG_ERR("raw_tx: bind() failed: %d ifidx=%d", raw_tx_sockfd,
			sa->sll_ifindex);
		close(raw_tx_sockfd);
		raw_tx_sockfd = -1;
		raw_tx_setup_ret = 200 + sa->sll_ifindex;
		return -EIO;
	}

	raw_tx_setup_ret = 0; /* success */
	return 0;
}

int wifi_data_raw_frame_tx(struct net_if *iface, const uint8_t *frame, size_t len)
{
	struct raw_tx_pkt_header hdr;
	int ret;

	raw_tx_last_frame_len = len;
	raw_tx_last_iface_wifi = (iface != NULL) ? net_if_is_wifi(iface) : 0;
	if (iface == NULL || frame == NULL) {
		raw_tx_last_ret = -EINVAL;
		return -EINVAL;
	}
	if (!net_if_is_wifi(iface)) {
		LOG_ERR("raw_tx: interface is not Wi-Fi");
		raw_tx_last_ret = -EINVAL;
		return -EINVAL;
	}
	if (len < RAW_FRAME_MIN_LEN || len > RAW_FRAME_MAX_LEN) {
		LOG_ERR("raw_tx: frame length %zu out of range [%d..%d]",
			len, RAW_FRAME_MIN_LEN, RAW_FRAME_MAX_LEN);
		raw_tx_last_ret = -EMSGSIZE;
		return -EMSGSIZE;
	}

	/* Enable the driver's TX-injection mode (idempotent). */
	raw_tx_injection_ret = net_eth_txinjection_mode(iface, true);
	if (raw_tx_injection_ret) {
		LOG_ERR("raw_tx: enabling TX injection failed: %d", raw_tx_injection_ret);
		raw_tx_last_ret = raw_tx_injection_ret;
		return raw_tx_injection_ret;
	}

	ret = raw_tx_socket_setup(iface);
	if (ret) {
		raw_tx_last_ret = ret;
		return ret;
	}

	memset(&hdr, 0, sizeof(hdr));
	hdr.magic_num = NRF_WIFI_MAGIC_NUM_RAWTX;
	hdr.data_rate = RAW_TX_DATA_RATE;
	hdr.packet_length = (unsigned short)len;
	hdr.tx_mode = RAW_TX_MODE_LEGACY;
	hdr.queue = RAW_TX_QUEUE_DATA;

	memcpy(raw_tx_buf, &hdr, sizeof(hdr));
	memcpy(raw_tx_buf + sizeof(hdr), frame, len);

	ret = sendto(raw_tx_sockfd, raw_tx_buf, sizeof(hdr) + len, 0,
		     (struct sockaddr *)&raw_tx_addr, sizeof(raw_tx_addr));
	if (ret < 0) {
		LOG_ERR("raw_tx: sendto() failed: %d", ret);
		raw_tx_last_ret = -EIO;
		return -EIO;
	}

	raw_tx_frame_count++;
	raw_tx_last_ret = 0;
	LOG_INF("raw_tx: transmitted %zu-byte 802.11 frame (total %u)", len, raw_tx_frame_count);
	return 0;
}

/*
 * Raw 802.11 frame reception (promiscuous sniffer), for data connectivity
 * loopback testing. With the firmware packet filter set to
 * WIFI_PACKET_FILTER_ALL, the driver delivers every frame on the air
 * interface (including one it just transmitted) to the network stack as a raw
 * packet, readable on an AF_PACKET socket. Each received frame is forwarded to
 * the registered callback, which reports it to the host over the serial link.
 * Reference usage: nrf/tests/drivers/nrf_wifi/raw_tx_rx/src/main.c.
 */
#define RAW_RX_BUFFER_SIZE (RAW_FRAME_MAX_LEN + 64)
#define RAW_RX_FILTER_BUFFER_SIZE 1550
#define RAW_RX_THREAD_STACK_SIZE 2048

static int raw_rx_sockfd = -1;
static struct sockaddr_ll raw_rx_addr;
static uint8_t raw_rx_buf[RAW_RX_BUFFER_SIZE];
static struct net_if *raw_rx_iface;
static wifi_data_raw_rx_cb raw_rx_cb;

static struct k_thread raw_rx_thread;
static K_THREAD_STACK_DEFINE(raw_rx_thread_stack, RAW_RX_THREAD_STACK_SIZE);
static bool raw_rx_running;
static uint32_t raw_rx_frame_count;
static uint32_t raw_rx_mode_readback;

static void raw_rx_thread_fn(void *arg1, void *arg2, void *arg3)
{
	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	LOG_INF("raw_rx: RX thread started, sockfd=%d", raw_rx_sockfd);

	while (raw_rx_running) {
		struct pollfd pfd = {
			.fd = raw_rx_sockfd,
			.events = POLLIN,
		};
		int pret = poll(&pfd, 1, 100); /* 100 ms timeout */
		if (pret < 0) {
			LOG_ERR("raw_rx: poll() failed: %d", errno);
			raw_rx_running = false;
			break;
		}
		if (pret == 0) {
			continue; /* timeout, re-check flag */
		}
		ssize_t n = recvfrom(raw_rx_sockfd, raw_rx_buf, sizeof(raw_rx_buf),
				     0, NULL, NULL);
		if (n > 0) {
			LOG_INF("raw_rx: received %zd bytes (total %u)", n,
				++raw_rx_frame_count);
			if (raw_rx_cb != NULL) {
				raw_rx_cb(raw_rx_iface, raw_rx_buf, (size_t)n);
			}
		} else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
			LOG_ERR("raw_rx: recvfrom() failed: %d", (int)n);
			raw_rx_running = false;
		}
	}
}

void wifi_data_set_raw_rx_cb(wifi_data_raw_rx_cb cb)
{
	raw_rx_cb = cb;
}

int wifi_data_raw_frame_rx_enable(struct net_if *iface, uint32_t channel)
{
	struct wifi_mode_info mode_info;
	struct wifi_channel_info channel_info;
	struct wifi_filter_info filter_info;
	int ret;

	if (iface == NULL) {
		return -EINVAL;
	}
	if (!net_if_is_wifi(iface)) {
		LOG_ERR("raw_rx: interface is not Wi-Fi");
		return -EINVAL;
	}
	if (raw_rx_sockfd >= 0) {
		return 0; /* already enabled */
	}

	/* If the interface is already UP (from NRF_WIFI_IF_AUTO_START at
	 * boot), skip net_if_up() — calling it again returns -EALREADY.
	 * If it's not UP, try to bring it up so the driver's
	 * nrf_wifi_if_start_zep() completes the VIF setup and the UMAC
	 * emits CARRIER_ON. Without this, if_carr_state stays OFF and
	 * all raw TX frames are dropped.
	 */
	if (!net_if_flag_is_set(iface, NET_IF_UP)) {
		int up_ret = -EIO;

		for (int attempt = 0; attempt < 5; attempt++) {
			up_ret = net_if_up(iface);
			if (up_ret == 0) {
				break;
			}
			k_sleep(K_MSEC(500));
		}
		if (up_ret) {
			return -2000 - up_ret;
		}
	}

	/* Switch the radio to monitor mode so it captures every frame on the
	 * air, including the ones it transmits (STA mode cannot receive while
	 * transmitting).
	 */
	memset(&mode_info, 0, sizeof(mode_info));
	mode_info.oper = WIFI_MGMT_SET;
	mode_info.if_index = net_if_get_by_iface(iface);
	mode_info.mode = BIT(1); /* MONITOR */
	ret = net_mgmt(NET_REQUEST_WIFI_MODE, iface, &mode_info,
		       sizeof(mode_info));
	if (ret) {
		LOG_ERR("raw_rx: set monitor mode failed: %d", ret);
		return ret;
	}
	/* Read back the mode to verify it took effect. */
	memset(&mode_info, 0, sizeof(mode_info));
	mode_info.oper = WIFI_MGMT_GET;
	mode_info.if_index = net_if_get_by_iface(iface);
	if (net_mgmt(NET_REQUEST_WIFI_MODE, iface, &mode_info,
		     sizeof(mode_info)) == 0) {
		raw_rx_mode_readback = mode_info.mode;
	}

	/* Give the firmware time to process the mode change and set the
	 * carrier state to ON. The nRF7002 driver drops raw TX frames when
	 * if_carr_state != ON.
	 */
	k_sleep(K_MSEC(500));

	/* Enable the driver's TX-injection mode (idempotent). */
	ret = net_eth_txinjection_mode(iface, true);
	if (ret) {
		LOG_ERR("raw_rx: enabling TX injection failed: %d", ret);
		return ret;
	}

	/* Tune the radio to the listen channel. */
	if (channel != 0) {
		memset(&channel_info, 0, sizeof(channel_info));
		channel_info.oper = WIFI_MGMT_SET;
		channel_info.if_index = net_if_get_by_iface(iface);
		channel_info.band = WIFI_FREQ_BAND_2_4_GHZ;
		channel_info.channel = channel;
		ret = net_mgmt(NET_REQUEST_WIFI_CHANNEL, iface, &channel_info,
			       sizeof(channel_info));
		if (ret) {
			LOG_ERR("raw_rx: set channel %u failed: %d", channel,
				ret);
			return -1003; /* channel set failed */
		}
	}

	/* Configure the firmware packet filter to pass all frames. */
	memset(&filter_info, 0, sizeof(filter_info));
	filter_info.oper = WIFI_MGMT_SET;
	filter_info.if_index = net_if_get_by_iface(iface);
	filter_info.filter = WIFI_PACKET_FILTER_ALL;
	filter_info.buffer_size = RAW_RX_FILTER_BUFFER_SIZE;
	ret = net_mgmt(NET_REQUEST_WIFI_PACKET_FILTER, iface, &filter_info,
		       sizeof(filter_info));
	if (ret) {
		LOG_ERR("raw_rx: packet filter set failed: %d", ret);
		return -1004; /* packet filter set failed */
	}

	raw_rx_sockfd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
	if (raw_rx_sockfd < 0) {
		LOG_ERR("raw_rx: socket() failed: %d", raw_rx_sockfd);
		return -EIO;
	}

	memset(&raw_rx_addr, 0, sizeof(raw_rx_addr));
	raw_rx_addr.sll_family = AF_PACKET;
	raw_rx_addr.sll_ifindex = net_if_get_by_iface(iface);

	if (bind(raw_rx_sockfd, (struct sockaddr *)&raw_rx_addr,
		 sizeof(raw_rx_addr)) < 0) {
		LOG_ERR("raw_rx: bind() failed: %d", raw_rx_sockfd);
		close(raw_rx_sockfd);
		raw_rx_sockfd = -1;
		return -EIO;
	}

	raw_rx_iface = iface;
	raw_rx_running = true;
	k_thread_create(&raw_rx_thread, raw_rx_thread_stack,
			RAW_RX_THREAD_STACK_SIZE, raw_rx_thread_fn,
			NULL, NULL, NULL, K_PRIO_PREEMPT(7), 0, K_NO_WAIT);
	k_thread_start(&raw_rx_thread);

	LOG_INF("raw_rx: enabled on iface %d", raw_rx_addr.sll_ifindex);
	return 0;
}

int wifi_data_raw_frame_rx_disable(struct net_if *iface)
{
	ARG_UNUSED(iface);

	if (raw_rx_sockfd < 0) {
		return 0; /* not enabled */
	}

	raw_rx_running = false;
	/* Bounded wait for the RX thread to observe the flag and leave its
	 * recvfrom loop (SO_RCVTIMEO is 100 ms) before the socket is closed. */
	k_thread_join(&raw_rx_thread, K_MSEC(500));
	close(raw_rx_sockfd);
	raw_rx_sockfd = -1;
	raw_rx_iface = NULL;

	LOG_INF("raw_rx: disabled");
	return 0;
}

/* Defined in the NCS Wi-Fi driver (net_if.c). */
extern int nrf_wifi_start_zep_last_step;

void wifi_data_raw_rx_diag(struct wifi_data_raw_rx_diag *out)
{
	if (out == NULL) {
		return;
	}
	out->socket_fd = raw_rx_sockfd;
	out->thread_running = raw_rx_running;
	out->frames_received = raw_rx_frame_count;
	out->current_mode = raw_rx_mode_readback;
	out->start_zep_step = nrf_wifi_start_zep_last_step;
	out->tx_socket_fd = raw_tx_sockfd;
	out->tx_frames_sent = raw_tx_frame_count;
	out->tx_injection_ret = raw_tx_injection_ret;
	out->tx_setup_ret = raw_tx_setup_ret;
	out->tx_last_ret = raw_tx_last_ret;
	out->tx_last_len = raw_tx_last_frame_len;
	out->tx_last_wifi = raw_tx_last_iface_wifi;
}

static int init_wifi_data(void)
{
    net_if_foreach(wifi_iface_enumeration_cb, NULL);
    return 0;
}

SYS_INIT(init_wifi_data, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);