//! Integration test: Wi-Fi access point lifecycle against real hardware.
//!
//! Skipped unless `INTEG_PORT` is set to the device serial port. AP
//! parameters are configurable at run time through the environment:
//!
//! - `INTEG_PORT`         serial port (required to run, e.g. `/dev/ttyACM2`)
//! - `INTEG_AP_SSID`      AP SSID (default `TestAp`)
//! - `INTEG_AP_SECURITY`  none | wep | psk | sae (default `psk`)
//! - `INTEG_AP_PSK`       pre-shared key (default `TestApPass1`)
//! - `INTEG_AP_CHANNEL`   channel number, 0 = driver's choice (default `6`)
//!
//! Flow: send AP_ENABLE, then (when the device accepts it) wait for the
//! `ApEnableResultEvent` (status 0) and verify the interface reports
//! `mode=AP`; send AP_DISABLE, wait for the `ApDisableResultEvent` (status 0)
//! and verify the interface is back in infrastructure mode.
//!
//! The nrf70 driver in the current NCS release does not implement AP mode,
//! so the device answers the enable request with `error_code=-ENOTSUP`. In
//! that case the test asserts the well-formed protocol response and passes,
//! reporting the driver limitation — the protocol round trip is still
//! verified.

use std::time::{Duration, Instant};

use cli_test::protocol::wifi_mgmt::{WifiMode, WifiSecurityType};
use cli_test::protocol::{
    Frame, SerialType, Session, ap_disable_request_payload, ap_enable_request_payload,
    decode_ap_result_notification, decode_iface_status_response, decode_wifi_mgmt_response,
    decode_wifi_status_response, iface_status_request_payload,
};

/// Timeout for a single request/response round trip.
const RESPONSE_TIMEOUT: Duration = Duration::from_secs(10);
/// Timeout for the AP result notification to arrive.
const AP_RESULT_TIMEOUT: Duration = Duration::from_secs(15);
const IFACE_INDEX: u32 = 0;

fn env_var(key: &str, default: &str) -> String {
    std::env::var(key).unwrap_or_else(|_| default.to_string())
}

fn parse_ap_security(value: &str) -> WifiSecurityType {
    match value.to_ascii_lowercase().as_str() {
        "none" => WifiSecurityType::None,
        "wep" => WifiSecurityType::Wep,
        "psk" => WifiSecurityType::Psk,
        "sae" => WifiSecurityType::Sae,
        other => panic!("INTEG_AP_SECURITY must be one of none|wep|psk|sae, got '{other}'"),
    }
}

fn is_wifi_mgmt_response(frame: &Frame) -> bool {
    decode_wifi_mgmt_response(frame).is_ok()
}

fn is_ap_result(is_enable: bool) -> impl Fn(&Frame) -> bool {
    move |frame: &Frame| decode_ap_result_notification(frame).is_some_and(|(en, _)| en == is_enable)
}

/// Sends an AP request and returns the decoded status response.
fn ap_request(
    session: &mut Session,
    payload: &[u8],
) -> Result<cli_test::protocol::wifi_mgmt::WifiStatusResponse, String> {
    let frame = session
        .request(
            SerialType::WifiMgmt,
            payload,
            Instant::now() + RESPONSE_TIMEOUT,
            &is_wifi_mgmt_response,
        )
        .map_err(|e| e.to_string())?;
    decode_wifi_status_response(&frame)
}

/// Queries the device for the Wi-Fi interface mode.
fn iface_mode(session: &mut Session) -> Result<WifiMode, String> {
    let payload = iface_status_request_payload(IFACE_INDEX);
    let frame = session
        .request(
            SerialType::WifiMgmt,
            &payload,
            Instant::now() + RESPONSE_TIMEOUT,
            &is_wifi_mgmt_response,
        )
        .map_err(|e| e.to_string())?;
    decode_iface_status_response(&frame)
        .map(|status| WifiMode::try_from(status.mode).unwrap_or(WifiMode::Infra))
}

