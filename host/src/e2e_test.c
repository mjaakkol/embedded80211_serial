/*
 * E2E test for the host app.
 *
 * WiFi: bring up the virtual interface, scan, connect (SSID/PSK from Kconfig,
 * build-time only), wait for DHCP. Scan/connect are issued as net_mgmt
 * requests (NET_REQUEST_WIFI_SCAN / NET_REQUEST_WIFI_CONNECT); results arrive
 * via a net_mgmt event callback.
 * BT: periodic BLE scan windows, logging discovered devices.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/net/icmp.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/addr.h>
#include <string.h>

LOG_MODULE_REGISTER(e80211_e2e, LOG_LEVEL_INF);

#define WIFI_TIMEOUT_MS   30000U
#define SCAN_TIMEOUT_S    15U
#define DHCP_TIMEOUT_S    30U
#define BT_SCAN_WINDOW_S  10U
#define BT_SCAN_PERIOD_S  30U

#define VIRT_WIFI_IFACE_INDEX 1

static struct net_if *wifi_iface;
static struct net_mgmt_event_callback mgmt_cb;
static K_SEM_DEFINE(scan_done_sem, 0, 1);
static K_SEM_DEFINE(connect_sem, 0, 1);
static K_SEM_DEFINE(dhcp_sem, 0, 1);
static K_SEM_DEFINE(ping_sem, 0, 1);
static struct net_icmp_ctx ping_ctx;
static int connect_status = -1;
static int bt_found_count;

static void sprint_ipv4(const struct net_in_addr *addr, char *buf, size_t size)
{
	snprintf(buf, size, "%u.%u.%u.%u",
		 addr->s4_addr[0], addr->s4_addr[1],
		 addr->s4_addr[2], addr->s4_addr[3]);
}

static void net_mgmt_handler(struct net_mgmt_event_callback *cb,
			     uint64_t mgmt_event, struct net_if *iface)
{
	if (iface != NULL && iface != wifi_iface) {
		return;
	}

	switch (mgmt_event) {
	case NET_EVENT_WIFI_SCAN_RESULT: {
		const struct wifi_scan_result *res = cb->info;
		char ssid[WIFI_SSID_MAX_LEN + 1];
		size_t len;

		if (res == NULL) {
			break;
		}

		len = res->ssid_length;
		if (len > sizeof(ssid) - 1) {
			len = sizeof(ssid) - 1;
		}
		memcpy(ssid, res->ssid, len);
		ssid[len] = '\0';

		LOG_INF("E2E WIFI: SCAN rssi=%d sec=%d %s", res->rssi,
			(int)res->security, ssid);
		break;
	}
	case NET_EVENT_WIFI_SCAN_DONE:
		k_sem_give(&scan_done_sem);
		break;
	case NET_EVENT_WIFI_CONNECT_RESULT: {
		const int *status = cb->info;

		connect_status = (status != NULL) ? *status : -1;
		if (connect_status == 0) {
			LOG_INF("E2E WIFI: CONNECTED");
		} else {
			LOG_ERR("E2E WIFI: CONNECT FAIL status=%d",
				connect_status);
		}
		k_sem_give(&connect_sem);
		break;
	}
	case NET_EVENT_IPV4_CMD_DHCP_BOUND: {
		struct net_if_ipv4 *ipv4 = NULL;
		char ip[16] = "0.0.0.0";
		char gw[16] = "0.0.0.0";

		if (net_if_config_ipv4_get(wifi_iface, &ipv4) == 0 &&
		    ipv4 != NULL) {
			sprint_ipv4(&ipv4->unicast[0].ipv4.address.in_addr,
				    ip, sizeof(ip));
			sprint_ipv4(&ipv4->gw, gw, sizeof(gw));
		}

		LOG_INF("E2E WIFI: DHCP BOUND %s gw %s", ip, gw);
		k_sem_give(&dhcp_sem);
		break;
	}
	default:
		break;
	}
}

static uint64_t e2e_event_mask(void)
{
	return NET_EVENT_WIFI_SCAN_RESULT |
	       NET_EVENT_WIFI_SCAN_DONE |
	       NET_EVENT_WIFI_CONNECT_RESULT |
	       NET_EVENT_IPV4_CMD_DHCP_BOUND;
}

static enum net_verdict ping_reply_handler(struct net_icmp_ctx *ctx,
					   struct net_pkt *pkt,
					   struct net_icmp_ip_hdr *ip_hdr,
					   struct net_icmp_hdr *icmp_hdr,
					   void *user_data)
{
	ARG_UNUSED(ctx);
	ARG_UNUSED(pkt);
	ARG_UNUSED(ip_hdr);
	ARG_UNUSED(icmp_hdr);
	ARG_UNUSED(user_data);

	LOG_INF("E2E WIFI: GATEWAY PING RECV");
	k_sem_give(&ping_sem);
	return NET_OK;
}


static void wifi_e2e_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	if (CONFIG_HOST_TEST_WIFI_SSID[0] == '\0') {
		LOG_INF("E2E WIFI: SKIPPED (no SSID configured)");
		return;
	}

	wifi_iface = net_if_get_by_index(VIRT_WIFI_IFACE_INDEX);
	if (wifi_iface == NULL) {
		LOG_ERR("E2E WIFI: no interface at index %d",
			VIRT_WIFI_IFACE_INDEX);
		return;
	}

	net_if_up(wifi_iface);
	LOG_INF("E2E WIFI: IFACE UP");

	/* Scan (results arrive via net_mgmt events). */
	struct wifi_scan_params scan = { 0 };
	int rc = net_mgmt(NET_REQUEST_WIFI_SCAN, wifi_iface, &scan,
			  sizeof(scan));

	if (rc != 0) {
		LOG_ERR("E2E WIFI: scan start failed: %d", rc);
	} else {
		k_sem_take(&scan_done_sem, K_SECONDS(SCAN_TIMEOUT_S));
	}

	/* Connect. */
	struct wifi_connect_req_params params = {
		.ssid = (const uint8_t *)CONFIG_HOST_TEST_WIFI_SSID,
		.ssid_length = sizeof(CONFIG_HOST_TEST_WIFI_SSID) - 1,
		.psk = (const uint8_t *)CONFIG_HOST_TEST_WIFI_PSK,
		.psk_length = sizeof(CONFIG_HOST_TEST_WIFI_PSK) - 1,
		.security = (CONFIG_HOST_TEST_WIFI_PSK[0] == '\0') ?
				WIFI_SECURITY_TYPE_NONE : WIFI_SECURITY_TYPE_PSK,
		.channel = WIFI_CHANNEL_ANY,
		.band = WIFI_FREQ_BAND_2_4_GHZ,
	};

	rc = net_mgmt(NET_REQUEST_WIFI_CONNECT, wifi_iface, &params,
		      sizeof(params));
	if (rc != 0) {
		LOG_ERR("E2E WIFI: connect start failed: %d", rc);
		return;
	}

	if (k_sem_take(&connect_sem, K_MSEC(WIFI_TIMEOUT_MS)) != 0 ||
	    connect_status != 0) {
		LOG_ERR("E2E WIFI: connect did not succeed (status=%d)",
			connect_status);
		return;
	}

	/* Wait for DHCP. */
	if (k_sem_take(&dhcp_sem, K_SECONDS(DHCP_TIMEOUT_S)) != 0) {
		LOG_ERR("E2E WIFI: DHCP timeout");
		return;
	}

	/* Ping the default gateway: the E2E success criterion is an IP
	 * address plus a gateway echo reply. */
	struct net_if_ipv4 *ipv4 = NULL;
	union {
		struct net_sockaddr_in dst4;
		struct net_sockaddr dst;
	} daddr;
	int prc;

	if (net_if_config_ipv4_get(wifi_iface, &ipv4) != 0 ||
	    ipv4 == NULL || ipv4->gw.s4_addr[0] == 0) {
		LOG_ERR("E2E WIFI: no default gateway");
		return;
	}

	net_icmp_init_ctx(&ping_ctx, AF_INET, NET_ICMPV4_ECHO_REPLY, 0,
			  ping_reply_handler);
	daddr.dst4.sin_family = AF_INET;
	daddr.dst4.sin_addr = ipv4->gw;

	prc = net_icmp_send_echo_request(&ping_ctx, wifi_iface, &daddr.dst,
					 NULL, NULL);
	if (prc != 0) {
		LOG_ERR("E2E WIFI: ping send failed: %d", prc);
	} else if (k_sem_take(&ping_sem, K_SECONDS(5)) != 0) {
		LOG_ERR("E2E WIFI: GATEWAY PING TIMEOUT");
	} else {
		LOG_INF("E2E WIFI: GATEWAY PING OK");
		LOG_INF("E2E WIFI: OK");
	}
	net_icmp_cleanup_ctx(&ping_ctx);
}

