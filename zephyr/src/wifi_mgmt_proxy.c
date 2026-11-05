#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>
#include <errno.h>

#include <pb_decode.h>
#include <pb_encode.h>
#include "wifi.pb.h"
#include "wifi_mgmt_proxy.h"
#include "multiplexer.h"

LOG_MODULE_REGISTER(wifi_mgmt_proxy, LOG_LEVEL_INF);

/* 8 KiB: process_wifi_mgmt_request() keeps the full WifiMgmtRequest/response
 * protobuf structs plus callback contexts on the stack, and the nanopb decode
 * + logging + net_mgmt call chains add ~1 KiB more. 2 KiB overflowed on the
 * first version request (Zephyr fatal error 2: stack overflow).
 *
 * 4 KiB overflowed on the CONNECT path: net_if_wifi_connect() -> nrf_wifi ->
 * Zephyr wpa_supplicant (supp_api.c) -> wpa_cli_cmd_v("remove_network all")
 * -> wpa_cli_cmd(), which alone allocates ~1 KiB (cmd[512] + buf[512] + argv)
 * on top of ~3 KiB of caller frames (proxy + net + nrf_wifi + supp_api),
 * tripping the stack sentinel check (K_ERR_STACK_CHK_FAIL). 8 KiB leaves
 * ~3 KiB of headroom for deeper connect/EAP call chains. */
#define WIFI_MGMT_THREAD_STACK_SIZE 8192
#define WIFI_MGMT_THREAD_PRIORITY 7


// --- Nanopb Max Size Defines (ensure these match your .options file or Zephyr limits) ---
// These are illustrative; actual limits come from Zephyr's wifi.h or your .options file.
#define MAX_SSID_LEN_CB WIFI_SSID_MAX_LEN
#define MAX_PSK_LEN_CB 64 // Example for WPA/WPA2 PSK
#define MAX_SAE_PASSWORD_LEN_CB 128 // Example for SAE password
#define MAX_DIRECTED_SCAN_SSIDS_CB 4
#define MAX_SCAN_CHANNELS_CB 11 // Example, from WIFI_SCAN_MAX_CHANNELS
#define MAX_ERROR_MSG_LEN 63 // For a 64-byte buffer including null terminator if treated as C string
#define MAX_FIRMWARE_VERSION_LEN 31 // For a 32-byte buffer
#define MAX_AP_PSK_LEN 64

#define DATA_COMPLETE_EVENT BIT(0) // Event bit for data completion

/* ---------------------------------------------------------------------------
 * Proto <-> Zephyr enum mappings
 *
 * The protobuf enums in protos/wifi.proto and the Zephyr enums in
 * <zephyr/net/wifi.h> assign DIFFERENT numeric values to the same concepts
 * (e.g. security: Zephyr NONE=0/PSK=1/SAE=3 vs proto NONE=1/PSK=3/SAE=7).
 * Raw casts are therefore wrong in both directions and must go through these
 * translators.
 * ------------------------------------------------------------------------- */

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
		/* UNKNOWN (and future values): Zephyr's UNKNOWN == NONE (0). */
		return WIFI_SECURITY_TYPE_NONE;
	}
}

static embedded_wifi_mgmt_WifiBand zephyr_to_proto_band(
	enum wifi_frequency_bands band)
{
	switch (band) {
	case WIFI_FREQ_BAND_2_4_GHZ:
		return embedded_wifi_mgmt_WifiBand_WIFI_BAND_2_4_GHZ;
	case WIFI_FREQ_BAND_5_GHZ:
		return embedded_wifi_mgmt_WifiBand_WIFI_BAND_5_GHZ;
	case WIFI_FREQ_BAND_6_GHZ:
		return embedded_wifi_mgmt_WifiBand_WIFI_BAND_6_GHZ;
	default:
		return embedded_wifi_mgmt_WifiBand_WIFI_BAND_ANY;
	}
}

