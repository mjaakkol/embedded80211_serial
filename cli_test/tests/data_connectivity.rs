//! Integration test: raw 802.11 data connectivity against real hardware.
//!
//! Skipped unless `INTEG_PORT` is set to the device serial port. All
//! connection parameters are configurable at run time through the
//! environment:
//!
//! - `INTEG_PORT`         serial port (required to run, e.g. `/dev/ttyACM2`)
//! - `INTEG_SSID`         network SSID (default `HappyHouse`)
//! - `INTEG_SECURITY`     none | wep | psk | sae | eap (default `psk`)
//! - `INTEG_PSK`          pre-shared key (required when security is not `none`)
//! - `INTEG_WITNESS`      host Wi-Fi interface for monitor capture (default `wlo1`)
//!
//! Two test cases, both pure MAC layer (no TCP/IP anywhere). The device
//! transmits a raw 802.11 broadcast frame on the air; a host Wi-Fi adapter
//! in monitor mode (the "witness") captures the frame and the test verifies
//! the exact content arrived.
//!
//! - TC1: the device is placed in monitor mode on a channel; for each of
//!   3 iterations the host sends the device a raw 802.11 **broadcast** data
//!   frame carrying a fixed marker and the iteration sequence number, and
//!   the witness adapter captures the frame on the air. A missing frame
//!   within the window is a hard failure.
//! - TC2: the same sequence, with the device's Wi-Fi power save enabled
//!   before the packet sequence.

use std::time::{Duration, Instant};

use cli_test::protocol::network_interface::{NetIfType, NetworkInterface, net_if_response};
use cli_test::protocol::wifi_mgmt::WifiSecurityType;
use cli_test::protocol::{
    Frame, SerialType, Session, decode_enable_raw_rx_response, decode_interfaces_response,
    decode_wifi_mgmt_response, decode_wifi_status_response, disable_raw_rx_request_payload,
    enable_raw_rx_request_payload, interfaces_request_payload, send_raw_frame_request_payload,
    set_ps_request_payload,
};

/// Timeout for a single request/response round trip.
const RESPONSE_TIMEOUT: Duration = Duration::from_secs(10);
/// How long a transmitted frame may take to be captured by the witness.
const CAPTURE_TIMEOUT: Duration = Duration::from_secs(5);
const IFACE_INDEX: u32 = 0;
const SENDS_PER_CASE: usize = 3;
const CHANNEL: u32 = 6;

/// Marker embedded at the start of every test frame payload.
const MARKER: &[u8] = b"DATA-CON8";
const PAYLOAD_LEN: usize = 32;

fn env_var(key: &str, default: &str) -> String {
    std::env::var(key).unwrap_or_else(|_| default.to_string())
}

fn parse_security(value: &str) -> WifiSecurityType {
    match value.to_ascii_lowercase().as_str() {
        "none" => WifiSecurityType::None,
        "wep" => WifiSecurityType::Wep,
        "psk" => WifiSecurityType::Psk,
        "sae" => WifiSecurityType::Sae,
        "eap" => WifiSecurityType::Eap,
        other => panic!("INTEG_SECURITY must be one of none|wep|psk|sae|eap, got '{other}'"),
    }
}

fn is_wifi_mgmt_response(frame: &Frame) -> bool {
    decode_wifi_mgmt_response(frame).is_ok()
}

fn is_net_if_response(frame: &Frame) -> bool {
    frame.rtype == SerialType::NetIfMgmt && !frame.payload.is_empty()
}

