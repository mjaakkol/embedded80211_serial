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

#define WIFI_SCAN_MAX_CHANNELS 11

#define WIFI_SCAN_MAX_SSIDS 4

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

static void handle_disconnect_request(const embedded_wifi_mgmt_WifiDisconnectRequest *req_payload,
                                      embedded_wifi_mgmt_WifiStatusResponse *status_resp_payload) {
    status_resp_payload->success = false;
    status_resp_payload->error_code = -EIO;
    set_status_error_msg(status_resp_payload, "Disconnect handler init error");

    LOG_INF("Handling Disconnect Request for iface %u", req_payload->iface_index);
    struct net_if *iface = net_if_get_by_index(req_payload->iface_index);
    if (!iface) {
        LOG_ERR("Interface with index %u not found", req_payload->iface_index);
        status_resp_payload->error_code = -ENODEV;
        set_status_error_msg(status_resp_payload, "Interface not found");
        return;
    }

    int ret = net_mgmt(NET_REQUEST_WIFI_DISCONNECT, iface, NULL, 0);
    if (ret) {
        LOG_ERR("NET_REQUEST_WIFI_DISCONNECT failed: %d", ret);
        status_resp_payload->error_code = ret;
        set_status_error_msg(status_resp_payload, "Wi-Fi disconnect request failed");
    } else {
        LOG_INF("NET_REQUEST_WIFI_DISCONNECT initiated");
        status_resp_payload->success = true;
        status_resp_payload->error_code = 0;
        set_status_error_msg(status_resp_payload, "Disconnect initiated");
    }
}

static void handle_scan_request(const embedded_wifi_mgmt_WifiScanRequest *req_payload,
                                struct ScanRequestContext *scan_ctx, // Contains decoded directed_scan_ssids
                                embedded_wifi_mgmt_WifiStatusResponse *status_resp_payload) {
    status_resp_payload->success = false;
    status_resp_payload->error_code = -EIO;
    set_status_error_msg(status_resp_payload, "Scan handler init error");

    LOG_INF("Handling Scan Request for iface %u", req_payload->iface_index);
    struct net_if *iface = net_if_get_by_index(req_payload->iface_index);
    if (!iface) {
        LOG_ERR("Interface with index %u not found for scan", req_payload->iface_index);
        status_resp_payload->error_code = -ENODEV;
        set_status_error_msg(status_resp_payload, "Interface not found for scan");
        return;
    }

    // Use wifi_scan_params_v2 for more features like directed scan
    struct wifi_scan_params params_v2 = {0};
    struct wifi_scan_init_params init_params = {0}; // For basic scan if params not provided

    if (req_payload->has_params) {
        const embedded_wifi_mgmt_WifiScanParams *proto_params = &req_payload->params;
        params_v2.scan_type = (enum wifi_scan_type)proto_params->scan_type;
        params_v2.band = (enum wifi_band)proto_params->band;
        params_v2.dwell_time_active = proto_params->dwell_time_active; // Assuming dwell_time_ms applies to active
        params_v2.dwell_time_passive_ms = proto_params->dwell_time_ms; // And passive, adjust if proto has separate

        if (proto_params->channels_count > 0) {
            if (proto_params->channels_count > WIFI_SCAN_MAX_CHANNELS) {
                LOG_WRN("Too many scan channels (%u > %d)", proto_params->channels_count, WIFI_SCAN_MAX_CHANNELS);
                status_resp_payload->error_code = -EINVAL;
                set_status_error_msg(status_resp_payload, "Too many scan channels");
                return;
            }
            params_v2.channel_count = proto_params->channels_count;
            for (uint32_t i = 0; i < proto_params->channels_count; i++) {
                params_v2.channels[i] = (uint8_t)proto_params->channels[i];
            }
        }

        if (scan_ctx->current_ssid_count > 0) {
            if (scan_ctx->current_ssid_count > WIFI_SCAN_MAX_SSIDS) {
                 LOG_WRN("Too many directed scan SSIDs (%u > %d)", scan_ctx->current_ssid_count, WIFI_SCAN_MAX_SSIDS);
                 status_resp_payload->error_code = -EINVAL;
                 set_status_error_msg(status_resp_payload, "Too many directed scan SSIDs");
                 return;
            }
            params_v2.ssids = scan_ctx->ssids; // scan_ctx->ssids is array of wifi_scan_ssid_spec
            params_v2.ssid_count = scan_ctx->current_ssid_count;
        }
        // params_v2.max_bss_cnt = 0; // 0 for no limit or driver default
    } else {
        // Default scan if no params provided in request
        init_params.max_bss_cnt = 0; // Driver default
        // init_params.scan_type can be set if needed, or let driver decide
    }

    int ret;
    if (req_payload->has_params) {
        ret = net_mgmt(NET_REQUEST_WIFI_SCAN, iface, &params_v2, sizeof(params_v2));
    } else {
        // Fallback to basic scan if params_v2 is not suitable or not fully populated
        // This part might need adjustment based on how you want to handle default scans.
        // For simplicity, let's assume if params are present, v2 is used.
        // If not, a very basic scan is initiated.
        // Consider if NET_REQUEST_WIFI_SCAN with NULL params is valid for default scan.
        // Zephyr's wifi_shell uses NULL for default scan.
        ret = net_mgmt(NET_REQUEST_WIFI_SCAN, iface, NULL, 0);
    }


    if (ret) {
        LOG_ERR("NET_REQUEST_WIFI_SCAN failed: %d", ret);
        status_resp_payload->error_code = ret;
        set_status_error_msg(status_resp_payload, "Wi-Fi scan request failed");
    } else {
        LOG_INF("NET_REQUEST_WIFI_SCAN initiated");
        status_resp_payload->success = true;
        status_resp_payload->error_code = 0;
        set_status_error_msg(status_resp_payload, "Scan initiated");
    }
}

