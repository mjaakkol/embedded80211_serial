/*
 * Host-side virtual Wi-Fi driver.
 *
 * Exposes a Zephyr Wi-Fi L2 interface (`compatible = "embedded80211,virt-wifi"`)
 * that proxies management requests and data frames to the radio co-processor
 * over the link-protocol bus client. It is the symmetric mirror of the
 * device's wifi_mgmt_proxy.c / wifi_data.c.
 *
 *   - Management: WifiMgmtRequest over SERIAL_TYPE_WIFI_MGMT (bus_request);
 *     the device replies with a WifiMgmtResponse.
 *   - Data: raw Ethernet frames over SERIAL_TYPE_WIFI_DATA (bus_send / handler).
 *   - Events: WifiMgmtNotification over SERIAL_TYPE_WIFI_MGMT (handler) is
 *     fanned out as local net_mgmt events (the Wi-Fi layer raises the scan
 *     events via the stored scan_result_cb).
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/ethernet.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/net_buf.h>
#include <string.h>

#include <pb_encode.h>
#include <pb_decode.h>
#include "wifi.pb.h"
#include <embedded80211/bus.h>
#include <embedded80211/serial.h>
#include <embedded80211/host_wifi.h>

LOG_MODULE_REGISTER(virt_wifi, LOG_LEVEL_INF);

#define MGMT_TIMEOUT_MS 5000U

/* The virtual Wi-Fi is always interface index 1 (loopback is 0). */
#define VIRT_WIFI_IFACE_INDEX 1

#define DT_DRV_COMPAT embedded80211_virt_wifi

/* ---------------------------------------------------------------------------
 * Proto <-> Zephyr enum mappings (mirror of device/wifi_mgmt_proxy.c).
 * ------------------------------------------------------------------------- */

static enum wifi_security_type proto_to_zephyr_security(
	embedded_wifi_mgmt_WifiSecurityType psec)
{
	switch (psec) {
	case embedded_wifi_mgmt_WifiSecurityType_WIFI_SECURITY_TYPE_NONE:
		return WIFI_SECURITY_TYPE_NONE;
	case embedded_wifi_mgmt_WifiSecurityType_WIFI_SECURITY_TYPE_PSK:
		return WIFI_SECURITY_TYPE_PSK;
	case embedded_wifi_mgmt_WifiSecurityType_WIFI_SECURITY_TYPE_PSK_SHA256:
		return WIFI_SECURITY_TYPE_PSK_SHA256;
	case embedded_wifi_mgmt_WifiSecurityType_WIFI_SECURITY_TYPE_SAE:
		return WIFI_SECURITY_TYPE_SAE;
	case embedded_wifi_mgmt_WifiSecurityType_WIFI_SECURITY_TYPE_EAP:
		return WIFI_SECURITY_TYPE_EAP;
	case embedded_wifi_mgmt_WifiSecurityType_WIFI_SECURITY_TYPE_WEP:
		return WIFI_SECURITY_TYPE_WEP;
	case embedded_wifi_mgmt_WifiSecurityType_WIFI_SECURITY_TYPE_WAPI:
		return WIFI_SECURITY_TYPE_WAPI;
	default:
		return WIFI_SECURITY_TYPE_NONE;
	}
}

static embedded_wifi_mgmt_WifiSecurityType zephyr_to_proto_security(
	enum wifi_security_type zsec)
{
	switch (zsec) {
	case WIFI_SECURITY_TYPE_NONE:
		return embedded_wifi_mgmt_WifiSecurityType_WIFI_SECURITY_TYPE_NONE;
	case WIFI_SECURITY_TYPE_PSK:
	case WIFI_SECURITY_TYPE_WPA_PSK:
	case WIFI_SECURITY_TYPE_WPA_AUTO_PERSONAL:
		return embedded_wifi_mgmt_WifiSecurityType_WIFI_SECURITY_TYPE_PSK;
	case WIFI_SECURITY_TYPE_PSK_SHA256:
		return embedded_wifi_mgmt_WifiSecurityType_WIFI_SECURITY_TYPE_PSK_SHA256;
	case WIFI_SECURITY_TYPE_SAE:
	case WIFI_SECURITY_TYPE_SAE_H2E:
	case WIFI_SECURITY_TYPE_SAE_AUTO:
		return embedded_wifi_mgmt_WifiSecurityType_WIFI_SECURITY_TYPE_SAE;
	case WIFI_SECURITY_TYPE_EAP:
		return embedded_wifi_mgmt_WifiSecurityType_WIFI_SECURITY_TYPE_EAP;
	case WIFI_SECURITY_TYPE_WEP:
		return embedded_wifi_mgmt_WifiSecurityType_WIFI_SECURITY_TYPE_WEP;
	case WIFI_SECURITY_TYPE_WAPI:
		return embedded_wifi_mgmt_WifiSecurityType_WIFI_SECURITY_TYPE_WAPI;
	default:
		return embedded_wifi_mgmt_WifiSecurityType_WIFI_SECURITY_TYPE_UNKNOWN;
	}
}

