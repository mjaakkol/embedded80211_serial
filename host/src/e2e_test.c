/*
 * E2E network integration test for the host app.
 *
 * Brings up IPv4 over Wi-Fi and verifies the full path, in order:
 *   1. Print the interface MTU and MAC address.
 *   2. Connect to the AP (SSID/PSK from Kconfig, build-time only) via
 *      NET_REQUEST_WIFI_CONNECT; the result arrives as a net_mgmt event.
 *   3. Bring the interface up and start the DHCPv4 client; wait for the
 *      DHCP BOUND management event.
 *   4. Print the assigned IPv4 address, netmask, gateway and lease info.
 *   5. Ping the default gateway CONFIG_HOST_TEST_GATEWAY_PINGS times;
 *      print each reply (sequence, source, RTT) and a summary. The test
 *      passes when every packet gets an echo reply.
 *
 * The Bluetooth E2E part lives in bt_e2e.c.
 *
 * Every step prints a structured "E2E WIFI:" status line; a failure
 * prints "E2E WIFI: FAIL step=<n> reason=<...>" and stops the test.
 *
 * Log markers (consumed by host/tests/run_e2e.sh):
 *   E2E WIFI: CONNECTED
 *   E2E WIFI: DHCP BOUND
 *   E2E WIFI: PING SUMMARY
 *   E2E WIFI: OK
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/net/icmp.h>
#include <zephyr/net/dhcpv4.h>
#include <zephyr/net/net_ip.h>
#include <string.h>

LOG_MODULE_REGISTER(e80211_e2e, LOG_LEVEL_INF);

#define CONNECT_TIMEOUT_MS  30000U
#define DHCP_TIMEOUT_S      30U
#define PING_TIMEOUT_S      5U

/* Resolve the virtual Wi-Fi interface by type, not by index: an interface
 * index is an allocation order and is only stable when virt_wifi is the
 * sole non-loopback interface. */
static void find_wifi_iface_cb(struct net_if *iface, void *user)
{
	struct net_if **found = user;

	if (*found == NULL && net_if_is_wifi(iface)) {
		*found = iface;
	}
}

static struct net_if *wifi_iface;
static struct net_mgmt_event_callback mgmt_cb;
static K_SEM_DEFINE(connect_sem, 0, 1);
static K_SEM_DEFINE(dhcp_sem, 0, 1);
static K_SEM_DEFINE(ping_sem, 0, 1);
/* Given once the Wi-Fi connect phase (step 2) has finished, success or
 * failure. The BT thread waits on this before touching the controller so
 * HCI and Wi-Fi management never contend for the nRF7002 IPC link. */
static K_SEM_DEFINE(wifi_phase_sem, 0, 1);
static struct net_icmp_ctx ping_ctx;
static int connect_status = -1;
static int64_t ping_t0;

static void sprint_ipv4(const struct net_in_addr *addr, char *buf, size_t size)
{
	snprintf(buf, size, "%u.%u.%u.%u",
		 addr->s4_addr[0], addr->s4_addr[1],
		 addr->s4_addr[2], addr->s4_addr[3]);
}

static void e2e_fail(int step, const char *reason, int rc)
{
	LOG_ERR("E2E WIFI: FAIL step=%d reason=%s (%d)", step, reason, rc);
}

/* Step 1: print the interface MTU and MAC address. */
static int step1_iface_info(void)
{
	const struct net_linkaddr *mac = &wifi_iface->if_dev->link_addr;
	char mac_str[18] = "?";

	if (mac->len == 6) {
		snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x",
			 mac->addr[0], mac->addr[1], mac->addr[2],
			 mac->addr[3], mac->addr[4], mac->addr[5]);
	}

	LOG_INF("E2E WIFI: STEP 1 IFACE index=%d mtu=%d mac=%s",
		net_if_get_by_iface(wifi_iface), net_if_get_mtu(wifi_iface),
		mac_str);
	return 0;
}

static void net_mgmt_handler(struct net_mgmt_event_callback *cb,
			     uint64_t mgmt_event, struct net_if *iface)
{
	if (iface != NULL && iface != wifi_iface) {
		return;
	}

	switch (mgmt_event) {
	case NET_EVENT_WIFI_CONNECT_RESULT: {
		const int *status = cb->info;

		connect_status = (status != NULL) ? *status : -1;
		k_sem_give(&connect_sem);
		break;
	}
	case NET_EVENT_IPV4_DHCP_BOUND:
	case NET_EVENT_IPV4_CMD_DHCP_BOUND:
		k_sem_give(&dhcp_sem);
		break;
	default:
		break;
	}
}

