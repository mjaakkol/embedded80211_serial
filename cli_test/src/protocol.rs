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
//! A host-to-device command frame is answered with an ACK frame (same type,
//! length 0, error code, echoed sequence) followed by the proxy response
//! frame (length > 0), and then by any asynchronous notification frames.
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
    NetIfRequest, NetIfResponse, NetIfType, RequestInterfaces, net_if_request, net_if_response,
};
use wifi_mgmt::{
    DisableRawRxRequest, EnableRawRxRequest, EnableRawRxResponse, GetInterfaceStatusRequest,
    GetPowerSaveConfigRequest, GetRegulatoryDomainRequest, GetWifiVersionRequest, RequestType,
    SendRawFrameRequest, SetPowerSaveConfigRequest, SetRegulatoryDomainRequest,
    WifiApDisableRequest, WifiApEnableParams, WifiApEnableRequest, WifiConnectRequest,
    WifiDisconnectRequest, WifiInterfaceState, WifiInterfaceStatus, WifiMfpOptions,
    WifiMgmtNotification, WifiMgmtRequest, WifiMgmtResponse, WifiPowerSaveConfig,
    WifiPowerSaveType, WifiPsSettings, WifiPsState, WifiRegDomain, WifiScanRequest,
    WifiSecurityType, WifiStatusResponse, wifi_event, wifi_mgmt_request, wifi_mgmt_response,
};

pub mod network_interface {
    include!(concat!(env!("OUT_DIR"), "/embedded_wifi_mgmt.rs"));
}

pub mod wifi_mgmt {
    include!(concat!(env!("OUT_DIR"), "/embedded_wifi_mgmt.rs"));
}

/// Traffic types. Values are fixed by the link protocol and must match the
/// device-side `enum SerialType`.
#[non_exhaustive]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SerialType {
    /// Wi-Fi management (cfg80211-style control and scan events).
    WifiMgmt = 0,
    /// Wi-Fi data frames.
    WifiData = 1,
    /// Network interface management (`net_if` queries).
    NetIfMgmt = 2,
    /// Bluetooth HCI (H:4 packets).
    Hci = 3,
    /// OpenThread (Spinel).
    Thread = 4,
    /// Network offloading (reserved).
    NetOffloading = 5,
    /// AT commands (reserved).
    AtCmd = 6,
    /// Protocol information (version, supported types, sequence sync).
    ProtocolInfo = 7,
    /// Virtual serial.
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
#[non_exhaustive]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ErrorType {
    /// No error.
    NoError = 0,
    /// Sequence number mismatch; the device reset its expected sequence to zero.
    IncorrectSequenceNumber = 1,
    /// The device has no buffers available.
    OutOfBuffers = 2,
    /// The frame payload exceeded the device limit.
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
    /// Traffic type of the frame payload.
    pub traffic_type: SerialType,
    /// No-ACK flag: the receiver must not acknowledge this frame.
    pub no_ack: bool,
    /// True for device -> host direction frames.
    pub to_host: bool,
    /// Protocol error carried by the frame, if any.
    pub error: ErrorType,
    /// True when a CRC32C trailer follows the payload.
    pub crc: bool,
    /// Sequence number (0..15, wraps).
    pub seq: u8,
    /// Frame payload (empty for ACK frames).
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
        if length > MAX_PAYLOAD_LEN {
            return None;
        }
        let crc = data[1] & 0x01 != 0;
        let body_len = 4 + length;
        let total = body_len + if crc { 4 } else { 0 };
        if data.len() != total {
            return None;
        }
        if crc {
            let expected = u32::from_be_bytes([
                data[body_len],
                data[body_len + 1],
                data[body_len + 2],
                data[body_len + 3],
            ]);
            if crc32c::crc32c(&data[..body_len]) != expected {
                return None;
            }
        }
        let mut header = [0u8; 4];
        header.copy_from_slice(&data[0..4]);
        Self::from_parts(header, data[4..body_len].to_vec())
    }

    /// Validates a 4-byte header and pairs it with an already-read payload,
    /// avoiding a second copy of the payload bytes.
    fn from_parts(header: [u8; 4], payload: Vec<u8>) -> Option<Self> {
        let length = u16::from_be_bytes([header[2], header[3]]) as usize;
        if length > MAX_PAYLOAD_LEN || length != payload.len() {
            return None;
        }
        let traffic_type = SerialType::from_u8(header[0] & 0x0F)?;
        let no_ack = header[0] & (1 << 4) != 0;
        let to_host = header[0] & (1 << 5) != 0;
        let error = ErrorType::from(header[0] >> 6);
        let crc = header[1] & 0x01 != 0;
        let seq = (header[1] >> 4) & 0x0F;
        if header[1] & 0x0E != 0 {
            // Reserved bits (1..3) must be zero; bit 0 is the crc flag.
            return None;
        }
        Some(Self {
            traffic_type,
            no_ack,
            to_host,
            error,
            crc,
            seq,
            payload,
        })
    }

    /// Builds a host -> device frame. The device rejects frames with a
    /// zero-length payload, so callers must pass at least one payload byte.
    ///
    /// # Errors
    ///
    /// Returns [`SessionError::PayloadTooLong`] when `payload` exceeds the
    /// 4096-byte link limit.
    pub fn encode_host_frame(
        traffic_type: SerialType,
        seq: u8,
        crc: bool,
        payload: &[u8],
    ) -> Result<Vec<u8>, SessionError> {
        debug_assert!(!payload.is_empty(), "device rejects length-0 frames");
        if payload.len() > MAX_PAYLOAD_LEN {
            return Err(SessionError::PayloadTooLong(payload.len()));
        }
        let length = (payload.len() as u16).to_be_bytes();
        let mut frame = BytesMut::with_capacity(4 + payload.len() + 4);
        frame.put_u8(traffic_type.to_u8());
        frame.put_u8(((seq & 0x0F) << 4) | (if crc { 1 } else { 0 }));
        frame.put_u8(length[0]);
        frame.put_u8(length[1]);
        frame.put_slice(payload);
        if crc {
            // CRC32C over the 4-byte header plus the payload; put_u32 is big-endian.
            let c = crc32c::crc32c(&frame[..]);
            frame.put_u32(c);
        }
        Ok(frame.into())
    }

    /// PROTOCOL_INFO query frame. The device discards the payload and
    /// responds with version, supported-type mask and next expected
    /// sequence number.
    pub fn protocol_info_query(seq: u8, crc: bool) -> Vec<u8> {
        // A single fixed payload byte can never exceed the link limit.
        Self::encode_host_frame(SerialType::ProtocolInfo, seq, crc, &[0x00])
            .expect("1-byte payload always encodable")
    }
}

/// Information reported by the device in the PROTOCOL_INFO response.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct ProtocolInfo {
    /// Wire protocol version.
    pub version: u8,
    /// Bitmask of supported traffic types (bit N = `SerialType` N).
    pub supported_types_mask: u16,
    /// Sequence number the device expects on the next host -> device frame.
    pub next_seq: u8,
}

/// Failure to decode a fixed-layout protocol payload.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct MalformedPayload;

impl std::fmt::Display for MalformedPayload {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "malformed protocol payload")
    }
}

impl std::error::Error for MalformedPayload {}

impl TryFrom<&Frame> for ProtocolInfo {
    type Error = MalformedPayload;

    fn try_from(frame: &Frame) -> Result<Self, Self::Error> {
        if frame.traffic_type != SerialType::ProtocolInfo
            || frame.error != ErrorType::NoError
            || frame.payload.len() != 4
        {
            return Err(MalformedPayload);
        }
        Ok(Self {
            version: frame.payload[0],
            supported_types_mask: u16::from_le_bytes([frame.payload[1], frame.payload[2]]),
            next_seq: frame.payload[3],
        })
    }
}

// ---------------------------------------------------------------------------
// Bluetooth HCI (H:4) helpers
//
// HCI traffic frames (SerialType::Hci) carry raw H:4 HCI packets: the
// first payload byte is the H:4 packet type, followed by the HCI packet.
// The device bridges these to the local Bluetooth controller (Zephyr raw
// HCI channel) and reports controller events back as H:4 packets in HCI
// traffic frames.
// ---------------------------------------------------------------------------

/// H:4 packet types.
pub const HCI_H4_CMD: u8 = 0x01;
pub const HCI_H4_ACL: u8 = 0x02;
pub const HCI_H4_SCO: u8 = 0x03;
pub const HCI_H4_EVT: u8 = 0x04;
pub const HCI_H4_ISO: u8 = 0x05;