static embedded_wifi_mgmt_WifiMfpOptions zephyr_to_proto_mfp(enum wifi_mfp_options mfp)
{
	switch (mfp) {
	case WIFI_MFP_OPTIONAL:
		return embedded_wifi_mgmt_WifiMfpOptions_WIFI_MFP_OPTIONAL;
	case WIFI_MFP_REQUIRED:
		return embedded_wifi_mgmt_WifiMfpOptions_WIFI_MFP_REQUIRED;
	default:
		return embedded_wifi_mgmt_WifiMfpOptions_WIFI_MFP_DISABLE;
	}
}

static enum wifi_mfp_options proto_to_zephyr_mfp(embedded_wifi_mgmt_WifiMfpOptions pmfp)
{
	switch (pmfp) {
	case embedded_wifi_mgmt_WifiMfpOptions_WIFI_MFP_REQUIRED:
		return WIFI_MFP_REQUIRED;
	case embedded_wifi_mgmt_WifiMfpOptions_WIFI_MFP_DISABLE:
		return WIFI_MFP_DISABLE;
	default:
		return WIFI_MFP_OPTIONAL;
	}
}

static embedded_wifi_mgmt_WifiScanType zephyr_to_proto_scan_type(
	enum wifi_scan_type ztype)
{
	/* Proto: PASSIVE=0, ACTIVE=1. Zephyr: ACTIVE=0, PASSIVE=1. */
	return (ztype == WIFI_SCAN_TYPE_ACTIVE) ?
		embedded_wifi_mgmt_WifiScanType_WIFI_SCAN_TYPE_ACTIVE :
		embedded_wifi_mgmt_WifiScanType_WIFI_SCAN_TYPE_PASSIVE;
}

static enum wifi_iface_state proto_to_zephyr_state(
	embedded_wifi_mgmt_WifiInterfaceState pstate)
{
	switch (pstate) {
	case embedded_wifi_mgmt_WifiInterfaceState_WIFI_STATE_CONNECTED:
		return WIFI_STATE_COMPLETED;
	case embedded_wifi_mgmt_WifiInterfaceState_WIFI_STATE_DISCONNECTED:
		return WIFI_STATE_DISCONNECTED;
	case embedded_wifi_mgmt_WifiInterfaceState_WIFI_STATE_INACTIVE:
		return WIFI_STATE_INACTIVE;
	case embedded_wifi_mgmt_WifiInterfaceState_WIFI_STATE_SCANNING:
		return WIFI_STATE_SCANNING;
	case embedded_wifi_mgmt_WifiInterfaceState_WIFI_STATE_INTERFACE_DISABLED:
		return WIFI_STATE_INTERFACE_DISABLED;
	default:
		return WIFI_STATE_DISCONNECTED;
	}
}

static enum wifi_iface_mode proto_to_zephyr_mode(embedded_wifi_mgmt_WifiMode pmode)
{
	switch (pmode) {
	case embedded_wifi_mgmt_WifiMode_WIFI_MODE_AP:
		return WIFI_MODE_AP;
	case embedded_wifi_mgmt_WifiMode_WIFI_MODE_INFRA:
		return WIFI_MODE_INFRA;
	default:
		return WIFI_MODE_INFRA;
	}
}

/* ---------------------------------------------------------------------------
 * State and static buffers.
 * ------------------------------------------------------------------------- */