static void handle_ap_enable_request(const embedded_wifi_mgmt_WifiApEnableRequest *req_payload,
                                     struct ApEnableRequestContext *ap_ctx, // Contains decoded PSK
                                     embedded_wifi_mgmt_WifiStatusResponse *status_resp_payload) {
    status_resp_payload->success = false;
    status_resp_payload->error_code = -EIO;
    set_status_error_msg(status_resp_payload, "AP enable handler init error");

    LOG_INF("Handling AP Enable Request for iface %u", req_payload->iface_index);
    struct net_if *iface = net_if_get_by_index(req_payload->iface_index);
    if (!iface) {
        LOG_ERR("Interface with index %u not found for AP enable", req_payload->iface_index);
        status_resp_payload->error_code = -ENODEV;
        set_status_error_msg(status_resp_payload, "Interface not found for AP enable");
        return;
    }

    if (!req_payload->has_params) {
        LOG_ERR("AP Enable parameters missing");
        status_resp_payload->error_code = -EINVAL;
        set_status_error_msg(status_resp_payload, "AP parameters missing");
        return;
    }
    const embedded_wifi_mgmt_WifiApEnableParams *proto_params = &req_payload->params;
    struct wifi_ap_param params = {0};

    if (proto_params->ssid.size == 0 || proto_params->ssid.size > WIFI_SSID_MAX_LEN) {
        LOG_ERR("Invalid AP SSID length: %u", proto_params->ssid.size);
        status_resp_payload->error_code = -EINVAL;
        set_status_error_msg(status_resp_payload, "Invalid AP SSID length");
        return;
    }
    memcpy(params.ssid, proto_params->ssid.bytes, proto_params->ssid.size);
    params.ssid_len = proto_params->ssid.size;

    params.band = (enum wifi_band)proto_params->band;
    params.channel = (uint8_t)proto_params->channel;
    if (params.channel == embedded_wifi_mgmt_WifiChannelConst_WIFI_CHANNEL_ANY) {
        params.channel = WIFI_CHANNEL_ANY; // Or a default AP channel e.g. 1, 6, 11 for 2.4GHz
    }

    params.security = (enum wifi_security_type)proto_params->security_type;
    if (params.security == WIFI_SECURITY_TYPE_PSK) {
        if (ap_ctx->psk_len == 0 || ap_ctx->psk_len > WIFI_PSK_MAX_LEN) {
            LOG_ERR("Invalid AP PSK length: %zu", ap_ctx->psk_len);
            status_resp_payload->error_code = -EINVAL;
            set_status_error_msg(status_resp_payload, "Invalid AP PSK length");
            return;
        }
        memcpy(params.psk, ap_ctx->psk, ap_ctx->psk_len);
        params.psk_len = ap_ctx->psk_len;
    } else if (params.security != WIFI_SECURITY_TYPE_NONE && params.security != WIFI_SECURITY_TYPE_OWE) {
        // Add other security types if supported for AP mode
        LOG_WRN("Unsupported AP security type: %d", params.security);
        status_resp_payload->error_code = -ENOTSUP;
        set_status_error_msg(status_resp_payload, "Unsupported AP security type");
        return;
    }

    params.hidden_ssid = proto_params->hidden_ssid;
    params.beacon_interval = proto_params->beacon_interval_tu;
    params.dtim_period = proto_params->dtim_period;
    // params.mfp = (enum wifi_mfp_options)proto_params->mfp; // If mfp is added to WifiApEnableParams proto

    int ret = net_mgmt(NET_REQUEST_WIFI_AP_ENABLE, iface, &params, sizeof(params));
    if (ret) {
        LOG_ERR("NET_REQUEST_WIFI_AP_ENABLE failed: %d", ret);
        status_resp_payload->error_code = ret;
        set_status_error_msg(status_resp_payload, "Wi-Fi AP enable request failed");
    } else {
        LOG_INF("NET_REQUEST_WIFI_AP_ENABLE initiated");
        status_resp_payload->success = true;
        status_resp_payload->error_code = 0;
        set_status_error_msg(status_resp_payload, "AP enable initiated");
    }
}