/// HCI event codes.
pub const HCI_EVT_COMMAND_COMPLETE: u8 = 0x0E;
pub const HCI_EVT_COMMAND_STATUS: u8 = 0x0F;

/// Well-known HCI opcodes, encoded as `(OGF << 10) | OCF` per the HCI
/// spec (matching Zephyr's `BT_HCI_OP_*` in `hci_types.h`).
pub const HCI_OP_READ_LOCAL_SUPPORTED_COMMANDS: u16 = 0x1002;
pub const HCI_OP_READ_LOCAL_SUPPORTED_FEATURES: u16 = 0x1003;
pub const HCI_OP_READ_LOCAL_VERSION: u16 = 0x1001;
pub const HCI_OP_RESET: u16 = 0x0C03;
pub const HCI_OP_READ_BD_ADDR: u16 = 0x1009;

/// Builds an H:4 HCI command packet:
/// `[H4_CMD, opcode_lo, opcode_hi, param_len, params...]`.
pub fn hci_command_packet(opcode: u16, params: &[u8]) -> Vec<u8> {
    let mut pkt = Vec::with_capacity(4 + params.len());
    pkt.push(HCI_H4_CMD);
    pkt.push((opcode & 0xFF) as u8);
    pkt.push((opcode >> 8) as u8);
    pkt.push(params.len() as u8);
    pkt.extend_from_slice(params);
    pkt
}

/// A decoded Command Complete event (0x0E).
///
/// A Command Complete carries the command's return parameters; it has no
/// status field (that belongs to the Command Status event, 0x0F).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct HciCommandComplete {
    /// Number of command packets this event completes.
    pub num_packets: u8,
    /// Opcode of the completed command.
    pub opcode: u16,
    /// Command return parameters.
    pub params: Vec<u8>,
}

/// Parses an H:4 Command Complete event from an HCI frame payload.
///
/// Returns `None` for any other H:4 packet type, any other event code, or
/// truncated / length-inconsistent data.
pub fn hci_command_complete(payload: &[u8]) -> Option<HciCommandComplete> {
    // H4 (1) + event header (evt 1, plen 1) + CC body (num 1, opcode 2,
    // param_len 1) + params.
    if payload.len() < 7 || payload[0] != HCI_H4_EVT {
        return None;
    }
    let evt_plen = payload[2] as usize;
    if payload[1] != HCI_EVT_COMMAND_COMPLETE || payload.len() != 3 + evt_plen {
        return None;
    }
    // The CC body is 4 bytes (num, opcode, param_len); the remaining event
    // bytes are the command's return parameters. Use `evt_plen` as the
    // source of truth for the length — some controllers do not fill the
    // param_len byte consistently.
    if evt_plen < 4 {
        return None;
    }
    Some(HciCommandComplete {
        num_packets: payload[3],
        opcode: u16::from_le_bytes([payload[4], payload[5]]),
        params: payload[7..].to_vec(),
    })
}

/// Decoded Read Local Version Information (0x0103) parameters.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct HciVersionInfo {
    pub hci_version: u8,
    pub lmp_version: u8,
    pub utf: u16,
    pub manufacturer: u16,
    pub sdp: [u8; 8],
}

impl TryFrom<&HciCommandComplete> for HciVersionInfo {
    type Error = MalformedPayload;

    fn try_from(cc: &HciCommandComplete) -> Result<Self, Self::Error> {
        // The fixed header is 6 bytes (hci, lmp, utf, mfr); the SDP product
        // field is nominally 8 bytes, but some controllers return a shorter
        // blob. Decode what is present and zero-pad a short SDP field.
        if cc.opcode != HCI_OP_READ_LOCAL_VERSION || cc.params.len() < 6 {
            return Err(MalformedPayload);
        }
        let p = &cc.params;
        let mut sdp = [0u8; 8];
        let sdp_len = p.len().saturating_sub(6).min(8);
        sdp[..sdp_len].copy_from_slice(&p[6..6 + sdp_len]);
        Ok(Self {
            hci_version: p[0],
            lmp_version: p[1],
            utf: u16::from_le_bytes([p[2], p[3]]),
            manufacturer: u16::from_le_bytes([p[4], p[5]]),
            sdp,
        })
    }
}

/// Decoded Read BD_ADDR (0x1009) parameters.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct HciBdAddr {
    pub addr: [u8; 6],
}

impl TryFrom<&HciCommandComplete> for HciBdAddr {
    type Error = MalformedPayload;

    fn try_from(cc: &HciCommandComplete) -> Result<Self, Self::Error> {
        if cc.opcode != HCI_OP_READ_BD_ADDR || cc.params.len() != 6 {
            return Err(MalformedPayload);
        }
        // `params` is exactly 6 bytes here, so this conversion cannot fail.
        Ok(Self {
            addr: cc.params[..6].try_into().unwrap(),
        })
    }
}

/// Human-readable name for an LMP/LL version number.
pub fn hci_lmp_version_name(version: u8) -> &'static str {
    match version {
        0 => "Reserved",
        1 => "Bluetooth 2.0+EDR",
        2 => "Bluetooth 2.1+EDR",
        3 => "Bluetooth 3.0+HS",
        4 => "Bluetooth 4.0",
        5 => "Bluetooth 4.1",
        6 => "Bluetooth 4.2",
        7 => "Bluetooth 5.0",
        8 => "Bluetooth 5.1",
        9 => "Bluetooth 5.2",
        10 => "Bluetooth 5.3",
        11 => "Bluetooth 5.4",
        _ => "Unknown",
    }
}

/// Human-readable name for a well-known HCI manufacturer id.
pub fn hci_manufacturer_name(id: u16) -> Option<&'static str> {
    match id {
        0x000D => Some("Qualcomm"),
        0x000F => Some("Broadcom"),
        0x000A => Some("Cavium"),
        0x0015 => Some("Atheros"),
        0x0018 => Some("Mitsubishi"),
        0x001D => Some("CSR"),
        0x0024 => Some("Apple"),
        0x002C => Some("Texas Instruments"),
        0x002F => Some("Ericsson"),
        0x004C => Some("Samsung"),
        0x0059 => Some("Nordic Semiconductor"),
        0x0075 => Some("MediaTek"),
        _ => None,
    }
}

/// Renders a Command Complete event as a human-readable string, decoding
/// the parameters for the well-known opcodes.
pub fn format_hci_command_complete(cc: &HciCommandComplete) -> String {
    match cc.opcode {
        HCI_OP_RESET => "Reset: complete".to_string(),
        HCI_OP_READ_LOCAL_VERSION => match HciVersionInfo::try_from(cc) {
            Ok(v) => {
                let mfr = hci_manufacturer_name(v.manufacturer)
                    .map(|m| format!(" ({m})"))
                    .unwrap_or_default();
                let sdp: String = v
                    .sdp
                    .iter()
                    .map(|b| format!("{b:02X}"))
                    .collect::<Vec<_>>()
                    .join("");
                format!(
                    "Version: HCI {} LMP/LL {} ({}) UTC 0x{:04X} Manufacturer 0x{:04X}{mfr} SDP {sdp}",
                    v.hci_version,
                    v.lmp_version,
                    hci_lmp_version_name(v.lmp_version),
                    v.utf,
                    v.manufacturer,
                )
            }
            Err(_) => "Version: malformed parameters".to_string(),
        },
        HCI_OP_READ_BD_ADDR => match HciBdAddr::try_from(cc) {
            Ok(a) => format!(
                "BD ADDR: {:02X}:{:02X}:{:02X}:{:02X}:{:02X}:{:02X}",
                a.addr[5], a.addr[4], a.addr[3], a.addr[2], a.addr[1], a.addr[0]
            ),
            Err(_) => "BD ADDR: malformed parameters".to_string(),
        },
        _ => {
            let params: String = cc
                .params
                .iter()
                .map(|b| format!("{b:02X}"))
                .collect::<Vec<_>>()
                .join(" ");
            format!(
                "Opcode 0x{opcode:04X}: complete, params[{len}]: {params}",
                opcode = cc.opcode,
                len = cc.params.len()
            )
        }
    }
}

/// Session errors reported back to the caller.
#[derive(Debug)]
pub enum SessionError {
    /// A serial port read, write, or flush failed.
    Io(std::io::Error),
    /// No complete frame arrived before the deadline.
    Timeout,
    /// A device frame carried a protocol error.
    DeviceError(ErrorType),
    /// A frame arrived that does not match the expected response.
    UnexpectedFrame(Frame),
    /// A protobuf payload failed to decode.
    Protobuf(prost::DecodeError),
    /// An outgoing payload exceeded the 4096-byte link limit.
    PayloadTooLong(usize),
}