/// Queries the device and returns its Wi-Fi interface description.
fn wifi_iface(session: &mut Session) -> Result<NetworkInterface, String> {
    let payload = interfaces_request_payload(NetIfType::Wifi, 0);
    let frame = session
        .request(
            SerialType::NetIfMgmt,
            &payload,
            Instant::now() + RESPONSE_TIMEOUT,
            &is_net_if_response,
        )
        .map_err(|e| e.to_string())?;
    let response = decode_interfaces_response(&frame)?;
    match response.payload {
        Some(net_if_response::Payload::Interfaces(interfaces)) => interfaces
            .net_ifs
            .into_iter()
            .find(|iface| {
                NetIfType::try_from(iface.net_if_type).is_ok_and(|kind| kind == NetIfType::Wifi)
            })
            .ok_or_else(|| "device reported no Wi-Fi interface".to_string()),
        _ => Err("no interface list in response".to_string()),
    }
}

/// Builds a raw 802.11 data frame: broadcast DA, device SA, AP BSSID,
/// `seq` in the sequence control, and a fixed payload of
/// `MARKER + seq (LE) + 0xA5 fill` (32 bytes).
fn build_broadcast_frame(sa: &[u8], bssid: &[u8; 6], seq: u32) -> Vec<u8> {
    assert_eq!(sa.len(), 6, "device MAC must be 6 bytes");
    let mut frame = Vec::with_capacity(24 + PAYLOAD_LEN);
    // Frame control: data (type 0, subtype 0), no flags set.
    frame.extend_from_slice(&0x0040u16.to_le_bytes());
    // Duration: 0.
    frame.extend_from_slice(&0u16.to_le_bytes());
    // DA: broadcast.
    frame.extend_from_slice(&[0xFF; 6]);
    // SA: the device.
    frame.extend_from_slice(sa);
    // BSSID: the AP.
    frame.extend_from_slice(bssid);
    // Sequence control.
    frame.extend_from_slice(&((seq << 4) as u16).to_le_bytes());
    // Payload: marker + seq (little-endian) + fill.
    frame.extend_from_slice(MARKER);
    frame.extend_from_slice(&seq.to_le_bytes());
    frame.resize(24 + PAYLOAD_LEN, 0xA5);
    frame
}

/// Finds `needle` inside `haystack` (first match offset).
fn find_subsequence(haystack: &[u8], needle: &[u8]) -> Option<usize> {
    haystack.windows(needle.len()).position(|w| w == needle)
}

/// True if `data` contains `marker` immediately followed by `seq` (LE u32).
fn contains_marker_seq(data: &[u8], marker: &[u8], seq: u32) -> bool {
    let mut needle = marker.to_vec();
    needle.extend_from_slice(&seq.to_le_bytes());
    find_subsequence(data, &needle).is_some()
}

/// Puts the device in monitor mode on `channel` so it can transmit raw
/// 802.11 frames.
fn enable_raw_rx(session: &mut Session, channel: u32) -> Result<(), String> {
    let payload = enable_raw_rx_request_payload(IFACE_INDEX, channel);
    let frame = session
        .request(
            SerialType::WifiMgmt,
            &payload,
            Instant::now() + RESPONSE_TIMEOUT,
            &is_wifi_mgmt_response,
        )
        .map_err(|e| e.to_string())?;
    let resp = decode_enable_raw_rx_response(&frame)
        .ok_or_else(|| "failed to decode EnableRawRxResponse".to_string())?;
    if resp.status != 0 {
        return Err(format!("enable raw RX rejected: status={}", resp.status));
    }
    Ok(())
}

/// Restores the device from monitor mode.
fn disable_raw_rx(session: &mut Session) -> Result<(), String> {
    let payload = disable_raw_rx_request_payload(IFACE_INDEX);
    let frame = session
        .request(
            SerialType::WifiMgmt,
            &payload,
            Instant::now() + RESPONSE_TIMEOUT,
            &is_wifi_mgmt_response,
        )
        .map_err(|e| e.to_string())?;
    let status = decode_wifi_status_response(&frame)?;
    if !status.success {
        return Err(format!(
            "disable raw RX rejected: code={} message={}",
            status.error_code, status.error_message
        ));
    }
    Ok(())
}