static uint8_t req_buf[sizeof(embedded_wifi_mgmt_WifiMgmtRequest)];
static uint8_t resp_buf[sizeof(embedded_wifi_mgmt_WifiMgmtResponse)];
static uint8_t notify_buf[sizeof(embedded_wifi_mgmt_WifiMgmtNotification)];

static scan_result_cb_t scan_cb;

/* The virtual Wi-Fi is the only non-loopback interface in the host app, so
 * index 1 resolves to it. Fall back to the default interface if that fails. */
static struct net_if *virt_wifi_netif(void)
{
	struct net_if *iface = net_if_get_by_index(1);

	return (iface != NULL) ? iface : net_if_get_default();
}

/* ---------------------------------------------------------------------------
 * L2 data path.
 * ------------------------------------------------------------------------- */

static int virt_wifi_l2_send(const struct device *dev, struct net_pkt *pkt)
{
	const uint8_t *data = pkt->frags->data;
	size_t len = net_buf_frags_len(pkt->frags);
	int rc = bus_send(SERIAL_TYPE_WIFI_DATA, data, len);

	net_pkt_unref(pkt);
	return rc;
}

static void wifi_data_rx(const uint8_t *payload, size_t len, void *user)
{
	struct net_if *iface = user;
	struct net_pkt *pkt;

	if (iface == NULL || len == 0) {
		return;
	}

	pkt = net_pkt_alloc_with_buffer(iface, len, AF_UNSPEC, 0, K_NO_WAIT);
	if (pkt == NULL) {
		LOG_WRN("WiFi data RX: no buffer (%u bytes)", (unsigned int)len);
		return;
	}

	if (net_buf_add_mem(pkt->frags, payload, len) < 0) {
		net_pkt_unref(pkt);
		return;
	}

	net_if_recv_data(iface, pkt);
}

/* ---------------------------------------------------------------------------
 * Notification handlers (device -> host events).
 * ------------------------------------------------------------------------- */

static void handle_scan_done(const embedded_wifi_mgmt_ScanDoneEvent *ev,
			     struct net_if *iface)
{
	if (scan_cb == NULL) {
		return;
	}

	for (int i = 0; i < ev->all_results_count; i++) {
		const embedded_wifi_mgmt_WifiScanResult *r =
			&ev->all_results[i];
		struct wifi_scan_result res = { 0 };

		res.rssi = (int8_t)r->rssi;
		res.security = proto_to_zephyr_security(r->security);
		res.mfp = proto_to_zephyr_mfp(r->mfp);
		res.channel = (uint8_t)r->channel;
		if (r->ssid.size <= WIFI_SSID_MAX_LEN) {
			memcpy(res.ssid, r->ssid.bytes, r->ssid.size);
			res.ssid_length = (uint8_t)r->ssid.size;
		}
		memcpy(res.mac, r->bssid, WIFI_MAC_ADDR_LEN);
		res.mac_length = WIFI_MAC_ADDR_LEN;
		scan_cb(iface, ev->status, &res);
	}

	/* entry == NULL signals scan completion to the Wi-Fi layer. */
	scan_cb(iface, ev->status, NULL);
}

