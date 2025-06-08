// TODO: Implement functions to create and send WifiMgmtResponse messages.
// Example:
// void send_status_response(uint32_t request_id, bool success, int32_t error_code, const char* msg);
// void send_get_version_response(uint32_t request_id, const embedded_wifi_mgmt_WifiVersion* version_data);
// etc.
```// filepath: zephyr_wifi_handler.c
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/sys/byteorder.h> // For potential byte order conversions if needed

#include <pb_decode.h>
#include "generated/wifi.pb.h" // Path to your generated nanopb header

LOG_MODULE_REGISTER(wifi_handler, LOG_LEVEL_INF);

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


// --- Forward declarations of request handlers ---
static void handle_connect_request(const embedded_wifi_mgmt_WifiConnectRequest *req);
static void handle_disconnect_request(const embedded_wifi_mgmt_WifiDisconnectRequest *req);
static void handle_scan_request(const embedded_wifi_mgmt_WifiScanRequest *req, struct ScanRequestContext *scan_ctx);
static void handle_ap_enable_request(const embedded_wifi_mgmt_WifiApEnableRequest *req, struct ApEnableRequestContext *ap_ctx);
static void handle_ap_disable_request(const embedded_wifi_mgmt_WifiApDisableRequest *req);
static void handle_get_version_request(const embedded_wifi_mgmt_GetWifiVersionRequest *req);
static void handle_set_ps_config_request(const embedded_wifi_mgmt_SetPowerSaveConfigRequest *req);
static void handle_get_ps_config_request(const embedded_wifi_mgmt_GetPowerSaveConfigRequest *req);
static void handle_set_reg_domain_request(const embedded_wifi_mgmt_SetRegulatoryDomainRequest *req, struct RegDomainRequestContext *reg_ctx);
static void handle_get_reg_domain_request(const embedded_wifi_mgmt_GetRegulatoryDomainRequest *req);
static void handle_get_iface_status_request(const embedded_wifi_mgmt_GetInterfaceStatusRequest *req);


// --- Main processing function ---
void process_wifi_mgmt_request(uint8_t *buffer, size_t length) {
    embedded_wifi_mgmt_WifiMgmtRequest request = embedded_wifi_mgmt_WifiMgmtRequest_init_zero;
    pb_istream_t stream = pb_istream_from_buffer(buffer, length);

    // Prepare contexts for potential callbacks
    struct ScanRequestContext scan_ctx = {0};
    struct ApEnableRequestContext ap_ctx = {0};
    struct RegDomainRequestContext reg_ctx = {0};

    // --- IMPORTANT: Setup callbacks for fields that are pb_callback_t BEFORE decoding ---
    // This needs to be done based on the expected message type, or by inspecting the request_id first,
    // or by setting them on all possible callback fields in the union.
    // For simplicity here, we set them on the relevant sub-message fields.
    // Nanopb will only use them if that part of the union is being decoded.

    // For Scan Request: WifiScanParams.directed_scan_ssids
    request.payload.scan_req.params.directed_scan_ssids.funcs.decode = &decode_directed_scan_ssid_cb;
    request.payload.scan_req.params.directed_scan_ssids.arg = &scan_ctx;

    // For AP Enable Request: WifiApEnableParams.psk
    request.payload.ap_enable_req.params.psk.funcs.decode = &decode_ap_psk_cb;
    request.payload.ap_enable_req.params.psk.arg = &ap_ctx;

    // For Set Regulatory Domain Request: WifiRegDomain.country_code
    request.payload.set_reg_domain_req.reg_domain.country_code.funcs.decode = &decode_country_code_cb;
    request.payload.set_reg_domain_req.reg_domain.country_code.arg = &reg_ctx;

    if (!pb_decode(&stream, embedded_wifi_mgmt_WifiMgmtRequest_fields, &request)) {
        LOG_ERR("Failed to decode WifiMgmtRequest: %s", PB_GET_ERROR(&stream));
        // TODO: Send WifiMgmtResponse with error status
        return;
    }

    LOG_INF("Processing RequestType: %u (tag %u)", request.request_id, request.which_payload);

    switch (request.request_id) {
        case embedded_wifi_mgmt_RequestType_REQUEST_TYPE_CONNECT:
            if (request.which_payload == embedded_wifi_mgmt_WifiMgmtRequest_connect_req_tag) {
                handle_connect_request(&request.payload.connect_req);
            }
            break;
        case embedded_wifi_mgmt_RequestType_REQUEST_TYPE_DISCONNECT:
            if (request.which_payload == embedded_wifi_mgmt_WifiMgmtRequest_disconnect_req_tag) {
                handle_disconnect_request(&request.payload.disconnect_req);
            }
            break;
        case embedded_wifi_mgmt_RequestType_REQUEST_TYPE_SCAN:
            if (request.which_payload == embedded_wifi_mgmt_WifiMgmtRequest_scan_req_tag) {
                handle_scan_request(&request.payload.scan_req, &scan_ctx);
            }
            break;
        case embedded_wifi_mgmt_RequestType_REQUEST_TYPE_AP_ENABLE:
            if (request.which_payload == embedded_wifi_mgmt_WifiMgmtRequest_ap_enable_req_tag) {
                handle_ap_enable_request(&request.payload.ap_enable_req, &ap_ctx);
            }
            break;
        case embedded_wifi_mgmt_RequestType_REQUEST_TYPE_AP_DISABLE:
            if (request.which_payload == embedded_wifi_mgmt_WifiMgmtRequest_ap_disable_req_tag) {
                handle_ap_disable_request(&request.payload.ap_disable_req);
            }
            break;
        case embedded_wifi_mgmt_RequestType_REQUEST_TYPE_GET_VERSION:
             if (request.which_payload == embedded_wifi_mgmt_WifiMgmtRequest_get_version_req_tag) {
                handle_get_version_request(&request.payload.get_version_req);
            }
            break;
        case embedded_wifi_mgmt_RequestType_REQUEST_TYPE_SET_PS_CONFIG:
            if (request.which_payload == embedded_wifi_mgmt_WifiMgmtRequest_set_ps_config_req_tag) {
                handle_set_ps_config_request(&request.payload.set_ps_config_req);
            }
            break;
        case embedded_wifi_mgmt_RequestType_REQUEST_TYPE_GET_PS_CONFIG:
            if (request.which_payload == embedded_wifi_mgmt_WifiMgmtRequest_get_ps_config_req_tag) {
                handle_get_ps_config_request(&request.payload.get_ps_config_req);
            }
            break;
        case embedded_wifi_mgmt_RequestType_REQUEST_TYPE_SET_REG_DOMAIN:
            if (request.which_payload == embedded_wifi_mgmt_WifiMgmtRequest_set_reg_domain_req_tag) {
                handle_set_reg_domain_request(&request.payload.set_reg_domain_req, &reg_ctx);
            }
            break;
        case embedded_wifi_mgmt_RequestType_REQUEST_TYPE_GET_REG_DOMAIN:
            if (request.which_payload == embedded_wifi_mgmt_WifiMgmtRequest_get_reg_domain_req_tag) {
                handle_get_reg_domain_request(&request.payload.get_reg_domain_req);
            }
            break;
        case embedded_wifi_mgmt_RequestType_REQUEST_TYPE_GET_IFACE_STATUS:
            if (request.which_payload == embedded_wifi_mgmt_WifiMgmtRequest_get_iface_status_req_tag) {
                handle_get_iface_status_request(&request.payload.get_iface_status_req);
            }
            break;
        default:
            LOG_WRN("Unknown or unhandled RequestType: %d", request.request_id);
            // TODO: Send WifiMgmtResponse with error status (e.g., invalid argument)
            break;
    }
     // TODO: Implement sending WifiMgmtResponse for each request.
     // The response should include the original request.request_id.
}

// --- Helper to get net_if ---
static struct net_if *get_iface(uint32_t iface_idx) {
    // In Zephyr, interface indices usually start from 1.
    // net_if_get_by_index(0) might return NULL or loopback.
    // Adjust if your iface_idx mapping is different.
    struct net_if *iface = net_if_get_by_index(iface_idx);
    if (!iface) {
        LOG_ERR("Interface with index %u not found", iface_idx);
    }
    return iface;
}

// --- Request Handler Implementations ---

static void handle_connect_request(const embedded_wifi_mgmt_WifiConnectRequest *req) {
    LOG_INF("Handling Connect Request for iface %u, SSID: %.*s",
            req->iface_index, (int)req->ssid.size, req->ssid.bytes);

    struct net_if *iface = get_iface(req->iface_index);
    if (!iface) { /* TODO: Send error response */ return; }

    struct wifi_connect_req_params params = {0};

    if (req->ssid.size > WIFI_SSID_MAX_LEN) {
        LOG_WRN("Connect SSID too long (%u > %d)", req->ssid.size, WIFI_SSID_MAX_LEN);
        // TODO: Send error response
        return;
    }
    params.ssid = (uint8_t*)req->ssid.bytes;
    params.ssid_length = req->ssid.size;

    params.security = (enum wifi_security_type)req->security_type; // Direct cast, ensure enums match

    if (params.security == WIFI_SECURITY_TYPE_PSK ||
        params.security == WIFI_SECURITY_TYPE_PSK_SHA256 ||
        params.security == WIFI_SECURITY_TYPE_WEP) {
        if (req->psk.size > WIFI_PSK_MAX_LEN) {
             LOG_WRN("Connect PSK too long (%u > %d)", req->psk.size, WIFI_PSK_MAX_LEN);
             // TODO: Send error response
             return;
        }
        params.psk = (uint8_t*)req->psk.bytes;
        params.psk_length = req->psk.size;
    } else if (params.security == WIFI_SECURITY_TYPE_SAE) {
         if (req->sae_password.size > WIFI_SAE_PASSWORD_MAX_LEN) { // Assuming WIFI_SAE_PASSWORD_MAX_LEN exists
             LOG_WRN("Connect SAE password too long (%u > %d)", req->sae_password.size, WIFI_SAE_PASSWORD_MAX_LEN);
             // TODO: Send error response
             return;
         }
        // Zephyr's wifi_connect_req_params uses 'sae_password' field for SAE.
        params.sae_password = (uint8_t*)req->sae_password.bytes;
        params.sae_password_length = req->sae_password.size;
    }

    params.channel = (req->channel == embedded_wifi_mgmt_WifiChannelConst_WIFI_CHANNEL_ANY) ?
                     WIFI_CHANNEL_ANY : (uint8_t)req->channel;

    if (req->bssid.size > 0) {
        if (req->bssid.size != WIFI_MAC_ADDR_LEN) {
            LOG_WRN("Connect BSSID invalid length (%u != %d)", req->bssid.size, WIFI_MAC_ADDR_LEN);
            // TODO: Send error response
            return;
        }
        params.bssid = (uint8_t*)req->bssid.bytes;
    }
    params.timeout = req->timeout_ms; // Zephyr's wifi_connect_req_params.timeout is in ms.
    params.mfp = (enum wifi_mfp_options)req->mfp; // Direct cast

    int ret = net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, &params, sizeof(params));
    if (ret) {
        LOG_ERR("NET_REQUEST_WIFI_CONNECT failed: %d", ret);
    } else {
        LOG_INF("NET_REQUEST_WIFI_CONNECT initiated");
    }
    // TODO: Send WifiStatusResponse back to host.
}

static void handle_disconnect_request(const embedded_wifi_mgmt_WifiDisconnectRequest *req) {
    LOG_INF("Handling Disconnect Request for iface %u", req->iface_index);
    struct net_if *iface = get_iface(req->iface_index);
    if (!iface) { /* TODO: Send error response */ return; }

    int ret = net_mgmt(NET_REQUEST_WIFI_DISCONNECT, iface, NULL, 0);
    if (ret) {
        LOG_ERR("NET_REQUEST_WIFI_DISCONNECT failed: %d", ret);
    } else {
        LOG_INF("NET_REQUEST_WIFI_DISCONNECT initiated");
    }
    // TODO: Send WifiStatusResponse back to host.
}

static void handle_scan_request(const embedded_wifi_mgmt_WifiScanRequest *req, struct ScanRequestContext *scan_ctx) {
    LOG_INF("Handling Scan Request for iface %u", req->iface_index);
    struct net_if *iface = get_iface(req->iface_index);
    if (!iface) { /* TODO: Send error response */ return; }

    // Use wifi_scan_params_v2 for directed scan support if available and needed.
    // Otherwise, adapt to wifi_scan_params.
    // For this example, assuming wifi_scan_params (simpler, no directed scan by default in struct)
    // If using wifi_scan_params_v2, the structure is different.
    struct wifi_scan_params zephyr_scan_params = {0};
    // If you have Zephyr >= v3.2 (approx) and need directed scan, use struct wifi_scan_params_v2
    // struct wifi_scan_params_v2 zephyr_scan_params_v2 = {0};
    // For this example, we'll map to wifi_scan_params and use scan_ctx for directed scan if possible.

    if (req->has_params) {
        const embedded_wifi_mgmt_WifiScanParams *proto_params = &req->params;
        zephyr_scan_params.scan_type = (enum wifi_scan_type)proto_params->scan_type;
        // wifi_scan_params doesn't have 'band'. It's usually controlled by channels or driver capability.
        // If using wifi_scan_params_v2: zephyr_scan_params_v2.band = (enum wifi_band)proto_params->band;

        zephyr_scan_params.dwell_time_active = proto_params->dwell_time_ms;
        zephyr_scan_params.dwell_time_passive = proto_params->dwell_time_ms;

        if (proto_params->channels_count > 0) {
            if (proto_params->channels_count > WIFI_SCAN_MAX_CHANNELS) {
                 LOG_WRN("Too many channels for scan (%u > %d)", proto_params->channels_count, WIFI_SCAN_MAX_CHANNELS);
                 // TODO: Send error response or truncate
                 zephyr_scan_params.num_channels = WIFI_SCAN_MAX_CHANNELS;
            } else {
                zephyr_scan_params.num_channels = proto_params->channels_count;
            }
            for (size_t i = 0; i < zephyr_scan_params.num_channels; ++i) {
                zephyr_scan_params.channels[i] = (uint8_t)proto_params->channels[i];
            }
        } else {
            zephyr_scan_params.channels[0] = WIFI_CHANNEL_ANY;
            zephyr_scan_params.num_channels = 0; // 0 means scan all channels in current band or configured bands
        }

        // Handling directed_scan_ssids from scan_ctx (populated by callback)
        if (scan_ctx->current_ssid_count > 0) {
            // Zephyr's basic wifi_scan_params doesn't directly support multiple directed SSIDs.
            // wifi_scan_params_v2 does (via .ssids and .num_ssids fields).
            // If using wifi_scan_params_v2:
            // zephyr_scan_params_v2.ssids = scan_ctx->ssids;
            // zephyr_scan_params_v2.num_ssids = scan_ctx->current_ssid_count;
            LOG_INF("Directed scan requested for %u SSIDs (requires wifi_scan_params_v2 or custom handling)", scan_ctx->current_ssid_count);
            // For basic wifi_scan_params, you might pick the first one if your driver supports it via a single SSID field.
            if (scan_ctx->current_ssid_count > 0 && scan_ctx->ssids[0].ssid_len > 0) {
                 // zephyr_scan_params.ssid = scan_ctx->ssids[0].ssid; // if such a field exists
                 // zephyr_scan_params.ssid_len = scan_ctx->ssids[0].ssid_len;
                 LOG_WRN("Basic wifi_scan_params may not support directed scan as in proto. Using first SSID if applicable.");
            }
        }
    } else {
        // Default scan: all channels, active scan usually
        zephyr_scan_params.scan_type = WIFI_SCAN_TYPE_ACTIVE;
        zephyr_scan_params.channels[0] = WIFI_CHANNEL_ANY;
        zephyr_scan_params.num_channels = 0;
    }

    // int ret = net_mgmt(NET_REQUEST_WIFI_SCAN, iface, &zephyr_scan_params_v2, sizeof(zephyr_scan_params_v2)); // if using v2
    int ret = net_mgmt(NET_REQUEST_WIFI_SCAN, iface, &zephyr_scan_params, sizeof(zephyr_scan_params));
    if (ret) {
        LOG_ERR("NET_REQUEST_WIFI_SCAN failed: %d", ret);
    } else {
        LOG_INF("NET_REQUEST_WIFI_SCAN initiated");
    }
    // Scan results are delivered via net_mgmt events.
    // TODO: Send WifiStatusResponse to acknowledge the request.
}

static void handle_ap_enable_request(const embedded_wifi_mgmt_WifiApEnableRequest *req, struct ApEnableRequestContext *ap_ctx) {
    LOG_INF("Handling AP Enable Request for iface %u", req->iface_index);
    struct net_if *iface = get_iface(req->iface_index);
    if (!iface) { /* TODO: Send error response */ return; }

    struct wifi_ap_config ap_config = {0};

    if (!req->has_params) {
        LOG_WRN("AP Enable request with no parameters.");
        // TODO: Send error response
        return;
    }

    const embedded_wifi_mgmt_WifiApEnableParams *proto_params = &req->params;

    if (proto_params->ssid.size == 0 || proto_params->ssid.size > WIFI_SSID_MAX_LEN) {
        LOG_WRN("AP Enable SSID invalid length (%u)", proto_params->ssid.size);
        // TODO: Send error response
        return;
    }
    memcpy(ap_config.ssid, proto_params->ssid.bytes, proto_params->ssid.size);
    ap_config.ssid_len = proto_params->ssid.size;

    ap_config.channel = (uint8_t)proto_params->channel; // Note: proto_params->channel is tag 3 in pb.h, proto tag 2
    ap_config.band = (enum wifi_band)proto_params->band; // Note: proto_params->band is tag 4 in pb.h, proto tag 3
    ap_config.security = (enum wifi_security_type)proto_params->security_type; // Note: proto_params->security_type is tag 5 in pb.h, proto tag 4

    if (ap_config.security == WIFI_SECURITY_TYPE_PSK) {
        // PSK was decoded by decode_ap_psk_cb into ap_ctx
        if (ap_ctx->psk_len == 0 || ap_ctx->psk_len > WIFI_PSK_MAX_LEN) {
            LOG_WRN("AP Enable PSK invalid length from callback (%u)", ap_ctx->psk_len);
            // TODO: Send error response
            return;
        }
        memcpy(ap_config.psk, ap_ctx->psk, ap_ctx->psk_len);
        ap_config.psk_len = ap_ctx->psk_len;
    }
    // The field `proto_params->psk_length` (tag 7 in pb.h) is anomalous as it's not in the .proto.
    // We rely on the callback context (ap_ctx) for PSK data.

    ap_config.hidden_ssid = proto_params->hidden_ssid; // Note: proto_params->hidden_ssid is tag 8 in pb.h, proto tag 6
    ap_config.beacon_int = (uint16_t)proto_params->beacon_interval_tu; // Note: proto_params->beacon_interval_tu is tag 9 in pb.h, proto tag 7
    ap_config.dtim_period = (uint8_t)proto_params->dtim_period; // Note: proto_params->dtim_period is tag 10 in pb.h, proto tag 8

    // Note on WifiApEnableParams field tag mismatches:
    // The C code uses struct fields from `generated/wifi.pb.h`.
    // If the sender uses the `protos/wifi.proto` tags, there will be a mismatch.
    // e.g., sender sends channel with tag 2, receiver expects it at tag 3.

    int ret = net_mgmt(NET_REQUEST_WIFI_AP_ENABLE, iface, &ap_config, sizeof(ap_config));
    if (ret) {
        LOG_ERR("NET_REQUEST_WIFI_AP_ENABLE failed: %d", ret);
    } else {
        LOG_INF("NET_REQUEST_WIFI_AP_ENABLE initiated");
    }
    // TODO: Send WifiStatusResponse back to host.
}

static void handle_ap_disable_request(const embedded_wifi_mgmt_WifiApDisableRequest *req) {
    LOG_INF("Handling AP Disable Request for iface %u", req->iface_index);
    struct net_if *iface = get_iface(req->iface_index);
    if (!iface) { /* TODO: Send error response */ return; }

    int ret = net_mgmt(NET_REQUEST_WIFI_AP_DISABLE, iface, NULL, 0);
    if (ret) {
        LOG_ERR("NET_REQUEST_WIFI_AP_DISABLE failed: %d", ret);
    } else {
        LOG_INF("NET_REQUEST_WIFI_AP_DISABLE initiated");
    }
    // TODO: Send WifiStatusResponse back to host.
}

static void handle_get_version_request(const embedded_wifi_mgmt_GetWifiVersionRequest *req) {
    LOG_INF("Handling Get Version Request");
    // This is a custom request. Zephyr does not have a standard NET_REQUEST_WIFI_GET_VERSION.
    // Populate embedded_wifi_mgmt_WifiVersion with your driver/firmware version.
    embedded_wifi_mgmt_GetWifiVersionResponse response_payload = embedded_wifi_mgmt_GetWifiVersionResponse_init_zero;
    response_payload.has_version = true;
    response_payload.version.driver_major = 1; // Example
    response_payload.version.driver_minor = 0; // Example
    response_payload.version.driver_patch = 0; // Example
    strncpy(response_payload.version.firmware_version, "ZephyrWiFi-vX.Y.Z", sizeof(response_payload.version.firmware_version) - 1);
    response_payload.version.firmware_version[sizeof(response_payload.version.firmware_version) - 1] = '\0';

    LOG_WRN("GetVersionRequest is custom; implementation is application-specific.");
    // TODO: Encode response_payload into a WifiMgmtResponse and send back to host.
}

static void handle_set_ps_config_request(const embedded_wifi_mgmt_SetPowerSaveConfigRequest *req) {
    LOG_INF("Handling Set Power Save Config Request for iface %u", req->iface_index);
    struct net_if *iface = get_iface(req->iface_index);
    if (!iface) { /* TODO: Send error response */ return; }

    if (!req->has_config) {
        LOG_WRN("SetPowerSaveConfigRequest has no config payload.");
        // TODO: Send error response
        return;
    }

    const embedded_wifi_mgmt_WifiPowerSaveConfig *proto_cfg = &req->config;
    struct wifi_ps_config ps_zephyr_cfg = {0};

    ps_zephyr_cfg.type = (enum wifi_power_save_type)proto_cfg->type;
    if (proto_cfg->has_ps_settings) {
        const embedded_wifi_mgmt_WifiPsSettings *proto_settings = &proto_cfg->ps_settings;
        ps_zephyr_cfg.params.enabled = (enum wifi_ps_state)proto_settings->enabled;
        // Note: Zephyr's wifi_ps_params might have different fields or interpretations for wakeup_mode, listen_interval, etc.
        // This is a direct mapping attempt.
        if (ps_zephyr_cfg.params.enabled == WIFI_PS_STATE_ENABLED) {
             // Example: map wakeup mode if applicable. Zephyr's struct might not have wakeup_mode directly.
             // It might be implicit or part of 'type' or driver-specific.
             LOG_INF("PS Wakeup mode from proto: %d", proto_settings->wakeup_mode);
             ps_zephyr_cfg.params.listen_interval = proto_settings->listen_interval_beacons;
             ps_zephyr_cfg.params.timeout_ms = proto_settings->timeout_ms; // Check if Zephyr uses ms or ticks
        }
        ps_zephyr_cfg.params.fail_all_scans = proto_settings->fail_all_scans_if_ps;
    }
    // ps_zephyr_cfg.num_apsd_queues = ...; // If applicable and in proto

    int ret = net_mgmt(NET_REQUEST_WIFI_PS_CONFIG, iface, &ps_zephyr_cfg, sizeof(ps_zephyr_cfg));
    if (ret) {
        LOG_ERR("NET_REQUEST_WIFI_PS_CONFIG (Set) failed: %d", ret);
    } else {
        LOG_INF("NET_REQUEST_WIFI_PS_CONFIG (Set) initiated");
    }
    // TODO: Send WifiStatusResponse back to host.
}

static void handle_get_ps_config_request(const embedded_wifi_mgmt_GetPowerSaveConfigRequest *req) {
    LOG_INF("Handling Get Power Save Config Request for iface %u", req->iface_index);
    struct net_if *iface = get_iface(req->iface_index);
    if (!iface) { /* TODO: Send error response */ return; }

    struct wifi_ps_config ps_zephyr_cfg_resp = {0};
    // For GET, the third argument to net_mgmt is a pointer to store the result.
    int ret = net_mgmt(NET_REQUEST_WIFI_PS_CONFIG, iface, &ps_zephyr_cfg_resp, sizeof(ps_zephyr_cfg_resp));
    if (ret) {
        LOG_ERR("NET_REQUEST_WIFI_PS_CONFIG (Get) failed: %d", ret);
        // TODO: Send error response
    } else {
        LOG_INF("NET_REQUEST_WIFI_PS_CONFIG (Get) successful.");
        embedded_wifi_mgmt_GetPowerSaveConfigResponse response_payload = embedded_wifi_mgmt_GetPowerSaveConfigResponse_init_zero;
        response_payload.has_config = true;
        response_payload.config.type = (embedded_wifi_mgmt_WifiPowerSaveType)ps_zephyr_cfg_resp.type;
        response_payload.config.has_ps_settings = true;
        response_payload.config.ps_settings.enabled = (embedded_wifi_mgmt_WifiPsState)ps_zephyr_cfg_resp.params.enabled;
        // Map other fields from ps_zephyr_cfg_resp.params to response_payload.config.ps_settings
        // response_payload.config.ps_settings.wakeup_mode = ... ; // Zephyr struct might not have direct match
        response_payload.config.ps_settings.listen_interval_beacons = ps_zephyr_cfg_resp.params.listen_interval;
        response_payload.config.ps_settings.timeout_ms = ps_zephyr_cfg_resp.params.timeout_ms;
        response_payload.config.ps_settings.fail_all_scans_if_ps = ps_zephyr_cfg_resp.params.fail_all_scans;

        // TODO: Encode response_payload into a WifiMgmtResponse and send back to host.
    }
}

static void handle_set_reg_domain_request(const embedded_wifi_mgmt_SetRegulatoryDomainRequest *req, struct RegDomainRequestContext *reg_ctx) {
    LOG_INF("Handling Set Regulatory Domain Request for iface %u", req->iface_index);
    struct net_if *iface = get_iface(req->iface_index); // Reg domain might be global.

    if (!req->has_reg_domain) {
        LOG_WRN("SetRegulatoryDomainRequest has no reg_domain payload.");
        // TODO: Send error response
        return;
    }

    struct wifi_reg_domain zephyr_reg_domain = {0};
    // Country code was decoded by decode_country_code_cb into reg_ctx
    if (reg_ctx->country_code_len == 2) {
        memcpy(zephyr_reg_domain.country_code, reg_ctx->country_code, 2);
    } else if (reg_ctx->country_code_len > 0) { // Allow empty country code if that's valid for driver
        LOG_WRN("Set Reg Domain: Invalid country code length from callback (%u)", reg_ctx->country_code_len);
        // TODO: Send error response
        return;
    }
    // zephyr_reg_domain.dfs_region = WIFI_DFS_REGION_UNSET; // Or map from proto if added

    int ret = net_mgmt(NET_REQUEST_WIFI_REG_DOMAIN, iface, &zephyr_reg_domain, sizeof(zephyr_reg_domain));
     if (ret) {
        LOG_ERR("NET_REQUEST_WIFI_REG_DOMAIN (Set) failed: %d", ret);
    } else {
        LOG_INF("NET_REQUEST_WIFI_REG_DOMAIN (Set) initiated for country: %.*s", (int)reg_ctx->country_code_len, reg_ctx->country_code);
    }
    // TODO: Send WifiStatusResponse back to host.
}

static void handle_get_reg_domain_request(const embedded_wifi_mgmt_GetRegulatoryDomainRequest *req) {
    LOG_INF("Handling Get Regulatory Domain Request for iface %u", req->iface_index);
    struct net_if *iface = get_iface(req->iface_index);

    struct wifi_reg_domain zephyr_reg_domain_resp = {0};
    int ret = net_mgmt(NET_REQUEST_WIFI_REG_DOMAIN, iface, &zephyr_reg_domain_resp, sizeof(zephyr_reg_domain_resp));
    if (ret) {
        LOG_ERR("NET_REQUEST_WIFI_REG_DOMAIN (Get) failed: %d", ret);
        // TODO: Send error response
    } else {
        LOG_INF("NET_REQUEST_WIFI_REG_DOMAIN (Get) successful. Country: %c%c, DFS: %d",
                zephyr_reg_domain_resp.country_code[0], zephyr_reg_domain_resp.country_code[1],
                zephyr_reg_domain_resp.dfs_region);

        embedded_wifi_mgmt_GetRegulatoryDomainResponse response_payload = embedded_wifi_mgmt_GetRegulatoryDomainResponse_init_zero;
        response_payload.has_reg_domain = true;

        // For WifiRegDomain.country_code (string/callback in proto response):
        // This requires encoding the string using a callback if it's defined as pb_callback_t in the response.
        // If GetRegulatoryDomainResponse.reg_domain.country_code is pb_callback_t for encoding:
        // response_payload.reg_domain.country_code.funcs.encode = &encode_country_code_str_cb; // Need an ENCODE callback
        // response_payload.reg_domain.country_code.arg = &zephyr_reg_domain_resp.country_code; // Pass pointer to the char array[2]
        // For now, assuming it's a fixed char array in proto for response, or you handle encoding.
        // Since WifiRegDomain.country_code is pb_callback_t for decoding, it's likely the same for encoding.
        // This part is complex for sending back. A simpler GetRegulatoryDomainResponse might avoid nested callbacks.
        LOG_WRN("Sending country_code in GetRegulatoryDomainResponse requires an ENCODE callback if it's pb_callback_t.");

        // TODO: Encode response_payload into a WifiMgmtResponse and send back to host.
        // This will be tricky if country_code is a callback for encoding.
    }
}

static void handle_get_iface_status_request(const embedded_wifi_mgmt_GetInterfaceStatusRequest *req) {
    LOG_INF("Handling Get Interface Status Request for iface %u", req->iface_index);
    struct net_if *iface = get_iface(req->iface_index);
    if (!iface) { /* TODO: Send error response */ return; }

    struct wifi_iface_status zephyr_status_resp = {0};
    int ret = net_mgmt(NET_REQUEST_WIFI_IFACE_STATUS, iface, &zephyr_status_resp, sizeof(zephyr_status_resp));

    if (ret) {
        LOG_ERR("NET_REQUEST_WIFI_IFACE_STATUS failed: %d", ret);
        // TODO: Send error response
    } else {
        LOG_INF("NET_REQUEST_WIFI_IFACE_STATUS successful. State: %d, SSID: %.*s",
                zephyr_status_resp.state, zephyr_status_resp.ssid_len, zephyr_status_resp.ssid);

        embedded_wifi_mgmt_GetInterfaceStatusResponse response_payload = embedded_wifi_mgmt_GetInterfaceStatusResponse_init_zero;
        response_payload.has_status = true;

        // Map from zephyr_status_resp to response_payload.status
        response_payload.status.state = (embedded_wifi_mgmt_WifiInterfaceState)zephyr_status_resp.state;
        response_payload.status.mode = (embedded_wifi_mgmt_WifiMode)zephyr_status_resp.iface_mode; // Note: field name diff

        if (zephyr_status_resp.ssid_len > 0 && zephyr_status_resp.ssid_len <= sizeof(response_payload.status.ssid.bytes)) {
            response_payload.status.ssid.size = zephyr_status_resp.ssid_len;
            memcpy(response_payload.status.ssid.bytes, zephyr_status_resp.ssid, zephyr_status_resp.ssid_len);
        }
        if (zephyr_status_resp.bssid_len == WIFI_MAC_ADDR_LEN) { // bssid_len might not exist, bssid is fixed array
             response_payload.status.bssid.size = WIFI_MAC_ADDR_LEN;
             memcpy(response_payload.status.bssid.bytes, zephyr_status_resp.bssid, WIFI_MAC_ADDR_LEN);
        }
        response_payload.status.channel = zephyr_status_resp.channel;
        response_payload.status.security = (embedded_wifi_mgmt_WifiSecurityType)zephyr_status_resp.security;
        response_payload.status.mfp = (embedded_wifi_mgmt_WifiMfpOptions)zephyr_status_resp.mfp;
        response_payload.status.rssi = zephyr_status_resp.rssi;
        response_payload.status.link_mode = (embedded_wifi_mgmt_WifiLinkMode)zephyr_status_resp.link_mode; // Check if this field exists
        response_payload.status.ht_mcs = zephyr_status_resp.ht_mcs;
        response_payload.status.vht_mcs = zephyr_status_resp.vht_mcs;
        response_payload.status.he_mcs = zephyr_status_resp.he_mcs; // Check if this field exists
        response_payload.status.tx_bitrate_kbps = sys_cpu_to_le32(zephyr_status_resp.tx_bitrate); // Assuming tx_bitrate is in kbps and needs LE conversion if from network
        response_payload.status.rx_bitrate_kbps = sys_cpu_to_le32(zephyr_status_resp.rx_bitrate); // Assuming rx_bitrate is in kbps

        // TODO: Encode response_payload into a WifiMgmtResponse and send back to host.
    }
}

// TODO: Implement functions to create and send WifiMgmtResponse messages.
// Example:
// void send_status_response(uint32_t request_id, bool success, int32_t error_code, const char* msg);
// void send_get_version_response(uint32_t request_id, const embedded_wifi_mgmt_WifiVersion* version_data);
// etc.