static enum wifi_frequency_bands proto_to_zephyr_band(embedded_wifi_mgmt_WifiBand pband)
{
	switch (pband) {
	case embedded_wifi_mgmt_WifiBand_WIFI_BAND_5_GHZ:
		return WIFI_FREQ_BAND_5_GHZ;
	case embedded_wifi_mgmt_WifiBand_WIFI_BAND_6_GHZ:
		return WIFI_FREQ_BAND_6_GHZ;
	case embedded_wifi_mgmt_WifiBand_WIFI_BAND_2_4_GHZ:
	default:
		/* ANY (and 2.4 GHz): Zephyr has no "any" band; default to 2.4 GHz. */
		return WIFI_FREQ_BAND_2_4_GHZ;
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

/*
 * Proto (PASSIVE=0, ACTIVE=1) and Zephyr (ACTIVE=0, PASSIVE=1) use opposite
 * numbering, so a raw cast inverts the requested scan type.
 */
static enum wifi_scan_type proto_to_zephyr_scan_type(embedded_wifi_mgmt_WifiScanType ptype)
{
	switch (ptype) {
	case embedded_wifi_mgmt_WifiScanType_WIFI_SCAN_TYPE_ACTIVE:
		return WIFI_SCAN_TYPE_ACTIVE;
	default:
		/* PASSIVE (and any unknown value): passive is the safe default. */
		return WIFI_SCAN_TYPE_PASSIVE;
	}
}

static embedded_wifi_mgmt_WifiInterfaceState zephyr_to_proto_state(
	enum wifi_iface_state state)
{
	switch (state) {
	case WIFI_STATE_INACTIVE:
		return embedded_wifi_mgmt_WifiInterfaceState_WIFI_STATE_INACTIVE;
	case WIFI_STATE_SCANNING:
		return embedded_wifi_mgmt_WifiInterfaceState_WIFI_STATE_SCANNING;
	case WIFI_STATE_AUTHENTICATING:
		return embedded_wifi_mgmt_WifiInterfaceState_WIFI_STATE_AUTHENTICATING;
	case WIFI_STATE_ASSOCIATING:
		return embedded_wifi_mgmt_WifiInterfaceState_WIFI_STATE_ASSOCIATING;
	case WIFI_STATE_ASSOCIATED:
		return embedded_wifi_mgmt_WifiInterfaceState_WIFI_STATE_ASSOCIATED;
	case WIFI_STATE_4WAY_HANDSHAKE:
		return embedded_wifi_mgmt_WifiInterfaceState_WIFI_STATE_4WAY_HANDSHAKE;
	case WIFI_STATE_GROUP_HANDSHAKE:
		return embedded_wifi_mgmt_WifiInterfaceState_WIFI_STATE_GROUP_HANDSHAKE;
	case WIFI_STATE_COMPLETED:
		return embedded_wifi_mgmt_WifiInterfaceState_WIFI_STATE_CONNECTED;
	case WIFI_STATE_INTERFACE_DISABLED:
		return embedded_wifi_mgmt_WifiInterfaceState_WIFI_STATE_INTERFACE_DISABLED;
	default: /* DISCONNECTED, DISCONNECTING, UNKNOWN */
		return embedded_wifi_mgmt_WifiInterfaceState_WIFI_STATE_DISCONNECTED;
	}
}

static embedded_wifi_mgmt_WifiMode zephyr_to_proto_mode(enum wifi_iface_mode mode)
{
	switch (mode) {
	case WIFI_MODE_AP:
		return embedded_wifi_mgmt_WifiMode_WIFI_MODE_AP;
	case WIFI_MODE_MESH:
		return embedded_wifi_mgmt_WifiMode_WIFI_MODE_MESH_POINT;
	case WIFI_MODE_P2P_GO:
		return embedded_wifi_mgmt_WifiMode_WIFI_MODE_P2P_GO;
	default: /* INFRA, IBSS, P2P_GROUP_FORMATION, UNKNOWN */
		return embedded_wifi_mgmt_WifiMode_WIFI_MODE_INFRA;
	}
}

static embedded_wifi_mgmt_WifiLinkMode zephyr_to_proto_link_mode(
	enum wifi_link_mode link_mode)
{
	switch (link_mode) {
	case WIFI_1:
		return embedded_wifi_mgmt_WifiLinkMode_WIFI_LINK_MODE_LEGACY_B;
	case WIFI_3:
		return embedded_wifi_mgmt_WifiLinkMode_WIFI_LINK_MODE_LEGACY_G;
	case WIFI_2:
		return embedded_wifi_mgmt_WifiLinkMode_WIFI_LINK_MODE_LEGACY_A;
	case WIFI_4:
		return embedded_wifi_mgmt_WifiLinkMode_WIFI_LINK_MODE_HT_20;
	case WIFI_5:
		return embedded_wifi_mgmt_WifiLinkMode_WIFI_LINK_MODE_VHT_20;
	case WIFI_6:
	case WIFI_6E:
		return embedded_wifi_mgmt_WifiLinkMode_WIFI_LINK_MODE_HE_20;
	default: /* WIFI_0 (plain legacy), unknown */
		return embedded_wifi_mgmt_WifiLinkMode_WIFI_LINK_MODE_UNKNOWN;
	}
}

// Data structure for WiFi management


K_EVENT_DEFINE(wifi_mgmt_event);

static uint8_t wifi_mgmt_tx_buffer[sizeof(embedded_wifi_mgmt_WifiMgmtResponse)];
static uint8_t wifi_mgmt_rx_buffer[sizeof(embedded_wifi_mgmt_WifiMgmtRequest)];
static size_t message_length;

static void process_wifi_mgmt_request(const uint8_t *buffer, size_t length, uint8_t *response_buffer, size_t* response_buffer_len_io);

// --- WiFi Management Thread Entry Point ---

static void wifi_mgmt_thread_entry_point(void *p1, void *p2, void *p3)
{
    // This is the entry point for the WiFi management thread.
    // You can implement your thread logic here, such as handling incoming requests,
    // processing scan results, managing connections, etc.

    while (true) {
        LOG_INF("WiFi Management Thread waiting for events...");
        uint32_t event = k_event_wait(&wifi_mgmt_event, DATA_COMPLETE_EVENT, true, K_FOREVER);

        if (event & DATA_COMPLETE_EVENT) {
            LOG_INF("Data complete event received, length: %zu", message_length);

            size_t response_length = sizeof(wifi_mgmt_tx_buffer);

            process_wifi_mgmt_request(wifi_mgmt_rx_buffer, message_length,
                                      wifi_mgmt_tx_buffer, &response_length);

            if (response_length > 0) {
                int tx_ret = serial_mux_send(SERIAL_TYPE_WIFI_MGMT,
                                             wifi_mgmt_tx_buffer,
                                             response_length);
                if (tx_ret < 0) {
                    LOG_ERR("Failed to send Wi-Fi mgmt response: %d", tx_ret);
                }
            }

            /* Reset RX buffer for next message. */
            message_length = 0;
        }
    }
}

bool wifi_mgmt_acquire_buffer(uint8_t **data, size_t len)
{
    // Check if the ring buffer has enough space
    if (len > (sizeof(wifi_mgmt_rx_buffer) - message_length)) {
        LOG_WRN("Not enough space in buffer for %zu bytes, Abort", len);
        // TODO: This is severe error condition and should happen. Reseting things would be an appropriate action.
        return false;
    }

    *data = &wifi_mgmt_rx_buffer[message_length];
    return true;
}

void wifi_mgmt_commit_data(size_t len)
{
    message_length += len; // Update the total message length
}

void wifi_mgmt_message_complete(size_t len)
{
    ARG_UNUSED(len); // The length is already stored in message_length and not needed in this proxy.
    // This function is called when a complete message has been received.
    // It can be used to signal the main thread or other components that a message is ready for processing.

    LOG_INF("Message complete, length: %zu", message_length);
    k_event_set(&wifi_mgmt_event, DATA_COMPLETE_EVENT); // Signal the event
}


/* --- Wi-Fi scan result reporting -------------------------------------------
 *
 * The scan itself is asynchronous: NET_REQUEST_WIFI_SCAN only reports whether
 * the request was accepted. Individual results arrive as
 * NET_EVENT_WIFI_SCAN_RESULT events and the completion as
 * NET_EVENT_WIFI_SCAN_DONE. Results are collected and flushed to the host as
 * a single WifiMgmtNotification (ScanDoneEvent) frame.
 *
 * net_mgmt events are dispatched serialized on the network thread, so the
 * collection state needs no locking.
 */

static uint8_t wifi_mgmt_notify_buffer[sizeof(embedded_wifi_mgmt_WifiMgmtNotification)];
static embedded_wifi_mgmt_WifiScanResult scan_results[CONFIG_WIFI_MGMT_PROXY_MAX_SCAN_RESULTS];
static embedded_wifi_mgmt_WifiMgmtNotification scan_done_notification;
static size_t scan_result_count;

static void send_scan_done_notification(struct net_if *iface, int32_t status)
{
    memset(&scan_done_notification, 0, sizeof(scan_done_notification));
    scan_done_notification.has_event = true;
    scan_done_notification.event.iface_index = (iface != NULL) ? net_if_get_by_iface(iface) : 0;
    scan_done_notification.event.which_event_data =
        embedded_wifi_mgmt_WifiEvent_scan_done_event_tag;
    scan_done_notification.event.event_data.scan_done_event.status = status;
    scan_done_notification.event.event_data.scan_done_event.all_results_count =
        (pb_size_t)scan_result_count;
    memcpy(scan_done_notification.event.event_data.scan_done_event.all_results,
           scan_results,
           sizeof(scan_results[0]) * scan_result_count);

    pb_ostream_t ostream = pb_ostream_from_buffer(wifi_mgmt_notify_buffer,
                                                  sizeof(wifi_mgmt_notify_buffer));

    if (pb_encode(&ostream, embedded_wifi_mgmt_WifiMgmtNotification_fields,
                  &scan_done_notification)) {
        int tx_ret = serial_mux_send(SERIAL_TYPE_WIFI_MGMT, wifi_mgmt_notify_buffer,
                                     ostream.bytes_written);
        if (tx_ret < 0) {
            LOG_ERR("Failed to send scan done notification: %d", tx_ret);
        }
    } else {
        LOG_ERR("Failed to encode scan done notification: %s", ostream.errmsg);
    }

    scan_result_count = 0;
}

static void handle_wifi_scan_result(const struct wifi_scan_result *entry)
{
    if (scan_result_count >= ARRAY_SIZE(scan_results)) {
        LOG_WRN("Scan result buffer full, dropping additional results");
        return;
    }

    embedded_wifi_mgmt_WifiScanResult *result = &scan_results[scan_result_count];

    size_t ssid_len = entry->ssid_length;

    if (ssid_len > sizeof(result->ssid.bytes)) {
        ssid_len = sizeof(result->ssid.bytes);
    }
    result->ssid.size = (pb_size_t)ssid_len;
    memcpy(result->ssid.bytes, entry->ssid, ssid_len);
    memcpy(result->bssid, entry->mac, WIFI_MAC_ADDR_LEN);
    result->channel = entry->channel;
    result->rssi = entry->rssi;
    result->security = zephyr_to_proto_security(entry->security);
    result->wpa3_enterprise_type =
        (embedded_wifi_mgmt_WifiWpa3EnterpriseType)entry->wpa3_ent_type;
    result->mfp = zephyr_to_proto_mfp(entry->mfp);
    result->band = zephyr_to_proto_band(entry->band);

    scan_result_count++;
}

/* --- Wi-Fi AP status reporting ----------------------------------------------
 *
 * AP mode is started and stopped by the host with NET_REQUEST_WIFI_AP_ENABLE
 * and NET_REQUEST_WIFI_AP_DISABLE. The outcomes arrive as
 * NET_EVENT_WIFI_AP_ENABLE_RESULT / NET_EVENT_WIFI_AP_DISABLE_RESULT events
 * and station activity as NET_EVENT_WIFI_AP_STA_CONNECTED /
 * NET_EVENT_WIFI_AP_STA_DISCONNECTED. Each is forwarded to the host as a
 * WifiMgmtNotification frame (ApEnableResultEvent, ApDisableResultEvent,
 * ApStaConnectedEvent, ApStaDisconnectedEvent).
 *
 * net_mgmt events are dispatched serialized on the network thread, so the
 * shared notification buffer needs no locking.
 */

/* Zephyr's wifi_ap_status values are mirrored 1:1 by the proto WifiApStatus
 * enum, but the event status field is errno-style (0 for success, negative
 * error code otherwise), so failures are negated: -1 = FAIL,
 * -2 = CHANNEL_NOT_SUPPORTED, and so on. */
static int32_t ap_status_to_proto(int status)
{
	return (status == WIFI_STATUS_AP_SUCCESS) ? 0 : -(int32_t)status;
}

static void send_ap_result_notification(struct net_if *iface, int which_tag, int32_t status)
{
	embedded_wifi_mgmt_WifiMgmtNotification notification =
		embedded_wifi_mgmt_WifiMgmtNotification_init_zero;

	notification.has_event = true;
	notification.event.iface_index = (iface != NULL) ? net_if_get_by_iface(iface) : 0;
	notification.event.which_event_data = which_tag;
	/* The enable and disable result events share the same layout: a
	 * single int32 status field at the start of the event union. */
	notification.event.event_data.ap_enable_result_event.status = status;

	pb_ostream_t ostream = pb_ostream_from_buffer(wifi_mgmt_notify_buffer,
	                                              sizeof(wifi_mgmt_notify_buffer));

	if (pb_encode(&ostream, embedded_wifi_mgmt_WifiMgmtNotification_fields,
	                &notification)) {
		int tx_ret = serial_mux_send(SERIAL_TYPE_WIFI_MGMT, wifi_mgmt_notify_buffer,
		                                 ostream.bytes_written);
		if (tx_ret < 0) {
			LOG_ERR("Failed to send AP result notification: %d", tx_ret);
		}
	} else {
		LOG_ERR("Failed to encode AP result notification: %s", ostream.errmsg);
	}
}

static void send_ap_sta_notification(struct net_if *iface, bool connected,
                                     const struct wifi_ap_sta_info *sta_info)
{
	embedded_wifi_mgmt_WifiMgmtNotification notification =
		embedded_wifi_mgmt_WifiMgmtNotification_init_zero;

	if (sta_info == NULL) {
		LOG_ERR("AP STA event without station info");
		return;
	}

	notification.has_event = true;
	notification.event.iface_index = (iface != NULL) ? net_if_get_by_iface(iface) : 0;
	/* sta_mac_address is a fixed 6-byte array (nanopb fixed_length), no
	 * size field to set. */
	if (connected) {
		notification.event.which_event_data =
			embedded_wifi_mgmt_WifiEvent_ap_sta_connected_event_tag;
		memcpy(notification.event.event_data.ap_sta_connected_event.sta_mac_address,
		       sta_info->mac, WIFI_MAC_ADDR_LEN);
	} else {
		notification.event.which_event_data =
			embedded_wifi_mgmt_WifiEvent_ap_sta_disconnected_event_tag;
		memcpy(notification.event.event_data.ap_sta_disconnected_event.sta_mac_address,
		       sta_info->mac, WIFI_MAC_ADDR_LEN);
	}

	pb_ostream_t ostream = pb_ostream_from_buffer(wifi_mgmt_notify_buffer,
	                                              sizeof(wifi_mgmt_notify_buffer));

	if (pb_encode(&ostream, embedded_wifi_mgmt_WifiMgmtNotification_fields,
	                &notification)) {
		int tx_ret = serial_mux_send(SERIAL_TYPE_WIFI_MGMT, wifi_mgmt_notify_buffer,
		                                 ostream.bytes_written);
		if (tx_ret < 0) {
			LOG_ERR("Failed to send AP STA notification: %d", tx_ret);
		}
	} else {
		LOG_ERR("Failed to encode AP STA notification: %s", ostream.errmsg);
	}
}

static void wifi_mgmt_net_event_handler(struct net_mgmt_event_callback *cb,
					uint64_t mgmt_event,
					struct net_if *iface)
{
    switch (mgmt_event) {
    case NET_EVENT_WIFI_SCAN_RESULT:
        handle_wifi_scan_result((const struct wifi_scan_result *)cb->info);
        break;
    case NET_EVENT_WIFI_SCAN_DONE: {
        const struct wifi_status *wstatus =
            (const struct wifi_status *)cb->info;
        int32_t status = (wstatus != NULL) ? wstatus->status : 0;

        send_scan_done_notification(iface, status);
        break;
    }
    case NET_EVENT_WIFI_AP_ENABLE_RESULT:
        send_ap_result_notification(iface,
                                    embedded_wifi_mgmt_WifiEvent_ap_enable_result_event_tag,
                                    ap_status_to_proto(*(const enum wifi_ap_status *)cb->info));
        break;
    case NET_EVENT_WIFI_AP_DISABLE_RESULT:
        send_ap_result_notification(iface,
                                    embedded_wifi_mgmt_WifiEvent_ap_disable_result_event_tag,
                                    ap_status_to_proto(*(const enum wifi_ap_status *)cb->info));
        break;
    case NET_EVENT_WIFI_AP_STA_CONNECTED:
        send_ap_sta_notification(iface, true, (const struct wifi_ap_sta_info *)cb->info);
        break;
    case NET_EVENT_WIFI_AP_STA_DISCONNECTED:
        send_ap_sta_notification(iface, false, (const struct wifi_ap_sta_info *)cb->info);
        break;
    default:
        break;
    }
}

static struct net_mgmt_event_callback wifi_mgmt_event_cb;

static int wifi_mgmt_events_init(void)
{
    net_mgmt_init_event_callback(&wifi_mgmt_event_cb, wifi_mgmt_net_event_handler,
                                 NET_EVENT_WIFI_SCAN_RESULT | NET_EVENT_WIFI_SCAN_DONE |
                                 NET_EVENT_WIFI_AP_ENABLE_RESULT |
                                 NET_EVENT_WIFI_AP_DISABLE_RESULT |
                                 NET_EVENT_WIFI_AP_STA_CONNECTED |
                                 NET_EVENT_WIFI_AP_STA_DISCONNECTED);
    net_mgmt_add_event_callback(&wifi_mgmt_event_cb);
    return 0;
}

SYS_INIT(wifi_mgmt_events_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);


K_THREAD_DEFINE(wifi_mgmt_proxy_thread_id,      // Name for the thread ID
                WIFI_MGMT_THREAD_STACK_SIZE,     // Stack size in bytes
                wifi_mgmt_thread_entry_point,    // Thread entry function
                NULL, NULL, NULL,         // Parameters to entry function (p1, p2, p3)
                WIFI_MGMT_THREAD_PRIORITY,       // Thread priority
                0,                        // Thread options (e.g., K_FP_REGS for FPU)
                0);                       // Scheduling delay (K_NO_WAIT for immediate start)

/// Bulk code below


// --- Callback Context Structures ---
struct wifi_scan_ssid_spec {
    uint8_t ssid[MAX_SSID_LEN_CB];
    size_t ssid_len;
};

struct ScanRequestContext {
    struct wifi_scan_ssid_spec ssids[MAX_DIRECTED_SCAN_SSIDS_CB];
    uint8_t current_ssid_count;
    // Add other fields if scan params have more callback-decoded parts
};



// --- Nanopb Decoder Callback Functions ---
// (Assuming these are correctly implemented as in your provided context to populate the context structs)

static bool decode_directed_scan_ssid_cb(pb_istream_t *stream, const pb_field_t *field, void **arg) {
    struct ScanRequestContext *ctx = (struct ScanRequestContext *)*arg;
    if (ctx->current_ssid_count >= MAX_DIRECTED_SCAN_SSIDS_CB) {
        LOG_WRN("Max directed scan SSIDs reached (%u)", MAX_DIRECTED_SCAN_SSIDS_CB);
        embedded_wifi_mgmt_WifiDirectedScanSsid dummy_ssid = embedded_wifi_mgmt_WifiDirectedScanSsid_init_zero;
        if (!pb_decode(stream, embedded_wifi_mgmt_WifiDirectedScanSsid_fields, &dummy_ssid)) return false;
        return true; // Skip
    }
    embedded_wifi_mgmt_WifiDirectedScanSsid proto_ssid = embedded_wifi_mgmt_WifiDirectedScanSsid_init_zero;
    if (!pb_decode(stream, embedded_wifi_mgmt_WifiDirectedScanSsid_fields, &proto_ssid)) return false;

    if (proto_ssid.ssid.size > MAX_SSID_LEN_CB) {
        LOG_WRN("Directed SSID too long (%u > %d)", proto_ssid.ssid.size, MAX_SSID_LEN_CB);
        return true; // Skip this SSID
    }
    memcpy(ctx->ssids[ctx->current_ssid_count].ssid, proto_ssid.ssid.bytes, proto_ssid.ssid.size);
    ctx->ssids[ctx->current_ssid_count].ssid_len = proto_ssid.ssid.size;
    ctx->current_ssid_count++;
    return true;
}


// --- Forward declarations of request handlers ---
static void handle_connect_request(const embedded_wifi_mgmt_WifiConnectRequest *req, embedded_wifi_mgmt_WifiStatusResponse *resp);
static void handle_disconnect_request(const embedded_wifi_mgmt_WifiDisconnectRequest *req, embedded_wifi_mgmt_WifiStatusResponse *resp);
static void handle_scan_request(const embedded_wifi_mgmt_WifiScanRequest *req, struct ScanRequestContext *scan_ctx, embedded_wifi_mgmt_WifiStatusResponse *resp);
static void handle_ap_enable_request(const embedded_wifi_mgmt_WifiApEnableRequest *req, embedded_wifi_mgmt_WifiStatusResponse *resp);
static void handle_ap_disable_request(const embedded_wifi_mgmt_WifiApDisableRequest *req, embedded_wifi_mgmt_WifiStatusResponse *resp);
static void handle_get_version_request(const embedded_wifi_mgmt_GetWifiVersionRequest *req, embedded_wifi_mgmt_GetWifiVersionResponse *resp);
static void handle_set_ps_config_request(const embedded_wifi_mgmt_SetPowerSaveConfigRequest *req, embedded_wifi_mgmt_WifiStatusResponse *resp);
static void handle_get_ps_config_request(const embedded_wifi_mgmt_GetPowerSaveConfigRequest *req, embedded_wifi_mgmt_GetPowerSaveConfigResponse *data_resp, embedded_wifi_mgmt_WifiStatusResponse *err_resp);
static void handle_set_reg_domain_request(const embedded_wifi_mgmt_SetRegulatoryDomainRequest *req, embedded_wifi_mgmt_WifiStatusResponse *resp);
static void handle_get_reg_domain_request(const embedded_wifi_mgmt_GetRegulatoryDomainRequest *req, embedded_wifi_mgmt_GetRegulatoryDomainResponse *data_resp, embedded_wifi_mgmt_WifiStatusResponse *err_resp);
static void handle_get_iface_status_request(const embedded_wifi_mgmt_GetInterfaceStatusRequest *req, embedded_wifi_mgmt_GetInterfaceStatusResponse *data_resp, embedded_wifi_mgmt_WifiStatusResponse *err_resp);
// Add other handlers as needed, e.g., for TWT, AP STA disconnect, etc.
// static void handle_set_twt_request(const embedded_wifi_mgmt_SetTwtRequest *req, embedded_wifi_mgmt_WifiStatusResponse *resp);


// Helper to fill WifiStatusResponse error message
// Assumes error_message field is pb_bytes_array_t (e.g. struct { uint8_t bytes[N]; pb_size_t size; })
static void set_status_error_msg_bytes(char *target_bytes_array, const char *msg) {
    if (!target_bytes_array || !msg) return;

    strncpy(target_bytes_array, msg, MAX_ERROR_MSG_LEN);
}


// --- Main processing function ---
static void process_wifi_mgmt_request(const uint8_t *buffer, size_t length, uint8_t *response_buffer, size_t* response_buffer_len_io) {
    embedded_wifi_mgmt_WifiMgmtRequest request = embedded_wifi_mgmt_WifiMgmtRequest_init_zero;
    pb_istream_t stream = pb_istream_from_buffer(buffer, length);

    struct ScanRequestContext scan_ctx = {0};

    // --- Setup decode callbacks if specific fields in the request need them ---
    // This depends on your .options file. If a field like `scan_req.params.directed_scan_ssids`
    // is a repeated message and uses a callback for decoding.
    // Example:
    // if (request.which_payload == embedded_wifi_mgmt_WifiMgmtRequest_scan_req_tag) {
    //    request.payload.scan_req.params.directed_scan_ssids.funcs.decode = &decode_directed_scan_ssid_cb;
    //    request.payload.scan_req.params.directed_scan_ssids.arg = &scan_ctx;
    // }
    // Note: Callbacks are often set on the top-level message for fields that are *always* present,
    // or on submessage fields if the .options file is configured for that.
    // For simplicity, we assume callbacks are set if needed by the .options file and nanopb handles it,
    // or they are set on the specific sub-message fields *before* decoding that sub-message.
    // The current structure decodes the whole request first, then uses contexts. This works if
    // the .options file has set up the callbacks on the fields themselves.

    if (!pb_decode(&stream, embedded_wifi_mgmt_WifiMgmtRequest_fields, &request)) {
        LOG_ERR("Failed to decode WifiMgmtRequest: %s", PB_GET_ERROR(&stream));
        embedded_wifi_mgmt_WifiMgmtResponse err_response = embedded_wifi_mgmt_WifiMgmtResponse_init_zero;
        err_response.request_id = 0; // Or a special value for decode failure
        err_response.which_payload = embedded_wifi_mgmt_WifiMgmtResponse_status_resp_tag;
        err_response.payload.status_resp.success = false;
        err_response.payload.status_resp.error_code = -EBADMSG;
        set_status_error_msg_bytes(err_response.payload.status_resp.error_message, "Request decode failed");

        pb_ostream_t ostream = pb_ostream_from_buffer(response_buffer, *response_buffer_len_io);
        if (pb_encode(&ostream, embedded_wifi_mgmt_WifiMgmtResponse_fields, &err_response)) {
            *response_buffer_len_io = ostream.bytes_written;
        } else {
            *response_buffer_len_io = 0;
        }
        return;
    }

    LOG_INF("Processing RequestType enum: %u (payload tag %u)", request.request_id, request.which_payload);

    embedded_wifi_mgmt_WifiMgmtResponse response = embedded_wifi_mgmt_WifiMgmtResponse_init_zero;
    response.request_id = (uint32_t)request.request_id;

    bool payload_type_mismatch = false;

    switch (request.request_id) {
        case embedded_wifi_mgmt_RequestType_REQUEST_TYPE_CONNECT:
            if (request.which_payload == embedded_wifi_mgmt_WifiMgmtRequest_connect_req_tag) {
                response.which_payload = embedded_wifi_mgmt_WifiMgmtResponse_status_resp_tag;
                handle_connect_request(&request.payload.connect_req, &response.payload.status_resp);
            } else { payload_type_mismatch = true; }
            break;
        case embedded_wifi_mgmt_RequestType_REQUEST_TYPE_DISCONNECT:
            if (request.which_payload == embedded_wifi_mgmt_WifiMgmtRequest_disconnect_req_tag) {
                response.which_payload = embedded_wifi_mgmt_WifiMgmtResponse_status_resp_tag;
                handle_disconnect_request(&request.payload.disconnect_req, &response.payload.status_resp);
            } else { payload_type_mismatch = true; }
            break;
        case embedded_wifi_mgmt_RequestType_REQUEST_TYPE_SCAN:
            if (request.which_payload == embedded_wifi_mgmt_WifiMgmtRequest_scan_req_tag) {
                // If scan_req.params.directed_scan_ssids uses a callback, scan_ctx is populated by pb_decode
                response.which_payload = embedded_wifi_mgmt_WifiMgmtResponse_status_resp_tag;
                handle_scan_request(&request.payload.scan_req, &scan_ctx, &response.payload.status_resp);
            } else { payload_type_mismatch = true; }
            break;
        case embedded_wifi_mgmt_RequestType_REQUEST_TYPE_AP_ENABLE:
            if (request.which_payload == embedded_wifi_mgmt_WifiMgmtRequest_ap_enable_req_tag) {
                response.which_payload = embedded_wifi_mgmt_WifiMgmtResponse_status_resp_tag;
                handle_ap_enable_request(&request.payload.ap_enable_req, &response.payload.status_resp);
            } else { payload_type_mismatch = true; }
            break;
        case embedded_wifi_mgmt_RequestType_REQUEST_TYPE_AP_DISABLE:
            if (request.which_payload == embedded_wifi_mgmt_WifiMgmtRequest_ap_disable_req_tag) {
                response.which_payload = embedded_wifi_mgmt_WifiMgmtResponse_status_resp_tag;
                handle_ap_disable_request(&request.payload.ap_disable_req, &response.payload.status_resp);
            } else { payload_type_mismatch = true; }
            break;
        case embedded_wifi_mgmt_RequestType_REQUEST_TYPE_GET_WIFI_VERSION:
             if (request.which_payload == embedded_wifi_mgmt_WifiMgmtRequest_get_version_req_tag) {
                response.which_payload = embedded_wifi_mgmt_WifiMgmtResponse_get_version_resp_tag;
                handle_get_version_request(&request.payload.get_version_req, &response.payload.get_version_resp);
            } else { payload_type_mismatch = true; }
            break;
        case embedded_wifi_mgmt_RequestType_REQUEST_TYPE_SET_PS_CONFIG:
            if (request.which_payload == embedded_wifi_mgmt_WifiMgmtRequest_set_ps_config_req_tag) {
                response.which_payload = embedded_wifi_mgmt_WifiMgmtResponse_status_resp_tag;
                handle_set_ps_config_request(&request.payload.set_ps_config_req, &response.payload.status_resp);
            } else { payload_type_mismatch = true; }
            break;
        case embedded_wifi_mgmt_RequestType_REQUEST_TYPE_GET_PS_CONFIG:
            if (request.which_payload == embedded_wifi_mgmt_WifiMgmtRequest_get_ps_config_req_tag) {
                handle_get_ps_config_request(&request.payload.get_ps_config_req,
                                             &response.payload.get_ps_config_resp,
                                             &response.payload.status_resp);
                if (response.payload.get_ps_config_resp.has_config) {
                    response.which_payload = embedded_wifi_mgmt_WifiMgmtResponse_get_ps_config_resp_tag;
                } else {
                    response.which_payload = embedded_wifi_mgmt_WifiMgmtResponse_status_resp_tag;
                }
            } else { payload_type_mismatch = true; }
            break;
        case embedded_wifi_mgmt_RequestType_REQUEST_TYPE_SET_REG_DOMAIN:
            if (request.which_payload == embedded_wifi_mgmt_WifiMgmtRequest_set_reg_domain_req_tag) {
                response.which_payload = embedded_wifi_mgmt_WifiMgmtResponse_status_resp_tag;
                handle_set_reg_domain_request(&request.payload.set_reg_domain_req, &response.payload.status_resp);
            } else { payload_type_mismatch = true; }
            break;
        case embedded_wifi_mgmt_RequestType_REQUEST_TYPE_GET_REG_DOMAIN:
            if (request.which_payload == embedded_wifi_mgmt_WifiMgmtRequest_get_reg_domain_req_tag) {
                handle_get_reg_domain_request(&request.payload.get_reg_domain_req,
                                              &response.payload.get_reg_domain_resp,
                                              &response.payload.status_resp);
                if (response.payload.get_reg_domain_resp.has_reg_domain) {
                    response.which_payload = embedded_wifi_mgmt_WifiMgmtResponse_get_reg_domain_resp_tag;
                } else {
                    response.which_payload = embedded_wifi_mgmt_WifiMgmtResponse_status_resp_tag;
                }
            } else { payload_type_mismatch = true; }
            break;
        case embedded_wifi_mgmt_RequestType_REQUEST_TYPE_GET_IFACE_STATUS:
            if (request.which_payload == embedded_wifi_mgmt_WifiMgmtRequest_get_iface_status_req_tag) {
                handle_get_iface_status_request(&request.payload.get_iface_status_req,
                                                &response.payload.get_iface_status_resp,
                                                &response.payload.status_resp);
                if (response.payload.get_iface_status_resp.has_status) {
                     response.which_payload = embedded_wifi_mgmt_WifiMgmtResponse_get_iface_status_resp_tag;
                } else {
                     response.which_payload = embedded_wifi_mgmt_WifiMgmtResponse_status_resp_tag;
                }
            } else { payload_type_mismatch = true; }
            break;
        // Add cases for other RequestTypes defined in your wifi.proto
        // Example:
        // case embedded_wifi_mgmt_RequestType_REQUEST_TYPE_SET_TWT:
        //     if (request.which_payload == embedded_wifi_mgmt_WifiMgmtRequest_set_twt_req_tag) {
        //         response.which_payload = embedded_wifi_mgmt_WifiMgmtResponse_status_resp_tag;
        //         handle_set_twt_request(&request.payload.set_twt_req, &response.payload.status_resp);
        //     } else { payload_type_mismatch = true; }
        //     break;

        case embedded_wifi_mgmt_RequestType_REQUEST_TYPE_GET_PROTOCOL_VERSION:
            response.which_payload = embedded_wifi_mgmt_WifiMgmtResponse_get_protocol_version_resp_tag;

            embedded_wifi_mgmt_ProtocolVersionResponse version_resp = embedded_wifi_mgmt_ProtocolVersionResponse_init_zero;
            version_resp.major = 1; // Set your protocol major version
            version_resp.minor = 2; // Set your protocol minor version
            response.payload.get_protocol_version_resp = version_resp;
            break;

        default:
            LOG_WRN("Unknown or unhandled RequestType enum: %d", request.request_id);
            response.which_payload = embedded_wifi_mgmt_WifiMgmtResponse_status_resp_tag;
            response.payload.status_resp.success = false;
            response.payload.status_resp.error_code = -ENOTSUP;
            set_status_error_msg_bytes(response.payload.status_resp.error_message, "Unsupported request type");
            break;
    }

    if (payload_type_mismatch) {
        LOG_ERR("Payload type mismatch for RequestType enum %u, expected matching tag, got tag %u",
                request.request_id, request.which_payload);
        response.which_payload = embedded_wifi_mgmt_WifiMgmtResponse_status_resp_tag; // Ensure status_resp is active
        response.payload.status_resp.success = false;
        response.payload.status_resp.error_code = -EBADMSG;
        set_status_error_msg_bytes(response.payload.status_resp.error_message, "Payload type mismatch for request ID");
    }

    pb_ostream_t ostream = pb_ostream_from_buffer(response_buffer, *response_buffer_len_io);
    if (pb_encode(&ostream, embedded_wifi_mgmt_WifiMgmtResponse_fields, &response)) {
        *response_buffer_len_io = ostream.bytes_written;
    } else {
        LOG_ERR("Failed to encode WifiMgmtResponse: %s", PB_GET_ERROR(&ostream));
        *response_buffer_len_io = 0;
    }
}


// --- Request Handler Implementations ---

/* Resolve the WiFi interface for a request. An ifindex of 0 means
 * "unspecified" and maps to the first registered WiFi interface, so
 * host CLIs that default to index 0 work on single-radio boards.
 * Explicit indices are looked up as-is.
 */
static struct net_if *resolve_wifi_iface(uint32_t ifindex)
{
    if (ifindex != 0) {
        return net_if_get_by_index(ifindex);
    }

    /* ifindex 0 = unspecified: use the first registered WiFi interface. */
    for (int i = 1; ; i++) {
        struct net_if *iface = net_if_get_by_index(i);

        if (iface == NULL) {
            break;
        }
        if (net_if_is_wifi(iface)) {
            return iface;
        }
    }

    return NULL;
}

static void handle_connect_request(const embedded_wifi_mgmt_WifiConnectRequest *req,
                                   embedded_wifi_mgmt_WifiStatusResponse *resp) {
    resp->success = false; // Default to failure
    resp->error_code = -EIO; // Generic I/O error
    set_status_error_msg_bytes(resp->error_message, "Connect handler init error");

    LOG_INF("Handling Connect Request for iface %u, SSID: %.*s",
            req->iface_index, (int)req->ssid.size, req->ssid.bytes);

    struct net_if *iface = resolve_wifi_iface(req->iface_index);
    if (!iface) {
        LOG_ERR("Interface with index %u not found", req->iface_index);
        resp->error_code = -ENODEV;
        set_status_error_msg_bytes(resp->error_message, "Interface not found");
        return;
    }

    struct wifi_connect_req_params params = {0};

    if (req->ssid.size == 0 || req->ssid.size > WIFI_SSID_MAX_LEN) {
        LOG_WRN("Invalid SSID length: %zu", req->ssid.size);
        resp->error_code = -EINVAL;
        set_status_error_msg_bytes(resp->error_message, "Invalid SSID length");
        return;
    }
    params.ssid = (uint8_t*)req->ssid.bytes;
    params.ssid_length = req->ssid.size;

    params.security = proto_to_zephyr_security(req->security_type);

    if (params.security == WIFI_SECURITY_TYPE_PSK ||
        params.security == WIFI_SECURITY_TYPE_WPA_PSK || // Added WPA_PSK
        params.security == WIFI_SECURITY_TYPE_PSK_SHA256) {
        if (req->psk.size == 0 || req->psk.size > MAX_PSK_LEN_CB) {
            LOG_WRN("Invalid PSK length: %zu", req->psk.size);
            resp->error_code = -EINVAL;
            set_status_error_msg_bytes(resp->error_message, "Invalid PSK length");
            return;
        }
        params.psk = (uint8_t*)req->psk.bytes;
        params.psk_length = req->psk.size;
    } else if (params.security == WIFI_SECURITY_TYPE_SAE) {
        if (req->sae_password.size == 0 || req->sae_password.size > MAX_SAE_PASSWORD_LEN_CB) {
            LOG_WRN("Invalid SAE password length: %zu", req->sae_password.size);
            resp->error_code = -EINVAL;
            set_status_error_msg_bytes(resp->error_message, "Invalid SAE password length");
            return;
        }
        params.sae_password = (uint8_t*)req->sae_password.bytes;
        params.sae_password_length = req->sae_password.size;
    }
    // Add handling for EAP fields if security_type is EAP
    if (params.security == WIFI_SECURITY_TYPE_EAP || params.security == WIFI_SECURITY_TYPE_EAP_TLS) { // Broad EAP or specific
        params.eap_identity = req->eap_identity.bytes;
        params.eap_id_length = req->eap_identity.size;
        params.eap_password = req->eap_password.bytes;
        params.eap_passwd_length = req->eap_password.size;
        // params.anon_id = req->anonymous_id.bytes; // Map anonymous_id if used
        // params.aid_length = req->anonymous_id.size;
    }


    params.channel = (req->channel == embedded_wifi_mgmt_WifiChannelConst_WIFI_CHANNEL_ANY) ?
                     WIFI_CHANNEL_ANY : (uint8_t)req->channel;

    /* nanopb uses a fixed 6-byte array here: an absent field decodes to all
     * zeros, which the stack treats as "any BSSID". */
    memcpy(params.bssid, req->bssid, WIFI_MAC_ADDR_LEN);

    /* Zephyr's wifi_connect_req_params.timeout is in seconds; the proto field
     * is in milliseconds. Round up so a sub-second request gets at least 1 s.
     */
    params.timeout = DIV_ROUND_UP(req->timeout_ms, 1000);
    params.mfp = proto_to_zephyr_mfp(req->mfp);
    params.band = proto_to_zephyr_band(req->band);
    // params.bandwidth = (enum wifi_frequency_bandwidths) req->bandwidth; // Map from WifiFrequencyBandwidth proto

    int ret = net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, &params, sizeof(params));
    if (ret) {
        LOG_ERR("NET_REQUEST_WIFI_CONNECT failed: %d", ret);
        resp->error_code = ret;
        set_status_error_msg_bytes(resp->error_message, "Wi-Fi connect request failed");
    } else {
        LOG_INF("NET_REQUEST_WIFI_CONNECT initiated");
        resp->success = true;
        resp->error_code = 0;
        set_status_error_msg_bytes(resp->error_message, "Connect initiated");
    }
}

static void handle_disconnect_request(const embedded_wifi_mgmt_WifiDisconnectRequest *req,
                                      embedded_wifi_mgmt_WifiStatusResponse *resp) {
    resp->success = false;
    resp->error_code = -EIO;
    set_status_error_msg_bytes(resp->error_message, "Disconnect handler init error");

    LOG_INF("Handling Disconnect Request for iface %u", req->iface_index);
    struct net_if *iface = resolve_wifi_iface(req->iface_index);
    if (!iface) {
        LOG_ERR("Interface with index %u not found", req->iface_index);
        resp->error_code = -ENODEV;
        set_status_error_msg_bytes(resp->error_message, "Interface not found");
        return;
    }

    int ret = net_mgmt(NET_REQUEST_WIFI_DISCONNECT, iface, NULL, 0);
    if (ret) {
        LOG_ERR("NET_REQUEST_WIFI_DISCONNECT failed: %d", ret);
        resp->error_code = ret;
        set_status_error_msg_bytes(resp->error_message, "Wi-Fi disconnect request failed");
    } else {
        LOG_INF("NET_REQUEST_WIFI_DISCONNECT initiated");
        resp->success = true;
        resp->error_code = 0;
        set_status_error_msg_bytes(resp->error_message, "Disconnect initiated");
    }
}

static void handle_scan_request(const embedded_wifi_mgmt_WifiScanRequest *req,
                                struct ScanRequestContext *scan_ctx,
                                embedded_wifi_mgmt_WifiStatusResponse *resp) {
    resp->success = false;
    resp->error_code = -EIO;
    set_status_error_msg_bytes(resp->error_message, "Scan handler init error");

    LOG_INF("Handling Scan Request for iface %u", req->iface_index);
    struct net_if *iface = resolve_wifi_iface(req->iface_index);
    if (!iface) {
        LOG_ERR("Interface with index %u not found for scan", req->iface_index);
        resp->error_code = -ENODEV;
        set_status_error_msg_bytes(resp->error_message, "Interface not found for scan");
        return;
    }

    struct wifi_scan_params params = {0}; // Zephyr's scan params struct

    if (req->has_params) {
        const embedded_wifi_mgmt_WifiScanParams *proto_params = &req->params;
        params.scan_type = proto_to_zephyr_scan_type(proto_params->scan_type);
        params.bands = WIFI_FREQ_BAND_2_4_GHZ; // Initialize bands to zero

        // TODO: Fix later
        // params.bands = (uint8_t)proto_params->band; // Assuming WifiBand maps to a bitmask or single value
        //if (proto_params->band == embedded_wifi_mgmt_WifiBand_WIFI_BAND_2_4_GHZ) params.bands |= WIFI_FREQ_BAND_2_4_GHZ;
        //if (proto_params->band == embedded_wifi_mgmt_WifiBand_WIFI_BAND_5_GHZ) params.bands |=  WIFI_FREQ_BAND_5_GHZ;
        // ... map other bands if your Zephyr version supports them with bitmasks

        params.dwell_time_active = proto_params->dwell_time_active;
        params.dwell_time_passive = proto_params->dwell_time_passive; // If distinct field in Zephyr struct

        /*
        if (proto_params->band_chan_count > 0) {
            if (proto_params->band_chan_count > MAX_SCAN_CHANNELS_CB) { // Use a define for max channels
                LOG_WRN("Too many scan channels (%u > %d)", proto_params->band_chan_count, MAX_SCAN_CHANNELS_CB);
                resp->error_code = -EINVAL;
                set_status_error_msg_bytes(resp->error_message, "Too many scan channels");
                return;
            }
            // Zephyr's wifi_scan_params might take band_chan array
            // For simplicity, if channels are provided, we might need to map them to band_chan
            // This part depends heavily on the exact Zephyr `wifi_scan_params` structure.
            // Assuming it has a simple channel list for now, or you adapt to band_chan.
            params.max_bss_cnt = proto_params->band_chan_count;
            for (uint32_t i = 0; i < proto_params->band_chan_count; i++) {
                params.band_chan[i] = (uint8_t)proto_params->band_chan[i];
            }
        } */

        if (scan_ctx->current_ssid_count > 0) {
            // Zephyr's wifi_scan_params takes `const char **ssids`
            // We need to adapt `scan_ctx->ssids` (array of structs) to `const char* []`
            // This is complex here. A simpler Zephyr API might take one SSID or a simpler struct.
            // For now, this part is illustrative and needs careful mapping to your Zephyr's API.
            // params.ssids = (const char **) ... // Requires careful setup
            // params.ssid_count = scan_ctx->current_ssid_count;
            LOG_WRN("Directed scan SSIDs from callback not fully implemented in handler yet.");
        }
         params.max_bss_cnt = WIFI_MGMT_SCAN_MAX_BSS_CNT; // Or from proto_params if available
    }

    int ret = net_mgmt(NET_REQUEST_WIFI_SCAN, iface, req->has_params ? &params : NULL, req->has_params ? sizeof(params) : 0);
    if (ret) {
        LOG_ERR("NET_REQUEST_WIFI_SCAN failed: %d", ret);
        resp->error_code = ret;
        set_status_error_msg_bytes(resp->error_message, "Wi-Fi scan request failed");
    } else {
        LOG_INF("NET_REQUEST_WIFI_SCAN initiated");
        resp->success = true;
        resp->error_code = 0;
        set_status_error_msg_bytes(resp->error_message, "Scan initiated");
    }
}


/* AP enable parameters must outlive the net_mgmt() call: the request is
 * handled asynchronously on the network thread, so ssid/psk pointers into
 * the stack-decoded protobuf would dangle. Only one AP enable can be in
 * flight at a time, so a single static copy is enough. */
static struct wifi_connect_req_params ap_enable_params;
static uint8_t ap_enable_ssid[WIFI_SSID_MAX_LEN];
static uint8_t ap_enable_psk[MAX_AP_PSK_LEN];

static void handle_ap_enable_request(const embedded_wifi_mgmt_WifiApEnableRequest *req,
                                     embedded_wifi_mgmt_WifiStatusResponse *resp) {
    resp->success = false;
    resp->error_code = -EIO;
    set_status_error_msg_bytes(resp->error_message, "AP enable handler init error");

    LOG_INF("Handling AP Enable Request for iface %u", req->iface_index);
    struct net_if *iface = resolve_wifi_iface(req->iface_index);
    if (!iface) {
        LOG_ERR("Interface with index %u not found for AP enable", req->iface_index);
        resp->error_code = -ENODEV;
        set_status_error_msg_bytes(resp->error_message, "Interface not found for AP enable");
        return;
    }

    if (!req->has_params) {
        LOG_ERR("AP Enable parameters missing");
        resp->error_code = -EINVAL;
        set_status_error_msg_bytes(resp->error_message, "AP parameters missing");
        return;
    }
    const embedded_wifi_mgmt_WifiApEnableParams *proto_params = &req->params;

    if (proto_params->ssid.size == 0 || proto_params->ssid.size > WIFI_SSID_MAX_LEN) {
        LOG_ERR("Invalid AP SSID length: %zu", proto_params->ssid.size);
        resp->error_code = -EINVAL;
        set_status_error_msg_bytes(resp->error_message, "Invalid AP SSID length");
        return;
    }

    /* Zephyr's wifi_connect_req_params in this release has no mapping for a
     * hidden SSID, beacon interval, or DTIM period; the driver applies its
     * defaults. Warn instead of silently dropping host-set values. */
    if (proto_params->hidden_ssid || proto_params->beacon_interval_tu != 0 ||
        proto_params->dtim_period != 0) {
        LOG_WRN("AP hidden_ssid/beacon_interval_tu/dtim_period not supported by the "
                "driver API; driver defaults will be used");
    }

    enum wifi_security_type security =
        proto_to_zephyr_security(proto_params->security_type);
    if (security != WIFI_SECURITY_TYPE_NONE &&
        security != WIFI_SECURITY_TYPE_PSK &&
        security != WIFI_SECURITY_TYPE_SAE &&
        security != WIFI_SECURITY_TYPE_WEP) {
        LOG_ERR("Unsupported AP security type: %d", security);
        resp->error_code = -ENOTSUP;
        set_status_error_msg_bytes(resp->error_message, "AP security type not supported");
        return;
    }

    if (proto_params->psk.size > MAX_AP_PSK_LEN) {
        LOG_ERR("AP PSK too long (%zu > %d)", proto_params->psk.size, MAX_AP_PSK_LEN);
        resp->error_code = -EINVAL;
        set_status_error_msg_bytes(resp->error_message, "AP PSK too long");
        return;
    }
    if ((security == WIFI_SECURITY_TYPE_PSK || security == WIFI_SECURITY_TYPE_SAE) &&
        proto_params->psk.size == 0) {
        LOG_ERR("AP PSK missing for PSK/SAE security");
        resp->error_code = -EINVAL;
        set_status_error_msg_bytes(resp->error_message, "AP PSK missing");
        return;
    }

    if (proto_params->channel != embedded_wifi_mgmt_WifiChannelConst_WIFI_CHANNEL_ANY &&
        proto_params->channel > 233) {
        LOG_ERR("Invalid AP channel: %u", proto_params->channel);
        resp->error_code = -EINVAL;
        set_status_error_msg_bytes(resp->error_message, "Invalid AP channel");
        return;
    }

    memcpy(ap_enable_ssid, proto_params->ssid.bytes, proto_params->ssid.size);
    if (proto_params->psk.size > 0) {
        memcpy(ap_enable_psk, proto_params->psk.bytes, proto_params->psk.size);
    }

    ap_enable_params = (struct wifi_connect_req_params){0};
    ap_enable_params.ssid = ap_enable_ssid;
    ap_enable_params.ssid_length = proto_params->ssid.size;
    ap_enable_params.band = proto_to_zephyr_band(proto_params->band);
    ap_enable_params.channel =
        (proto_params->channel == embedded_wifi_mgmt_WifiChannelConst_WIFI_CHANNEL_ANY)
            ? WIFI_CHANNEL_ANY
            : (uint8_t)proto_params->channel;
    ap_enable_params.security = security;
    if (proto_params->psk.size > 0) {
        ap_enable_params.psk = ap_enable_psk;
        ap_enable_params.psk_length = proto_params->psk.size;
    }

    int ret = net_mgmt(NET_REQUEST_WIFI_AP_ENABLE, iface, &ap_enable_params,
                       sizeof(ap_enable_params));
    if (ret) {
        LOG_ERR("NET_REQUEST_WIFI_AP_ENABLE failed: %d", ret);
        resp->error_code = ret;
        set_status_error_msg_bytes(resp->error_message, "Wi-Fi AP enable request failed");
    } else {
        LOG_INF("NET_REQUEST_WIFI_AP_ENABLE initiated");
        resp->success = true;
        resp->error_code = 0;
        set_status_error_msg_bytes(resp->error_message, "AP enable initiated");
    }
}

static void handle_ap_disable_request(const embedded_wifi_mgmt_WifiApDisableRequest *req,
                                      embedded_wifi_mgmt_WifiStatusResponse *resp) {
    resp->success = false;
    resp->error_code = -EIO;
    set_status_error_msg_bytes(resp->error_message, "AP disable handler init error");

    LOG_INF("Handling AP Disable Request for iface %u", req->iface_index);
    struct net_if *iface = resolve_wifi_iface(req->iface_index);
    if (!iface) {
        LOG_ERR("Interface with index %u not found for AP disable", req->iface_index);
        resp->error_code = -ENODEV;
        set_status_error_msg_bytes(resp->error_message, "Interface not found for AP disable");
        return;
    }

    int ret = net_mgmt(NET_REQUEST_WIFI_AP_DISABLE, iface, NULL, 0);
    if (ret) {
        LOG_ERR("NET_REQUEST_WIFI_AP_DISABLE failed: %d", ret);
        resp->error_code = ret;
        set_status_error_msg_bytes(resp->error_message, "Wi-Fi AP disable request failed");
    } else {
        LOG_INF("NET_REQUEST_WIFI_AP_DISABLE initiated");
        resp->success = true;
        resp->error_code = 0;
        set_status_error_msg_bytes(resp->error_message, "AP disable initiated");
    }
}

static void handle_get_version_request(const embedded_wifi_mgmt_GetWifiVersionRequest *req,
                                       embedded_wifi_mgmt_GetWifiVersionResponse *resp) {
    LOG_INF("Handling Get Version Request");
    // Zephyr's wifi_mgmt.h has NET_REQUEST_WIFI_VERSION and struct wifi_version
    struct wifi_version ver = {0};
    struct net_if *iface = net_if_get_default(); // Version is usually not iface specific, but API might take one

    resp->has_version = false; // Default

    if (!iface) {
        LOG_WRN("No default interface for GetVersion, trying with NULL iface");
        // Some drivers might allow NULL iface for global version
    }

    int ret = net_mgmt(NET_REQUEST_WIFI_VERSION, iface, &ver, sizeof(ver));
    if (ret == 0) {
        resp->has_version = true;
        // Assuming wifi_version struct has const char* drv_version and const char* fw_version
        // And proto WifiVersion has driver_major, minor, patch, and firmware_version (pb_bytes_array_t)

        // Placeholder for parsing drv_version string into major/minor/patch
        // This is highly dependent on the format of ver.drv_version
        // Example: sscanf(ver.drv_version, "v%u.%u.%u", &major, &minor, &patch);
        resp->version.driver_major = 0; // Placeholder
        resp->version.driver_minor = 0; // Placeholder
        resp->version.driver_patch = 0; // Placeholder
        LOG_INF("Driver Version String: %s", ver.drv_version ? ver.drv_version : "N/A");


        if (ver.fw_version) {
            set_status_error_msg_bytes(resp->version.firmware_version, ver.fw_version);
            LOG_INF("Firmware Version String: %s", ver.fw_version);
        } else {
            set_status_error_msg_bytes(resp->version.firmware_version, "N/A");
        }
    } else {
        LOG_ERR("NET_REQUEST_WIFI_VERSION failed: %d", ret);
        // For GET requests that fail to retrieve data, we typically don't send a separate status_resp.
        // The absence of `has_version` (or it being false) indicates failure.
        // However, the main switch case expects a payload. If GetVersionResponse is the only one,
        // it will be sent with has_version=false.
        // Alternatively, the main switch could be modified to send a status_resp on error for GETs.
        // For now, just log and send GetVersionResponse with has_version=false.
        set_status_error_msg_bytes(resp->version.firmware_version, "Failed to get version");
    }
}


static void handle_set_ps_config_request(const embedded_wifi_mgmt_SetPowerSaveConfigRequest *req,
                                         embedded_wifi_mgmt_WifiStatusResponse *resp) {
    resp->success = false;
    resp->error_code = -EIO;
    set_status_error_msg_bytes(resp->error_message, "Set PS config handler init error");

    LOG_INF("Handling Set Power Save Config Request for iface %u", req->iface_index);
    struct net_if *iface = resolve_wifi_iface(req->iface_index);
    if (!iface) {
        LOG_ERR("Interface with index %u not found for PS config", req->iface_index);
        resp->error_code = -ENODEV;
        set_status_error_msg_bytes(resp->error_message, "Interface not found for PS config");
        return;
    }

    if (!req->has_config || !req->config.has_ps_settings) {
        LOG_ERR("Set PS Config: config data missing");
        resp->error_code = -EINVAL;
        set_status_error_msg_bytes(resp->error_message, "PS config data missing");
        return;
    }

    const embedded_wifi_mgmt_WifiPsSettings *s = &req->config.ps_settings;
    struct wifi_ps_params params = {0};
    int ret;

    /* The Wi-Fi driver applies exactly one parameter per net_mgmt call,
     * selected by wifi_ps_params.type, so each requested parameter needs
     * its own NET_REQUEST_WIFI_PS call. Zero listen interval / timeout
     * values mean "not provided" (proto3 scalars have no presence).
     */

    /* Power save state (on/off). */
    params.type = WIFI_PS_PARAM_STATE;
    params.enabled =
        (s->enabled == embedded_wifi_mgmt_WifiPsState_WIFI_PS_STATE_ENABLE)
            ? WIFI_PS_ENABLED
            : WIFI_PS_DISABLED;
    ret = net_mgmt(NET_REQUEST_WIFI_PS, iface, &params, sizeof(params));
    if (ret) {
        LOG_ERR("Set PS state failed: %d", ret);
        resp->error_code = ret;
        set_status_error_msg_bytes(resp->error_message, "Set PS state failed");
        return;
    }

    if (s->listen_interval_beacons > 0) {
        params.type = WIFI_PS_PARAM_LISTEN_INTERVAL;
        params.listen_interval = (unsigned short)s->listen_interval_beacons;
        ret = net_mgmt(NET_REQUEST_WIFI_PS, iface, &params, sizeof(params));
        if (ret) {
            LOG_ERR("Set PS listen interval failed: %d", ret);
            resp->error_code = ret;
            set_status_error_msg_bytes(resp->error_message, "Set PS listen interval failed");
            return;
        }
    }

    if (s->timeout_ms > 0) {
        params.type = WIFI_PS_PARAM_TIMEOUT;
        params.timeout_ms = s->timeout_ms;
        ret = net_mgmt(NET_REQUEST_WIFI_PS, iface, &params, sizeof(params));
        if (ret) {
            LOG_ERR("Set PS timeout failed: %d", ret);
            resp->error_code = ret;
            set_status_error_msg_bytes(resp->error_message, "Set PS timeout failed");
            return;
        }
    }

    /* Power save mode: legacy (PSPOLL) vs WMM (U-APSD). */
    params.type = WIFI_PS_PARAM_MODE;
    params.mode = (req->config.type == embedded_wifi_mgmt_WifiPowerSaveType_WIFI_PS_TYPE_WMM)
                      ? WIFI_PS_MODE_WMM
                      : WIFI_PS_MODE_LEGACY;
    ret = net_mgmt(NET_REQUEST_WIFI_PS, iface, &params, sizeof(params));
    if (ret) {
        LOG_ERR("Set PS mode failed: %d", ret);
        resp->error_code = ret;
        set_status_error_msg_bytes(resp->error_message, "Set PS mode failed");
        return;
    }

    LOG_INF("NET_REQUEST_WIFI_PS (SET) success: enabled=%d li=%lu tmo=%lu mode=%d",
            (int)params.enabled, (unsigned long)s->listen_interval_beacons,
            (unsigned long)s->timeout_ms, (int)params.mode);
    resp->success = true;
    resp->error_code = 0;
    set_status_error_msg_bytes(resp->error_message, "Set PS config success");
}

static void handle_get_ps_config_request(const embedded_wifi_mgmt_GetPowerSaveConfigRequest *req,
                                         embedded_wifi_mgmt_GetPowerSaveConfigResponse *data_resp,
                                         embedded_wifi_mgmt_WifiStatusResponse *err_resp) {
    data_resp->has_config = false;
    /* NOTE: status_resp (err_resp) is a union member aliasing data_resp.
     * Do not pre-write the status here — it would clobber the data region
     * before it is filled. Failure paths set the status explicitly; on
     * success the payload is selected via which_payload in the caller. */

    LOG_INF("Handling Get Power Save Config Request for iface %u", req->iface_index);
    struct net_if *iface = resolve_wifi_iface(req->iface_index);
    if (!iface) {
        LOG_ERR("Interface with index %u not found for PS config", req->iface_index);
        err_resp->success = false;
        err_resp->error_code = -ENODEV;
        set_status_error_msg_bytes(err_resp->error_message, "Interface not found for PS config");
        return;
    }

    struct wifi_ps_config params_retrieved = {0};
    int ret = net_mgmt(NET_REQUEST_WIFI_PS_CONFIG, iface, &params_retrieved, sizeof(params_retrieved));

    if (ret) {
        LOG_ERR("NET_REQUEST_WIFI_PS_CONFIG (GET) failed: %d", ret);
        err_resp->success = false;
        err_resp->error_code = ret;
        set_status_error_msg_bytes(err_resp->error_message, "Get PS config failed");
        return;
    }

    const struct wifi_ps_params *p = &params_retrieved.ps_params;
    LOG_INF("PS config: enabled=%d li=%u tmo=%u mode=%d wakeup=%d exit=%d",
            (int)p->enabled, p->listen_interval, p->timeout_ms, (int)p->mode,
            (int)p->wakeup_mode, (int)p->exit_strategy);

    data_resp->has_config = true;
    embedded_wifi_mgmt_WifiPowerSaveConfig *cfg = &data_resp->config;
    cfg->has_ps_settings = true;
    cfg->ps_settings.enabled =
        (p->enabled == WIFI_PS_ENABLED)
            ? embedded_wifi_mgmt_WifiPsState_WIFI_PS_STATE_ENABLE
            : embedded_wifi_mgmt_WifiPsState_WIFI_PS_STATE_DISABLE;
    cfg->ps_settings.listen_interval_beacons = p->listen_interval;
    cfg->ps_settings.wakeup_mode = (embedded_wifi_mgmt_WifiPsWakeupMode)p->wakeup_mode;
    cfg->ps_settings.timeout_ms = p->timeout_ms;
    cfg->ps_settings.exit_strategy = (embedded_wifi_mgmt_WifiPsExitStrategy)p->exit_strategy;
    cfg->type = (p->mode == WIFI_PS_MODE_WMM)
                    ? embedded_wifi_mgmt_WifiPowerSaveType_WIFI_PS_TYPE_WMM
                    : embedded_wifi_mgmt_WifiPowerSaveType_WIFI_PS_TYPE_MODEM;

    /* Do NOT write err_resp on success: status_resp is a union member
     * aliasing data_resp (error_code overlaps config's first bytes and
     * error_message overlaps ps_settings), so writing it here would
     * corrupt the payload about to be encoded. Success is signalled by
     * which_payload selection in the caller. */
}

static void handle_set_reg_domain_request(const embedded_wifi_mgmt_SetRegulatoryDomainRequest *req,
                                          embedded_wifi_mgmt_WifiStatusResponse *resp) {
    resp->success = false;
    resp->error_code = -EIO;
    set_status_error_msg_bytes(resp->error_message, "Set RegDomain handler init error");

    LOG_INF("Handling Set Regulatory Domain Request for iface %u", req->iface_index);

    struct net_if *iface = resolve_wifi_iface(req->iface_index);
    if (!iface) {
        LOG_ERR("Interface with index %u not found for RegDomain", req->iface_index);
        resp->error_code = -ENODEV;
        set_status_error_msg_bytes(resp->error_message, "Interface not found for RegDomain");
        return;
    }

    if (!req->has_reg_domain) {
        LOG_ERR("Set RegDomain: reg_domain data missing");
        resp->error_code = -EINVAL;
        set_status_error_msg_bytes(resp->error_message, "RegDomain data missing");
        return;
    }

    const char *cc = req->reg_domain.country_code;
    /* ISO 3166-1 alpha-2: exactly two letters. Reject malformed codes fast
     * instead of paying the driver's ~10 s firmware timeout for them. */
    if (cc[0] == '\0' || cc[1] == '\0' ||
        !((cc[0] >= 'a' && cc[0] <= 'z') || (cc[0] >= 'A' && cc[0] <= 'Z')) ||
        !((cc[1] >= 'a' && cc[1] <= 'z') || (cc[1] >= 'A' && cc[1] <= 'Z'))) {
        LOG_ERR("Set RegDomain: invalid country code '%c%c'", cc[0], cc[1]);
        resp->error_code = -EINVAL;
        set_status_error_msg_bytes(resp->error_message, "Invalid country code");
        return;
    }

    char want[2] = { cc[0] >= 'a' && cc[0] <= 'z' ? (char)(cc[0] - 'a' + 'A') : cc[0],
                     cc[1] >= 'a' && cc[1] <= 'z' ? (char)(cc[1] - 'a' + 'A') : cc[1] };

    struct wifi_reg_domain reg_domain_param = {0}; // Zephyr's struct
    reg_domain_param.oper = WIFI_MGMT_SET; // Explicitly set operation for Zephyr API
    reg_domain_param.force = req->reg_domain.force_update;
    reg_domain_param.country_code[0] = want[0];
    reg_domain_param.country_code[1] = want[1];

    int ret = net_mgmt(NET_REQUEST_WIFI_REG_DOMAIN, iface, &reg_domain_param, sizeof(reg_domain_param));
    if (ret) {
        LOG_ERR("NET_REQUEST_WIFI_REG_DOMAIN (SET) failed: %d", ret);
        resp->error_code = ret;
        set_status_error_msg_bytes(resp->error_message, "Set RegDomain failed");
        return;
    }

    /* The driver returns 0 even when the firmware rejects a well-formed but
     * unknown country code, so confirm the new domain with a follow-up GET. */
    static struct wifi_reg_chan_info verify_chan_buf[MAX_REG_CHAN_NUM];
    struct wifi_reg_domain reg_domain_retrieved = {0};
    reg_domain_retrieved.oper = WIFI_MGMT_GET; // Explicitly set operation
    reg_domain_retrieved.num_channels = MAX_REG_CHAN_NUM;
    reg_domain_retrieved.chan_info = verify_chan_buf;
    ret = net_mgmt(NET_REQUEST_WIFI_REG_DOMAIN, iface, &reg_domain_retrieved, sizeof(reg_domain_retrieved));
    if (ret) {
        LOG_ERR("RegDomain verify GET failed: %d", ret);
        resp->error_code = ret;
        set_status_error_msg_bytes(resp->error_message, "Set RegDomain verify failed");
        return;
    }

    if (reg_domain_retrieved.country_code[0] != want[0] ||
        reg_domain_retrieved.country_code[1] != want[1]) {
        LOG_ERR("Set RegDomain not confirmed: requested %c%c, driver reports %c%c",
                want[0], want[1],
                reg_domain_retrieved.country_code[0], reg_domain_retrieved.country_code[1]);
        resp->error_code = -EIO;
        set_status_error_msg_bytes(resp->error_message, "Set RegDomain not confirmed by driver");
        return;
    }

    LOG_INF("NET_REQUEST_WIFI_REG_DOMAIN (SET) success: %c%c confirmed", want[0], want[1]);
    resp->success = true;
    resp->error_code = 0;
    set_status_error_msg_bytes(resp->error_message, "Set RegDomain success");
}

static void handle_get_reg_domain_request(const embedded_wifi_mgmt_GetRegulatoryDomainRequest *req,
                                          embedded_wifi_mgmt_GetRegulatoryDomainResponse *data_resp,
                                          embedded_wifi_mgmt_WifiStatusResponse *err_resp) {
    data_resp->has_reg_domain = false;
    /* NOTE: status_resp (err_resp) is a union member aliasing data_resp.
     * Do not pre-write the status here — it would clobber the data region
     * before it is filled. Failure paths set the status explicitly; on
     * success the payload is selected via which_payload in the caller. */

    LOG_INF("Handling Get Regulatory Domain Request for iface %u", req->iface_index);
    struct net_if *iface = resolve_wifi_iface(req->iface_index);
    if (!iface) {
        LOG_ERR("Interface with index %u not found for RegDomain", req->iface_index);
        err_resp->success = false;
        err_resp->error_code = -ENODEV;
        set_status_error_msg_bytes(err_resp->error_message, "Interface not found for RegDomain");
        return;
    }

    struct wifi_reg_domain reg_domain_retrieved = {0}; // Zephyr's struct
    reg_domain_retrieved.oper = WIFI_MGMT_GET; // Explicitly set operation

    /* The driver requires a non-NULL chan_info buffer and copies up to
     * MAX_REG_CHAN_NUM channels into it. */
    static struct wifi_reg_chan_info chan_buf[MAX_REG_CHAN_NUM];
    reg_domain_retrieved.num_channels = MAX_REG_CHAN_NUM;
    reg_domain_retrieved.chan_info = chan_buf;

    int ret = net_mgmt(NET_REQUEST_WIFI_REG_DOMAIN, iface, &reg_domain_retrieved, sizeof(reg_domain_retrieved));

    if (ret) {
        LOG_ERR("NET_REQUEST_WIFI_REG_DOMAIN (GET) failed: %d", ret);
        err_resp->success = false;
        err_resp->error_code = ret;
        set_status_error_msg_bytes(err_resp->error_message, "Get RegDomain failed");
        return;
    }

    LOG_INF("NET_REQUEST_WIFI_REG_DOMAIN (GET) success. Country: %c%c, %u channels",
            reg_domain_retrieved.country_code[0], reg_domain_retrieved.country_code[1],
            reg_domain_retrieved.num_channels);
    data_resp->has_reg_domain = true;
    embedded_wifi_mgmt_WifiRegDomain *proto_rd = &data_resp->reg_domain;

    /* country_code shares union bytes with status_resp.error_code, so the
     * initial status write may have left garbage in the 3rd byte. Write all
     * 3 bytes explicitly and null-terminate (nanopb STRING needs it). */
    proto_rd->country_code[0] = reg_domain_retrieved.country_code[0];
    proto_rd->country_code[1] = reg_domain_retrieved.country_code[1];
    proto_rd->country_code[2] = '\0';
    proto_rd->force_update = reg_domain_retrieved.force; // Map back if relevant for GET

    size_t chan_count = reg_domain_retrieved.num_channels;
    if (chan_count > (size_t)ARRAY_SIZE(proto_rd->channel_info)) {
        chan_count = ARRAY_SIZE(proto_rd->channel_info);
    }
    proto_rd->channel_info_count = (pb_size_t)chan_count;
    for (size_t i = 0; i < chan_count; i++) {
        proto_rd->channel_info[i].center_frequency_mhz = chan_buf[i].center_frequency;
        proto_rd->channel_info[i].max_power_dbm = chan_buf[i].max_power;
        proto_rd->channel_info[i].supported = chan_buf[i].supported;
        proto_rd->channel_info[i].passive_only = chan_buf[i].passive_only;
        proto_rd->channel_info[i].dfs = chan_buf[i].dfs;
    }

    /* Do NOT write err_resp on success: status_resp is a union member
     * aliasing data_resp (error_message overlaps channel_info_count),
     * so writing it here would corrupt the payload about to be encoded.
     * Success is signalled by which_payload selection in the caller. */
}

static void handle_get_iface_status_request(const embedded_wifi_mgmt_GetInterfaceStatusRequest *req,
                                            embedded_wifi_mgmt_GetInterfaceStatusResponse *data_resp,
                                            embedded_wifi_mgmt_WifiStatusResponse *err_resp) {
    data_resp->has_status = false;
    /* NOTE: status_resp (err_resp) is a union member aliasing data_resp.
     * Do not pre-write the status here — it would clobber the data region
     * before it is filled. Failure paths set the status explicitly; on
     * success the payload is selected via which_payload in the caller. */

    LOG_INF("Handling Get Interface Status Request for iface %u", req->iface_index);
    struct net_if *iface = resolve_wifi_iface(req->iface_index);
    if (!iface) {
        LOG_ERR("Interface with index %u not found for status", req->iface_index);
        err_resp->success = false;
        err_resp->error_code = -ENODEV;
        set_status_error_msg_bytes(err_resp->error_message, "Interface not found for status");
        return;
    }

    struct wifi_iface_status status_retrieved = {0}; // Zephyr's struct
    int ret = net_mgmt(NET_REQUEST_WIFI_IFACE_STATUS, iface, &status_retrieved, sizeof(status_retrieved));

    if (ret) {
        LOG_ERR("NET_REQUEST_WIFI_IFACE_STATUS failed: %d", ret);
        err_resp->success = false;
        err_resp->error_code = ret;
        set_status_error_msg_bytes(err_resp->error_message, "Get IfaceStatus failed");
    } else {
        LOG_INF("NET_REQUEST_WIFI_IFACE_STATUS success. State: %d, SSID: %.*s",
                status_retrieved.state, status_retrieved.ssid_len, status_retrieved.ssid);
        data_resp->has_status = true;
        embedded_wifi_mgmt_WifiInterfaceStatus *proto_s = &data_resp->status;

        proto_s->state = zephyr_to_proto_state((enum wifi_iface_state)status_retrieved.state);
        proto_s->mode = zephyr_to_proto_mode(status_retrieved.iface_mode);

        if (status_retrieved.ssid_len > 0 && status_retrieved.ssid_len <= sizeof(proto_s->ssid.bytes)) {
            memcpy(proto_s->ssid.bytes, status_retrieved.ssid, status_retrieved.ssid_len);
            proto_s->ssid.size = status_retrieved.ssid_len;
        } else { proto_s->ssid.size = 0; }

        /* BSSID is meaningful once associated (a fully connected STA
         * reports WIFI_STATE_COMPLETED) or in AP mode. */
        if ((status_retrieved.state == WIFI_STATE_ASSOCIATED ||
             status_retrieved.state == WIFI_STATE_COMPLETED) ||
            status_retrieved.iface_mode == WIFI_MODE_AP) {
            memcpy(proto_s->bssid, status_retrieved.bssid, WIFI_MAC_ADDR_LEN);
        }

        proto_s->band = zephyr_to_proto_band(status_retrieved.band);
        proto_s->channel = status_retrieved.channel;
        proto_s->security = zephyr_to_proto_security(status_retrieved.security);
        proto_s->mfp = zephyr_to_proto_mfp(status_retrieved.mfp);
        proto_s->rssi = status_retrieved.rssi;
        proto_s->link_mode = zephyr_to_proto_link_mode(status_retrieved.link_mode);
        // proto_s->ht_mcs = status_retrieved.ht_mcs; // If available in Zephyr struct and proto
        // proto_s->vht_mcs = status_retrieved.vht_mcs; // If available
        // proto_s->he_mcs = status_retrieved.he_mcs; // If available
        /* Zephyr reports current_phy_tx_rate in Mbps (float); the proto field
         * is in kbps, so scale by 1000. */
        proto_s->tx_bitrate_kbps = (uint32_t)(status_retrieved.current_phy_tx_rate * 1000.0f);
        // proto_s->rx_bitrate_kbps = status_retrieved.rx_bitrate; // If available
        proto_s->dtim_period = status_retrieved.dtim_period;
        proto_s->beacon_interval = status_retrieved.beacon_interval;
        proto_s->twt_capable = status_retrieved.twt_capable;

        /* Do NOT write err_resp on success: status_resp is a union
         * member aliasing data_resp (error_code overlaps status.state,
         * success overlaps has_status), so writing it here would corrupt
         * the payload about to be encoded. Success is signalled by
         * which_payload selection in the caller. */
    }
}

int init_proxy(void) {
    LOG_INF("Initializing Wi-Fi Management Proxy");



    LOG_INF("Wi-Fi Management Proxy initialized successfully");
    return 0;
}

SYS_INIT(init_proxy, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);