static void bt_adv_cb(const bt_addr_le_t *addr, int8_t rssi,
		      uint8_t adv_type, struct net_buf_simple *buf)
{
	char addr_str[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(addr, addr_str, sizeof(addr_str));
	bt_found_count++;
	LOG_INF("E2E BT: DEV %s rssi=%d", addr_str, rssi);
}

static const struct bt_le_scan_param scan_params[] = BT_LE_SCAN_ACTIVE;

static void bt_e2e_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (true) {
		bt_found_count = 0;
		int rc = bt_le_scan_start(scan_params, bt_adv_cb);

		if (rc != 0) {
			LOG_ERR("E2E BT: scan start failed: %d", rc);
		} else {
			k_sleep(K_SECONDS(BT_SCAN_WINDOW_S));
			bt_le_scan_stop();
			LOG_INF("E2E BT: SCAN DONE found=%d", bt_found_count);
		}

		k_sleep(K_SECONDS(BT_SCAN_PERIOD_S - BT_SCAN_WINDOW_S));
	}
}

K_THREAD_STACK_DEFINE(wifi_stack, 4096);
static struct k_thread wifi_thread;
K_THREAD_STACK_DEFINE(bt_stack, 2048);
static struct k_thread bt_thread;

void e2e_test_start(void)
{
	net_mgmt_init_event_callback(&mgmt_cb, net_mgmt_handler,
				    e2e_event_mask());
	net_mgmt_add_event_callback(&mgmt_cb);

	k_thread_create(&wifi_thread, wifi_stack,
			K_THREAD_STACK_SIZEOF(wifi_stack),
			wifi_e2e_thread, NULL, NULL, NULL,
			K_PRIO_COOP(7), 0, K_NO_WAIT);
	k_thread_name_set(&wifi_thread, "wifi_e2e");

	k_thread_create(&bt_thread, bt_stack,
			K_THREAD_STACK_SIZEOF(bt_stack),
			bt_e2e_thread, NULL, NULL, NULL,
			K_PRIO_COOP(8), 0, K_NO_WAIT);
	k_thread_name_set(&bt_thread, "bt_e2e");
}