static void handle_ap_disable_request(const embedded_wifi_mgmt_WifiApDisableRequest *req_payload,
                                      embedded_wifi_mgmt_WifiStatusResponse *status_resp_payload) {
    status_resp_payload->success = false;
    status_resp_payload->error_code = -EIO;
    set_status_error_msg(status_resp_payload, "AP disable handler init error");

    LOG_INF("Handling AP Disable Request for iface %u", req_payload->iface_index);
    struct net_if *iface = net_if_get_by_index(req_payload->iface_index);
    if (!iface) {
        LOG_ERR("Interface with index %u not found for AP disable", req_payload->iface_index);
        status_resp_payload->error_code = -ENODEV;
        set_status_error_msg(status_resp_payload, "Interface not found for AP disable");
        return;
    }

    int ret = net_mgmt(NET_REQUEST_WIFI_AP_DISABLE, iface, NULL, 0);
    if (ret) {
        LOG_ERR("NET_REQUEST_WIFI_AP_DISABLE failed: %d", ret);
        status_resp_payload->error_code = ret;
        set_status_error_msg(status_resp_payload, "Wi-Fi AP disable request failed");
    } else {
        LOG_INF("NET_REQUEST_WIFI_AP_DISABLE initiated");
        status_resp_payload->success = true;
        status_resp_payload->error_code = 0;
        set_status_error_msg(status_resp_payload, "AP disable initiated");
    }
}

