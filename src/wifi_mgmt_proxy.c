#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/sys/byteorder.h> // For potential byte order conversions if needed
//#include <zephyr/sys/ring_buffer.h> // For ring buffer support
#include <errno.h> // For error codes like EBADMSG, ENODEV, EINVAL, ENOTSUP

#include <pb_decode.h>
#include <pb_encode.h>
#include "generated/wifi.pb.h" // Path to your generated nanopb header

LOG_MODULE_REGISTER(wifi_mgmt_proxy, LOG_LEVEL_INF);

#define WIFI_MGMT_THREAD_STACK_SIZE 2048
#define WIFI_MGMT_THREAD_PRIORITY 7


// --- Nanopb Max Size Defines (ensure these match your .options file or Zephyr limits) ---
// These are illustrative; actual limits come from Zephyr's wifi.h or your .options file.
#define MAX_SSID_LEN_CB WIFI_SSID_MAX_LEN
#define MAX_PSK_LEN_CB 64 // Example for WPA/WPA2 PSK
#define MAX_SAE_PASSWORD_LEN_CB 128 // Example for SAE password
#define MAX_COUNTRY_CODE_LEN_CB 2
#define MAX_DIRECTED_SCAN_SSIDS_CB 4
#define MAX_SCAN_CHANNELS_CB 11 // Example, from WIFI_SCAN_MAX_CHANNELS
#define MAX_ERROR_MSG_LEN 63 // For a 64-byte buffer including null terminator if treated as C string
#define MAX_FIRMWARE_VERSION_LEN 31 // For a 32-byte buffer
#define MAX_AP_PSK_LEN 64

#define DATA_COMPLETE_EVENT BIT(0) // Event bit for data completion
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
            // Handle the complete message received
            LOG_INF("Data complete event received, length: %zu", message_length);

            size_t response_length = 0; // Length of the response to be filled by processing function

            process_wifi_mgmt_request(wifi_mgmt_rx_buffer, message_length, wifi_mgmt_tx_buffer, &response_length);

            // Send the response out.
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

struct ApEnableRequestContext {
    uint8_t psk[MAX_AP_PSK_LEN + 1]; // +1 for null terminator if used as string
    size_t psk_len;
    // Add other fields if AP enable params have more callback-decoded parts
};

struct RegDomainRequestContext {
    char country_code[MAX_COUNTRY_CODE_LEN_CB + 1]; // +1 for null terminator
    size_t country_code_len;
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

static bool decode_ap_psk_cb(pb_istream_t *stream, const pb_field_t *field, void **arg) {
    struct ApEnableRequestContext *ctx = (struct ApEnableRequestContext*)*arg;
    if (stream->bytes_left > MAX_AP_PSK_LEN) {
        LOG_ERR("AP PSK too long (max %d, got %zu)", MAX_AP_PSK_LEN, stream->bytes_left);
        return false;
    }
    ctx->psk_len = stream->bytes_left;
    if (!pb_read(stream, ctx->psk, ctx->psk_len)) return false;
    ctx->psk[ctx->psk_len] = '\0';
    return true;
}

static bool decode_country_code_cb(pb_istream_t *stream, const pb_field_t *field, void **arg) {
    struct RegDomainRequestContext *ctx = (struct RegDomainRequestContext*)*arg;
    if (stream->bytes_left > MAX_COUNTRY_CODE_LEN_CB) {
         LOG_ERR("Country code too long (max %d, got %zu)", MAX_COUNTRY_CODE_LEN_CB, stream->bytes_left);
         return false;
    }
    ctx->country_code_len = stream->bytes_left;
    if (!pb_read(stream, (pb_byte_t*)ctx->country_code, ctx->country_code_len)) return false;
    ctx->country_code[ctx->country_code_len] = '\0';
    return true;
}


// --- Forward declarations of request handlers ---
static void handle_connect_request(const embedded_wifi_mgmt_WifiConnectRequest *req, embedded_wifi_mgmt_WifiStatusResponse *resp);
static void handle_disconnect_request(const embedded_wifi_mgmt_WifiDisconnectRequest *req, embedded_wifi_mgmt_WifiStatusResponse *resp);
static void handle_scan_request(const embedded_wifi_mgmt_WifiScanRequest *req, struct ScanRequestContext *scan_ctx, embedded_wifi_mgmt_WifiStatusResponse *resp);
static void handle_ap_enable_request(const embedded_wifi_mgmt_WifiApEnableRequest *req, struct ApEnableRequestContext *ap_ctx, embedded_wifi_mgmt_WifiStatusResponse *resp);
static void handle_ap_disable_request(const embedded_wifi_mgmt_WifiApDisableRequest *req, embedded_wifi_mgmt_WifiStatusResponse *resp);
static void handle_get_version_request(const embedded_wifi_mgmt_GetWifiVersionRequest *req, embedded_wifi_mgmt_GetWifiVersionResponse *resp);
static void handle_set_ps_config_request(const embedded_wifi_mgmt_SetPowerSaveConfigRequest *req, embedded_wifi_mgmt_WifiStatusResponse *resp);
static void handle_get_ps_config_request(const embedded_wifi_mgmt_GetPowerSaveConfigRequest *req, embedded_wifi_mgmt_WifiStatusResponse *err_resp);
static void handle_set_reg_domain_request(const embedded_wifi_mgmt_SetRegulatoryDomainRequest *req, struct RegDomainRequestContext *reg_ctx, embedded_wifi_mgmt_WifiStatusResponse *resp);
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
    struct ApEnableRequestContext ap_ctx = {0};
    struct RegDomainRequestContext reg_ctx = {0};