static void wifi_mgmt_notify(const uint8_t *payload, size_t len, void *user)
{
	ARG_UNUSED(user);

	embedded_wifi_mgmt_WifiMgmtNotification notif =
		embedded_wifi_mgmt_WifiMgmtNotification_init_zero;
	pb_istream_t stream = pb_istream_from_buffer(payload, len);

	if (!pb_decode(&stream, embedded_wifi_mgmt_WifiMgmtNotification_fields,
		       &notif)) {
		LOG_ERR("WiFi notify decode failed: %s", PB_GET_ERROR(&stream));
		return;
	}

	struct net_if *iface = virt_wifi_netif();

	switch (notif.event.which_event_data) {
	case embedded_wifi_mgmt_WifiEvent_scan_done_event_tag:
		handle_scan_done(&notif.event.event_data.scan_done_event, iface);
		break;
	case embedded_wifi_mgmt_WifiEvent_connect_result_event_tag:
		wifi_mgmt_raise_connect_result_event(
			iface, notif.event.event_data.connect_result_event.status);
		break;
	case embedded_wifi_mgmt_WifiEvent_disconnect_result_event_tag:
		wifi_mgmt_raise_disconnect_result_event(iface, 0);
		break;
	case embedded_wifi_mgmt_WifiEvent_interface_status_event_tag: {
		const embedded_wifi_mgmt_WifiInterfaceStatus *s =
			&notif.event.event_data.interface_status_event.status;
		struct wifi_iface_status istatus = { 0 };

		istatus.state = proto_to_zephyr_state(s->state);
		istatus.iface_mode = proto_to_zephyr_mode(s->mode);
		istatus.security = proto_to_zephyr_security(s->security);
		istatus.mfp = proto_to_zephyr_mfp(s->mfp);
		istatus.channel = (unsigned int)s->channel;
		istatus.rssi = (int)s->rssi;
		if (s->ssid.size <= WIFI_SSID_MAX_LEN) {
			memcpy(istatus.ssid, s->ssid.bytes, s->ssid.size);
			istatus.ssid_len = (unsigned int)s->ssid.size;
		}
		memcpy(istatus.bssid, s->bssid, WIFI_MAC_ADDR_LEN);
		wifi_mgmt_raise_iface_status_event(iface, &istatus);
		break;
	}
	case embedded_wifi_mgmt_WifiEvent_ap_enable_result_event_tag:
		wifi_mgmt_raise_ap_enable_result_event(
			iface,
			notif.event.event_data.ap_enable_result_event.status == 0 ?
				WIFI_STATUS_AP_SUCCESS : WIFI_STATUS_AP_FAIL);
		break;
	case embedded_wifi_mgmt_WifiEvent_ap_disable_result_event_tag:
		wifi_mgmt_raise_ap_disable_result_event(
			iface,
			notif.event.event_data.ap_disable_result_event.status == 0 ?
				WIFI_STATUS_AP_SUCCESS : WIFI_STATUS_AP_FAIL);
		break;
	case embedded_wifi_mgmt_WifiEvent_ap_sta_connected_event_tag: {
		struct wifi_ap_sta_info sta = { 0 };

		memcpy(sta.mac,
			notif.event.event_data.ap_sta_connected_event
				.sta_mac_address,
			WIFI_MAC_ADDR_LEN);
		net_mgmt_event_notify_with_info(NET_EVENT_WIFI_AP_STA_CONNECTED,
			iface, &sta, sizeof(sta));
		break;
	}
	case embedded_wifi_mgmt_WifiEvent_ap_sta_disconnected_event_tag: {
		struct wifi_ap_sta_info sta = { 0 };

		memcpy(sta.mac,
			notif.event.event_data.ap_sta_disconnected_event
				.sta_mac_address,
			WIFI_MAC_ADDR_LEN);
		net_mgmt_event_notify_with_info(NET_EVENT_WIFI_AP_STA_DISCONNECTED,
			iface, &sta, sizeof(sta));
		break;
	}
	case embedded_wifi_mgmt_WifiEvent_raw_frame_rx_event_tag:
		LOG_DBG("Raw frame RX: %u bytes",
			(unsigned int)notif.event.event_data.raw_frame_rx_event.frame.size);
		break;
	default:
		break;
	}
}

/* ---------------------------------------------------------------------------
 * Management ops.
 * ------------------------------------------------------------------------- */

/* Encode a request, send it, and decode the response. Returns 0 on success. */
static int mgmt_roundtrip(const embedded_wifi_mgmt_WifiMgmtRequest *req,
			  embedded_wifi_mgmt_WifiMgmtResponse *resp)
{
	pb_ostream_t stream = pb_ostream_from_buffer(req_buf, sizeof(req_buf));

	if (!pb_encode(&stream, embedded_wifi_mgmt_WifiMgmtRequest_fields, req)) {
		LOG_ERR("WiFi mgmt encode failed: %s", PB_GET_ERROR(&stream));
		return -EMSGSIZE;
	}

	size_t resp_len = sizeof(resp_buf);
	int rc = bus_request(SERIAL_TYPE_WIFI_MGMT, req_buf, stream.bytes_written,
			     resp_buf, &resp_len, MGMT_TIMEOUT_MS);

	if (rc != 0) {
		return rc;
	}

	pb_istream_t istream = pb_istream_from_buffer(resp_buf, resp_len);

	if (!pb_decode(&istream, embedded_wifi_mgmt_WifiMgmtResponse_fields,
		       resp)) {
		LOG_ERR("WiFi mgmt response decode failed: %s",
			PB_GET_ERROR(&istream));
		return -EBADMSG;
	}
	return 0;
}

