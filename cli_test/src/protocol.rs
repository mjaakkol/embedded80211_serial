//! Protocol definitions for the the wire protocol used between host and device.
//!

use bytes::{BytesMut, BufMut};
use prost::Message;
use network_interface::{
    net_if_request, net_if_response, NetIfRequest, NetIfResponse, NetIfType, RequestInterfaces,
};
use wifi_mgmt::{
    wifi_event, wifi_mgmt_request, wifi_mgmt_response, GetWifiVersionRequest, RequestType,
    WifiConnectRequest, WifiMgmtNotification, WifiMgmtRequest, WifiMgmtResponse,
    WifiMfpOptions, WifiScanRequest, WifiSecurityType,
};

pub mod network_interface {
    include!(concat!(env!("OUT_DIR"), "/embedded_wifi_mgmt.rs"));
}

pub mod wifi_mgmt {
    include!(concat!(env!("OUT_DIR"), "/embedded_wifi_mgmt.rs"));
}



enum SerialType {
    WiFiMgmt,
    WiFiData,
    NetIfMgmt,
    HCI,
    Thread,
    AtCmd,
    Unknown,
}

impl SerialType {
    #[inline]
    fn to_u8(&self) -> u8 {
        match self {
            SerialType::WiFiMgmt => 0x01,
            SerialType::WiFiData => 0x02,
            SerialType::NetIfMgmt => 0x02,
            SerialType::HCI => 0x03,
            SerialType::Thread => 0x04,
            SerialType::AtCmd => 0x05,
            SerialType::Unknown => panic!("Unknown SerialType cannot be converted to u32"),
        }
    }
}


fn create_header(stype: SerialType, no_ack: bool, length: u16) -> BytesMut {
    let mut header = BytesMut::with_capacity(4 + length as usize);

    header.put_u8(
        stype.to_u8() << 4 | (no_ack as u8) << 3,
    );
    header.put_u8(0); // reserved byte
    header.put_u8(length as u8);
    header.put_u8((length >> 8) as u8); // length high byte

    header
}

fn encode_wifi_mgmt_request(request: WifiMgmtRequest) -> BytesMut {
    let payload_len = request.encoded_len().try_into().unwrap();
    let mut frame = create_header(SerialType::WiFiMgmt, false, payload_len);
    request.encode(&mut frame).unwrap();
    frame
}

fn encode_net_if_request(request: NetIfRequest) -> BytesMut {
    let payload_len = request.encoded_len().try_into().unwrap();
    let mut frame = create_header(SerialType::NetIfMgmt, false, payload_len);
    request.encode(&mut frame).unwrap();
    frame
}

pub fn create_version_query() -> BytesMut {
    let request = WifiMgmtRequest {
        request_id: RequestType::GetWifiVersion as i32,
        payload: Some(wifi_mgmt_request::Payload::GetVersionReq(
            GetWifiVersionRequest {},
        )),
    };

    encode_wifi_mgmt_request(request)
}

pub fn create_scan_query(iface_index: u32) -> BytesMut {
    let request = WifiMgmtRequest {
        request_id: RequestType::Scan as i32,
        payload: Some(wifi_mgmt_request::Payload::ScanReq(WifiScanRequest {
            iface_index,
            params: None,
        })),
    };

    encode_wifi_mgmt_request(request)
}

pub fn create_connect_query(
    iface_index: u32,
    ssid: &str,
    psk: Option<&str>,
    security_type: WifiSecurityType,
    timeout_ms: u32,
) -> BytesMut {
    let key = psk.unwrap_or_default().as_bytes().to_vec();
    let request = WifiMgmtRequest {
        request_id: RequestType::Connect as i32,
        payload: Some(wifi_mgmt_request::Payload::ConnectReq(WifiConnectRequest {
            iface_index,
            ssid: ssid.as_bytes().to_vec(),
            security_type: security_type as i32,
            psk: key.clone(),
            sae_password: key,
            channel: 0,
            bssid: vec![],
            timeout_ms,
            mfp: WifiMfpOptions::WifiMfpOptional as i32,
            band: 0,
            anonymous_id: vec![],
            key_password: vec![],
            key2_password: vec![],
            wpa3_enterprise_type: 0,
            tls_cipher_suite: 0,
            eap_version: 0,
            eap_identity: vec![],
            eap_password: vec![],
            ft_enabled: false,
            ignore_broadcast_ssid: false,
            bandwidth: 0,
        })),
    };

    encode_wifi_mgmt_request(request)
}