static void handle_get_version_request(const embedded_wifi_mgmt_GetWifiVersionRequest *req_payload,
                                       embedded_wifi_mgmt_GetWifiVersionResponse *version_resp_payload) {
    LOG_INF("Handling Get Version Request");
    // Zephyr does not have a direct net_mgmt call for a structured version.
    // This would typically be hardcoded or fetched from driver-specific means.
    // For this example, we'll use placeholder values.
    version_resp_payload->has_version = true;
    version_resp_payload->version.driver_major = 1; // Example
    version_resp_payload->version.driver_minor = 0; // Example
    version_resp_payload->version.driver_patch = 0; // Example

    const char *fw_ver_str = "ZephyrWiFi-Sim-v1.0.0"; // Example
    size_t fw_len = strlen(fw_ver_str);
    if (fw_len >= sizeof(version_resp_payload->version.firmware_version.bytes)) {
        fw_len = sizeof(version_resp_payload->version.firmware_version.bytes) - 1;
    }
    memcpy(version_resp_payload->version.firmware_version.bytes, fw_ver_str, fw_len);
    version_resp_payload->version.firmware_version.size = fw_len;
}

static void handle_set_ps_config_request(const embedded_wifi_mgmt_SetPowerSaveConfigRequest *req_payload,
                                         embedded_wifi_mgmt_WifiStatusResponse *status_resp_payload) {
    status_resp_payload->success = false;
    status_resp_payload->error_code = -EIO;
    set_status_error_msg(status_resp_payload, "Set PS config handler init error");

    LOG_INF("Handling Set Power Save Config Request for iface %u", req_payload->iface_index);
    struct net_if *iface = net_if_get_by_index(req_payload->iface_index);
    if (!iface) {
        LOG_ERR("Interface with index %u not found for PS config", req_payload->iface_index);
        status_resp_payload->error_code = -ENODEV;
        set_status_error_msg(status_resp_payload, "Interface not found for PS config");
        return;
    }

    if (!req_payload->has_config) {
        LOG_ERR("Set PS Config: config data missing");
        status_resp_payload->error_code = -EINVAL;
        set_status_error_msg(status_resp_payload, "PS config data missing");
        return;
    }

    struct wifi_ps_config params = {0};
    params.ps_params.enabled = (enum wifi_ps_state)req_payload->config.ps_settings.enabled;
    params.ps_params.wakeup_mode = (enum wifi_ps_wake_up_mode)req_payload->config.ps_settings.wakeup_mode;
    params.ps_params.listen_interval = req_payload->config.ps_settings.listen_interval_beacons;
    params.ps_params.timeout_ms = req_payload->config.ps_settings.timeout_ms;
    // params.ps_params.fail_all_scans_if_ps = req_payload->config.ps_settings.fail_all_scans_if_ps; // This field might not be in older Zephyr wifi_ps_params
    params.type = (enum wifi_power_save_type)req_payload->config.type;
    // params.num_apsd_queues = req_payload->config.num_apsd_queues; // If added to proto

    int ret = net_mgmt(NET_REQUEST_WIFI_PS_CONFIG, iface, &params, sizeof(params));
    if (ret) {
        LOG_ERR("NET_REQUEST_WIFI_PS_CONFIG (SET) failed: %d", ret);
        status_resp_payload->error_code = ret;
        set_status_error_msg(status_resp_payload, "Set PS config failed");
    } else {
        LOG_INF("NET_REQUEST_WIFI_PS_CONFIG (SET) success");
        status_resp_payload->success = true;
        status_resp_payload->error_code = 0;
        set_status_error_msg(status_resp_payload, "Set PS config success");
    }
}