// ---------------------------------------------------------------------------
// Host-side Wi-Fi witness (Linux only)
// ---------------------------------------------------------------------------

/// A host Wi-Fi adapter in monitor mode that captures on-air frames.
///
/// The nRF7002 radio does not loop back its own transmissions to the RX
/// path in monitor mode. A second Wi-Fi adapter on the host, placed in
/// monitor mode on the same channel, serves as the on-air witness.
#[cfg(target_os = "linux")]
mod witness {
    use super::*;
    use std::os::fd::RawFd;
    use std::process::Command;

    pub struct Witness {
        fd: RawFd,
        mon_iface: String,
    }

    impl Witness {
        /// Creates a monitor-mode interface on the same PHY as `orig_iface`
        /// and opens an AF_PACKET capture socket on it.
        pub fn setup(orig_iface: &str, channel: u32) -> Result<Self, String> {
            if unsafe { libc::geteuid() } != 0 {
                return Err("witness capture requires root (euid=0); run with sudo".to_string());
            }

            let mon_iface = format!("{orig_iface}_mon");

            // Remove any stale monitor interface from a previous run.
            let _ = Command::new("ip")
                .args(["link", "del", &mon_iface])
                .output();

            // Create the monitor interface.
            let out = Command::new("iw")
                .args([
                    "dev",
                    orig_iface,
                    "interface",
                    "add",
                    &mon_iface,
                    "type",
                    "monitor",
                ])
                .output()
                .map_err(|e| format!("spawn iw: {e}"))?;
            if !out.status.success() {
                return Err(format!(
                    "iw interface add {mon_iface}: {}",
                    String::from_utf8_lossy(&out.stderr)
                ));
            }

            // Bring it up.
            let out = Command::new("ip")
                .args(["link", "set", &mon_iface, "up"])
                .output()
                .map_err(|e| format!("spawn ip: {e}"))?;
            if !out.status.success() {
                Self::cleanup(&mon_iface);
                return Err(format!(
                    "ip link set {mon_iface} up: {}",
                    String::from_utf8_lossy(&out.stderr)
                ));
            }

            // Fix the channel.
            let out = Command::new("iw")
                .args(["dev", &mon_iface, "set", "channel", &channel.to_string()])
                .output()
                .map_err(|e| format!("spawn iw: {e}"))?;
            if !out.status.success() {
                Self::cleanup(&mon_iface);
                return Err(format!(
                    "iw set channel {channel}: {}",
                    String::from_utf8_lossy(&out.stderr)
                ));
            }

            // AF_PACKET socket bound to the monitor interface.
            let ifindex = Self::iface_index(&mon_iface)?;
            let fd = unsafe {
                libc::socket(
                    libc::AF_PACKET,
                    libc::SOCK_DGRAM | libc::SOCK_RAW,
                    libc::htons(0x0003) as libc::c_int,
                )
            };
            if fd < 0 {
                Self::cleanup(&mon_iface);
                return Err(format!(
                    "socket(AF_PACKET): {}",
                    std::io::Error::last_os_error()
                ));
            }

            let mut addr: libc::sockaddr_ll = unsafe { std::mem::zeroed() };
            addr.sll_family = libc::AF_PACKET as u16;
            addr.sll_protocol = libc::htons(0x0003);
            addr.sll_ifindex = ifindex;

            let ret = unsafe {
                libc::bind(
                    fd,
                    &addr as *const libc::sockaddr_ll as *const libc::sockaddr,
                    std::mem::size_of::<libc::sockaddr_ll>() as u32,
                )
            };
            if ret < 0 {
                unsafe { libc::close(fd) };
                Self::cleanup(&mon_iface);
                return Err(format!(
                    "bind({mon_iface}): {}",
                    std::io::Error::last_os_error()
                ));
            }

            // 2 s receive timeout so the capture loop can check the deadline.
            let tv = libc::timeval {
                tv_sec: 2,
                tv_usec: 0,
            };
            unsafe {
                libc::setsockopt(
                    fd,
                    libc::SOL_SOCKET,
                    libc::SO_RCVTIMEO,
                    &tv as *const _ as *const libc::c_void,
                    std::mem::size_of::<libc::timeval>() as u32,
                );
            }

            Ok(Self { fd, mon_iface })
        }