/// Waits for the AP result notification and returns its status code.
fn wait_ap_result(session: &mut Session, is_enable: bool) -> Result<i32, String> {
    let frame = session
        .wait_for(Instant::now() + AP_RESULT_TIMEOUT, &is_ap_result(is_enable))
        .map_err(|e| e.to_string())?;
    match decode_ap_result_notification(&frame) {
        Some((en, status)) if en == is_enable => Ok(status),
        _ => Err("malformed AP result notification".to_string()),
    }
}

#[test]
fn ap_enable_disable_lifecycle() {
    let port = match std::env::var("INTEG_PORT") {
        Ok(port) => port,
        Err(_) => {
            eprintln!("INTEG_PORT not set; skipping AP integration test");
            return;
        }
    };

    let serial = match serialport::new(&port, 115200)
        .timeout(Duration::from_millis(100))
        .open()
    {
        Ok(port) => port,
        Err(e) => panic!("failed to open {port}: {e}"),
    };
    let mut session = Session::new(serial);
    match session.sync(Instant::now() + RESPONSE_TIMEOUT) {
        Ok(info) => eprintln!(
            "Synced: protocol version {}, supported types 0x{:04X}, next seq {}",
            info.version, info.supported_types_mask, info.next_seq
        ),
        Err(e) => panic!("protocol sync failed: {e}"),
    }

    let ssid = env_var("INTEG_AP_SSID", "TestAp");
    let security = parse_ap_security(&env_var("INTEG_AP_SECURITY", "psk"));
    let psk = env_var("INTEG_AP_PSK", "TestApPass1");
    let channel: u32 = env_var("INTEG_AP_CHANNEL", "6")
        .parse()
        .unwrap_or_else(|_| panic!("INTEG_AP_CHANNEL must be a number"));
    let psk_opt = match security {
        WifiSecurityType::Psk | WifiSecurityType::Sae => Some(psk.as_str()),
        _ => None,
    };

    // 1. AP enable.
    let payload = ap_enable_request_payload(
        IFACE_INDEX,
        &ssid,
        channel,
        1, // 2.4 GHz band
        security,
        psk_opt,
    );
    let status = ap_request(&mut session, &payload)
        .expect("AP enable request should return a well-formed status response");
    eprintln!(
        "AP enable: success={} error_code={} message={}",
        status.success, status.error_code, status.error_message
    );

    if !status.success {
        // The protocol round trip is verified; a -ENOTSUP rejection is the
        // documented behavior of drivers without AP support. Zephyr's libc
        // defines ENOTSUP as 134 (not the Linux value 98).
        assert_eq!(
            status.error_code, -134,
            "AP enable rejected with unexpected error_code {} (expected -ENOTSUP, Zephyr value -134)",
            status.error_code
        );
        eprintln!(
            "Device driver does not support AP mode (-ENOTSUP); \
             protocol round trip verified. Skipping AP lifecycle."
        );
        return;
    }

    // 2. AP enable result event must report success.
    let ap_status =
        wait_ap_result(&mut session, true).expect("AP enable result notification should arrive");
    assert_eq!(
        ap_status, 0,
        "AP enable result status should be 0, got {ap_status}"
    );

    // 3. The interface must report AP mode.
    assert_eq!(
        iface_mode(&mut session).expect("interface status query"),
        WifiMode::Ap,
        "interface should report AP mode after enable"
    );

    // 4. AP disable.
    let payload = ap_disable_request_payload(IFACE_INDEX);
    let status = ap_request(&mut session, &payload)
        .expect("AP disable request should return a well-formed status response");
    assert!(status.success, "AP disable request should be accepted");

    // 5. AP disable result event must report success.
    let ap_status =
        wait_ap_result(&mut session, false).expect("AP disable result notification should arrive");
    assert_eq!(
        ap_status, 0,
        "AP disable result status should be 0, got {ap_status}"
    );

    // 6. The interface must be back in infrastructure mode.
    assert_eq!(
        iface_mode(&mut session).expect("interface status query"),
        WifiMode::Infra,
        "interface should report infrastructure mode after disable"
    );

    eprintln!("AP lifecycle OK: enable -> result -> mode=AP -> disable -> result -> mode=infra");
}