static void handle_get_ps_config_request(const embedded_wifi_mgmt_GetPowerSaveConfigRequest *req_payload,
                                         embedded_wifi_mgmt_GetPowerSaveConfigResponse *ps_config_resp_payload,
                                         embedded_wifi_mgmt_WifiStatusResponse *status_resp_for_error) {
    ps_config_resp_payload->has_config = false; // Default to no data
    status_resp_for_error->success = false;
    status_resp_for_error->error_code = -EIO;
    set_status_error_msg(status_resp_for_error, "Get PS config handler init error");

    LOG_INF("Handling Get Power Save Config Request for iface %u", req_payload->iface_index);
    struct net_if *iface = net_if_get_by_index(req_payload->iface_index);
    if (!iface) {
        LOG_ERR("Interface with index %u not found for PS config", req_payload->iface_index);
        status_resp_for_error->error_code = -ENODEV;
        set_status_error_msg(status_resp_for_error, "Interface not found for PS config");
        return;
    }

    struct wifi_ps_config params_retrieved = {0};
    int ret = net_mgmt(NET_REQUEST_WIFI_PS_CONFIG, iface, &params_retrieved, sizeof(params_retrieved)); // Pass buffer to get data

    if (ret) {
        LOG_ERR("NET_REQUEST_WIFI_PS_CONFIG (GET) failed: %d", ret);
        status_resp_for_error->error_code = ret;
        set_status_error_msg(status_resp_for_error, "Get PS config failed");
    } else {
        LOG_INF("NET_REQUEST_WIFI_PS_CONFIG (GET) success");
        ps_config_resp_payload->has_config = true;
        ps_config_resp_payload->config.ps_settings.enabled = (embedded_wifi_mgmt_WifiPsState)params_retrieved.ps_params.enabled;
        ps_config_resp_payload->config.ps_settings.wakeup_mode = (embedded_wifi_mgmt_WifiPsWakeupMode)params_retrieved.ps_params.wakeup_mode;
        ps_config_resp_payload->config.ps_settings.listen_interval_beacons = params_retrieved.ps_params.listen_interval;
        ps_config_resp_payload->config.ps_settings.timeout_ms = params_retrieved.ps_params.timeout_ms;
        // ps_config_resp_payload->config.ps_settings.fail_all_scans_if_ps = params_retrieved.ps_params.fail_all_scans_if_ps; // If field exists
        ps_config_resp_payload->config.type = (embedded_wifi_mgmt_WifiPowerSaveType)params_retrieved.type;
        // ps_config_resp_payload->config.num_apsd_queues = params_retrieved.num_apsd_queues; // If field exists

        // Indicate success through the main response path
        status_resp_for_error->success = true; // Not strictly needed if has_config is true
        status_resp_for_error->error_code = 0;
    }
}

static void handle_set_reg_domain_request(const embedded_wifi_mgmt_SetRegulatoryDomainRequest *req_payload,
                                          struct RegDomainRequestContext *reg_ctx, // Contains decoded country_code
                                          embedded_wifi_mgmt_WifiStatusResponse *status_resp_payload) {
    status_resp_payload->success = false;
    status_resp_payload->error_code = -EIO;
    set_status_error_msg(status_resp_payload, "Set RegDomain handler init error");

    LOG_INF("Handling Set Regulatory Domain Request for iface %u, Country: %s",
            req_payload->iface_index, reg_ctx->country_code_len > 0 ? reg_ctx->country_code : "N/A");

    struct net_if *iface = net_if_get_by_index(req_payload->iface_index);
    // Note: Regulatory domain might be global rather than per-interface for some drivers.
    // Zephyr's API takes an iface, so we'll use it.
    if (!iface && req_payload->iface_index != 0) { // Allow iface_index 0 for global if driver supports
        LOG_ERR("Interface with index %u not found for RegDomain", req_payload->iface_index);
        status_resp_payload->error_code = -ENODEV;
        set_status_error_msg(status_resp_payload, "Interface not found for RegDomain");
        return;
    }


    if (!req_payload->has_reg_domain || reg_ctx->country_code_len == 0) {
        LOG_ERR("Set RegDomain: reg_domain data or country code missing");
        status_resp_payload->error_code = -EINVAL;
        set_status_error_msg(status_resp_payload, "RegDomain data or country code missing");
        return;
    }

    struct wifi_reg_domain reg_domain_param = {0};
    if (reg_ctx->country_code_len != 2) {
        LOG_ERR("Invalid country code length: %zu (expected 2)", reg_ctx->country_code_len);
        status_resp_payload->error_code = -EINVAL;
        set_status_error_msg(status_resp_payload, "Invalid country code length");
        return;
    }
    memcpy(reg_domain_param.country_code, reg_ctx->country_code, 2);
    // reg_domain_param.dfs_region can be set if provided in proto

    int ret = net_mgmt(NET_REQUEST_WIFI_REG_DOMAIN, iface, &reg_domain_param, sizeof(reg_domain_param));
    if (ret) {
        LOG_ERR("NET_REQUEST_WIFI_REG_DOMAIN (SET) failed: %d", ret);
        status_resp_payload->error_code = ret;
        set_status_error_msg(status_resp_payload, "Set RegDomain failed");
    } else {
        LOG_INF("NET_REQUEST_WIFI_REG_DOMAIN (SET) success");
        status_resp_payload->success = true;
        status_resp_payload->error_code = 0;
        set_status_error_msg(status_resp_payload, "Set RegDomain success");
    }
}