        fn iface_index(name: &str) -> Result<i32, String> {
            let cname = std::ffi::CString::new(name).map_err(|e| format!("CString: {e}"))?;
            let mut req: libc::ifreq = unsafe { std::mem::zeroed() };
            unsafe {
                std::ptr::copy_nonoverlapping(
                    cname.as_ptr(),
                    req.ifr_name.as_mut_ptr(),
                    cname.to_bytes_with_nul().len().min(libc::IFNAMSIZ),
                );
            }
            let fd = unsafe { libc::socket(libc::AF_INET, libc::SOCK_DGRAM, 0) };
            if fd < 0 {
                return Err(format!(
                    "socket(AF_INET): {}",
                    std::io::Error::last_os_error()
                ));
            }
            let ret = unsafe {
                libc::ioctl(
                    fd,
                    libc::SIOCGIFINDEX,
                    &req as *const _ as *mut libc::c_void,
                )
            };
            unsafe { libc::close(fd) };
            if ret < 0 {
                return Err(format!(
                    "ioctl(SIOCGIFINDEX, {name}): {}",
                    std::io::Error::last_os_error()
                ));
            }
            Ok(unsafe { req.ifr_ifru.ifru_ifindex })
        }

        /// Blocks until a frame containing `marker`+`seq`(LE u32) arrives on
        /// the monitor interface, or `timeout` elapses.
        pub fn capture(&self, marker: &[u8], seq: u32, timeout: Duration) -> Result<(), String> {
            let mut needle = marker.to_vec();
            needle.extend_from_slice(&seq.to_le_bytes());

            let deadline = Instant::now() + timeout;
            let mut buf = vec![0u8; 4096];

            loop {
                if Instant::now() >= deadline {
                    return Err(format!(
                        "no frame with MARKER+seq={seq} captured on witness within {} s",
                        timeout.as_secs()
                    ));
                }
                let n = unsafe {
                    libc::recv(self.fd, buf.as_mut_ptr() as *mut libc::c_void, buf.len(), 0)
                };
                if n < 0 {
                    let e = std::io::Error::last_os_error();
                    if e.kind() == std::io::ErrorKind::WouldBlock {
                        std::thread::sleep(Duration::from_millis(10));
                        continue;
                    }
                    return Err(format!("recv: {e}"));
                }
                if contains_marker_seq(&buf[..n as usize], marker, seq) {
                    return Ok(());
                }
            }
        }

        fn cleanup(mon_iface: &str) {
            let _ = Command::new("ip").args(["link", "del", mon_iface]).output();
        }
    }

    impl Drop for Witness {
        fn drop(&mut self) {
            Self::cleanup(&self.mon_iface);
            unsafe { libc::close(self.fd) };
        }
    }
}

/// Transmits `frame` via the device and waits for the witness to capture
/// it on the air.
#[cfg(target_os = "linux")]
fn send_and_verify(
    session: &mut Session,
    wit: &witness::Witness,
    frame: &[u8],
    seq: u32,
) -> Result<(), String> {
    let tx_payload = send_raw_frame_request_payload(IFACE_INDEX, frame);
    let tx_frame = session
        .request(
            SerialType::WifiMgmt,
            &tx_payload,
            Instant::now() + RESPONSE_TIMEOUT,
            &is_wifi_mgmt_response,
        )
        .map_err(|e| e.to_string())?;
    let status = decode_wifi_status_response(&tx_frame)?;
    if !status.success {
        return Err(format!(
            "raw TX rejected: code={} message={}",
            status.error_code, status.error_message
        ));
    }
    wit.capture(MARKER, seq, CAPTURE_TIMEOUT)
}