    // --- Setup decode callbacks if specific fields in the request need them ---
    // This depends on your .options file. If a field like `scan_req.params.directed_scan_ssids`
    // is a repeated message and uses a callback for decoding.
    // Example:
    // if (request.which_payload == embedded_wifi_mgmt_WifiMgmtRequest_scan_req_tag) {
    //    request.payload.scan_req.params.directed_scan_ssids.funcs.decode = &decode_directed_scan_ssid_cb;
    //    request.payload.scan_req.params.directed_scan_ssids.arg = &scan_ctx;
    // }
    // Similar for ap_enable_req.params.psk if it's bytes with callback
    // request.payload.ap_enable_req.params.psk.funcs.decode = &decode_ap_psk_cb;
    // request.payload.ap_enable_req.params.psk.arg = &ap_ctx;
    // And for set_reg_domain_req.reg_domain.country_code if it's string with callback
    // request.payload.set_reg_domain_req.reg_domain.country_code.funcs.decode = &decode_country_code_cb;
    // request.payload.set_reg_domain_req.reg_domain.country_code.arg = &reg_ctx;
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
                // If ap_enable_req.params.psk uses a callback, ap_ctx is populated
                response.which_payload = embedded_wifi_mgmt_WifiMgmtResponse_status_resp_tag;
                handle_ap_enable_request(&request.payload.ap_enable_req, &ap_ctx, &response.payload.status_resp);
            } else { payload_type_mismatch = true; }
            break;
        case embedded_wifi_mgmt_RequestType_REQUEST_TYPE_AP_DISABLE:
            if (request.which_payload == embedded_wifi_mgmt_WifiMgmtRequest_ap_disable_req_tag) {
                response.which_payload = embedded_wifi_mgmt_WifiMgmtResponse_status_resp_tag;
                handle_ap_disable_request(&request.payload.ap_disable_req, &response.payload.status_resp);
            } else { payload_type_mismatch = true; }
            break;
        case embedded_wifi_mgmt_RequestType_REQUEST_TYPE_GET_VERSION:
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
                                             //&response.payload.get_ps_config_resp,
                                             &response.payload.status_resp); // Pass status_resp for error reporting
                //if (response.payload.get_ps_config_resp.has_config) {
                //     response.which_payload = embedded_wifi_mgmt_WifiMgmtResponse_get_ps_config_resp_tag;
                //} else
                { // Error occurred, handler should have filled status_resp
                     response.which_payload = embedded_wifi_mgmt_WifiMgmtResponse_status_resp_tag;
                }
            } else { payload_type_mismatch = true; }
            break;
        case embedded_wifi_mgmt_RequestType_REQUEST_TYPE_SET_REG_DOMAIN:
            if (request.which_payload == embedded_wifi_mgmt_WifiMgmtRequest_set_reg_domain_req_tag) {
                // If set_reg_domain_req.reg_domain.country_code uses callback, reg_ctx is populated
                response.which_payload = embedded_wifi_mgmt_WifiMgmtResponse_status_resp_tag;
                handle_set_reg_domain_request(&request.payload.set_reg_domain_req, &reg_ctx, &response.payload.status_resp);
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

static void handle_connect_request(const embedded_wifi_mgmt_WifiConnectRequest *req,
                                   embedded_wifi_mgmt_WifiStatusResponse *resp) {
    resp->success = false; // Default to failure
    resp->error_code = -EIO; // Generic I/O error
    set_status_error_msg_bytes(resp->error_message, "Connect handler init error");

    LOG_INF("Handling Connect Request for iface %u, SSID: %.*s",
            req->iface_index, (int)req->ssid.size, req->ssid.bytes);

    struct net_if *iface = net_if_get_by_index(req->iface_index);
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

    params.security = (enum wifi_security_type)req->security_type;

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

    memcpy(params.bssid, req->bssid, WIFI_MAC_ADDR_LEN);

    params.timeout = req->timeout_ms;
    params.mfp = req->mfp;
    params.band = req->band; // Map from WifiBand proto enum
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
    struct net_if *iface = net_if_get_by_index(req->iface_index);
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
    struct net_if *iface = net_if_get_by_index(req->iface_index);
    if (!iface) {
        LOG_ERR("Interface with index %u not found for scan", req->iface_index);
        resp->error_code = -ENODEV;
        set_status_error_msg_bytes(resp->error_message, "Interface not found for scan");
        return;
    }

    struct wifi_scan_params params = {0}; // Zephyr's scan params struct

    if (req->has_params) {
        const embedded_wifi_mgmt_WifiScanParams *proto_params = &req->params;
        params.scan_type = (enum wifi_scan_type)proto_params->scan_type;
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
    }
}


static void handle_ap_enable_request(const embedded_wifi_mgmt_WifiApEnableRequest *req,
                                     struct ApEnableRequestContext *ap_ctx,
                                     embedded_wifi_mgmt_WifiStatusResponse *resp) {
    resp->success = false;
    resp->error_code = -EIO;
    set_status_error_msg_bytes(resp->error_message, "AP enable handler init error");

    LOG_INF("Handling AP Enable Request for iface %u", req->iface_index);
    struct net_if *iface = net_if_get_by_index(req->iface_index);
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
    struct wifi_connect_req_params params = {0}; // Zephyr's AP enable often reuses connect_req_params

    if (proto_params->ssid.size == 0 || proto_params->ssid.size > WIFI_SSID_MAX_LEN) {
        LOG_ERR("Invalid AP SSID length: %zu", proto_params->ssid.size);
        resp->error_code = -EINVAL;
        set_status_error_msg_bytes(resp->error_message, "Invalid AP SSID length");
        return;
    }
    params.ssid = (uint8_t*)proto_params->ssid.bytes;
    params.ssid_length = proto_params->ssid.size;

    params.band = proto_params->band;
    params.channel = (uint8_t)proto_params->channel;
    if (params.channel == embedded_wifi_mgmt_WifiChannelConst_WIFI_CHANNEL_ANY) {
        params.channel = WIFI_CHANNEL_ANY; // Or a default AP channel e.g. 1, 6, 11 for 2.4GHz
    }

    params.security = (enum wifi_security_type)proto_params->security_type;
    if (params.security == WIFI_SECURITY_TYPE_PSK) {
        // Use PSK from ap_ctx if callback was used, or directly from proto_params if not
        if (ap_ctx->psk_len > 0) { // Assuming callback populated ap_ctx
             if (ap_ctx->psk_len > MAX_AP_PSK_LEN) {
                LOG_ERR("AP PSK (from ctx) too long: %zu", ap_ctx->psk_len);
                resp->error_code = -EINVAL;
                set_status_error_msg_bytes(resp->error_message, "AP PSK too long");
                return;
            }
            params.psk = ap_ctx->psk;
            params.psk_length = ap_ctx->psk_len;
        } else if (proto_params->psk.size > 0) { // Fallback to direct PSK from proto
            if (proto_params->psk.size > MAX_AP_PSK_LEN) {
                LOG_ERR("AP PSK (from proto) too long: %zu", proto_params->psk.size);
                resp->error_code = -EINVAL;
                set_status_error_msg_bytes(resp->error_message, "AP PSK too long");
                return;
            }
            params.psk = (uint8_t*)proto_params->psk.bytes;
            params.psk_length = proto_params->psk.size;
        } else {
            LOG_ERR("AP PSK missing for PSK security");
            resp->error_code = -EINVAL;
            set_status_error_msg_bytes(resp->error_message, "AP PSK missing");
            return;
        }
    } else if (params.security != WIFI_SECURITY_TYPE_NONE) { // TODO: Requiring security
        LOG_WRN("Unsupported/unhandled AP security type for PSK: %d", params.security);
        // Allow to proceed if driver handles it, or return error:
        // resp->error_code = -ENOTSUP;
        // set_status_error_msg_bytes(resp->error_message, "Unsupported AP security type");
        // return;
    }
    // params.mfp = (enum wifi_mfp_options)proto_params->mfp; // If MFP is part of AP params in your proto

    int ret = net_mgmt(NET_REQUEST_WIFI_AP_ENABLE, iface, &params, sizeof(params));
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
    struct net_if *iface = net_if_get_by_index(req->iface_index);
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
    struct net_if *iface = net_if_get_by_index(req->iface_index);
    if (!iface) {
        LOG_ERR("Interface with index %u not found for PS config", req->iface_index);
        resp->error_code = -ENODEV;
        set_status_error_msg_bytes(resp->error_message, "Interface not found for PS config");
        return;
    }

    if (!req->has_config) {
        LOG_ERR("Set PS Config: config data missing");
        resp->error_code = -EINVAL;
        set_status_error_msg_bytes(resp->error_message, "PS config data missing");
        return;
    }

    struct wifi_ps_config params = {0}; // Zephyr's struct wifi_ps_config
    // This struct might contain wifi_ps_params and TWT flow info.
    // Mapping from embedded_wifi_mgmt_WifiPowerSaveConfig to struct wifi_ps_config

    params.ps_params.enabled = (enum wifi_ps)req->config.ps_settings.enabled; // Map WifiPsState
    params.ps_params.listen_interval = req->config.ps_settings.listen_interval_beacons;
    params.ps_params.wakeup_mode = (enum wifi_ps_wakeup_mode)req->config.ps_settings.wakeup_mode;
    params.ps_params.mode = (enum wifi_ps_mode)req->config.type; // Map WifiPowerSaveType
    params.ps_params.timeout_ms = req->config.ps_settings.timeout_ms;
    // params.ps_params.type = ... // if there's a param_type in Zephyr struct
    // params.ps_params.fail_reason = ... // This is usually a result, not a param
    // params.ps_params.exit_strategy = (enum wifi_ps_exit_strategy)req->config.ps_settings.exit_strategy;

    // Map TWT flows if your proto and Zephyr struct support it
    // params.num_twt_flows = req->config.twt_flows_count;
    // for (int i=0; i < params.num_twt_flows; ++i) { ... map twt_flows ... }


    int ret = net_mgmt(NET_REQUEST_WIFI_PS_CONFIG, iface, &params, sizeof(params)); // MGMT_OP_SET is implicit
    if (ret) {
        LOG_ERR("NET_REQUEST_WIFI_PS_CONFIG (SET) failed: %d", ret);
        resp->error_code = ret;
        set_status_error_msg_bytes(resp->error_message, "Set PS config failed");
    } else {
        LOG_INF("NET_REQUEST_WIFI_PS_CONFIG (SET) success");
        resp->success = true;
        resp->error_code = 0;
        set_status_error_msg_bytes(resp->error_message, "Set PS config success");
    }
}

static void handle_get_ps_config_request(const embedded_wifi_mgmt_GetPowerSaveConfigRequest *req,
                                         //embedded_wifi_mgmt_GetPowerSaveConfigResponse *data_resp,
                                         embedded_wifi_mgmt_WifiStatusResponse *err_resp) {
    //data_resp->has_config = false; // Default to no data
    err_resp->success = false;     // Default to error for the status part
    err_resp->error_code = -EIO;
    set_status_error_msg_bytes(err_resp->error_message, "Get PS config handler init error");

    LOG_INF("Handling Get Power Save Config Request for iface %u", req->iface_index);
    struct net_if *iface = net_if_get_by_index(req->iface_index);
    if (!iface) {
        LOG_ERR("Interface with index %u not found for PS config", req->iface_index);
        err_resp->error_code = -ENODEV;
        set_status_error_msg_bytes(err_resp->error_message, "Interface not found for PS config");
        return;
    }

    struct wifi_ps_config params_retrieved = {0}; // Zephyr's struct
    // For GET, pass the struct to be filled. MGMT_OP_GET is implicit.
    int ret = net_mgmt(NET_REQUEST_WIFI_PS_CONFIG, iface, &params_retrieved, sizeof(params_retrieved));

    if (ret) {
        LOG_ERR("NET_REQUEST_WIFI_PS_CONFIG (GET) failed: %d", ret);
        err_resp->error_code = ret;
        set_status_error_msg_bytes(err_resp->error_message, "Get PS config failed");
    } else {
        LOG_INF("NET_REQUEST_WIFI_PS_CONFIG (GET) success");
        //data_resp->has_config = true;

        /*
        embedded_wifi_mgmt_WifiPowerSaveConfig *proto_config = &data_resp->config;

        proto_config->ps_settings.enabled = (embedded_wifi_mgmt_WifiPsState)params_retrieved.ps_params.enabled;
        proto_config->ps_settings.listen_interval_beacons = params_retrieved.ps_params.listen_interval;
        proto_config->ps_settings.wakeup_mode = (embedded_wifi_mgmt_WifiPsWakeupMode)params_retrieved.ps_params.wakeup_mode;
        proto_config->type = (embedded_wifi_mgmt_WifiPowerSaveType)params_retrieved.ps_params.mode;
        proto_config->ps_settings.timeout_ms = params_retrieved.ps_params.timeout_ms;
        // proto_config->ps_settings.exit_strategy = (embedded_wifi_mgmt_WifiPsExitStrategy)params_retrieved.ps_params.exit_strategy;
        // Map TWT flows if present
        */
        // If successful, the err_resp part is not used by the main switch, but good to set it correctly.
        err_resp->success = true;
        err_resp->error_code = 0;
        set_status_error_msg_bytes(err_resp->error_message, "Get PS config success");
    }
}

static void handle_set_reg_domain_request(const embedded_wifi_mgmt_SetRegulatoryDomainRequest *req,
                                          struct RegDomainRequestContext *reg_ctx,
                                          embedded_wifi_mgmt_WifiStatusResponse *resp) {
    resp->success = false;
    resp->error_code = -EIO;
    set_status_error_msg_bytes(resp->error_message, "Set RegDomain handler init error");

    LOG_INF("Handling Set Regulatory Domain Request for iface %u, Country: %s",
            req->iface_index, reg_ctx->country_code_len > 0 ? reg_ctx->country_code : "N/A (or from proto direct)");

    struct net_if *iface = net_if_get_by_index(req->iface_index);
    if (!iface && req->iface_index != 0) {
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

    struct wifi_reg_domain reg_domain_param = {0}; // Zephyr's struct
    reg_domain_param.oper = WIFI_MGMT_SET; // Explicitly set operation for Zephyr API
    reg_domain_param.force = req->reg_domain.force_update;

    strncpy(reg_domain_param.country_code, req->reg_domain.country_code, 2);
    // Map channel_info if present in proto and Zephyr struct
    // This requires dynamic allocation or a fixed-size buffer in wifi_reg_domain
    // For simplicity, this part is omitted. Zephyr's set reg domain might just take country code.
    // reg_domain_param.num_channels = req->reg_domain.channel_info_count;
    // reg_domain_param.chan_info = ... (needs allocation and mapping)

    int ret = net_mgmt(NET_REQUEST_WIFI_REG_DOMAIN, iface, &reg_domain_param, sizeof(reg_domain_param));
    if (ret) {
        LOG_ERR("NET_REQUEST_WIFI_REG_DOMAIN (SET) failed: %d", ret);
        resp->error_code = ret;
        set_status_error_msg_bytes(resp->error_message, "Set RegDomain failed");
    } else {
        LOG_INF("NET_REQUEST_WIFI_REG_DOMAIN (SET) success");
        resp->success = true;
        resp->error_code = 0;
    }
}

static void handle_get_reg_domain_request(const embedded_wifi_mgmt_GetRegulatoryDomainRequest *req,
                                          embedded_wifi_mgmt_GetRegulatoryDomainResponse *data_resp,
                                          embedded_wifi_mgmt_WifiStatusResponse *err_resp) {
    data_resp->has_reg_domain = false;
    err_resp->success = false;
    err_resp->error_code = -EIO;
    set_status_error_msg_bytes(err_resp->error_message, "Get RegDomain handler init error");

    LOG_INF("Handling Get Regulatory Domain Request for iface %u", req->iface_index);
    struct net_if *iface = net_if_get_by_index(req->iface_index);
    if (!iface && req->iface_index != 0) {
        LOG_ERR("Interface with index %u not found for RegDomain", req->iface_index);
        err_resp->error_code = -ENODEV;
        set_status_error_msg_bytes(err_resp->error_message, "Interface not found for RegDomain");
        return;
    }

    struct wifi_reg_domain reg_domain_retrieved = {0}; // Zephyr's struct
    reg_domain_retrieved.oper = WIFI_MGMT_GET; // Explicitly set operation

    int ret = net_mgmt(NET_REQUEST_WIFI_REG_DOMAIN, iface, &reg_domain_retrieved, sizeof(reg_domain_retrieved));

    if (ret) {
        LOG_ERR("NET_REQUEST_WIFI_REG_DOMAIN (GET) failed: %d", ret);
        err_resp->error_code = ret;
        set_status_error_msg_bytes(err_resp->error_message, "Get RegDomain failed");
    } else {
        LOG_INF("NET_REQUEST_WIFI_REG_DOMAIN (GET) success. Country: %c%c",
                reg_domain_retrieved.country_code[0], reg_domain_retrieved.country_code[1]);
        data_resp->has_reg_domain = true;
        embedded_wifi_mgmt_WifiRegDomain *proto_rd = &data_resp->reg_domain;

        strncpy(proto_rd->country_code, reg_domain_retrieved.country_code, 2);
        proto_rd->force_update = reg_domain_retrieved.force; // Map back if relevant for GET

        // Map channel_info if retrieved and proto supports it
        // proto_rd->channel_info_count = reg_domain_retrieved.num_channels;
        // For each channel... map to proto_rd->channel_info[i]

        err_resp->success = true;
        err_resp->error_code = 0;
        set_status_error_msg_bytes(err_resp->error_message, "Get RegDomain success");
    }
}

static void handle_get_iface_status_request(const embedded_wifi_mgmt_GetInterfaceStatusRequest *req,
                                            embedded_wifi_mgmt_GetInterfaceStatusResponse *data_resp,
                                            embedded_wifi_mgmt_WifiStatusResponse *err_resp) {
    data_resp->has_status = false;
    err_resp->success = false;
    err_resp->error_code = -EIO;
    set_status_error_msg_bytes(err_resp->error_message, "Get IfaceStatus handler init error");

    LOG_INF("Handling Get Interface Status Request for iface %u", req->iface_index);
    struct net_if *iface = net_if_get_by_index(req->iface_index);
    if (!iface) {
        LOG_ERR("Interface with index %u not found for status", req->iface_index);
        err_resp->error_code = -ENODEV;
        set_status_error_msg_bytes(err_resp->error_message, "Interface not found for status");
        return;
    }

    struct wifi_iface_status status_retrieved = {0}; // Zephyr's struct
    int ret = net_mgmt(NET_REQUEST_WIFI_IFACE_STATUS, iface, &status_retrieved, sizeof(status_retrieved));

    if (ret) {
        LOG_ERR("NET_REQUEST_WIFI_IFACE_STATUS failed: %d", ret);
        err_resp->error_code = ret;
        set_status_error_msg_bytes(err_resp->error_message, "Get IfaceStatus failed");
    } else {
        LOG_INF("NET_REQUEST_WIFI_IFACE_STATUS success. State: %d, SSID: %.*s",
                status_retrieved.state, status_retrieved.ssid_len, status_retrieved.ssid);
        data_resp->has_status = true;
        embedded_wifi_mgmt_WifiInterfaceStatus *proto_s = &data_resp->status;

        proto_s->state = (embedded_wifi_mgmt_WifiInterfaceState)status_retrieved.state;
        proto_s->mode = (embedded_wifi_mgmt_WifiMode)status_retrieved.iface_mode;

        if (status_retrieved.ssid_len > 0 && status_retrieved.ssid_len <= sizeof(proto_s->ssid.bytes)) {
            memcpy(proto_s->ssid.bytes, status_retrieved.ssid, status_retrieved.ssid_len);
            proto_s->ssid.size = status_retrieved.ssid_len;
        } else { proto_s->ssid.size = 0; }

        if ((status_retrieved.state >= WIFI_STATE_ASSOCIATED && status_retrieved.state <= WIFI_STATE_ASSOCIATED ) ||
             status_retrieved.iface_mode == WIFI_MODE_AP) { // BSSID relevant if connected or AP mode
            memcpy(proto_s->bssid, status_retrieved.bssid, WIFI_MAC_ADDR_LEN);
        }

        proto_s->band = (embedded_wifi_mgmt_WifiBand)status_retrieved.band;
        proto_s->channel = status_retrieved.channel;
        proto_s->security = (embedded_wifi_mgmt_WifiSecurityType)status_retrieved.security;
        proto_s->mfp = (embedded_wifi_mgmt_WifiMfpOptions)status_retrieved.mfp;
        proto_s->rssi = status_retrieved.rssi;
        proto_s->link_mode = (embedded_wifi_mgmt_WifiLinkMode)status_retrieved.link_mode;
        // proto_s->ht_mcs = status_retrieved.ht_mcs; // If available in Zephyr struct and proto
        // proto_s->vht_mcs = status_retrieved.vht_mcs; // If available
        // proto_s->he_mcs = status_retrieved.he_mcs; // If available
        proto_s->tx_bitrate_kbps = status_retrieved.current_phy_tx_rate; // Assuming Zephyr provides in kbps or needs conversion
        // proto_s->rx_bitrate_kbps = status_retrieved.rx_bitrate; // If available
        proto_s->dtim_period = status_retrieved.dtim_period;
        proto_s->beacon_interval = status_retrieved.beacon_interval;
        proto_s->twt_capable = status_retrieved.twt_capable;

        err_resp->success = true;
        err_resp->error_code = 0;
    }
}

int init_proxy(void) {
    LOG_INF("Initializing Wi-Fi Management Proxy");



    LOG_INF("Wi-Fi Management Proxy initialized successfully");
    return 0;
}

SYS_INIT(init_proxy, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);