static void handle_get_reg_domain_request(const embedded_wifi_mgmt_GetRegulatoryDomainRequest *req_payload,
                                          embedded_wifi_mgmt_GetRegulatoryDomainResponse *reg_domain_resp_payload,
                                          embedded_wifi_mgmt_WifiStatusResponse *status_resp_for_error) {
    reg_domain_resp_payload->has_reg_domain = false;
    status_resp_for_error->success = false;
    status_resp_for_error->error_code = -EIO;
    set_status_error_msg(status_resp_for_error, "Get RegDomain handler init error");

    LOG_INF("Handling Get Regulatory Domain Request for iface %u", req_payload->iface_index);
    struct net_if *iface = net_if_get_by_index(req_payload->iface_index);
    if (!iface && req_payload->iface_index != 0) {
        LOG_ERR("Interface with index %u not found for RegDomain", req_payload->iface_index);
        status_resp_for_error->error_code = -ENODEV;
        set_status_error_msg(status_resp_for_error, "Interface not found for RegDomain");
        return;
    }

    struct wifi_reg_domain reg_domain_retrieved = {0};
    // Pass buffer to get data. If iface is NULL, it might fetch global.
    int ret = net_mgmt(NET_REQUEST_WIFI_REG_DOMAIN, iface, &reg_domain_retrieved, sizeof(reg_domain_retrieved));

    if (ret) {
        LOG_ERR("NET_REQUEST_WIFI_REG_DOMAIN (GET) failed: %d", ret);
        status_resp_for_error->error_code = ret;
        set_status_error_msg(status_resp_for_error, "Get RegDomain failed");
    } else {
        LOG_INF("NET_REQUEST_WIFI_REG_DOMAIN (GET) success. Country: %c%c",
                reg_domain_retrieved.country_code[0], reg_domain_retrieved.country_code[1]);
        reg_domain_resp_payload->has_reg_domain = true;

        // Assuming reg_domain.country_code is pb_bytes_array_t
        if (sizeof(reg_domain_resp_payload->reg_domain.country_code.bytes) >= 2) {
            reg_domain_resp_payload->reg_domain.country_code.bytes[0] = reg_domain_retrieved.country_code[0];
            reg_domain_resp_payload->reg_domain.country_code.bytes[1] = reg_domain_retrieved.country_code[1];
            reg_domain_resp_payload->reg_domain.country_code.size = 2;
        } else {
            LOG_WRN("RegDomain response country_code buffer too small");
            reg_domain_resp_payload->reg_domain.country_code.size = 0;
        }
        // Map other fields like dfs_region if they are in your proto
        status_resp_for_error->success = true;
        status_resp_for_error->error_code = 0;
    }
}