static int decode_status(const embedded_wifi_mgmt_WifiMgmtResponse *resp)
{
	if (resp->which_payload ==
	    embedded_wifi_mgmt_WifiMgmtResponse_status_resp_tag) {
		if (resp->payload.status_resp.success) {
			return 0;
		}
		return resp->payload.status_resp.error_code;
	}
	return -EBADMSG;
}

static int virt_wifi_scan(const struct device *dev,
			  struct wifi_scan_params *params,
			  scan_result_cb_t cb)
{
	ARG_UNUSED(dev);

	scan_cb = cb;

	embedded_wifi_mgmt_WifiMgmtRequest req =
		embedded_wifi_mgmt_WifiMgmtRequest_init_zero;
	embedded_wifi_mgmt_WifiMgmtResponse resp =
		embedded_wifi_mgmt_WifiMgmtResponse_init_zero;

	req.request_id = embedded_wifi_mgmt_RequestType_REQUEST_TYPE_SCAN;
	req.which_payload = embedded_wifi_mgmt_WifiMgmtRequest_scan_req_tag;
	req.payload.scan_req.iface_index = VIRT_WIFI_IFACE_INDEX;
	req.payload.scan_req.has_params = true;
	req.payload.scan_req.params.scan_type = zephyr_to_proto_scan_type(
		params->scan_type);
	req.payload.scan_req.params.bands = params->bands;
	req.payload.scan_req.params.dwell_time_active = params->dwell_time_active;
	req.payload.scan_req.params.dwell_time_passive = params->dwell_time_passive;
	req.payload.scan_req.params.max_bss_cnt = params->max_bss_cnt;

	int rc = mgmt_roundtrip(&req, &resp);

	if (rc != 0) {
		scan_cb = NULL;
		return rc;
	}
	/* Results arrive via wifi_mgmt_notify(); the scan_result_cb is invoked
	 * there. */
	return decode_status(&resp);
}

static int virt_wifi_connect(const struct device *dev,
			     struct wifi_connect_req_params *params)
{
	ARG_UNUSED(dev);

	embedded_wifi_mgmt_WifiMgmtRequest req =
		embedded_wifi_mgmt_WifiMgmtRequest_init_zero;
	embedded_wifi_mgmt_WifiMgmtResponse resp =
		embedded_wifi_mgmt_WifiMgmtResponse_init_zero;

	req.request_id = embedded_wifi_mgmt_RequestType_REQUEST_TYPE_CONNECT;
	req.which_payload = embedded_wifi_mgmt_WifiMgmtRequest_connect_req_tag;
	req.payload.connect_req.iface_index = VIRT_WIFI_IFACE_INDEX;
	if (params->ssid != NULL && params->ssid_length <= 32) {
		memcpy(req.payload.connect_req.ssid.bytes,
			params->ssid, params->ssid_length);
		req.payload.connect_req.ssid.size = params->ssid_length;
	}
	req.payload.connect_req.security_type =
		zephyr_to_proto_security(params->security);
	if (params->psk != NULL && params->psk_length <= 65) {
		memcpy(req.payload.connect_req.psk.bytes,
			params->psk, params->psk_length);
		req.payload.connect_req.psk.size = params->psk_length;
	}
	req.payload.connect_req.mfp = zephyr_to_proto_mfp(params->mfp);
	req.payload.connect_req.channel = params->channel;

	int rc = mgmt_roundtrip(&req, &resp);

	if (rc != 0) {
		return rc;
	}
	return decode_status(&resp);
}

