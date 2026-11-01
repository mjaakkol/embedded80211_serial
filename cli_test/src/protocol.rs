//! Protocol definitions for the wire protocol used between host and device.
//!
//! Wire format (4-byte header, little-endian payload length is NOT used — the
//! length is big-endian on the wire, matching the device parser):
//!
//! ```text
//! byte0: type(0..3) | no_ack(4) | host(5) | error(6..7)
//! byte1: reserved(0..3, zero) | sequence_number(4..7)
//! byte2: payload length, high byte
//! byte3: payload length, low byte
//! ```
//!
//! `host` is 0 for host->device frames and 1 for device->host frames.
//! Length 0 occurs only in device->host ACK/error frames; host->device
//! frames must carry a non-empty payload (the device rejects length 0).
//! The maximum payload length is 4096.
//!
//! Host-to-device command frames are answered with:
//! 1. an ACK frame (same type, length 0, error code, echoed sequence),
//! 2. the proxy response frame (length > 0),
//! followed by any asynchronous notification frames.
//!
//! Before any command the host must sync with a PROTOCOL_INFO frame (type 7,
//! payload >= 1 byte). The device answers with a 4-byte payload:
//! `[version, supported_types_lo, supported_types_hi, next_rx_sequence]`.
//! The host must use `next_rx_sequence` as the sequence number of the next
//! host->device frame. The PROTOCOL_INFO exchange consumes no sequence slot
//! and is never sequence-checked, so it works even after a desync.

use bytes::{BufMut, BytesMut};
use prost::Message;
use std::time::Instant;

use network_interface::{
    net_if_request, net_if_response, NetIfRequest, NetIfResponse, NetIfType, RequestInterfaces,
};
use wifi_mgmt::{
    wifi_event, wifi_mgmt_request, wifi_mgmt_response, GetWifiVersionRequest, RequestType,
    WifiConnectRequest, WifiMgmtNotification, WifiMgmtRequest, WifiMgmtResponse, WifiMfpOptions,
    WifiScanRequest, WifiSecurityType,
};

pub mod network_interface {
    include!(concat!(env!("OUT_DIR"), "/embedded_wifi_mgmt.rs"));
}

pub mod wifi_mgmt {
    include!(concat!(env!("OUT_DIR"), "/embedded_wifi_mgmt.rs"));
}

/// Traffic types. Values are fixed by the link protocol and must match the
/// device-side `enum SerialType`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SerialType {
    WifiMgmt = 0,
    WifiData = 1,
    NetIfMgmt = 2,
    Hci = 3,
    Thread = 4,
    NetOffloading = 5,
    AtCmd = 6,
    ProtocolInfo = 7,
    VSerial = 8,
}

impl SerialType {
    #[inline]
    pub fn to_u8(self) -> u8 {
        self as u8
    }

    #[inline]
    pub fn from_u8(value: u8) -> Option<Self> {
        match value {
            0 => Some(Self::WifiMgmt),
            1 => Some(Self::WifiData),
            2 => Some(Self::NetIfMgmt),
            3 => Some(Self::Hci),
            4 => Some(Self::Thread),
            5 => Some(Self::NetOffloading),
            6 => Some(Self::AtCmd),
            7 => Some(Self::ProtocolInfo),
            8 => Some(Self::VSerial),
            _ => None,
        }
    }
}

/// Error codes in the 2-bit error field of the frame header.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ErrorType {
    NoError = 0,
    IncorrectSequenceNumber = 1,
    OutOfBuffers = 2,
    LengthTooLong = 3,
}

impl From<u8> for ErrorType {
    fn from(value: u8) -> Self {
        match value & 0x03 {
            1 => Self::IncorrectSequenceNumber,
            2 => Self::OutOfBuffers,
            3 => Self::LengthTooLong,
            _ => Self::NoError,
        }
    }
}