static uint64_t e2e_event_mask(void)
{
	return NET_EVENT_WIFI_CONNECT_RESULT |
	       NET_EVENT_IPV4_DHCP_BOUND |
	       NET_EVENT_IPV4_CMD_DHCP_BOUND;
}

/* Step 2: connect to the AP and wait for the connect result event. */
static int step2_connect(void)
{
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
	int rc;

	rc = net_mgmt(NET_REQUEST_WIFI_CONNECT, wifi_iface, &params,
		      sizeof(params));
	if (rc != 0) {
		e2e_fail(2, "connect start", rc);
		return rc;
	}

	if (k_sem_take(&connect_sem, K_MSEC(CONNECT_TIMEOUT_MS)) != 0) {
		e2e_fail(2, "connect timeout", -ETIMEDOUT);
		return -ETIMEDOUT;
	}

	if (connect_status != 0) {
		e2e_fail(2, "connect rejected", connect_status);
		return connect_status;
	}

	LOG_INF("E2E WIFI: CONNECTED ssid=%s", CONFIG_HOST_TEST_WIFI_SSID);
	return 0;
}

/* Step 3: bring the interface up and wait for the DHCP lease. */
static int step3_dhcp(void)
{
	/* Set NET_IF_UP and NET_IF_RUNNING directly. net_if_up() is
	 * broken for Wi-Fi offload devices (ethernet_enable type-puns
	 * dev->api). net_if_is_up() requires BOTH flags.
	 */
	net_if_flag_set(wifi_iface, NET_IF_UP);
	net_if_flag_set(wifi_iface, NET_IF_RUNNING);
	LOG_INF("E2E WIFI: IFACE UP (is_up=%d)", net_if_is_up(wifi_iface));

	net_dhcpv4_start(wifi_iface);

	if (k_sem_take(&dhcp_sem, K_SECONDS(DHCP_TIMEOUT_S)) != 0) {
		e2e_fail(3, "dhcp timeout", -ETIMEDOUT);
		return -ETIMEDOUT;
	}

	return 0;
}

/* Step 4: query and print the assigned IPv4 configuration. */
static int step4_print_config(char *gw_str, size_t gw_size,
			      struct net_in_addr *gw)
{
	struct net_in_addr *addr;
	struct net_in_addr mask;
	char ip[16];
	char mask_str[16];

	addr = net_if_ipv4_get_global_addr(wifi_iface, NET_ADDR_PREFERRED);
	if (addr == NULL) {
		e2e_fail(4, "no global ipv4 address", -ENOENT);
		return -ENOENT;
	}

	mask = net_if_ipv4_get_netmask_by_addr(wifi_iface, addr);
	*gw = net_if_ipv4_get_gw(wifi_iface);
	if (gw->s4_addr[0] == 0) {
		e2e_fail(4, "no default gateway", -ENOENT);
		return -ENOENT;
	}

	sprint_ipv4(addr, ip, sizeof(ip));
	sprint_ipv4(&mask, mask_str, sizeof(mask_str));
	sprint_ipv4(gw, gw_str, gw_size);

	LOG_INF("E2E WIFI: DHCP BOUND ip=%s mask=%s gw=%s lease=%us",
		ip, mask_str, gw_str, wifi_iface->config.dhcpv4.lease_time);
	return 0;
}

static enum net_verdict ping_reply_handler(struct net_icmp_ctx *ctx,
					   struct net_pkt *pkt,
					   struct net_icmp_ip_hdr *ip_hdr,
					   struct net_icmp_hdr *icmp_hdr,
					   void *user_data)
{
	const uint8_t *echo;
	uint16_t seq;

	ARG_UNUSED(ctx);
	ARG_UNUSED(pkt);

	/* The echo id/seq follow the 4-byte ICMP header on the wire; the
	 * Zephyr net_icmp_hdr struct does not carry them. Wire order is
	 * big-endian: id at offset 0..1, seq at offset 2..3.
	 */
	echo = (const uint8_t *)icmp_hdr + sizeof(struct net_icmp_hdr);
	seq = (uint16_t)echo[2] | ((uint16_t)echo[3] << 8);

	LOG_INF("E2E WIFI: PING RECV seq=%d from %u.%u.%u.%u rtt=%lldms",
		seq,
		ip_hdr->ipv4->src[0],
		ip_hdr->ipv4->src[1],
		ip_hdr->ipv4->src[2],
		ip_hdr->ipv4->src[3],
		(long long)(k_uptime_get() - *(int64_t *)user_data));
	k_sem_give(&ping_sem);
	return NET_OK;
}

