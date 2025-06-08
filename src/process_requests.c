// TODO: Implement functions to create and send WifiMgmtResponse messages.
// Example:
// void send_status_response(uint32_t request_id, bool success, int32_t error_code, const char* msg);
// void send_get_version_response(uint32_t request_id, const embedded_wifi_mgmt_WifiVersion* version_data);
// etc.

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/sys/byteorder.h> // For potential byte order conversions if needed

#include <pb_decode.h>
#include <pb_encode.h>
#include "generated/wifi.pb.h" // Path to your generated nanopb header

LOG_MODULE_REGISTER(wifi_handler, LOG_LEVEL_INF);

struct wifi_scan_ssid_spec {
    uint8_t ssid[WIFI_SSID_MAX_LEN]; // Adjust size as per your requirements
    size_t ssid_len;
};

// --- Callback Context Structures ---
// Context for WifiScanParams.directed_scan_ssids
#define MAX_DIRECTED_SCAN_SSIDS_CB 4 // Example, adjust as needed, must be <= wifi.options
#define MAX_SSID_LEN_CB WIFI_SSID_MAX_LEN // From Zephyr's wifi_mgmt.h
struct ScanRequestContext {
    struct wifi_scan_ssid_spec ssids[MAX_DIRECTED_SCAN_SSIDS_CB];
    uint8_t current_ssid_count;
};

// Context for WifiApEnableParams.psk
#define MAX_PSK_LEN_CB 64 // Example, should match Zephyr's max PSK len or wifi.options (65 for psk, 64 for sae_password)
struct ApEnableRequestContext {
    uint8_t psk[MAX_PSK_LEN_CB + 1]; // +1 for null terminator if used as string
    size_t psk_len;
};

// Context for WifiRegDomain.country_code
#define MAX_COUNTRY_CODE_LEN_CB 2 // e.g., "US"
struct RegDomainRequestContext {
    char country_code[MAX_COUNTRY_CODE_LEN_CB + 1]; // +1 for null terminator
    size_t country_code_len;
};


// --- Nanopb Decoder Callback Functions ---

// Callback for decoding repeated WifiDirectedScanSsid messages
static bool decode_directed_scan_ssid_cb(pb_istream_t *stream, const pb_field_t *field, void **arg) {
    struct ScanRequestContext *ctx = (struct ScanRequestContext *)*arg;

    if (ctx->current_ssid_count >= MAX_DIRECTED_SCAN_SSIDS_CB) {
        LOG_WRN("Max directed scan SSIDs reached in callback (%u)", MAX_DIRECTED_SCAN_SSIDS_CB);
        // To stop decoding further items for this field, but not fail the whole message:
        // return pb_skip_field(stream, field->type); // This might be tricky for repeated messages.
        // A simpler approach is to just not store more than MAX.
        // For now, let's try to skip to avoid buffer overflows if nanopb keeps calling.
        // However, the field is embedded_wifi_mgmt_WifiDirectedScanSsid, so we must decode it to skip.
        embedded_wifi_mgmt_WifiDirectedScanSsid dummy_ssid = embedded_wifi_mgmt_WifiDirectedScanSsid_init_zero;
        if (!pb_decode(stream, embedded_wifi_mgmt_WifiDirectedScanSsid_fields, &dummy_ssid)) {
             LOG_ERR("Failed to decode (and skip) WifiDirectedScanSsid submessage");
             return false;
        }
        return true; // Successfully skipped one item.
    }

    embedded_wifi_mgmt_WifiDirectedScanSsid proto_ssid = embedded_wifi_mgmt_WifiDirectedScanSsid_init_zero;
    if (!pb_decode(stream, embedded_wifi_mgmt_WifiDirectedScanSsid_fields, &proto_ssid)) {
        LOG_ERR("Failed to decode WifiDirectedScanSsid submessage in callback");
        return false;
    }

    if (proto_ssid.ssid.size > MAX_SSID_LEN_CB) {
        LOG_WRN("Decoded directed SSID too long (%u > %d)", proto_ssid.ssid.size, MAX_SSID_LEN_CB);
        // Potentially skip this SSID or truncate, for now, we skip adding it.
        return true; // Continue decoding other SSIDs if any
    }

    memcpy(ctx->ssids[ctx->current_ssid_count].ssid, proto_ssid.ssid.bytes, proto_ssid.ssid.size);
    ctx->ssids[ctx->current_ssid_count].ssid_len = proto_ssid.ssid.size;
    ctx->current_ssid_count++;
    return true;
}