/// A decoded link-protocol frame.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Frame {
    pub rtype: SerialType,
    pub no_ack: bool,
    /// true = device -> host direction
    pub host: bool,
    pub error: ErrorType,
    pub seq: u8,
    pub payload: Vec<u8>,
}

/// Maximum payload length accepted by the device (AGENTS.md: 4096).
const MAX_PAYLOAD_LEN: usize = 4096;

impl Frame {
    /// Parses a complete frame (header + payload). Returns `None` when the
    /// buffer does not contain exactly one well-formed frame. Length-0
    /// frames are the device's ACK/error frames and parse to an empty
    /// payload; host->device frames must carry a non-empty payload.
    pub fn parse(data: &[u8]) -> Option<Self> {
        if data.len() < 4 {
            return None;
        }
        let length = u16::from_be_bytes([data[2], data[3]]) as usize;
        if length > MAX_PAYLOAD_LEN || data.len() != 4 + length {
            return None;
        }
        let rtype = SerialType::from_u8(data[0] & 0x0F)?;
        let no_ack = data[0] & (1 << 4) != 0;
        let host = data[0] & (1 << 5) != 0;
        let error = ErrorType::from(data[0] >> 6);
        let seq = (data[1] >> 4) & 0x0F;
        if data[1] & 0x0F != 0 {
            // Reserved bits must be zero.
            return None;
        }
        Some(Self {
            rtype,
            no_ack,
            host,
            error,
            seq,
            payload: data[4..].to_vec(),
        })
    }

    /// Builds a host -> device frame. The device rejects frames with a
    /// zero-length payload, so callers must pass at least one payload byte.
    pub fn encode_host_frame(rtype: SerialType, seq: u8, payload: &[u8]) -> Vec<u8> {
        debug_assert!(!payload.is_empty(), "device rejects length-0 frames");
        let length = (payload.len() as u16).to_be_bytes();
        let mut frame = BytesMut::with_capacity(4 + payload.len());
        frame.put_u8(rtype.to_u8());
        frame.put_u8((seq & 0x0F) << 4);
        frame.put_u8(length[0]);
        frame.put_u8(length[1]);
        frame.put_slice(payload);
        frame.into()
    }

    /// PROTOCOL_INFO query frame. The device discards the payload and
    /// responds with version, supported-type mask and next expected
    /// sequence number.
    pub fn protocol_info_query(seq: u8) -> Vec<u8> {
        Self::encode_host_frame(SerialType::ProtocolInfo, seq, &[0x00])
    }
}

/// Information reported by the device in the PROTOCOL_INFO response.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct ProtocolInfo {
    pub version: u8,
    pub supported_types_mask: u16,
    /// Sequence number the device expects on the next host -> device frame.
    pub next_seq: u8,
}

impl TryFrom<&Frame> for ProtocolInfo {
    type Error = ();

    fn try_from(frame: &Frame) -> Result<Self, Self::Error> {
        if frame.rtype != SerialType::ProtocolInfo
            || frame.error != ErrorType::NoError
            || frame.payload.len() != 4
        {
            return Err(());
        }
        Ok(Self {
            version: frame.payload[0],
            supported_types_mask: u16::from_le_bytes([frame.payload[1], frame.payload[2]]),
            next_seq: frame.payload[3],
        })
    }
}

/// Session errors reported back to the caller.
#[derive(Debug)]
pub enum SessionError {
    Io(std::io::Error),
    /// No complete frame arrived before the deadline.
    Timeout,
    /// A device frame carried a protocol error.
    DeviceError(ErrorType),
    /// A frame arrived that does not match the expected response.
    UnexpectedFrame(Frame),
}

impl std::fmt::Display for SessionError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::Io(e) => write!(f, "serial I/O error: {e}"),
            Self::Timeout => write!(f, "timed out waiting for a response frame"),
            Self::DeviceError(e) => write!(f, "device reported protocol error {e:?}"),
            Self::UnexpectedFrame(frame) => write!(f, "unexpected frame: {frame:?}"),
        }
    }
}

impl std::error::Error for SessionError {}