static int virt_wifi_disconnect(const struct device *dev)
{
	ARG_UNUSED(dev);

	embedded_wifi_mgmt_WifiMgmtRequest req =
		embedded_wifi_mgmt_WifiMgmtRequest_init_zero;
	embedded_wifi_mgmt_WifiMgmtResponse resp =
		embedded_wifi_mgmt_WifiMgmtResponse_init_zero;

	req.request_id = embedded_wifi_mgmt_RequestType_REQUEST_TYPE_DISCONNECT;
	req.which_payload =
		embedded_wifi_mgmt_WifiMgmtRequest_disconnect_req_tag;
	req.payload.disconnect_req.iface_index = VIRT_WIFI_IFACE_INDEX;

	int rc = mgmt_roundtrip(&req, &resp);

	if (rc != 0) {
		return rc;
	}
	return decode_status(&resp);
}

static int virt_wifi_ap_enable(const struct device *dev,
			       struct wifi_connect_req_params *params)
{
	ARG_UNUSED(dev);

	embedded_wifi_mgmt_WifiMgmtRequest req =
		embedded_wifi_mgmt_WifiMgmtRequest_init_zero;
	embedded_wifi_mgmt_WifiMgmtResponse resp =
		embedded_wifi_mgmt_WifiMgmtResponse_init_zero;

	req.request_id = embedded_wifi_mgmt_RequestType_REQUEST_TYPE_AP_ENABLE;
	req.which_payload =
		embedded_wifi_mgmt_WifiMgmtRequest_ap_enable_req_tag;
	req.payload.ap_enable_req.iface_index = VIRT_WIFI_IFACE_INDEX;
	req.payload.ap_enable_req.has_params = true;
	if (params->ssid != NULL && params->ssid_length <= 32) {
		memcpy(req.payload.ap_enable_req.params.ssid.bytes,
			params->ssid, params->ssid_length);
		req.payload.ap_enable_req.params.ssid.size =
			params->ssid_length;
	}
	req.payload.ap_enable_req.params.security_type =
		zephyr_to_proto_security(params->security);
	if (params->psk != NULL && params->psk_length <= 65) {
		memcpy(req.payload.ap_enable_req.params.psk.bytes,
			params->psk, params->psk_length);
		req.payload.ap_enable_req.params.psk.size =
			params->psk_length;
	}

	int rc = mgmt_roundtrip(&req, &resp);

	if (rc != 0) {
		return rc;
	}
	return decode_status(&resp);
}

static int virt_wifi_ap_disable(const struct device *dev)
{
	ARG_UNUSED(dev);

	embedded_wifi_mgmt_WifiMgmtRequest req =
		embedded_wifi_mgmt_WifiMgmtRequest_init_zero;
	embedded_wifi_mgmt_WifiMgmtResponse resp =
		embedded_wifi_mgmt_WifiMgmtResponse_init_zero;

	req.request_id = embedded_wifi_mgmt_RequestType_REQUEST_TYPE_AP_DISABLE;
	req.which_payload =
		embedded_wifi_mgmt_WifiMgmtRequest_ap_disable_req_tag;
	req.payload.ap_disable_req.iface_index = VIRT_WIFI_IFACE_INDEX;

	int rc = mgmt_roundtrip(&req, &resp);

	if (rc != 0) {
		return rc;
	}
	return decode_status(&resp);
}

static int virt_wifi_iface_status(const struct device *dev,
				  struct wifi_iface_status *status)
{
	ARG_UNUSED(dev);

	embedded_wifi_mgmt_WifiMgmtRequest req =
		embedded_wifi_mgmt_WifiMgmtRequest_init_zero;
	embedded_wifi_mgmt_WifiMgmtResponse resp =
		embedded_wifi_mgmt_WifiMgmtResponse_init_zero;

	req.request_id =
		embedded_wifi_mgmt_RequestType_REQUEST_TYPE_GET_IFACE_STATUS;
	req.which_payload =
		embedded_wifi_mgmt_WifiMgmtRequest_get_iface_status_req_tag;
	req.payload.get_iface_status_req.iface_index =
		VIRT_WIFI_IFACE_INDEX;

	int rc = mgmt_roundtrip(&req, &resp);

	if (rc != 0) {
		return rc;
	}