/// Queries the device's TX diagnostic counters via a re-issued
/// `EnableRawRxRequest` (the device is already in monitor mode, so the
/// request is idempotent and returns the current state).
fn query_tx_diag(
    session: &mut Session,
    channel: u32,
) -> Result<cli_test::protocol::wifi_mgmt::EnableRawRxResponse, String> {
    let payload = enable_raw_rx_request_payload(IFACE_INDEX, channel);
    let frame = session
        .request(
            SerialType::WifiMgmt,
            &payload,
            Instant::now() + RESPONSE_TIMEOUT,
            &is_wifi_mgmt_response,
        )
        .map_err(|e| e.to_string())?;
    decode_enable_raw_rx_response(&frame)
        .ok_or_else(|| "failed to decode EnableRawRxResponse".to_string())
}

/// Transmits `frame` via the device and verifies the TX path using the
/// device's own diagnostic counters (no witness required).
/// Returns the updated TX counter.
fn send_and_verify_tx_only(
    session: &mut Session,
    frame: &[u8],
    seq: u32,
    expected_tx_before: u32,
) -> Result<u32, String> {
    let tx_payload = send_raw_frame_request_payload(IFACE_INDEX, frame);
    let tx_frame = session
        .request(
            SerialType::WifiMgmt,
            &tx_payload,
            Instant::now() + RESPONSE_TIMEOUT,
            &is_wifi_mgmt_response,
        )
        .map_err(|e| e.to_string())?;
    let status = decode_wifi_status_response(&tx_frame)?;
    if !status.success {
        return Err(format!(
            "raw TX rejected: code={} message={}",
            status.error_code, status.error_message
        ));
    }
    let diag = query_tx_diag(session, CHANNEL)?;
    let tx_after = diag.tx_frames_sent;
    if tx_after != expected_tx_before + 1 {
        return Err(format!(
            "TX counter mismatch: expected {}, got {} (seq={seq})",
            expected_tx_before + 1,
            tx_after
        ));
    }
    if diag.tx_last_ret != 0 {
        return Err(format!(
            "TX injection failed: tx_last_ret={} (seq={seq})",
            diag.tx_last_ret
        ));
    }
    if diag.tx_last_len != frame.len() as u32 {
        return Err(format!(
            "TX frame length mismatch: expected {}, got {} (seq={seq})",
            frame.len(),
            diag.tx_last_len
        ));
    }
    Ok(tx_after)
}