pub fn create_interfaces_query(net_if_type: NetIfType, index: u32) -> BytesMut {
    let request = NetIfRequest {
        request_id: 1,
        payload: Some(net_if_request::Payload::GetInterfaces(RequestInterfaces {
            net_if_type: net_if_type as i32,
            index,
        })),
    };

    encode_net_if_request(request)
}

pub fn parse_version_response(data: &[u8]) -> String {
    let stype = (data[0] >> 4) & 0x0F;
    let no_ack = (data[0] >> 3) & 0x01;
    let host = (data[0] >> 2) & 0x01;
    let error = (data[0] >> 0) & 0x03;
    let _sequence_number = (data[1] >> 5) & 0x07;
    let length = ((data[1] & 0x1F) as u16) << 8 | (data[2] as u16);

    if stype != SerialType::WiFiMgmt.to_u8() {
        return "Invalid response type".into();
    }

    if no_ack != 0 {
        println!("Unexpected no_ack in response");
    }

    if host != 1 {
        println!("Response from device expected");
    }

    if error != 0 {
        println!("Error in response {error}");
    }

    if length as usize != data.len() - 4 {
        println!("Length mismatch in response {} vs actual {}", length, data.len() - 4);
    }

    let buf = BytesMut::from(&data[4..]);

    let resp = WifiMgmtResponse::decode(buf).expect("Failed to decode version response");
    match resp.payload {
        Some(wifi_mgmt_response::Payload::GetVersionResp(version_resp)) => {
            if let Some(version) = version_resp.version {
                format!(
                    "Major: {}, Minor: {} Patch: {} Firmware: {}",
                    version.driver_major,
                    version.driver_minor,
                    version.driver_patch,
                    version.firmware_version
                )
            } else {
                "Unknown version".into()
            }
        }
        Some(_) => "Unexpected response payload".into(),
        None => "Empty response payload".into(),
    }
}

pub fn parse_scan_response(data: &[u8]) -> String {
    if data.len() < 4 {
        return "Invalid scan frame".into();
    }

    let buf = BytesMut::from(&data[4..]);

    if let Ok(notification) = WifiMgmtNotification::decode(buf.clone()) {
        return match notification.event.and_then(|event| event.event_data) {
            Some(wifi_event::EventData::ScanDoneEvent(done)) => {
                format!(
                    "Scan done: status={}, results={}",
                    done.status,
                    done.all_results.len()
                )
            }
            Some(_) => "Received non-scan notification".into(),
            None => "Empty notification payload".into(),
        };
    }

    if let Ok(response) = WifiMgmtResponse::decode(buf) {
        return match response.payload {
            Some(wifi_mgmt_response::Payload::StatusResp(status)) => {
                if status.success {
                    "Scan accepted".into()
                } else {
                    format!(
                        "Scan failed: code={} message={}",
                        status.error_code, status.error_message
                    )
                }
            }
            Some(_) => "Received non-scan response".into(),
            None => "Empty response payload".into(),
        };
    }

    "Unable to decode scan frame".into()
}

pub fn parse_connect_response(data: &[u8]) -> String {
    if data.len() < 4 {
        return "Invalid connect frame".into();
    }

    let buf = BytesMut::from(&data[4..]);
    let response = match WifiMgmtResponse::decode(buf) {
        Ok(response) => response,
        Err(_) => return "Unable to decode connect response".into(),
    };

    match response.payload {
        Some(wifi_mgmt_response::Payload::StatusResp(status)) => {
            if status.success {
                "Connect accepted".into()
            } else {
                format!(
                    "Connect failed: code={} message={}",
                    status.error_code, status.error_message
                )
            }
        }
        Some(_) => "Received non-status connect response".into(),
        None => "Empty response payload".into(),
    }
}