	if (resp.which_payload ==
	    embedded_wifi_mgmt_WifiMgmtResponse_get_iface_status_resp_tag) {
		const embedded_wifi_mgmt_WifiInterfaceStatus *s =
			&resp.payload.get_iface_status_resp.status;

		status->state = proto_to_zephyr_state(s->state);
		status->iface_mode = proto_to_zephyr_mode(s->mode);
		status->security = proto_to_zephyr_security(s->security);
		status->mfp = proto_to_zephyr_mfp(s->mfp);
		status->channel = (uint8_t)s->channel;
		status->rssi = s->rssi;
		if (s->ssid.size <= WIFI_SSID_MAX_LEN) {
			memcpy(status->ssid, s->ssid.bytes, s->ssid.size);
			status->ssid_len = (uint8_t)s->ssid.size;
		}
		memcpy(status->bssid, s->bssid, WIFI_MAC_ADDR_LEN);
		return 0;
	}
	return decode_status(&resp);
}

static int virt_wifi_get_version(const struct device *dev,
				 struct wifi_version *params)
{
	ARG_UNUSED(dev);

	embedded_wifi_mgmt_WifiMgmtRequest req =
		embedded_wifi_mgmt_WifiMgmtRequest_init_zero;
	embedded_wifi_mgmt_WifiMgmtResponse resp =
		embedded_wifi_mgmt_WifiMgmtResponse_init_zero;

	req.request_id =
		embedded_wifi_mgmt_RequestType_REQUEST_TYPE_GET_WIFI_VERSION;
	req.which_payload =
		embedded_wifi_mgmt_WifiMgmtRequest_get_version_req_tag;

	int rc = mgmt_roundtrip(&req, &resp);

	if (rc != 0) {
		return rc;
	}

	if (resp.which_payload ==
	    embedded_wifi_mgmt_WifiMgmtResponse_get_version_resp_tag) {
		const embedded_wifi_mgmt_WifiVersion *v =
			&resp.payload.get_version_resp.version;
		static char drv_buf[16];
		static char fw_buf[32];

		snprintf(drv_buf, sizeof(drv_buf), "%u.%u.%u",
			(unsigned int)v->driver_major,
			(unsigned int)v->driver_minor,
			(unsigned int)v->driver_patch);
		params->drv_version = drv_buf;

		if (v->firmware_version[0] != '\0') {
			snprintf(fw_buf, sizeof(fw_buf), "%s",
				v->firmware_version);
			params->fw_version = fw_buf;
		} else {
			params->fw_version = "";
		}
		return 0;
	}
	return decode_status(&resp);
}

static int virt_wifi_get_ps_config(const struct device *dev,
				   struct wifi_ps_config *config)
{
	ARG_UNUSED(dev);

	embedded_wifi_mgmt_WifiMgmtRequest req =
		embedded_wifi_mgmt_WifiMgmtRequest_init_zero;
	embedded_wifi_mgmt_WifiMgmtResponse resp =
		embedded_wifi_mgmt_WifiMgmtResponse_init_zero;

	req.request_id =
		embedded_wifi_mgmt_RequestType_REQUEST_TYPE_GET_PS_CONFIG;
	req.which_payload =
		embedded_wifi_mgmt_WifiMgmtRequest_get_ps_config_req_tag;
	req.payload.get_ps_config_req.iface_index =
		VIRT_WIFI_IFACE_INDEX;

	int rc = mgmt_roundtrip(&req, &resp);

	if (rc != 0) {
		return rc;
	}

	if (resp.which_payload ==
	    embedded_wifi_mgmt_WifiMgmtResponse_get_ps_config_resp_tag) {
		const embedded_wifi_mgmt_WifiPowerSaveConfig *c =
			&resp.payload.get_ps_config_resp.config;

		config->ps_params.enabled =
			(c->ps_settings.enabled ==
			 embedded_wifi_mgmt_WifiPsState_WIFI_PS_STATE_ENABLE) ?
			WIFI_PS_ENABLED : WIFI_PS_DISABLED;
		return 0;
	}
	return decode_status(&resp);
}

static int virt_wifi_reg_domain(const struct device *dev,
				struct wifi_reg_domain *reg_domain)
{
	ARG_UNUSED(dev);

	embedded_wifi_mgmt_WifiMgmtRequest req =
		embedded_wifi_mgmt_WifiMgmtRequest_init_zero;
	embedded_wifi_mgmt_WifiMgmtResponse resp =
		embedded_wifi_mgmt_WifiMgmtResponse_init_zero;