// Callback for decoding WifiApEnableParams.psk (bytes)
static bool decode_ap_psk_cb(pb_istream_t *stream, const pb_field_t *field, void **arg) {
    struct ApEnableRequestContext *ctx = (struct ApEnableRequestContext*)*arg;

    if (stream->bytes_left > MAX_PSK_LEN_CB) {
        LOG_ERR("PSK too long in callback (max %d, got %u)", MAX_PSK_LEN_CB, stream->bytes_left);
        return false; // Indicate error, this will fail the main pb_decode
    }

    ctx->psk_len = stream->bytes_left;
    if (!pb_read(stream, ctx->psk, ctx->psk_len)) {
        LOG_ERR("Failed to read PSK bytes in callback");
        return false;
    }
    ctx->psk[ctx->psk_len] = '\0'; // Null-terminate, useful if PSK is ASCII
    return true;
}

// Callback for decoding WifiRegDomain.country_code (string)
static bool decode_country_code_cb(pb_istream_t *stream, const pb_field_t *field, void **arg) {
    struct RegDomainRequestContext *ctx = (struct RegDomainRequestContext*)*arg;

    if (stream->bytes_left > MAX_COUNTRY_CODE_LEN_CB) {
         LOG_ERR("Country code too long in callback (max %d, got %u)", MAX_COUNTRY_CODE_LEN_CB, stream->bytes_left);
         return false; // Indicate error
    }
    ctx->country_code_len = stream->bytes_left;
    if (!pb_read(stream, (pb_byte_t*)ctx->country_code, ctx->country_code_len)) {
        LOG_ERR("Failed to read country code bytes in callback");
        return false;
    }
    ctx->country_code[ctx->country_code_len] = '\0'; // Null-terminate
    return true;
}

// ... (existing includes and context structs) ...

// --- Nanopb Decoder Callback Functions (decode_directed_scan_ssid_cb, decode_ap_psk_cb, decode_country_code_cb) ---
// ... (These remain as they are for request decoding) ...


// --- Modified forward declarations of request handlers ---
// Handlers now take a pointer to their specific response payload structure to fill
static void handle_connect_request(const embedded_wifi_mgmt_WifiConnectRequest *req_payload,
                                   embedded_wifi_mgmt_WifiStatusResponse *status_resp_payload);
static void handle_disconnect_request(const embedded_wifi_mgmt_WifiDisconnectRequest *req_payload,
                                      embedded_wifi_mgmt_WifiStatusResponse *status_resp_payload);
static void handle_scan_request(const embedded_wifi_mgmt_WifiScanRequest *req_payload,
                                struct ScanRequestContext *scan_ctx,
                                embedded_wifi_mgmt_WifiStatusResponse *status_resp_payload);
static void handle_ap_enable_request(const embedded_wifi_mgmt_WifiApEnableRequest *req_payload,
                                     struct ApEnableRequestContext *ap_ctx,
                                     embedded_wifi_mgmt_WifiStatusResponse *status_resp_payload);
static void handle_ap_disable_request(const embedded_wifi_mgmt_WifiApDisableRequest *req_payload,
                                      embedded_wifi_mgmt_WifiStatusResponse *status_resp_payload);
static void handle_get_version_request(const embedded_wifi_mgmt_GetWifiVersionRequest *req_payload,
                                       embedded_wifi_mgmt_GetWifiVersionResponse *version_resp_payload);
static void handle_set_ps_config_request(const embedded_wifi_mgmt_SetPowerSaveConfigRequest *req_payload,
                                         embedded_wifi_mgmt_WifiStatusResponse *status_resp_payload);
static void handle_get_ps_config_request(const embedded_wifi_mgmt_GetPowerSaveConfigRequest *req_payload,
                                         embedded_wifi_mgmt_GetPowerSaveConfigResponse *ps_config_resp_payload,
                                         embedded_wifi_mgmt_WifiStatusResponse *status_resp_for_error); // For errors during GET