/// A host session over a serial port: owns the host->device sequence
/// counter and the frame read loop.
pub struct Session {
    port: Box<dyn serialport::SerialPort>,
    tx_seq: u8,
}

impl Session {
    pub fn new(port: Box<dyn serialport::SerialPort>) -> Self {
        let session = Self { port, tx_seq: 0 };
        session
    }

    /// Reads exactly `buf.len()` bytes, or returns the number of bytes read
    /// (0) when the deadline passes first.
    fn read_bytes(&mut self, buf: &mut [u8], deadline: Instant) -> Result<usize, SessionError> {
        let mut total = 0;
        while total < buf.len() {
            if Instant::now() >= deadline {
                break;
            }
            // The port has a short read timeout: a timed-out read just
            // means no data yet, so retry until the deadline.
            let n = match self.port.read(&mut buf[total..]) {
                Ok(n) => n,
                Err(e) if e.kind() == std::io::ErrorKind::TimedOut => 0,
                Err(e) => return Err(SessionError::Io(e)),
            };
            total += n;
        }
        Ok(total)
    }

    /// Reads one complete frame, blocking until the deadline.
    pub fn read_frame(&mut self, deadline: Instant) -> Result<Frame, SessionError> {
        let mut header = [0u8; 4];
        if self.read_bytes(&mut header, deadline)? != 4 {
            return Err(SessionError::Timeout);
        }
        if u16::from_be_bytes([header[2], header[3]]) as usize == 0 {
            // ACK / error frames carry no payload.
            return Frame::parse(&header).ok_or(SessionError::Timeout);
        }
        let length = u16::from_be_bytes([header[2], header[3]]) as usize;
        if length > MAX_PAYLOAD_LEN {
            return Err(SessionError::Timeout);
        }
        let mut frame_bytes = Vec::with_capacity(4 + length);
        frame_bytes.extend_from_slice(&header);
        let mut payload = vec![0u8; length];
        if self.read_bytes(&mut payload, deadline)? != length {
            return Err(SessionError::Timeout);
        }
        frame_bytes.extend_from_slice(&payload);
        Frame::parse(&frame_bytes).ok_or(SessionError::Timeout)
    }

    /// Sends a raw frame and drains the port first so a stale response
    /// cannot be mistaken for the answer to this command.
    fn send(&mut self, frame: &[u8]) -> Result<(), SessionError> {
        self.drain();
        self.port.write_all(frame).map_err(SessionError::Io)?;
        self.port.flush().map_err(SessionError::Io)
    }

    /// Discards bytes that are already buffered in the port.
    pub fn drain(&mut self) {
        let mut buf = [0u8; 256];
        loop {
            match self.port.read(&mut buf) {
                Ok(0) | Err(_) => break,
                Ok(_) => {}
            }
        }
    }

    /// Handshakes with the device and returns the next expected sequence
    /// number. Must be called before any command frame.
    pub fn sync(&mut self, deadline: Instant) -> Result<ProtocolInfo, SessionError> {
        for _ in 0..4 {
            self.send(&Frame::protocol_info_query(self.tx_seq))?;
            let frame = self.read_frame(deadline)?;
            if let Ok(info) = ProtocolInfo::try_from(&frame) {
                self.tx_seq = info.next_seq;
                return Ok(info);
            }
            // Unexpected frame (e.g. error ACK): retry the handshake.
        }
        Err(SessionError::Timeout)
    }