	bool is_get = (reg_domain->oper == WIFI_MGMT_GET);

	req.request_id = is_get ?
		embedded_wifi_mgmt_RequestType_REQUEST_TYPE_GET_REG_DOMAIN :
		embedded_wifi_mgmt_RequestType_REQUEST_TYPE_SET_REG_DOMAIN;
	req.which_payload = is_get ?
		embedded_wifi_mgmt_WifiMgmtRequest_get_reg_domain_req_tag :
		embedded_wifi_mgmt_WifiMgmtRequest_set_reg_domain_req_tag;

	if (is_get) {
		req.payload.get_reg_domain_req.iface_index =
			VIRT_WIFI_IFACE_INDEX;
	} else {
		req.payload.set_reg_domain_req.has_reg_domain = true;
		req.payload.set_reg_domain_req.reg_domain.country_code[0] =
			(char)reg_domain->country_code[0];
		req.payload.set_reg_domain_req.reg_domain.country_code[1] =
			(char)reg_domain->country_code[1];
		req.payload.set_reg_domain_req.reg_domain.country_code[2] = '\0';
	}

	int rc = mgmt_roundtrip(&req, &resp);

	if (rc != 0) {
		return rc;
	}

	if (is_get && resp.which_payload ==
	    embedded_wifi_mgmt_WifiMgmtResponse_get_reg_domain_resp_tag) {
		const char *cc =
			resp.payload.get_reg_domain_resp.reg_domain.country_code;

		reg_domain->country_code[0] = (uint8_t)cc[0];
		reg_domain->country_code[1] = (uint8_t)cc[1];
		reg_domain->country_code[2] = '\0';
		return 0;
	}
	return decode_status(&resp);
}

static const struct wifi_mgmt_ops virt_wifi_ops = {
	.scan = virt_wifi_scan,
	.connect = virt_wifi_connect,
	.disconnect = virt_wifi_disconnect,
	.ap_enable = virt_wifi_ap_enable,
	.ap_disable = virt_wifi_ap_disable,
	.iface_status = virt_wifi_iface_status,
	.get_version = virt_wifi_get_version,
	.get_power_save_config = virt_wifi_get_ps_config,
	.reg_domain = virt_wifi_reg_domain,
};

static const struct net_wifi_mgmt_offload virt_wifi_offload = {
	.wifi_iface = {
		.send = virt_wifi_l2_send,
	},
	.wifi_mgmt_api = &virt_wifi_ops,
};

static int virt_wifi_init(const struct device *dev)
{
	ARG_UNUSED(dev);

	int rc = bus_open();

	if (rc != 0) {
		LOG_ERR("virt_wifi: bus_open failed: %d", rc);
		return rc;
	}

	struct net_if *iface = virt_wifi_netif();

	/* Mark the Ethernet L2 context as a Wi-Fi interface so that
	 * net_if_is_wifi() (and thus get_wifi_api()) recognizes this
	 * interface for NET_REQUEST_WIFI_SCAN/CONNECT routing. Without this,
	 * the wifi_mgmt subsystem returns -ENOTSUP for scan/connect.
	 */
	struct ethernet_context *eth_ctx = net_if_l2_data(iface);

	if (eth_ctx != NULL) {
		eth_ctx->eth_if_type = L2_ETH_IF_TYPE_WIFI;
	}

	bus_register_handler(SERIAL_TYPE_WIFI_DATA, wifi_data_rx);
	bus_set_handler_user(SERIAL_TYPE_WIFI_DATA, iface);
	bus_register_handler(SERIAL_TYPE_WIFI_MGMT, wifi_mgmt_notify);

	LOG_INF("virt_wifi: registered (iface index %d)",
		VIRT_WIFI_IFACE_INDEX);
	return 0;
}

NET_DEVICE_DT_INST_DEFINE(0, virt_wifi_init, NULL, NULL, NULL,
			  CONFIG_WIFI_INIT_PRIORITY, &virt_wifi_offload,
			  ETHERNET_L2, NET_L2_GET_CTX_TYPE(ETHERNET_L2),
			  CONFIG_VIRT_WIFI_MTU);