static void handle_set_reg_domain_request(const embedded_wifi_mgmt_SetRegulatoryDomainRequest *req_payload,
                                          struct RegDomainRequestContext *reg_ctx,
                                          embedded_wifi_mgmt_WifiStatusResponse *status_resp_payload);
static void handle_get_reg_domain_request(const embedded_wifi_mgmt_GetRegulatoryDomainRequest *req_payload,
                                          embedded_wifi_mgmt_GetRegulatoryDomainResponse *reg_domain_resp_payload,
                                          embedded_wifi_mgmt_WifiStatusResponse *status_resp_for_error); // For errors during GET
static void handle_get_iface_status_request(const embedded_wifi_mgmt_GetInterfaceStatusRequest *req_payload,
                                            embedded_wifi_mgmt_GetInterfaceStatusResponse *iface_status_resp_payload,
                                            embedded_wifi_mgmt_WifiStatusResponse *status_resp_for_error); // For errors during GET

// Helper to fill WifiStatusResponse error message (assuming pb_bytes_array_t or char[])
static void set_status_error_msg(embedded_wifi_mgmt_WifiStatusResponse *status_resp, const char *msg) {
    if (!status_resp || !msg) return;
    size_t msg_len = strlen(msg);
    // Assuming error_message is pb_bytes_array_t due to .options file with max_size
    // If it's char error_message[N], ensure N is large enough.
    // For pb_bytes_array_t:
    if (msg_len >= sizeof(status_resp->error_message)) {
        msg_len = sizeof(status_resp->error_message) - 1; // Leave space if null term desired by some debug
    }
    memcpy(status_resp->error_message, msg, msg_len);
    //status_resp->error_message.size = msg_len;
    // If char error_message[N]:
    // strncpy(status_resp->error_message, msg, N - 1);
    // status_resp->error_message[N - 1] = '\0';
}