impl std::fmt::Display for SessionError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::Io(e) => write!(f, "serial I/O error: {e}"),
            Self::Timeout => write!(f, "timed out waiting for a response frame"),
            Self::DeviceError(e) => write!(f, "device reported protocol error {e:?}"),
            Self::UnexpectedFrame(frame) => write!(f, "unexpected frame: {frame:?}"),
            Self::Protobuf(e) => write!(f, "protobuf decode failed: {e}"),
            Self::PayloadTooLong(len) => {
                write!(f, "payload length {len} exceeds the 4096-byte limit")
            }
        }
    }
}

impl std::error::Error for SessionError {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            Self::Io(e) => Some(e),
            Self::Protobuf(e) => Some(e),
            _ => None,
        }
    }
}

/// A host session over a serial port: owns the host->device sequence
/// counter and the frame read loop.
pub struct Session {
    port: Box<dyn serialport::SerialPort>,
    tx_seq: u8,
    crc_enabled: bool,
}

impl Session {
    /// Creates a session on an open serial port.
    ///
    /// The host sequence counter starts at zero; call [`Session::sync`]
    /// before sending any command frame.
    pub fn new(port: Box<dyn serialport::SerialPort>) -> Self {
        Self {
            port,
            tx_seq: 0,
            crc_enabled: false,
        }
    }

