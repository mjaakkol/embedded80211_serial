//! Integration test: repeated connect/disconnect cycles against real hardware.
//!
//! Skipped unless `INTEG_PORT` is set to the device serial port. All
//! connection parameters are configurable at run time through the
//! environment:
//!
//! - `INTEG_PORT`      serial port (required to run, e.g. `/dev/ttyACM2`)
//! - `INTEG_SSID`      network SSID (default `HappyHouse`)
//! - `INTEG_SECURITY`  none | wep | psk | sae | eap (default `psk`)
//! - `INTEG_PSK`       pre-shared key (required when security is not `none`)
//! - `INTEG_CYCLES`    number of connect/disconnect cycles (default `5`)
//!
//! Each cycle: send CONNECT, wait until the Wi-Fi interface reports carrier,
//! send DISCONNECT, wait until the carrier drops. Every failure is a hard
//! test failure with the cycle number in the message.

use std::time::{Duration, Instant};

use cli_test::protocol::network_interface::{NetIfType, NetworkInterface, net_if_response};
use cli_test::protocol::wifi_mgmt::WifiSecurityType;
use cli_test::protocol::{
    Frame, SerialType, Session, connect_request_payload, decode_interfaces_response,
    decode_wifi_mgmt_response, decode_wifi_status_response, disconnect_request_payload,
    interfaces_request_payload,
};

/// Timeout for a single request/response round trip.
const RESPONSE_TIMEOUT: Duration = Duration::from_secs(10);
/// Timeout for the interface to reach the expected carrier state.
const CARRIER_TIMEOUT: Duration = Duration::from_secs(30);
const CARRIER_POLL_INTERVAL: Duration = Duration::from_millis(1000);
const CONNECT_TIMEOUT_MS: u32 = 10000;
const IFACE_INDEX: u32 = 0;

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

/// Polls the Wi-Fi interface until it reaches the expected link state.
///
/// Connected = carrier present and interface not dormant. Disconnected =
/// interface dormant: the nrf Wi-Fi driver marks the net_if dormant on
/// deauthentication but does not clear the carrier flag, so `dormant` is the
/// reliable disconnect indicator.
fn wait_for_link_state(session: &mut Session, want_connected: bool) -> Result<(), String> {
    let deadline = Instant::now() + CARRIER_TIMEOUT;
    let mut last = String::new();
    loop {
        if let Ok(iface) = wifi_iface(session) {
            last = format!(
                "carrier_ok={} dormant={} oper_state={}",
                iface.carrier_ok, iface.dormant, iface.oper_state
            );
            if (iface.carrier_ok && !iface.dormant) == want_connected {
                return Ok(());
            }
        }
        if Instant::now() >= deadline {
            return Err(format!(
                "Wi-Fi did not reach the expected state (want_connected={want_connected}) within {} s (last: {last})",
                CARRIER_TIMEOUT.as_secs()
            ));
        }
        std::thread::sleep(CARRIER_POLL_INTERVAL);
    }
}

#[test]
fn connect_disconnect_cycles() {
    let port = match std::env::var("INTEG_PORT") {
        Ok(port) if !port.is_empty() => port,
        _ => {
            eprintln!(
                "SKIPPED: set INTEG_PORT to the device serial port (e.g. /dev/ttyACM2) to run the connect/disconnect integration test"
            );
            return;
        }
    };
    let ssid = env_var("INTEG_SSID", "HappyHouse");
    let security = parse_security(&env_var("INTEG_SECURITY", "psk"));
    let psk = env_var("INTEG_PSK", "");
    let cycles: usize = env_var("INTEG_CYCLES", "5").parse().unwrap_or(5);

    if security != WifiSecurityType::None && psk.is_empty() {
        panic!("INTEG_PSK must be set when INTEG_SECURITY is not 'none'");
    }

    let serial = serialport::new(&port, 115200)
        .timeout(Duration::from_millis(100))
        .open()
        .unwrap_or_else(|e| panic!("failed to open serial port {port}: {e}"));
    let mut session = Session::new(serial);
    session
        .sync(Instant::now() + RESPONSE_TIMEOUT)
        .unwrap_or_else(|e| panic!("protocol sync failed on {port}: {e}"));

    for cycle in 1..=cycles {
        // --- connect ---
        let payload =
            connect_request_payload(IFACE_INDEX, &ssid, Some(&psk), security, CONNECT_TIMEOUT_MS);
        let frame = session
            .request(
                SerialType::WifiMgmt,
                &payload,
                Instant::now() + RESPONSE_TIMEOUT,
                &is_wifi_mgmt_response,
            )
            .unwrap_or_else(|e| panic!("cycle {cycle}/{cycles}: connect request failed: {e}"));
        let status = decode_wifi_status_response(&frame)
            .unwrap_or_else(|e| panic!("cycle {cycle}/{cycles}: malformed connect response: {e}"));
        assert!(
            status.success,
            "cycle {cycle}/{cycles}: connect to '{ssid}' rejected: code={} message={}",
            status.error_code, status.error_message
        );

        wait_for_link_state(&mut session, true)
            .unwrap_or_else(|e| panic!("cycle {cycle}/{cycles}: {e}"));
        eprintln!("cycle {cycle}/{cycles}: associated with '{ssid}'");

        // --- disconnect ---
        let payload = disconnect_request_payload(IFACE_INDEX);
        let frame = session
            .request(
                SerialType::WifiMgmt,
                &payload,
                Instant::now() + RESPONSE_TIMEOUT,
                &is_wifi_mgmt_response,
            )
            .unwrap_or_else(|e| panic!("cycle {cycle}/{cycles}: disconnect request failed: {e}"));
        let status = decode_wifi_status_response(&frame).unwrap_or_else(|e| {
            panic!("cycle {cycle}/{cycles}: malformed disconnect response: {e}")
        });
        assert!(
            status.success,
            "cycle {cycle}/{cycles}: disconnect rejected: code={} message={}",
            status.error_code, status.error_message
        );

        wait_for_link_state(&mut session, false)
            .unwrap_or_else(|e| panic!("cycle {cycle}/{cycles}: {e}"));
        eprintln!("cycle {cycle}/{cycles}: disconnected");
    }

    eprintln!("PASS: {cycles} connect/disconnect cycles against SSID '{ssid}'");
}