    /// Sends a command frame and waits for the first device frame that
    /// satisfies `matches`. Empty (ACK) frames are skipped; protocol-error
    /// frames trigger a re-sync and one retry, per the link protocol
    /// (the device resets its expected sequence to zero on mismatch).
    pub fn request(
        &mut self,
        rtype: SerialType,
        payload: &[u8],
        deadline: Instant,
        matches: &dyn Fn(&Frame) -> bool,
    ) -> Result<Frame, SessionError> {
        let seq = self.tx_seq;
        self.tx_seq = (self.tx_seq + 1) & 0x0F;
        self.send(&Frame::encode_host_frame(rtype, seq, payload))?;

        let mut retried = false;
        loop {
            let frame = self.read_frame(deadline)?;
            if frame.error != ErrorType::NoError {
                if frame.error == ErrorType::IncorrectSequenceNumber && !retried {
                    let info = self.sync(deadline)?;
                    self.tx_seq = (info.next_seq + 1) & 0x0F;
                    self.send(&Frame::encode_host_frame(rtype, info.next_seq, payload))?;
                    retried = true;
                    continue;
                }
                return Err(SessionError::DeviceError(frame.error));
            }
            if frame.payload.is_empty() {
                continue; // ACK, the response follows.
            }
            if matches(&frame) {
                return Ok(frame);
            }
            // Asynchronous frames for other commands can interleave; keep
            // waiting for the matching response.
            continue;
        }
    }

    /// Waits (without sending anything) for the first frame that satisfies
    /// `matches`.
    pub fn wait_for(
        &mut self,
        deadline: Instant,
        matches: &dyn Fn(&Frame) -> bool,
    ) -> Result<Frame, SessionError> {
        loop {
            let frame = self.read_frame(deadline)?;
            if frame.error != ErrorType::NoError {
                return Err(SessionError::DeviceError(frame.error));
            }
            if frame.payload.is_empty() {
                continue;
            }
            if matches(&frame) {
                return Ok(frame);
            }
            continue;
        }
    }

    pub fn tx_seq(&self) -> u8 {
        self.tx_seq
    }
}

/// Decodes a device->host frame as a WifiMgmtResponse.
pub fn decode_wifi_mgmt_response(frame: &Frame) -> Result<WifiMgmtResponse, SessionError> {
    if frame.rtype != SerialType::WifiMgmt {
        return Err(SessionError::UnexpectedFrame(frame.clone()));
    }
    WifiMgmtResponse::decode(frame.payload.as_slice())
        .map_err(|e| SessionError::Io(std::io::Error::new(std::io::ErrorKind::InvalidData, e)))
}

/// Decodes a device->host frame as a WifiMgmtNotification carrying a
/// ScanDoneEvent.
pub fn decode_scan_done_notification(frame: &Frame) -> Option<(i32, Vec<wifi_mgmt::WifiScanResult>)> {
    if frame.rtype != SerialType::WifiMgmt {
        return None;
    }
    let notification = WifiMgmtNotification::decode(frame.payload.as_slice()).ok()?;
    let event = notification.event?;
    match event.event_data {
        Some(wifi_event::EventData::ScanDoneEvent(done)) => Some((done.status, done.all_results)),
        _ => None,
    }
}

/// Builds the encoded protobuf payload of a GET_WIFI_VERSION request.
pub fn version_request_payload() -> Vec<u8> {
    let request = WifiMgmtRequest {
        request_id: RequestType::GetWifiVersion as i32,
        payload: Some(wifi_mgmt_request::Payload::GetVersionReq(
            GetWifiVersionRequest {},
        )),
    };
    let mut out = Vec::new();
    request.encode(&mut out).unwrap();
    out
}

/// Builds the encoded protobuf payload of a SCAN request.
pub fn scan_request_payload(iface_index: u32) -> Vec<u8> {
    let request = WifiMgmtRequest {
        request_id: RequestType::Scan as i32,
        payload: Some(wifi_mgmt_request::Payload::ScanReq(WifiScanRequest {
            iface_index,
            params: None,
        })),
    };
    let mut out = Vec::new();
    request.encode(&mut out).unwrap();
    out
}

/// Builds the encoded protobuf payload of a CONNECT request.
pub fn connect_request_payload(
    iface_index: u32,
    ssid: &str,
    psk: Option<&str>,
    security_type: WifiSecurityType,
    timeout_ms: u32,
) -> Vec<u8> {
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
    let mut out = Vec::new();
    request.encode(&mut out).unwrap();
    out
}