pub fn parse_interfaces_response(data: &[u8]) -> String {
    if data.len() < 4 {
        return "Invalid network interface frame".into();
    }

    let buf = BytesMut::from(&data[4..]);
    let response = match NetIfResponse::decode(buf) {
        Ok(response) => response,
        Err(_) => return "Unable to decode network interface response".into(),
    };

    if response.status != 0 {
        return format!("Network interface query failed: status={}", response.status);
    }

    match response.payload {
        Some(net_if_response::Payload::Interfaces(interfaces)) => {
            if interfaces.net_ifs.is_empty() {
                return "No network interfaces found".into();
            }

            let mut lines = Vec::with_capacity(interfaces.net_ifs.len() + 1);
            lines.push(format!("Found {} network interface(s):", interfaces.net_ifs.len()));
            for iface in interfaces.net_ifs {
                lines.push(format!(
                    "- idx={} name={} type={} mtu={} admin_up={} carrier_ok={} dormant={}",
                    iface.index,
                    iface.name,
                    NetIfType::try_from(iface.net_if_type)
                        .map(|kind| kind.as_str_name())
                        .unwrap_or("NET_IF_TYPE_UNKNOWN"),
                    iface.mtu,
                    iface.admin_up,
                    iface.carrier_ok,
                    iface.dormant
                ));
            }
            lines.join("\n")
        }
        Some(_) => "Received non-interface response payload".into(),
        None => "Empty response payload".into(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn create_version_query_builds_a_wifi_management_request() {
        let query = create_version_query();

        assert!(query.len() > 4);
        assert_eq!(query[0] >> 4, SerialType::WiFiMgmt.to_u8());
        let request = WifiMgmtRequest::decode(&query[4..]).expect("request should decode");
        assert_eq!(request.request_id, RequestType::GetWifiVersion as i32);
        match request.payload {
            Some(wifi_mgmt_request::Payload::GetVersionReq(_)) => {}
            other => panic!("unexpected payload: {other:?}"),
        }
    }

    #[test]
    fn create_scan_query_builds_a_wifi_management_request() {
        let query = create_scan_query(7);

        assert!(query.len() > 4);
        assert_eq!(query[0] >> 4, SerialType::WiFiMgmt.to_u8());
        let request = WifiMgmtRequest::decode(&query[4..]).expect("request should decode");
        assert_eq!(request.request_id, RequestType::Scan as i32);
        match request.payload {
            Some(wifi_mgmt_request::Payload::ScanReq(scan_req)) => {
                assert_eq!(scan_req.iface_index, 7);
                assert!(scan_req.params.is_none());
            }
            other => panic!("unexpected payload: {other:?}"),
        }
    }

    #[test]
    fn create_connect_query_builds_a_wifi_management_request() {
        let query = create_connect_query(2, "test-ssid", Some("secretpass"), WifiSecurityType::Psk, 5000);

        assert!(query.len() > 4);
        assert_eq!(query[0] >> 4, SerialType::WiFiMgmt.to_u8());

        let request = WifiMgmtRequest::decode(&query[4..]).expect("request should decode");
        assert_eq!(request.request_id, RequestType::Connect as i32);
        match request.payload {
            Some(wifi_mgmt_request::Payload::ConnectReq(connect_req)) => {
                assert_eq!(connect_req.iface_index, 2);
                assert_eq!(connect_req.ssid, b"test-ssid");
                assert_eq!(connect_req.security_type, WifiSecurityType::Psk as i32);
                assert_eq!(connect_req.psk, b"secretpass");
                assert_eq!(connect_req.timeout_ms, 5000);
            }
            other => panic!("unexpected payload: {other:?}"),
        }
    }

    #[test]
    fn create_interfaces_query_builds_a_net_if_request() {
        let query = create_interfaces_query(NetIfType::Wifi, 3);

        assert!(query.len() > 4);
        assert_eq!(query[0] >> 4, SerialType::NetIfMgmt.to_u8());

        let request = NetIfRequest::decode(&query[4..]).expect("request should decode");
        assert_eq!(request.request_id, 1);
        match request.payload {
            Some(net_if_request::Payload::GetInterfaces(get_interfaces)) => {
                assert_eq!(get_interfaces.index, 3);
                assert_eq!(
                    get_interfaces.net_if_type,
                    NetIfType::Wifi as i32
                );
            }
            other => panic!("unexpected payload: {other:?}"),
        }
    }
}