// --- Main processing function ---
void process_wifi_mgmt_request(const uint8_t *buffer, size_t length, uint8_t *response_buffer, size_t* response_buffer_len_io) {
    embedded_wifi_mgmt_WifiMgmtRequest request = embedded_wifi_mgmt_WifiMgmtRequest_init_zero;
    pb_istream_t stream = pb_istream_from_buffer(buffer, length);

    // Prepare contexts for potential callbacks during request decoding
    struct ScanRequestContext scan_ctx = {0};
    struct ApEnableRequestContext ap_ctx = {0};
    struct RegDomainRequestContext reg_ctx = {0};

    if (!pb_decode(&stream, embedded_wifi_mgmt_WifiMgmtRequest_fields, &request)) {
        LOG_ERR("Failed to decode WifiMgmtRequest: %s", PB_GET_ERROR(&stream));
        embedded_wifi_mgmt_WifiMgmtResponse err_response = embedded_wifi_mgmt_WifiMgmtResponse_init_zero;
        err_response.request_id = 0; // Or a special value for decode failure
        err_response.which_payload = embedded_wifi_mgmt_WifiMgmtResponse_status_resp_tag;
        err_response.payload.status_resp.success = false;
        err_response.payload.status_resp.error_code = -EBADMSG; // Bad message
        set_status_error_msg(&err_response.payload.status_resp, "Request decode failed");

        pb_ostream_t ostream = pb_ostream_from_buffer(response_buffer, *response_buffer_len_io);
        if (pb_encode(&ostream, embedded_wifi_mgmt_WifiMgmtResponse_fields, &err_response)) {
            *response_buffer_len_io = ostream.bytes_written;
        } else {
            *response_buffer_len_io = 0;
        }
        return;
    }

    LOG_INF("Processing RequestType enum: %u (tag %u)", request.request_id, request.which_payload);

    embedded_wifi_mgmt_WifiMgmtResponse response = embedded_wifi_mgmt_WifiMgmtResponse_init_zero;
    // Echo the RequestType enum value as the response ID.
    // A sequence number in the request would be more typical for matching.
    response.request_id = (uint32_t)request.request_id;

    bool generate_response = true;
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
                response.which_payload = embedded_wifi_mgmt_WifiMgmtResponse_status_resp_tag;
                handle_scan_request(&request.payload.scan_req, &scan_ctx, &response.payload.status_resp);
            } else { payload_type_mismatch = true; }
            break;
        case embedded_wifi_mgmt_RequestType_REQUEST_TYPE_AP_ENABLE:
            if (request.which_payload == embedded_wifi_mgmt_WifiMgmtRequest_ap_enable_req_tag) {
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
                // This handler might return data or an error status
                handle_get_ps_config_request(&request.payload.get_ps_config_req,
                                             &response.payload.get_ps_config_resp,
                                             &response.payload.status_resp);
                // Determine which_payload based on handler's outcome (if it sets status_resp for error)
                if (response.payload.get_ps_config_resp.has_config) { // Assuming handler sets this on success
                     response.which_payload = embedded_wifi_mgmt_WifiMgmtResponse_get_ps_config_resp_tag;
                } else { // Error occurred, handler should have filled status_resp
                     response.which_payload = embedded_wifi_mgmt_WifiMgmtResponse_status_resp_tag;
                }
            } else { payload_type_mismatch = true; }
            break;
        case embedded_wifi_mgmt_RequestType_REQUEST_TYPE_SET_REG_DOMAIN:
            if (request.which_payload == embedded_wifi_mgmt_WifiMgmtRequest_set_reg_domain_req_tag) {
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
        default:
            LOG_WRN("Unknown or unhandled RequestType enum: %d", request.request_id);
            response.which_payload = embedded_wifi_mgmt_WifiMgmtResponse_status_resp_tag;
            response.payload.status_resp.success = false;
            response.payload.status_resp.error_code = -ENOTSUP; // Not supported
            set_status_error_msg(&response.payload.status_resp, "Unsupported request type");
            break;
    }

    if (payload_type_mismatch) {
        LOG_ERR("Payload type mismatch for RequestType enum %u, got tag %u", request.request_id, request.which_payload);
        response.which_payload = embedded_wifi_mgmt_WifiMgmtResponse_status_resp_tag; // Ensure status_resp is active
        response.payload.status_resp.success = false;
        response.payload.status_resp.error_code = -EBADMSG; // Bad message
        set_status_error_msg(&response.payload.status_resp, "Payload type mismatch for request ID");
    }

    if (generate_response) {
        pb_ostream_t ostream = pb_ostream_from_buffer(response_buffer, *response_buffer_len_io);
        if (pb_encode(&ostream, embedded_wifi_mgmt_WifiMgmtResponse_fields, &response)) {
            *response_buffer_len_io = ostream.bytes_written;
        } else {
            //TODO LOG_ERR("Failed to encode WifiMgmtResponse: %s", PB_GET_ERROR(&ostream));
            *response_buffer_len_io = 0; // Indicate error / no valid response
        }
    } else {
         // This case should ideally not be reached if all paths set up a response or error
        *response_buffer_len_io = 0;
    }
}

// --- Request Handler Implementations (Example: handle_connect_request) ---
// You need to refactor all your handler functions.
// Here's an example for handle_connect_request:

static void handle_connect_request(const embedded_wifi_mgmt_WifiConnectRequest *req_payload,
                                   embedded_wifi_mgmt_WifiStatusResponse *status_resp_payload) {
    // Initialize response to a default failure state
    status_resp_payload->success = false;
    status_resp_payload->error_code = -EIO; // Generic I/O error or uninitialized
    set_status_error_msg(status_resp_payload, "Connect handler error");


    LOG_INF("Handling Connect Request for iface %u, SSID: %.*s",
            req_payload->iface_index, (int)req_payload->ssid.size, req_payload->ssid.bytes);

    struct net_if *iface = net_if_get_by_index(req_payload->iface_index); // Assuming index starts at 1
    if (!iface) {
        LOG_ERR("Interface with index %u not found", req_payload->iface_index);
        status_resp_payload->error_code = -ENODEV;
        set_status_error_msg(status_resp_payload, "Interface not found");
        return;
    }

    struct wifi_connect_req_params params = {0};

    // Assuming req_payload->ssid is pb_bytes_array_t
    if (req_payload->ssid.size > WIFI_SSID_MAX_LEN) {
        LOG_WRN("Connect SSID too long (%u > %d)", req_payload->ssid.size, WIFI_SSID_MAX_LEN);
        status_resp_payload->error_code = -EINVAL;
        set_status_error_msg(status_resp_payload, "SSID too long");
        return;
    }
    params.ssid = (uint8_t*)req_payload->ssid.bytes;
    params.ssid_length = req_payload->ssid.size;

    params.security = (enum wifi_security_type)req_payload->security_type;

    if (params.security == WIFI_SECURITY_TYPE_PSK ||
        params.security == WIFI_SECURITY_TYPE_PSK_SHA256 ||
        params.security == WIFI_SECURITY_TYPE_WEP) {
        if (req_payload->psk.size > WIFI_PSK_MAX_LEN) { // Assuming WIFI_PSK_MAX_LEN
             LOG_WRN("Connect PSK too long (%u > %d)", req_payload->psk.size, WIFI_PSK_MAX_LEN);
             status_resp_payload->error_code = -EINVAL;
             set_status_error_msg(status_resp_payload, "PSK too long");
             return;
        }
        params.psk = (uint8_t*)req_payload->psk.bytes;
        params.psk_length = req_payload->psk.size;
     } else if (params.security == WIFI_SECURITY_TYPE_SAE) {
         if (req_payload->sae_password.size > WIFI_SAE_PSWD_MAX_LEN) { // Assuming WIFI_SAE_PSWD_MAX_LEN
             LOG_WRN("Connect SAE password too long (%u > %d)", req_payload->sae_password.size, WIFI_SAE_PSWD_MAX_LEN);
             status_resp_payload->error_code = -EINVAL;
             set_status_error_msg(status_resp_payload, "SAE password too long");
             return;
         }
        params.sae_password = (uint8_t*)req_payload->sae_password.bytes;
        params.sae_password_length = req_payload->sae_password.size;
    }
    // ... (rest of the parameter mapping from req_payload to params as in your original code)
    params.channel = (req_payload->channel == embedded_wifi_mgmt_WifiChannelConst_WIFI_CHANNEL_ANY) ?
                     WIFI_CHANNEL_ANY : (uint8_t)req_payload->channel;

    memcpy(params.bssid, req_payload->bssid, sizeof(params.bssid));

    params.timeout = req_payload->timeout_ms;
    params.mfp = (enum wifi_mfp_options)req_payload->mfp;


    int ret = net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, &params, sizeof(params));
    if (ret) {
        LOG_ERR("NET_REQUEST_WIFI_CONNECT failed: %d", ret);
        status_resp_payload->success = false;
        status_resp_payload->error_code = ret; // Zephyr error code
        set_status_error_msg(status_resp_payload, "Wi-Fi connect request failed");
    } else {
        LOG_INF("NET_REQUEST_WIFI_CONNECT initiated");
        status_resp_payload->success = true;
        status_resp_payload->error_code = 0;
        set_status_error_msg(status_resp_payload, "Connect initiated");
    }
}

// ... (Implement other handlers similarly, populating their respective response_payload parts) ...
// For GET operations that might fail before returning data (e.g. bad iface_index),
// they should populate the 'status_resp_for_error' and ensure the main data part (e.g. get_version_resp.has_version) is false.
// process_wifi_mgmt_request will then select status_resp_tag for the oneof.

// Example for handle_get_version_request:
static void handle_get_version_request(const embedded_wifi_mgmt_GetWifiVersionRequest *req_payload,
                                       embedded_wifi_mgmt_GetWifiVersionResponse *version_resp_payload) {
    LOG_INF("Handling Get Version Request");
    version_resp_payload->has_version = true;
    version_resp_payload->version.driver_major = 1; // Example
    version_resp_payload->version.driver_minor = 2; // Example
    version_resp_payload->version.driver_patch = 3; // Example

    // Assuming version.firmware_version is pb_bytes_array_t due to .options
    const char *fw_ver_str = "ZephyrWiFi-v0.1.0";
    size_t fw_len = strlen(fw_ver_str);
    if (fw_len >= sizeof(version_resp_payload->version.firmware_version)) {
        fw_len = sizeof(version_resp_payload->version.firmware_version) -1;
    }
    memcpy(version_resp_payload->version.firmware_version, fw_ver_str, fw_len);
    version_resp_payload->version.firmware_version[fw_len] = '\0';
}

// ... etc for all handlers ...