static void handle_get_iface_status_request(const embedded_wifi_mgmt_GetInterfaceStatusRequest *req_payload,
                                            embedded_wifi_mgmt_GetInterfaceStatusResponse *iface_status_resp_payload,
                                            embedded_wifi_mgmt_WifiStatusResponse *status_resp_for_error) {
    iface_status_resp_payload->has_status = false;
    status_resp_for_error->success = false;
    status_resp_for_error->error_code = -EIO;
    set_status_error_msg(status_resp_for_error, "Get IfaceStatus handler init error");

    LOG_INF("Handling Get Interface Status Request for iface %u", req_payload->iface_index);
    struct net_if *iface = net_if_get_by_index(req_payload->iface_index);
    if (!iface) {
        LOG_ERR("Interface with index %u not found for status", req_payload->iface_index);
        status_resp_for_error->error_code = -ENODEV;
        set_status_error_msg(status_resp_for_error, "Interface not found for status");
        return;
    }

    struct wifi_iface_status status_retrieved = {0};
    int ret = net_mgmt(NET_REQUEST_WIFI_IFACE_STATUS, iface, &status_retrieved, sizeof(status_retrieved));

    if (ret) {
        LOG_ERR("NET_REQUEST_WIFI_IFACE_STATUS failed: %d", ret);
        status_resp_for_error->error_code = ret;
        set_status_error_msg(status_resp_for_error, "Get IfaceStatus failed");
    } else {
        LOG_INF("NET_REQUEST_WIFI_IFACE_STATUS success. State: %d, SSID: %.*s",
                status_retrieved.state, status_retrieved.ssid_len, status_retrieved.ssid);
        iface_status_resp_payload->has_status = true;
        embedded_wifi_mgmt_WifiInterfaceStatus *proto_status = &iface_status_resp_payload->status;

        proto_status->state = (embedded_wifi_mgmt_WifiInterfaceState)status_retrieved.state;
        proto_status->mode = (embedded_wifi_mgmt_WifiMode)status_retrieved.iface_mode; // Map iface_mode to WifiMode

        if (status_retrieved.ssid_len > 0 && status_retrieved.ssid_len <= sizeof(proto_status->ssid.bytes)) {
            memcpy(proto_status->ssid.bytes, status_retrieved.ssid, status_retrieved.ssid_len);
            proto_status->ssid.size = status_retrieved.ssid_len;
        } else {
            proto_status->ssid.size = 0;
        }

        if (status_retrieved.state >= WIFI_STATE_ASSOCIATED && status_retrieved.state <= WIFI_STATE_CONNECTED) {
             if (sizeof(proto_status->bssid.bytes) >= WIFI_MAC_ADDR_LEN) {
                memcpy(proto_status->bssid.bytes, status_retrieved.bssid, WIFI_MAC_ADDR_LEN);
                proto_status->bssid.size = WIFI_MAC_ADDR_LEN;
             } else {
                proto_status->bssid.size = 0;
             }
        } else {
            proto_status->bssid.size = 0;
        }

        proto_status->channel = status_retrieved.channel;
        proto_status->security = (embedded_wifi_mgmt_WifiSecurityType)status_retrieved.security;
        proto_status->mfp = (embedded_wifi_mgmt_WifiMfpOptions)status_retrieved.mfp;
        proto_status->rssi = status_retrieved.rssi;
        proto_status->link_mode = (embedded_wifi_mgmt_WifiLinkMode)status_retrieved.link_mode; // Map link_mode
        proto_status->ht_mcs = status_retrieved.ht_mcs;
        proto_status->vht_mcs = status_retrieved.vht_mcs;
        proto_status->he_mcs = status_retrieved.he_mcs; // If available in Zephyr's struct
        proto_status->tx_bitrate_kbps = status_retrieved.tx_bitrate; // Assuming Zephyr provides in kbps
        proto_status->rx_bitrate_kbps = status_retrieved.rx_bitrate; // Assuming Zephyr provides in kbps

        status_resp_for_error->success = true;
        status_resp_for_error->error_code = 0;
    }
}