/// Builds the encoded protobuf payload of a GET_INTERFACES request.
pub fn interfaces_request_payload(net_if_type: NetIfType, index: u32) -> Vec<u8> {
    let request = NetIfRequest {
        request_id: 1,
        payload: Some(net_if_request::Payload::GetInterfaces(RequestInterfaces {
            net_if_type: net_if_type as i32,
            index,
        })),
    };
    let mut out = Vec::new();
    request.encode(&mut out).unwrap();
    out
}

/// Renders a GET_WIFI_VERSION response payload as a string.
pub fn format_version_response(frame: &Frame) -> String {
    let resp = match decode_wifi_mgmt_response(frame) {
        Ok(resp) => resp,
        Err(_) => return "Unable to decode version response".into(),
    };
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

/// Renders a CONNECT/SCAN status response payload as a string.
pub fn format_status_response(frame: &Frame) -> Result<String, String> {
    let resp = decode_wifi_mgmt_response(frame)
        .map_err(|_| "Unable to decode status response".to_string())?;
    match resp.payload {
        Some(wifi_mgmt_response::Payload::StatusResp(status)) => {
            let message = status.error_message;
            if status.success {
                Ok(if message.is_empty() {
                    "Success".to_string()
                } else {
                    format!("Success: {message}")
                })
            } else {
                Err(format!(
                    "Failed: code={} message={}",
                    status.error_code, message
                ))
            }
        }
        Some(_) => Err("Received non-status response payload".into()),
        None => Err("Empty response payload".into()),
    }
}

/// Renders a GET_INTERFACES response payload as a string.
pub fn format_interfaces_response(frame: &Frame) -> String {
    let response = match NetIfResponse::decode(frame.payload.as_slice()) {
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
            for iface in &interfaces.net_ifs {
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

/// Renders scan results (status + per-network lines) as a string.
pub fn format_scan_results(status: i32, results: &[wifi_mgmt::WifiScanResult]) -> String {
    let mut lines = Vec::new();
    lines.push(format!("Scan done: status={}, results={}", status, results.len()));
    for result in results {
        let ssid = if result.ssid.is_empty() {
            "(hidden)".to_string()
        } else {
            String::from_utf8_lossy(&result.ssid).into_owned()
        };
        let bssid = result
            .bssid
            .iter()
            .map(|b| format!("{b:02x}"))
            .collect::<Vec<_>>()
            .join(":");
        lines.push(format!(
            "- SSID: {ssid}  BSSID: {bssid}  ch: {}  RSSI: {}  security: {:?}",
            result.channel,
            result.rssi,
            WifiSecurityType::try_from(result.security)
                .map(|t| t.as_str_name().to_string())
                .unwrap_or_else(|_| format!("unknown({})", result.security)),
        ));
    }
    lines.join("\n")
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn host_frame_has_device_bit_layout() {
        // byte0: type in low nibble, no_ack/host/error zero for host->device
        // byte1: sequence in high nibble, reserved zero
        // bytes 2..3: length big-endian
        let frame = Frame::encode_host_frame(SerialType::WifiMgmt, 5, &[1, 2, 3, 4]);

        assert_eq!(frame.len(), 8);
        assert_eq!(frame[0], 0x00); // type 0 (WIFI_MGMT)
        assert_eq!(frame[1], 0x50); // seq 5, reserved 0
        assert_eq!(frame[2], 0x00); // length high
        assert_eq!(frame[3], 0x04); // length low

        let parsed = Frame::parse(&frame).expect("frame should parse");
        assert_eq!(parsed.rtype, SerialType::WifiMgmt);
        assert_eq!(parsed.seq, 5);
        assert!(!parsed.host);
        assert!(!parsed.no_ack);
        assert_eq!(parsed.error, ErrorType::NoError);
        assert_eq!(parsed.payload, vec![1, 2, 3, 4]);
    }

    #[test]
    fn parse_rejects_malformed_frames() {
        // truncated payload
        assert!(Frame::parse(&[0x00, 0x00, 0x00, 0x04, 1, 2]).is_none());
        // unknown type
        assert!(Frame::parse(&[0x09, 0x00, 0x00, 0x01, 1]).is_none());
        // non-zero reserved bits
        assert!(Frame::parse(&[0x00, 0x0F, 0x00, 0x01, 1]).is_none());
    }

    #[test]
    fn parse_device_ack_frame() {
        // Device ACK for a WIFI_MGMT request with seq 5 (as captured on
        // the wire): type=0, host=1, error=0, length 0.
        let parsed = Frame::parse(&[0x20, 0x50, 0x00, 0x00]).expect("ACK should parse");
        assert_eq!(parsed.rtype, SerialType::WifiMgmt);
        assert!(parsed.host);
        assert!(!parsed.no_ack);
        assert_eq!(parsed.error, ErrorType::NoError);
        assert_eq!(parsed.seq, 5);
        assert!(parsed.payload.is_empty());
    }

    #[test]
    fn parse_device_frame_with_error_and_host_bit() {
        // byte0: error=INCORRECT_SEQUENCE(1) << 6, host=1 << 5, type=0
        //       -> 0b0110_0000 = 0x60
        // byte1: seq=2 in high nibble
        let bytes: &[u8] = &[0x60, 0x20, 0x00, 0x01, 0xAB];
        let parsed = Frame::parse(bytes).expect("device frame should parse");
        assert_eq!(parsed.rtype, SerialType::WifiMgmt);
        assert!(parsed.host);
        assert_eq!(parsed.error, ErrorType::IncorrectSequenceNumber);
        assert_eq!(parsed.seq, 2);
        assert_eq!(parsed.payload, vec![0xAB]);
    }

    #[test]
    fn protocol_info_query_is_type_7_with_payload() {
        let query = Frame::protocol_info_query(0);
        let parsed = Frame::parse(&query).expect("info query should parse");
        assert_eq!(parsed.rtype, SerialType::ProtocolInfo);
        assert_eq!(parsed.payload.len(), 1);
    }

    #[test]
    fn protocol_info_payload_round_trip() {
        // [version=1, mask_lo=0x1F, mask_hi=0x00, next_seq=3]
        let payload = [1u8, 0x1F, 0x00, 3];
        let frame = Frame::encode_host_frame(SerialType::ProtocolInfo, 0, &payload);
        let parsed = Frame::parse(&frame).expect("frame should parse");
        let info = ProtocolInfo::try_from(&parsed).expect("info should decode");
        assert_eq!(info.version, 1);
        assert_eq!(info.supported_types_mask, 0x001F);
        assert_eq!(info.next_seq, 3);
    }

    #[test]
    fn scan_request_payload_encodes_iface_index() {
        let payload = scan_request_payload(7);
        let request =
            WifiMgmtRequest::decode(&payload[..]).expect("request should decode");
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
    fn connect_request_payload_encodes_credentials() {
        let payload =
            connect_request_payload(2, "test-ssid", Some("secretpass"), WifiSecurityType::Psk, 5000);
        let request =
            WifiMgmtRequest::decode(&payload[..]).expect("request should decode");
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
    fn interfaces_request_payload_encodes_type_and_index() {
        let payload = interfaces_request_payload(NetIfType::Wifi, 3);
        let request =
            NetIfRequest::decode(&payload[..]).expect("request should decode");
        assert_eq!(request.request_id, 1);
        match request.payload {
            Some(net_if_request::Payload::GetInterfaces(get_interfaces)) => {
                assert_eq!(get_interfaces.index, 3);
                assert_eq!(get_interfaces.net_if_type, NetIfType::Wifi as i32);
            }
            other => panic!("unexpected payload: {other:?}"),
        }
    }

    #[test]
    fn sequence_counter_wraps_at_16() {
        let mut seq: u8 = 15;
        seq = (seq + 1) & 0x0F;
        assert_eq!(seq, 0);
    }
}