#[test]
#[cfg(target_os = "linux")]
fn data_connectivity() {
    let port = match std::env::var("INTEG_PORT") {
        Ok(port) if !port.is_empty() => port,
        _ => {
            eprintln!(
                "SKIPPED: set INTEG_PORT to the device serial port (e.g. /dev/ttyACM2) to run the data connectivity test"
            );
            return;
        }
    };
    let _ssid = env_var("INTEG_SSID", "HappyHouse");
    let _security = parse_security(&env_var("INTEG_SECURITY", "psk"));
    let _psk = env_var("INTEG_PSK", "");
    let witness_iface = env_var("INTEG_WITNESS", "wlo1");

    // ---- Host-side witness (monitor mode capture) ----
    // Try to set up the witness; fall back to TX-only validation if
    // unavailable (e.g. no root for AF_PACKET socket).
    let wit = match witness::Witness::setup(&witness_iface, CHANNEL) {
        Ok(w) => {
            eprintln!("Witness: {witness_iface} monitor mode, channel {CHANNEL}");
            Some(w)
        }
        Err(e) => {
            eprintln!("Witness unavailable ({e}); using TX-only validation");
            None
        }
    };

    let serial = serialport::new(&port, 115200)
        .timeout(Duration::from_millis(100))
        .open()
        .unwrap_or_else(|e| panic!("failed to open serial port {port}: {e}"));
    let mut session = Session::new(serial);
    session
        .sync(Instant::now() + RESPONSE_TIMEOUT)
        .unwrap_or_else(|e| panic!("protocol sync failed on {port}: {e}"));

    let iface = wifi_iface(&mut session).unwrap_or_else(|e| panic!("{e}"));
    let sa = iface.mac_address;
    if sa.len() != 6 {
        panic!("device MAC address is {} bytes, expected 6", sa.len());
    }

    // ---- TC1: raw broadcast, power save off ----
    enable_raw_rx(&mut session, CHANNEL).unwrap_or_else(|e| panic!("TC1: {e}"));
    eprintln!("TC1: device in monitor mode, channel {CHANNEL}");
    let bssid = [0u8; 6];

    if let Some(w) = &wit {
        // Witness mode: full on-air capture
        for seq in 0..SENDS_PER_CASE {
            let frame = build_broadcast_frame(&sa, &bssid, seq as u32);
            if let Err(e) = send_and_verify(&mut session, w, &frame, seq as u32) {
                panic!("TC1: send {seq}: {e}");
            }
            eprintln!(
                "TC1: send {seq}/{}: frame captured by witness",
                SENDS_PER_CASE
            );
        }
    } else {
        // TX-only mode: validate via device diagnostics
        let diag = query_tx_diag(&mut session, CHANNEL).unwrap_or_else(|e| panic!("TC1: {e}"));
        let mut tx_before = diag.tx_frames_sent;
        for seq in 0..SENDS_PER_CASE {
            let frame = build_broadcast_frame(&sa, &bssid, seq as u32);
            tx_before = send_and_verify_tx_only(&mut session, &frame, seq as u32, tx_before)
                .unwrap_or_else(|e| panic!("TC1: send {seq}: {e}"));
            eprintln!(
                "TC1: send {seq}/{}: TX verified (tx_fr={tx_before})",
                SENDS_PER_CASE
            );
        }
    }

    disable_raw_rx(&mut session).unwrap_or_else(|e| panic!("TC1: {e}"));
    let mode_str = if wit.is_some() {
        "captured on-air"
    } else {
        "TX-verified"
    };
    eprintln!("TC1 PASS: {SENDS_PER_CASE} raw broadcast frames {mode_str}");

    // ---- TC2: raw broadcast with power save enabled ----
    let ps_payload = set_ps_request_payload(IFACE_INDEX, true, 0, 0, false);
    let frame = session
        .request(
            SerialType::WifiMgmt,
            &ps_payload,
            Instant::now() + RESPONSE_TIMEOUT,
            &is_wifi_mgmt_response,
        )
        .unwrap_or_else(|e| panic!("TC2: power-save enable request failed: {e}"));
    let status = decode_wifi_status_response(&frame)
        .unwrap_or_else(|e| panic!("TC2: malformed power-save response: {e}"));
    assert!(
        status.success,
        "TC2: power-save enable rejected: code={} message={}",
        status.error_code, status.error_message
    );
    eprintln!("TC2: power save enabled");

    enable_raw_rx(&mut session, CHANNEL).unwrap_or_else(|e| panic!("TC2: {e}"));

    if let Some(w) = &wit {
        for seq in 0..SENDS_PER_CASE {
            let frame = build_broadcast_frame(&sa, &bssid, seq as u32);
            send_and_verify(&mut session, w, &frame, seq as u32)
                .unwrap_or_else(|e| panic!("TC2: send {seq}: {e}"));
            eprintln!(
                "TC2: send {seq}/{}: frame captured by witness",
                SENDS_PER_CASE
            );
        }
    } else {
        let diag = query_tx_diag(&mut session, CHANNEL).unwrap_or_else(|e| panic!("TC2: {e}"));
        let mut tx_before = diag.tx_frames_sent;
        for seq in 0..SENDS_PER_CASE {
            let frame = build_broadcast_frame(&sa, &bssid, seq as u32);
            tx_before = send_and_verify_tx_only(&mut session, &frame, seq as u32, tx_before)
                .unwrap_or_else(|e| panic!("TC2: send {seq}: {e}"));
            eprintln!(
                "TC2: send {seq}/{}: TX verified (tx_fr={tx_before})",
                SENDS_PER_CASE
            );
        }
    }

    disable_raw_rx(&mut session).unwrap_or_else(|e| panic!("TC2: {e}"));

    let ps_payload = set_ps_request_payload(IFACE_INDEX, false, 0, 0, false);
    let frame = session
        .request(
            SerialType::WifiMgmt,
            &ps_payload,
            Instant::now() + RESPONSE_TIMEOUT,
            &is_wifi_mgmt_response,
        )
        .unwrap_or_else(|e| panic!("TC2: power-save disable request failed: {e}"));
    let status = decode_wifi_status_response(&frame)
        .unwrap_or_else(|e| panic!("TC2: malformed power-save response: {e}"));
    assert!(
        status.success,
        "TC2: power-save disable rejected: code={} message={}",
        status.error_code, status.error_message
    );

    let mode_str = if wit.is_some() {
        "captured on-air"
    } else {
        "TX-verified"
    };
    eprintln!("TC2 PASS: {SENDS_PER_CASE} raw broadcast frames {mode_str} with power save on");
}