/* Step 5: ping the gateway, print per-packet RTT and a summary.
 * Returns 0 when every packet got a reply.
 */
static int step5_ping(const struct net_in_addr *gw)
{
	union {
		struct net_sockaddr_in dst4;
		struct net_sockaddr dst;
	} daddr;
	char gw_str[16];
	int count = CONFIG_HOST_TEST_GATEWAY_PINGS;
	int sent = 0;
	int recv = 0;
	int64_t rtt_min = INT64_MAX;
	int64_t rtt_max = 0;
	int64_t rtt_sum = 0;
	int rc;

	sprint_ipv4(gw, gw_str, sizeof(gw_str));
	LOG_INF("E2E WIFI: STEP 5 PING gw=%s count=%d", gw_str, count);

	net_icmp_init_ctx(&ping_ctx, AF_INET, NET_ICMPV4_ECHO_REPLY, 0,
			  ping_reply_handler);
	ping_ctx.user_data = &ping_t0;
	daddr.dst4.sin_family = AF_INET;
	daddr.dst4.sin_addr = *gw;

	for (int i = 0; i < count; i++) {
		rc = net_icmp_send_echo_request(&ping_ctx, wifi_iface,
						&daddr.dst, NULL, NULL);
		if (rc != 0) {
			e2e_fail(5, "ping send", rc);
			break;
		}
		sent++;
		ping_t0 = k_uptime_get();

		if (k_sem_take(&ping_sem, K_SECONDS(PING_TIMEOUT_S)) == 0) {
			int64_t rtt = k_uptime_get() - ping_t0;

			recv++;
			rtt_min = MIN(rtt_min, rtt);
			rtt_max = MAX(rtt_max, rtt);
			rtt_sum += rtt;
		}
	}

	net_icmp_cleanup_ctx(&ping_ctx);

	if (recv > 0) {
		LOG_INF("E2E WIFI: PING SUMMARY sent=%d recv=%d loss=%d%% "
			"min=%lldms avg=%lldms max=%lldms",
			sent, recv, (sent - recv) * 100 / sent,
			(long long)rtt_min, (long long)(rtt_sum / recv),
			(long long)rtt_max);
	} else {
		LOG_INF("E2E WIFI: PING SUMMARY sent=%d recv=%d loss=100%%",
			sent, recv);
	}

	if (sent == 0 || recv != sent) {
		e2e_fail(5, "ping loss", recv - sent);
		return -EIO;
	}

	return 0;
}

static void e2e_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	if (CONFIG_HOST_TEST_WIFI_SSID[0] == '\0') {
		LOG_INF("E2E WIFI: SKIPPED (no SSID configured)");
		return;
	}

	net_if_foreach(find_wifi_iface_cb, &wifi_iface);
	if (wifi_iface == NULL) {
		e2e_fail(1, "no wifi interface", -ENOENT);
		return;
	}

	if (step1_iface_info() != 0) {
		return;
	}

	int rc2 = step2_connect();

	/* Release the BT thread: the connect phase is over (success or
	 * failure), so HCI and Wi-Fi management no longer contend. */
	k_sem_give(&wifi_phase_sem);

	if (rc2 != 0) {
		return;
	}

	if (step3_dhcp() != 0) {
		return;
	}

	char gw_str[16];
	struct net_in_addr gw;

	if (step4_print_config(gw_str, sizeof(gw_str), &gw) != 0) {
		return;
	}

	if (step5_ping(&gw) != 0) {
		return;
	}

	LOG_INF("E2E WIFI: OK");
}

K_THREAD_STACK_DEFINE(e2e_stack, 4096);
static struct k_thread e2e_thread_obj;

void e2e_test_start(void)
{
	net_mgmt_init_event_callback(&mgmt_cb, net_mgmt_handler,
				    e2e_event_mask());
	net_mgmt_add_event_callback(&mgmt_cb);

	k_thread_create(&e2e_thread_obj, e2e_stack,
			K_THREAD_STACK_SIZEOF(e2e_stack),
			e2e_thread, NULL, NULL, NULL,
			K_PRIO_COOP(7), 0, K_NO_WAIT);
	k_thread_name_set(&e2e_thread_obj, "wifi_e2e");
}

/* Block until the Wi-Fi connect phase has finished (or the timeout
 * elapses). Called by the BT thread so it never starts the controller
 * while the nRF7002 is mid-association. */
void e2e_wait_wifi_phase(uint32_t timeout_ms)
{
	(void)k_sem_take(&wifi_phase_sem, K_MSEC(timeout_ms));
}