    /// Enable or disable the per-frame CRC32C feature. When enabled, frames
    /// this session transmits set the `crc` header bit and append a 4-byte
    /// CRC32C trailer, and received frames carrying the bit are verified
    /// (a mismatch is dropped). Off by default.
    pub fn set_crc_enabled(&mut self, enabled: bool) {
        self.crc_enabled = enabled;
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
    ///
    /// # Errors
    ///
    /// Returns [`SessionError::Timeout`] when no complete, well-formed frame
    /// arrives before `deadline`, and [`SessionError::Io`] on port failures.
    pub fn read_frame(&mut self, deadline: Instant) -> Result<Frame, SessionError> {
        let mut header = [0u8; 4];
        if self.read_bytes(&mut header, deadline)? != 4 {
            return Err(SessionError::Timeout);
        }
        let length = u16::from_be_bytes([header[2], header[3]]) as usize;
        if length > MAX_PAYLOAD_LEN {
            return Err(SessionError::Timeout);
        }
        let crc = header[1] & 0x01 != 0;
        let mut full = Vec::with_capacity(4 + length + (if crc { 4 } else { 0 }));
        full.extend_from_slice(&header);
        if length > 0 {
            let mut payload = vec![0u8; length];
            if self.read_bytes(&mut payload, deadline)? != length {
                return Err(SessionError::Timeout);
            }
            full.extend_from_slice(&payload);
        }
        if crc {
            let mut trailer = [0u8; 4];
            if self.read_bytes(&mut trailer, deadline)? != 4 {
                return Err(SessionError::Timeout);
            }
            full.extend_from_slice(&trailer);
        }
        Frame::parse(&full).ok_or(SessionError::Timeout)
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
            self.send(&Frame::protocol_info_query(self.tx_seq, self.crc_enabled))?;
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
        traffic_type: SerialType,
        payload: &[u8],
        deadline: Instant,
        matches: &dyn Fn(&Frame) -> bool,
    ) -> Result<Frame, SessionError> {
        let seq = self.tx_seq;
        self.tx_seq = (self.tx_seq + 1) & 0x0F;
        self.send(&Frame::encode_host_frame(
            traffic_type,
            seq,
            self.crc_enabled,
            payload,
        )?)?;

        let mut retried = false;
        loop {
            let frame = self.read_frame(deadline)?;
            if frame.error != ErrorType::NoError {
                let retryable = matches!(
                    frame.error,
                    ErrorType::IncorrectSequenceNumber | ErrorType::LengthTooLong
                );
                if retryable && !retried {
                    let info = self.sync(deadline)?;
                    self.tx_seq = (info.next_seq + 1) & 0x0F;
                    self.send(&Frame::encode_host_frame(
                        traffic_type,
                        info.next_seq,
                        self.crc_enabled,
                        payload,
                    )?)?;
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

    /// The next host -> device sequence number this session will use.
    pub fn tx_seq(&self) -> u8 {
        self.tx_seq
    }
}

/// Decodes a device->host frame as a WifiMgmtResponse.
///
/// # Errors
///
/// Returns [`SessionError::UnexpectedFrame`] for non-Wi-Fi-management
/// frames and [`SessionError::Protobuf`] when the payload is not a valid
/// `WifiMgmtResponse`.
pub fn decode_wifi_mgmt_response(frame: &Frame) -> Result<WifiMgmtResponse, SessionError> {
    if frame.traffic_type != SerialType::WifiMgmt {
        return Err(SessionError::UnexpectedFrame(frame.clone()));
    }
    WifiMgmtResponse::decode(frame.payload.as_slice()).map_err(SessionError::Protobuf)
}

/// Decodes a device->host frame as a WifiMgmtNotification carrying a
/// ScanDoneEvent.
pub fn decode_scan_done_notification(
    frame: &Frame,
) -> Option<(i32, Vec<wifi_mgmt::WifiScanResult>)> {
    if frame.traffic_type != SerialType::WifiMgmt {
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
    request.encode_to_vec()
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
    request.encode_to_vec()
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
    request.encode_to_vec()
}

/// Builds the encoded protobuf payload of a DISCONNECT request.
pub fn disconnect_request_payload(iface_index: u32) -> Vec<u8> {
    let request = WifiMgmtRequest {
        request_id: RequestType::Disconnect as i32,
        payload: Some(wifi_mgmt_request::Payload::DisconnectReq(
            WifiDisconnectRequest { iface_index },
        )),
    };
    request.encode_to_vec()
}

/// Builds the encoded protobuf payload of an AP_ENABLE request.
pub fn ap_enable_request_payload(
    iface_index: u32,
    ssid: &str,
    channel: u32,
    band: i32,
    security_type: WifiSecurityType,
    psk: Option<&str>,
) -> Vec<u8> {
    let request = WifiMgmtRequest {
        request_id: RequestType::ApEnable as i32,
        payload: Some(wifi_mgmt_request::Payload::ApEnableReq(
            WifiApEnableRequest {
                iface_index,
                params: Some(WifiApEnableParams {
                    ssid: ssid.as_bytes().to_vec(),
                    channel,
                    band,
                    security_type: security_type as i32,
                    psk: psk.unwrap_or_default().as_bytes().to_vec(),
                    hidden_ssid: false,
                    beacon_interval_tu: 0,
                    dtim_period: 0,
                }),
            },
        )),
    };
    request.encode_to_vec()
}

/// Builds the encoded protobuf payload of an AP_DISABLE request.
pub fn ap_disable_request_payload(iface_index: u32) -> Vec<u8> {
    let request = WifiMgmtRequest {
        request_id: RequestType::ApDisable as i32,
        payload: Some(wifi_mgmt_request::Payload::ApDisableReq(
            WifiApDisableRequest { iface_index },
        )),
    };
    request.encode_to_vec()
}

/// Builds the encoded protobuf payload of a SEND_RAW_FRAME request.
///
/// `frame` is a complete IEEE 802.11 MAC frame (frame control through
/// payload, FCS not included). The device transmits it on the air interface
/// without any TCP/IP processing.
pub fn send_raw_frame_request_payload(iface_index: u32, frame: &[u8]) -> Vec<u8> {
    let request = WifiMgmtRequest {
        request_id: RequestType::SendRawFrame as i32,
        payload: Some(wifi_mgmt_request::Payload::SendRawFrameReq(
            SendRawFrameRequest {
                iface_index,
                frame: frame.to_vec(),
            },
        )),
    };
    request.encode_to_vec()
}

/// Builds the encoded protobuf payload of an ENABLE_RAW_RX request.
///
/// Enables the device's raw 802.11 RX (promiscuous sniffer) so frames it
/// transmits are captured and reported back to the host as `RawFrameRxEvent`
/// notifications (data connectivity loopback testing).
pub fn enable_raw_rx_request_payload(iface_index: u32, channel: u32) -> Vec<u8> {
    let request = WifiMgmtRequest {
        request_id: RequestType::EnableRawRx as i32,
        payload: Some(wifi_mgmt_request::Payload::EnableRawRxReq(
            EnableRawRxRequest {
                iface_index,
                channel,
            },
        )),
    };
    request.encode_to_vec()
}

/// Builds the encoded protobuf payload of a DISABLE_RAW_RX request.
///
/// Disables the device's raw 802.11 RX (promiscuous sniffer).
pub fn disable_raw_rx_request_payload(iface_index: u32) -> Vec<u8> {
    let request = WifiMgmtRequest {
        request_id: RequestType::DisableRawRx as i32,
        payload: Some(wifi_mgmt_request::Payload::DisableRawRxReq(
            DisableRawRxRequest { iface_index },
        )),
    };
    request.encode_to_vec()
}

/// Decodes a device->host frame as a WifiMgmtNotification carrying a
/// RawFrameRxEvent. Returns the received raw 802.11 frame bytes (which may
/// include a driver prefix before the 802.11 header).
pub fn decode_raw_frame_rx_event(frame: &Frame) -> Option<Vec<u8>> {
    if frame.traffic_type != SerialType::WifiMgmt {
        return None;
    }
    let notification = WifiMgmtNotification::decode(frame.payload.as_slice()).ok()?;
    let event = notification.event?;
    match event.event_data {
        Some(wifi_event::EventData::RawFrameRxEvent(e)) => Some(e.frame),
        _ => None,
    }
}

/// Decodes a device->host frame as a WifiMgmtResponse carrying an
/// EnableRawRxResponse with diagnostic state.
pub fn decode_enable_raw_rx_response(frame: &Frame) -> Option<EnableRawRxResponse> {
    if frame.traffic_type != SerialType::WifiMgmt {
        return None;
    }
    let response = WifiMgmtResponse::decode(frame.payload.as_slice()).ok()?;
    match response.payload {
        Some(wifi_mgmt_response::Payload::EnableRawRxResp(r)) => Some(r),
        _ => None,
    }
}

/// Decodes a device->host frame as a WifiMgmtNotification carrying an
/// ApEnableResultEvent. Returns `(is_enable, status)`; `status` is 0 on
/// success and a negative errno-style code on failure.
pub fn decode_ap_result_notification(frame: &Frame) -> Option<(bool, i32)> {
    if frame.traffic_type != SerialType::WifiMgmt {
        return None;
    }
    let notification = WifiMgmtNotification::decode(frame.payload.as_slice()).ok()?;
    let event = notification.event?;
    match event.event_data {
        Some(wifi_event::EventData::ApEnableResultEvent(e)) => Some((true, e.status)),
        Some(wifi_event::EventData::ApDisableResultEvent(e)) => Some((false, e.status)),
        _ => None,
    }
}

/// Decodes a device->host frame as a WifiMgmtNotification carrying an
/// ApStaConnectedEvent or ApStaDisconnectedEvent. Returns
/// `(connected, sta_mac)`.
pub fn decode_ap_sta_event(frame: &Frame) -> Option<(bool, [u8; 6])> {
    if frame.traffic_type != SerialType::WifiMgmt {
        return None;
    }
    let notification = WifiMgmtNotification::decode(frame.payload.as_slice()).ok()?;
    let event = notification.event?;
    match event.event_data {
        Some(wifi_event::EventData::ApStaConnectedEvent(e)) => {
            let mac: [u8; 6] = e.sta_mac_address.as_slice().try_into().ok()?;
            Some((true, mac))
        }
        Some(wifi_event::EventData::ApStaDisconnectedEvent(e)) => {
            let mac: [u8; 6] = e.sta_mac_address.as_slice().try_into().ok()?;
            Some((false, mac))
        }
        _ => None,
    }
}

/// Decodes a device->host frame as a WifiMgmtResponse and returns its
/// generic status payload (CONNECT/SCAN/DISCONNECT responses).
pub fn decode_wifi_status_response(frame: &Frame) -> Result<WifiStatusResponse, String> {
    let resp = decode_wifi_mgmt_response(frame)
        .map_err(|_| "unable to decode Wi-Fi management response".to_string())?;
    match resp.payload {
        Some(wifi_mgmt_response::Payload::StatusResp(status)) => Ok(status),
        Some(_) => Err("received non-status response payload".into()),
        None => Err("empty response payload".into()),
    }
}

/// Decodes a device->host frame as a NetIfResponse and verifies the
/// request status field.
pub fn decode_interfaces_response(frame: &Frame) -> Result<NetIfResponse, String> {
    let response = NetIfResponse::decode(frame.payload.as_slice())
        .map_err(|e| format!("unable to decode network interface response: {e}"))?;
    if response.status != 0 {
        return Err(format!(
            "network interface query failed: status={}",
            response.status
        ));
    }
    Ok(response)
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
    request.encode_to_vec()
}

/// Renders a GET_WIFI_VERSION response payload as a string.
pub fn format_version_response(frame: &Frame) -> String {
    let resp = match decode_wifi_mgmt_response(frame) {
        Ok(resp) => resp,
        Err(_) => return "unable to decode version response".into(),
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
        Some(_) => "unexpected response payload".into(),
        None => "empty response payload".into(),
    }
}

/// Renders a CONNECT/SCAN status response payload as a string.
pub fn format_status_response(frame: &Frame) -> Result<String, String> {
    let resp = decode_wifi_mgmt_response(frame)
        .map_err(|_| "unable to decode status response".to_string())?;
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
        Some(_) => Err("received non-status response payload".into()),
        None => Err("empty response payload".into()),
    }
}

/// Renders a GET_INTERFACES response payload as a string.
pub fn format_interfaces_response(frame: &Frame) -> String {
    let response = match NetIfResponse::decode(frame.payload.as_slice()) {
        Ok(response) => response,
        Err(_) => return "unable to decode network interface response".into(),
    };

    if response.status != 0 {
        return format!("network interface query failed: status={}", response.status);
    }

    match response.payload {
        Some(net_if_response::Payload::Interfaces(interfaces)) => {
            if interfaces.net_ifs.is_empty() {
                return "No network interfaces found".into();
            }

            let mut lines = Vec::with_capacity(interfaces.net_ifs.len() + 1);
            lines.push(format!(
                "Found {} network interface(s):",
                interfaces.net_ifs.len()
            ));
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
        Some(_) => "received non-interface response payload".into(),
        None => "empty response payload".into(),
    }
}

/// Builds the encoded protobuf payload of a SET_REG_DOMAIN request.
pub fn set_reg_domain_request_payload(iface_index: u32, country: &str, force: bool) -> Vec<u8> {
    let request = WifiMgmtRequest {
        request_id: RequestType::SetRegDomain as i32,
        payload: Some(wifi_mgmt_request::Payload::SetRegDomainReq(
            SetRegulatoryDomainRequest {
                iface_index,
                reg_domain: Some(WifiRegDomain {
                    country_code: country.to_uppercase(),
                    force_update: force,
                    channel_info: Vec::new(),
                }),
            },
        )),
    };
    request.encode_to_vec()
}

/// Builds the encoded protobuf payload of a GET_REG_DOMAIN request.
pub fn get_reg_domain_request_payload(iface_index: u32) -> Vec<u8> {
    let request = WifiMgmtRequest {
        request_id: RequestType::GetRegDomain as i32,
        payload: Some(wifi_mgmt_request::Payload::GetRegDomainReq(
            GetRegulatoryDomainRequest { iface_index },
        )),
    };
    request.encode_to_vec()
}

/// Decodes a device->host frame as a GET_REG_DOMAIN response.
pub fn decode_reg_domain_response(frame: &Frame) -> Result<WifiRegDomain, String> {
    let resp = decode_wifi_mgmt_response(frame)
        .map_err(|e| format!("unable to decode Wi-Fi management response: {e}"))?;
    match resp.payload {
        Some(wifi_mgmt_response::Payload::GetRegDomainResp(resp)) => resp
            .reg_domain
            .ok_or_else(|| "empty regulatory domain response".to_string()),
        Some(_) => Err("received non-reg-domain response payload".into()),
        None => Err("empty response payload".into()),
    }
}

/// Renders a GET_REG_DOMAIN response payload as a string.
pub fn format_reg_domain_response(frame: &Frame) -> String {
    let domain = match decode_reg_domain_response(frame) {
        Ok(domain) => domain,
        Err(e) => return e,
    };
    let mut lines = vec![format!(
        "Country: {} (force_update={})",
        domain.country_code, domain.force_update
    )];
    if domain.channel_info.is_empty() {
        lines.push("- no channel information".to_string());
    }
    for chan in &domain.channel_info {
        lines.push(format!(
            "- freq={} MHz max_power={} dBm supported={} passive_only={} dfs={}",
            chan.center_frequency_mhz,
            chan.max_power_dbm,
            chan.supported,
            chan.passive_only,
            chan.dfs,
        ));
    }
    lines.join("\n")
}

/// Builds the encoded protobuf payload of a SET_PS_CONFIG request.
pub fn set_ps_request_payload(
    iface_index: u32,
    enabled: bool,
    listen_interval: u32,
    timeout_ms: u32,
    wmm: bool,
) -> Vec<u8> {
    let request = WifiMgmtRequest {
        request_id: RequestType::SetPsConfig as i32,
        payload: Some(wifi_mgmt_request::Payload::SetPsConfigReq(
            SetPowerSaveConfigRequest {
                iface_index,
                config: Some(WifiPowerSaveConfig {
                    ps_settings: Some(WifiPsSettings {
                        enabled: (if enabled {
                            WifiPsState::Enable
                        } else {
                            WifiPsState::Disable
                        }) as i32,
                        // DTIM wakeup is the driver default.
                        wakeup_mode: 0,
                        listen_interval_beacons: listen_interval,
                        timeout_ms,
                        fail_all_scans_if_ps: false,
                        fail_reason: 0,
                        exit_strategy: 0,
                    }),
                    r#type: (if wmm {
                        WifiPowerSaveType::WifiPsTypeWmm
                    } else {
                        WifiPowerSaveType::WifiPsTypeModem
                    }) as i32,
                }),
            },
        )),
    };
    request.encode_to_vec()
}

/// Builds the encoded protobuf payload of a GET_PS_CONFIG request.
pub fn get_ps_request_payload(iface_index: u32) -> Vec<u8> {
    let request = WifiMgmtRequest {
        request_id: RequestType::GetPsConfig as i32,
        payload: Some(wifi_mgmt_request::Payload::GetPsConfigReq(
            GetPowerSaveConfigRequest { iface_index },
        )),
    };
    request.encode_to_vec()
}

/// Decodes a device->host frame as a GET_PS_CONFIG response.
pub fn decode_ps_response(frame: &Frame) -> Result<WifiPowerSaveConfig, String> {
    let resp = decode_wifi_mgmt_response(frame)
        .map_err(|e| format!("unable to decode Wi-Fi management response: {e}"))?;
    match resp.payload {
        Some(wifi_mgmt_response::Payload::GetPsConfigResp(resp)) => resp
            .config
            .ok_or_else(|| "empty power save config response".to_string()),
        Some(_) => Err("received non-power-save response payload".into()),
        None => Err("empty response payload".into()),
    }
}

/// Renders a GET_PS_CONFIG response payload as a string.
pub fn format_ps_response(frame: &Frame) -> String {
    let config = match decode_ps_response(frame) {
        Ok(config) => config,
        Err(e) => return e,
    };
    let settings = match config.ps_settings {
        Some(settings) => settings,
        None => return "no power save settings in response".into(),
    };
    let enabled = WifiPsState::try_from(settings.enabled)
        .map(|s| s.as_str_name())
        .unwrap_or("UNKNOWN");
    let ps_type = WifiPowerSaveType::try_from(config.r#type)
        .map(|t| t.as_str_name())
        .unwrap_or("UNKNOWN");
    format!(
        "Power save: {enabled} type={} listen_interval={} timeout_ms={} wakeup={} exit={}",
        ps_type,
        settings.listen_interval_beacons,
        settings.timeout_ms,
        settings.wakeup_mode,
        settings.exit_strategy,
    )
}

/// Builds the encoded protobuf payload of a GET_IFACE_STATUS request.
pub fn iface_status_request_payload(iface_index: u32) -> Vec<u8> {
    let request = WifiMgmtRequest {
        request_id: RequestType::GetIfaceStatus as i32,
        payload: Some(wifi_mgmt_request::Payload::GetIfaceStatusReq(
            GetInterfaceStatusRequest { iface_index },
        )),
    };
    request.encode_to_vec()
}

/// Decodes a device->host frame as a GET_IFACE_STATUS response.
pub fn decode_iface_status_response(frame: &Frame) -> Result<WifiInterfaceStatus, String> {
    let resp = decode_wifi_mgmt_response(frame)
        .map_err(|e| format!("unable to decode Wi-Fi management response: {e}"))?;
    match resp.payload {
        Some(wifi_mgmt_response::Payload::GetIfaceStatusResp(resp)) => resp
            .status
            .ok_or_else(|| "empty interface status response".to_string()),
        Some(_) => Err("received non-interface-status response payload".into()),
        None => Err("empty response payload".into()),
    }
}

/// Renders a GET_IFACE_STATUS response payload as a string.
pub fn format_iface_status_response(frame: &Frame) -> String {
    let status = match decode_iface_status_response(frame) {
        Ok(status) => status,
        Err(e) => return e,
    };
    let ssid = if status.ssid.is_empty() {
        "(none)".to_string()
    } else {
        String::from_utf8_lossy(&status.ssid).into_owned()
    };
    let bssid = status
        .bssid
        .iter()
        .map(|b| format!("{b:02x}"))
        .collect::<Vec<_>>()
        .join(":");
    let state = WifiInterfaceState::try_from(status.state)
        .map(|s| s.as_str_name())
        .unwrap_or("UNKNOWN");
    let security = WifiSecurityType::try_from(status.security)
        .map(|t| t.as_str_name())
        .unwrap_or("UNKNOWN");
    format!(
        "state={} ssid={} bssid={} ch={} rssi={} dBm security={} tx={} kbps rx={} kbps",
        state,
        ssid,
        bssid,
        status.channel,
        status.rssi,
        security,
        status.tx_bitrate_kbps,
        status.rx_bitrate_kbps,
    )
}

/// Renders scan results (status + per-network lines) as a string.
pub fn format_scan_results(status: i32, results: &[wifi_mgmt::WifiScanResult]) -> String {
    let mut lines = Vec::with_capacity(results.len() + 1);
    lines.push(format!(
        "Scan done: status={}, results={}",
        status,
        results.len()
    ));
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
    use crate::mock::{MockPort, MockSerialState};
    use wifi_mgmt::{
        ApDisableResultEvent, ApEnableResultEvent, ApStaConnectedEvent, ApStaDisconnectedEvent,
        ScanDoneEvent, WifiEvent,
    };

    #[test]
    fn host_frame_has_device_bit_layout() {
        // byte0: type in low nibble, no_ack/host/error zero for host->device
        // byte1: sequence in high nibble, reserved zero
        // bytes 2..3: length big-endian
        let frame =
            Frame::encode_host_frame(SerialType::WifiMgmt, 5, false, &[1, 2, 3, 4]).expect("encodes");

        assert_eq!(frame.len(), 8);
        assert_eq!(frame[0], 0x00); // type 0 (WIFI_MGMT)
        assert_eq!(frame[1], 0x50); // seq 5, reserved 0
        assert_eq!(frame[2], 0x00); // length high
        assert_eq!(frame[3], 0x04); // length low

        let parsed = Frame::parse(&frame).expect("frame should parse");
        assert_eq!(parsed.traffic_type, SerialType::WifiMgmt);
        assert_eq!(parsed.seq, 5);
        assert!(!parsed.to_host);
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
        assert_eq!(parsed.traffic_type, SerialType::WifiMgmt);
        assert!(parsed.to_host);
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
        assert_eq!(parsed.traffic_type, SerialType::WifiMgmt);
        assert!(parsed.to_host);
        assert_eq!(parsed.error, ErrorType::IncorrectSequenceNumber);
        assert_eq!(parsed.seq, 2);
        assert_eq!(parsed.payload, vec![0xAB]);
    }

    #[test]
    fn protocol_info_query_is_type_7_with_payload() {
        let query = Frame::protocol_info_query(0, false);
        let parsed = Frame::parse(&query).expect("info query should parse");
        assert_eq!(parsed.traffic_type, SerialType::ProtocolInfo);
        assert_eq!(parsed.payload.len(), 1);
    }

    #[test]
    fn protocol_info_payload_round_trip() {
        // [version=1, mask_lo=0x1F, mask_hi=0x00, next_seq=3]
        let payload = [1u8, 0x1F, 0x00, 3];
        let frame =
            Frame::encode_host_frame(SerialType::ProtocolInfo, 0, false, &payload).expect("encodes");
        let parsed = Frame::parse(&frame).expect("frame should parse");
        let info = ProtocolInfo::try_from(&parsed).expect("info should decode");
        assert_eq!(info.version, 1);
        assert_eq!(info.supported_types_mask, 0x001F);
        assert_eq!(info.next_seq, 3);
    }

    #[test]
    fn scan_request_payload_encodes_iface_index() {
        let payload = scan_request_payload(7);
        let request = WifiMgmtRequest::decode(&payload[..]).expect("request should decode");
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
        let payload = connect_request_payload(
            2,
            "test-ssid",
            Some("secretpass"),
            WifiSecurityType::Psk,
            5000,
        );
        let request = WifiMgmtRequest::decode(&payload[..]).expect("request should decode");
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

    fn ap_notification_frame(event_data: wifi_event::EventData) -> Frame {
        let notification = WifiMgmtNotification {
            event: Some(WifiEvent {
                iface_index: 0,
                event_data: Some(event_data),
            }),
        };
        let mut out = Vec::new();
        notification.encode(&mut out).unwrap();
        Frame {
            traffic_type: SerialType::WifiMgmt,
            no_ack: true,
            to_host: true,
            error: ErrorType::NoError,
            crc: false,
            seq: 0,
            payload: out,
        }
    }

    #[test]
    fn ap_enable_request_payload_encodes_params() {
        let payload =
            ap_enable_request_payload(1, "ap-ssid", 6, 1, WifiSecurityType::Psk, Some("ap-psk"));
        let request = WifiMgmtRequest::decode(&payload[..]).expect("request should decode");
        assert_eq!(request.request_id, RequestType::ApEnable as i32);
        match request.payload {
            Some(wifi_mgmt_request::Payload::ApEnableReq(ap_req)) => {
                assert_eq!(ap_req.iface_index, 1);
                let params = ap_req.params.expect("params should be set");
                assert_eq!(params.ssid, b"ap-ssid");
                assert_eq!(params.channel, 6);
                assert_eq!(params.band, 1);
                assert_eq!(params.security_type, WifiSecurityType::Psk as i32);
                assert_eq!(params.psk, b"ap-psk");
                assert!(!params.hidden_ssid);
                assert_eq!(params.beacon_interval_tu, 0);
                assert_eq!(params.dtim_period, 0);
            }
            other => panic!("unexpected payload: {other:?}"),
        }
    }

    #[test]
    fn ap_disable_request_payload_encodes_iface_index() {
        let payload = ap_disable_request_payload(2);
        let request = WifiMgmtRequest::decode(&payload[..]).expect("request should decode");
        assert_eq!(request.request_id, RequestType::ApDisable as i32);
        match request.payload {
            Some(wifi_mgmt_request::Payload::ApDisableReq(ap_req)) => {
                assert_eq!(ap_req.iface_index, 2);
            }
            other => panic!("unexpected payload: {other:?}"),
        }
    }

    #[test]
    fn send_raw_frame_request_payload_encodes_frame() {
        let frame = vec![0x40, 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66];
        let payload = send_raw_frame_request_payload(1, &frame);
        let request = WifiMgmtRequest::decode(&payload[..]).expect("request should decode");
        assert_eq!(request.request_id, RequestType::SendRawFrame as i32);
        match request.payload {
            Some(wifi_mgmt_request::Payload::SendRawFrameReq(raw_req)) => {
                assert_eq!(raw_req.iface_index, 1);
                assert_eq!(raw_req.frame, frame);
            }
            other => panic!("unexpected payload: {other:?}"),
        }
    }

    #[test]
    fn decode_ap_result_notification_parses_enable_and_disable() {
        let enable = ap_notification_frame(wifi_event::EventData::ApEnableResultEvent(
            ApEnableResultEvent { status: 0 },
        ));
        assert_eq!(decode_ap_result_notification(&enable), Some((true, 0)));

        let disable = ap_notification_frame(wifi_event::EventData::ApDisableResultEvent(
            ApDisableResultEvent { status: -1 },
        ));
        assert_eq!(decode_ap_result_notification(&disable), Some((false, -1)));
    }

    #[test]
    fn decode_ap_result_notification_ignores_other_events() {
        let scan_done =
            ap_notification_frame(wifi_event::EventData::ScanDoneEvent(ScanDoneEvent {
                status: 0,
                all_results: vec![],
            }));
        assert_eq!(decode_ap_result_notification(&scan_done), None);
        assert_eq!(decode_ap_sta_event(&scan_done), None);
    }

    #[test]
    fn decode_ap_sta_event_parses_connected_and_disconnected() {
        let mac = [0x01, 0x02, 0x03, 0x04, 0x05, 0x06];
        let connected = ap_notification_frame(wifi_event::EventData::ApStaConnectedEvent(
            ApStaConnectedEvent {
                sta_mac_address: mac.to_vec(),
            },
        ));
        assert_eq!(decode_ap_sta_event(&connected), Some((true, mac)));
        assert_eq!(decode_ap_result_notification(&connected), None);

        let disconnected = ap_notification_frame(wifi_event::EventData::ApStaDisconnectedEvent(
            ApStaDisconnectedEvent {
                sta_mac_address: mac.to_vec(),
            },
        ));
        assert_eq!(decode_ap_sta_event(&disconnected), Some((false, mac)));
    }

    #[test]
    fn interfaces_request_payload_encodes_type_and_index() {
        let payload = interfaces_request_payload(NetIfType::Wifi, 3);
        let request = NetIfRequest::decode(&payload[..]).expect("request should decode");
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

    // ---- In-memory device model for Session-level tests ---------------
    //
    // Mirrors the sequence handling in zephyr/src/serial/serial.c:
    //   * host frame with seq != expected -> IncorrectSequenceNumber error
    //     ACK (echoing the sequence the device expected), then the expected
    //     sequence resets to 0;
    //   * accepted host frame -> client processing (response frame carrying
    //     the device tx sequence) followed by a NoError ACK echoing the
    //     host seq, then the expected sequence advances (wrap at 16);
    //   * PROTOCOL_INFO -> 4-byte response [version, mask_lo, mask_hi,
    //     expected rx seq]; never sequence-checked, consumes no slot.

    struct MockDeviceState {
        /// Bytes the device has queued for the host to read.
        to_host: Vec<u8>,
        /// Maximum bytes a single read call returns (partial-read simulation).
        max_read: usize,
        /// Host bytes not yet forming a complete frame.
        from_host: Vec<u8>,
        /// Sequence the device expects on the next host->device frame.
        rx_seq: u8,
        /// Sequence the device uses on device->host response frames.
        tx_seq: u8,
        version: u8,
        supported_types_mask: u16,
        /// Payload the device sends as the response after each accepted
        /// command's ACK.
        response_payload: Vec<u8>,
        /// When set, PROTOCOL_INFO answers rx_seq + 1 instead of rx_seq:
        /// a device that lies about the sequence it expects.
        corrupt_protocol_info: bool,
        incorrect_seq_acks: u32,
        protocol_info_queries: u32,
        commands_accepted: u32,
    }

    impl MockDeviceState {
        fn new(rx_seq: u8, response_payload: Vec<u8>) -> Self {
            Self {
                to_host: Vec::new(),
                max_read: usize::MAX,
                from_host: Vec::new(),
                rx_seq,
                tx_seq: 0,
                version: 1,
                supported_types_mask: 0x019F,
                response_payload,
                corrupt_protocol_info: false,
                incorrect_seq_acks: 0,
                protocol_info_queries: 0,
                commands_accepted: 0,
            }
        }

        fn handle_host_frame(&mut self, frame: &Frame) {
            if frame.traffic_type == SerialType::ProtocolInfo {
                self.protocol_info_queries += 1;
                let next_seq = if self.corrupt_protocol_info {
                    (self.rx_seq + 1) & 0x0F
                } else {
                    self.rx_seq
                };
                let mask = self.supported_types_mask.to_le_bytes();
                let payload = [self.version, mask[0], mask[1], next_seq];
                self.emit_frame(
                    SerialType::ProtocolInfo,
                    ErrorType::NoError,
                    self.tx_seq,
                    &payload,
                    true,
                );
                return;
            }
            if frame.seq != self.rx_seq {
                // Echo the sequence we expected, then reset to 0.
                self.incorrect_seq_acks += 1;
                self.emit_frame(
                    frame.traffic_type,
                    ErrorType::IncorrectSequenceNumber,
                    self.rx_seq,
                    &[],
                    false,
                );
                self.rx_seq = 0;
                return;
            }
            self.commands_accepted += 1;
            let response_tx_seq = self.tx_seq;
            let response = self.response_payload.clone();
            if !response.is_empty() {
                self.emit_frame(
                    frame.traffic_type,
                    ErrorType::NoError,
                    response_tx_seq,
                    &response,
                    true,
                );
            }
            self.emit_frame(
                frame.traffic_type,
                ErrorType::NoError,
                frame.seq,
                &[],
                false,
            );
            self.rx_seq = (self.rx_seq + 1) & 0x0F;
        }

        fn emit_frame(
            &mut self,
            traffic_type: SerialType,
            error: ErrorType,
            seq: u8,
            payload: &[u8],
            advance_tx: bool,
        ) {
            let mut out: Vec<u8> = Vec::with_capacity(4 + payload.len());
            out.push(((error as u8) << 6) | (1 << 5) | traffic_type.to_u8()); // host = 1
            out.push((seq & 0x0F) << 4);
            out.extend_from_slice(&(payload.len() as u16).to_be_bytes());
            out.extend_from_slice(payload);
            self.to_host.extend_from_slice(&out);
            if advance_tx {
                self.tx_seq = (self.tx_seq + 1) & 0x0F;
            }
        }
    }
    impl MockSerialState for MockDeviceState {
        fn read_data(&mut self, buf: &mut [u8]) -> usize {
            let n = buf.len().min(self.to_host.len()).min(self.max_read);
            buf[..n].copy_from_slice(&self.to_host[..n]);
            self.to_host.drain(..n);
            n
        }

        fn write_data(&mut self, buf: &[u8]) {
            self.from_host.extend_from_slice(buf);
            loop {
                if self.from_host.len() < 4 {
                    break;
                }
                let len = u16::from_be_bytes([self.from_host[2], self.from_host[3]]) as usize;
                let total = 4 + len;
                if self.from_host.len() < total {
                    break;
                }
                let frame_bytes: Vec<u8> = self.from_host.drain(..total).collect();
                if let Some(frame) = Frame::parse(&frame_bytes) {
                    self.handle_host_frame(&frame);
                }
            }
        }

        fn bytes_ready(&self) -> u32 {
            self.to_host.len() as u32
        }
    }

    fn mock_session(
        start_rx_seq: u8,
        response_payload: Vec<u8>,
    ) -> (Session, std::sync::Arc<std::sync::Mutex<MockDeviceState>>) {
        let state = std::sync::Arc::new(std::sync::Mutex::new(MockDeviceState::new(
            start_rx_seq,
            response_payload,
        )));
        let port: Box<dyn serialport::SerialPort> = Box::new(MockPort(state.clone()));
        (Session::new(port), state)
    }

    fn test_deadline() -> Instant {
        Instant::now() + std::time::Duration::from_secs(2)
    }

    #[test]
    fn sync_aligns_to_device_state_from_previous_session() {
        // The device already consumed sequences 0..6 (e.g. an earlier CLI
        // run); a fresh host session must start at 7, not 0.
        let (mut session, device) = mock_session(7, vec![0x01]);
        let deadline = test_deadline();
        let is_response =
            |f: &Frame| f.traffic_type == SerialType::WifiMgmt && !f.payload.is_empty();

        let info = session.sync(deadline).expect("sync should succeed");
        assert_eq!(info.next_seq, 7);

        session
            .request(SerialType::WifiMgmt, &[0x01], deadline, &is_response)
            .expect("request must use the synced sequence");

        let state = device.lock().unwrap();
        assert_eq!(
            state.incorrect_seq_acks, 0,
            "no mismatch after a correct sync"
        );
        assert_eq!(state.commands_accepted, 1);
        assert_eq!(
            state.rx_seq,
            session.tx_seq(),
            "host and device stay aligned"
        );
    }

    #[test]
    fn request_resyncs_when_device_resets_mid_session() {
        const PAYLOAD: [u8; 4] = [0xDE, 0xAD, 0xBE, 0xEF];
        let (mut session, device) = mock_session(0, PAYLOAD.to_vec());
        let deadline = test_deadline();
        let is_response =
            |f: &Frame| f.traffic_type == SerialType::WifiMgmt && f.payload == PAYLOAD.to_vec();

        // Handshake plus two healthy requests: both sides advance to seq 2.
        let info = session.sync(deadline).expect("initial sync");
        assert_eq!(info.next_seq, 0);
        session
            .request(SerialType::WifiMgmt, &PAYLOAD, deadline, &is_response)
            .expect("request 1");
        session
            .request(SerialType::WifiMgmt, &PAYLOAD, deadline, &is_response)
            .expect("request 2");

        // Simulate a device reset: its expected sequence returns to 0 while
        // the host still believes the device expects 2.
        device.lock().unwrap().rx_seq = 0;

        // The next request must recover: mismatch -> error ACK ->
        // PROTOCOL_INFO resync (device reports 0) -> retry with seq 0.
        let frame = session
            .request(SerialType::WifiMgmt, &PAYLOAD, deadline, &is_response)
            .expect("request 3 must recover from the desync");
        assert_eq!(frame.payload, PAYLOAD.to_vec());

        let state = device.lock().unwrap();
        assert_eq!(
            state.incorrect_seq_acks, 1,
            "exactly one mismatch was reported"
        );
        assert_eq!(state.protocol_info_queries, 2, "initial sync + one resync");
        assert_eq!(
            state.commands_accepted, 3,
            "command delivered after the retry"
        );
        drop(state);

        // The link must stay aligned afterwards: a follow-up request
        // completes without another mismatch.
        session
            .request(SerialType::WifiMgmt, &PAYLOAD, deadline, &is_response)
            .expect("request 4 stays in sync");
        let state = device.lock().unwrap();
        assert_eq!(
            state.incorrect_seq_acks, 1,
            "follow-up request had no mismatch"
        );
        assert_eq!(state.commands_accepted, 4);
        assert_eq!(
            state.rx_seq,
            session.tx_seq(),
            "counters realigned after resync"
        );
    }

    #[test]
    fn request_gives_up_after_single_failed_retry() {
        // A device that answers PROTOCOL_INFO with a wrong expected
        // sequence: the retried frame mismatches again, and the session
        // must stop with an error instead of retrying forever.
        let (mut session, device) = mock_session(7, vec![0x01]);
        device.lock().unwrap().corrupt_protocol_info = true;
        let deadline = test_deadline();
        let is_response =
            |f: &Frame| f.traffic_type == SerialType::WifiMgmt && !f.payload.is_empty();

        let err = session
            .request(SerialType::WifiMgmt, &[0x01], deadline, &is_response)
            .expect_err("the retry cannot align the link");
        match err {
            SessionError::DeviceError(ErrorType::IncorrectSequenceNumber) => {}
            other => panic!("unexpected error: {other:?}"),
        }

        let state = device.lock().unwrap();
        assert_eq!(
            state.incorrect_seq_acks, 2,
            "original frame + one retried frame"
        );
        assert_eq!(state.protocol_info_queries, 1, "a single resync attempt");
        assert_eq!(state.commands_accepted, 0, "nothing was ever processed");
    }

    #[test]
    fn request_wraps_sequence_from_15_to_0() {
        let (mut session, device) = mock_session(15, vec![0x01]);
        let deadline = test_deadline();
        let is_response =
            |f: &Frame| f.traffic_type == SerialType::WifiMgmt && !f.payload.is_empty();

        let info = session.sync(deadline).expect("sync");
        assert_eq!(info.next_seq, 15);

        // seq 15 is accepted, both counters wrap to 0, and the next request
        // proceeds with seq 0.
        session
            .request(SerialType::WifiMgmt, &[0x01], deadline, &is_response)
            .expect("seq 15");
        session
            .request(SerialType::WifiMgmt, &[0x01], deadline, &is_response)
            .expect("seq 0 after wrap");

        let state = device.lock().unwrap();
        assert_eq!(state.incorrect_seq_acks, 0, "no mismatch across the wrap");
        assert_eq!(state.commands_accepted, 2);
        assert_eq!(state.rx_seq, session.tx_seq());
    }

    // ------------------------------------------------------------------
    // HCI helpers
    // ------------------------------------------------------------------

    fn cc_packet(opcode: u16, params: &[u8]) -> Vec<u8> {
        // H4 EVT + Command Complete event (no status field).
        let mut p = vec![
            HCI_H4_EVT,
            HCI_EVT_COMMAND_COMPLETE,
            (4 + params.len()) as u8,
            1,
            (opcode & 0xFF) as u8,
            (opcode >> 8) as u8,
            params.len() as u8,
        ];
        p.extend_from_slice(params);
        p
    }

    #[test]
    fn hci_command_packet_encodes_h4_cmd() {
        let pkt = hci_command_packet(HCI_OP_RESET, &[]);
        assert_eq!(pkt, vec![0x01, 0x03, 0x0C, 0x00]);

        let pkt = hci_command_packet(0x0813, &[0x01, 0x02]);
        assert_eq!(pkt, vec![0x01, 0x13, 0x08, 0x02, 0x01, 0x02]);
    }

    #[test]
    fn hci_command_complete_parses_valid() {
        let pkt = cc_packet(
            HCI_OP_READ_LOCAL_VERSION,
            &[1, 11, 0, 0, 0x59, 0, 0, 0, 0, 0, 0, 0, 0, 0],
        );
        let cc = hci_command_complete(&pkt).expect("parse");
        assert_eq!(cc.num_packets, 1);
        assert_eq!(cc.opcode, HCI_OP_READ_LOCAL_VERSION);
        assert_eq!(cc.params.len(), 14);

        let v = HciVersionInfo::try_from(&cc).expect("version");
        assert_eq!(v.hci_version, 1);
        assert_eq!(v.lmp_version, 11);
        assert_eq!(v.manufacturer, 0x0059);
    }

    #[test]
    fn hci_command_complete_rejects_malformed() {
        // Wrong H4 type.
        let mut p = cc_packet(HCI_OP_RESET, &[]);
        p[0] = HCI_H4_ACL;
        assert!(hci_command_complete(&p).is_none());

        // Wrong event code.
        let mut p = cc_packet(HCI_OP_RESET, &[]);
        p[1] = HCI_EVT_COMMAND_STATUS;
        assert!(hci_command_complete(&p).is_none());

        // Truncated.
        let p = cc_packet(HCI_OP_RESET, &[]);
        assert!(hci_command_complete(&p[..5]).is_none());

        // Length-inconsistent (plen disagrees with param_len).
        let mut p = cc_packet(HCI_OP_RESET, &[0xAA]);
        p[2] = 99;
        assert!(hci_command_complete(&p).is_none());

        // Empty.
        assert!(hci_command_complete(&[]).is_none());
    }

    #[test]
    fn hci_bd_addr_decodes() {
        let pkt = cc_packet(HCI_OP_READ_BD_ADDR, &[1, 2, 3, 4, 5, 6]);
        let cc = hci_command_complete(&pkt).expect("parse");
        let a = HciBdAddr::try_from(&cc).expect("bd addr");
        assert_eq!(a.addr, [1, 2, 3, 4, 5, 6]);
    }

    #[test]
    fn hci_version_info_rejects_wrong_opcode() {
        let pkt = cc_packet(HCI_OP_RESET, &[0; 11]);
        let cc = hci_command_complete(&pkt).expect("parse");
        assert!(HciVersionInfo::try_from(&cc).is_err());
    }

    #[test]
    fn format_hci_command_complete_decodes_version() {
        let params: [u8; 14] = [7, 9, 0x00, 0x02, 0x59, 0x00, 0, 0, 0, 0, 0, 0, 0, 0];
        let pkt = cc_packet(HCI_OP_READ_LOCAL_VERSION, &params);
        let cc = hci_command_complete(&pkt).expect("parse");
        let s = format_hci_command_complete(&cc);
        assert!(s.contains("LMP/LL 9"), "got: {s}");
        assert!(s.contains("Bluetooth 5.2"), "got: {s}");
        assert!(s.contains("Nordic Semiconductor"), "got: {s}");
    }

    #[test]
    fn format_hci_command_complete_generic_params() {
        let pkt = cc_packet(0x2010, &[0xDE, 0xAD]);
        let cc = hci_command_complete(&pkt).expect("parse");
        let s = format_hci_command_complete(&cc);
        assert!(s.contains("DE AD"), "got: {s}");
    }
    #[test]
    fn parse_rejects_oversized_payload_length() {
        // Length field above the 4096-byte limit.
        assert!(Frame::parse(&[0x00, 0x00, 0x10, 0x01]).is_none());
        // The encoder rejects the same input before it hits the wire.
        assert!(Frame::encode_host_frame(SerialType::WifiMgmt, 0, false, &[0u8; 4097]).is_err());
    }

    #[test]
    fn parse_accepts_max_length_payload() {
        let frame =
            Frame::encode_host_frame(SerialType::WifiMgmt, 0, false, &[0u8; 4096]).expect("encodes");
        let parsed = Frame::parse(&frame).expect("max-length frame parses");
        assert_eq!(parsed.payload.len(), 4096);
    }

    #[test]
    fn crc32c_known_vector() {
        // Pins the exact CRC32C (Castagnoli) parameters: init 0xFFFFFFFF,
        // final XOR 0xFFFFFFFF. Must match Zephyr crc32_c() and the C
        // serial_frame_crc() on the device and host.
        assert_eq!(crc32c::crc32c(b"123456789"), 0xE3069283);
    }

    #[test]
    fn encode_host_frame_with_crc_round_trips() {
        let frame =
            Frame::encode_host_frame(SerialType::WifiMgmt, 5, true, &[1, 2, 3, 4]).expect("encodes");
        // 4 header + 4 payload + 4 CRC trailer.
        assert_eq!(frame.len(), 12);
        // The crc bit (byte1 bit 0) is set.
        assert_eq!(frame[1] & 0x01, 1);
        let parsed = Frame::parse(&frame).expect("crc frame parses");
        assert!(parsed.crc);
        assert_eq!(parsed.seq, 5);
        assert_eq!(parsed.payload, vec![1, 2, 3, 4]);
    }

    #[test]
    fn encode_host_frame_without_crc_is_v1_layout() {
        let frame =
            Frame::encode_host_frame(SerialType::WifiMgmt, 5, false, &[1, 2, 3, 4]).expect("encodes");
        // No trailer: exactly 4 header + 4 payload.
        assert_eq!(frame.len(), 8);
        // The crc bit is clear, so the layout is byte-identical to v1.
        assert_eq!(frame[1] & 0x01, 0);
        let parsed = Frame::parse(&frame).expect("non-crc frame parses");
        assert!(!parsed.crc);
        assert_eq!(parsed.payload, vec![1, 2, 3, 4]);
    }

    #[test]
    fn parse_rejects_corrupted_crc_trailer() {
        let mut frame =
            Frame::encode_host_frame(SerialType::WifiMgmt, 5, true, &[1, 2, 3, 4]).expect("encodes");
        assert!(Frame::parse(&frame).is_some(), "intact frame parses");
        // Corrupt a payload byte: the trailer no longer matches.
        frame[4] ^= 0xFF;
        assert!(Frame::parse(&frame).is_none(), "corrupted payload rejected");
        // Corrupt the trailer byte itself: rejected too.
        let mut frame2 =
            Frame::encode_host_frame(SerialType::WifiMgmt, 5, true, &[1, 2, 3, 4]).expect("encodes");
        frame2[11] ^= 0x01;
        assert!(Frame::parse(&frame2).is_none(), "corrupted trailer rejected");
    }

    #[test]
    fn parse_rejects_missing_crc_trailer() {
        let frame =
            Frame::encode_host_frame(SerialType::WifiMgmt, 5, true, &[1, 2, 3, 4]).expect("encodes");
        // The crc bit is set but the 4-byte trailer is absent.
        assert!(Frame::parse(&frame[..8]).is_none(), "short crc frame rejected");
    }

    #[test]
    fn session_survives_byte_at_a_time_reads() {
        let (mut session, device) = mock_session(0, vec![0x01]);
        // Deliver one byte per read to exercise partial-read reassembly.
        device.lock().unwrap().max_read = 1;
        let deadline = test_deadline();
        let is_response =
            |f: &Frame| f.traffic_type == SerialType::WifiMgmt && !f.payload.is_empty();
        session.sync(deadline).expect("sync");
        session
            .request(SerialType::WifiMgmt, &[0x01], deadline, &is_response)
            .expect("request with chunked reads");
    }

    #[test]
    fn read_frame_times_out_when_port_stays_silent() {
        let (mut session, _device) = mock_session(0, vec![0x01]);
        let err = session
            .read_frame(Instant::now() + std::time::Duration::from_millis(50))
            .expect_err("no data before the deadline");
        match err {
            SessionError::Timeout => {}
            other => panic!("unexpected error: {other:?}"),
        }
    }

    #[test]
    fn request_times_out_when_device_never_responds() {
        // The mock ACKs the command but has no response payload, so the
        // request loop only sees empty ACK frames until the deadline.
        let (mut session, _device) = mock_session(0, Vec::new());
        let deadline = Instant::now() + std::time::Duration::from_millis(50);
        let is_response =
            |f: &Frame| f.traffic_type == SerialType::WifiMgmt && !f.payload.is_empty();
        let err = session
            .request(SerialType::WifiMgmt, &[0x01], deadline, &is_response)
            .expect_err("no response before the deadline");
        match err {
            SessionError::Timeout => {}
            other => panic!("unexpected error: {other:?}"),
        }
    }
}