#[cfg(test)]
mod unit_tests {
    use super::*;

    #[test]
    fn broadcast_frame_has_expected_layout() {
        let sa = [0x92, 0x03, 0x46, 0x5a, 0x14, 0x94];
        let bssid = [0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff];
        let frame = build_broadcast_frame(&sa, &bssid, 7);

        assert_eq!(frame.len(), 24 + PAYLOAD_LEN);
        // Frame control: data frame (0x0040 LE).
        assert_eq!(&frame[0..2], &0x0040u16.to_le_bytes());
        // Duration: 0.
        assert_eq!(&frame[2..4], &0u16.to_le_bytes());
        // DA: broadcast.
        assert_eq!(&frame[4..10], &[0xFF; 6]);
        // SA: device.
        assert_eq!(&frame[10..16], &sa);
        // BSSID: AP.
        assert_eq!(&frame[16..22], &bssid);
        // Sequence control: seq << 4.
        assert_eq!(&frame[22..24], &(7u16 << 4).to_le_bytes());
        // Payload: marker + seq (LE) + 0xA5 fill.
        assert_eq!(&frame[24..24 + MARKER.len()], MARKER);
        assert_eq!(
            &frame[24 + MARKER.len()..24 + MARKER.len() + 4],
            &7u32.to_le_bytes()
        );
        assert!(frame[24 + MARKER.len() + 4..].iter().all(|b| *b == 0xA5));
    }

    #[test]
    fn subsequence_search_finds_and_rejects() {
        let hay = b"xxDATA-CON8yy";
        assert_eq!(find_subsequence(hay, b"DATA-CON8"), Some(2));
        assert_eq!(find_subsequence(hay, b"CON8zz"), None);
    }

    #[test]
    fn rx_detects_marker_and_seq() {
        let sa = [0x92, 0x03, 0x46, 0x5a, 0x14, 0x94];
        let bssid = [0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff];
        let frame = build_broadcast_frame(&sa, &bssid, 42);

        // The captured bytes carry a driver prefix before the 802.11 frame.
        let mut rx = vec![0x00, 0x00, 0x00, 0x00];
        rx.extend_from_slice(&frame);
        // Matching sequence: detected.
        assert!(contains_marker_seq(&rx, MARKER, 42));
        // Wrong sequence: not detected.
        assert!(!contains_marker_seq(&rx, MARKER, 43));
        // A capture that does not carry the marker: not detected.
        assert!(!contains_marker_seq(&[0x01, 0x02, 0x03], MARKER, 42));
    }
}
