use std::time::{Duration, Instant};

use clap::{Parser, Subcommand, ValueEnum};

use cli_test::protocol::network_interface::NetIfType;
use cli_test::protocol::wifi_mgmt::WifiSecurityType;
use cli_test::protocol::{
    Frame, HCI_OP_READ_BD_ADDR, HCI_OP_READ_LOCAL_SUPPORTED_COMMANDS,
    HCI_OP_READ_LOCAL_SUPPORTED_FEATURES, HCI_OP_READ_LOCAL_VERSION, HCI_OP_RESET,
    HciCommandComplete, SerialType, Session, ap_disable_request_payload, ap_enable_request_payload,
    connect_request_payload, decode_ap_result_notification, decode_scan_done_notification,
    decode_wifi_mgmt_response, decode_wifi_status_response, disconnect_request_payload,
    format_hci_command_complete, format_iface_status_response, format_interfaces_response,
    format_ps_response, format_reg_domain_response, format_scan_results, format_status_response,
    format_version_response, get_ps_request_payload, get_reg_domain_request_payload,
    hci_command_complete, hci_command_packet, iface_status_request_payload,
    interfaces_request_payload, scan_request_payload, set_ps_request_payload,
    set_reg_domain_request_payload, version_request_payload,
};

/// How long to wait for a normal request/response round trip.
/* 15 s: the Wi-Fi driver blocks up to ~10 s on a firmware-side timeout for
 * rejected management operations (e.g. unknown regulatory domain) before
 * returning, so the proxy's honest response arrives after that delay. */
const RESPONSE_TIMEOUT: Duration = Duration::from_secs(15);
/// How long to wait for scan results to complete.
const SCAN_TIMEOUT: Duration = Duration::from_secs(30);

#[derive(Parser)]
#[command(name = "cli-serial")]
#[command(version = "1.0")]
#[command(about = "cli test application for cfg80211_serial", long_about = None)]
struct Cli {
    #[arg(short, long, action = clap::ArgAction::Count)]
    debug: u8,

    #[arg(short, long)]
    port: String,

    #[arg(short, long, default_value_t = 115200)]
    baud_rate: u32,

    #[command(subcommand)]
    command: Commands,
}

#[derive(Subcommand)]
enum Commands {
    /// does testing things
    Test {
        /// lists test values
        #[arg(short, long)]
        list: bool,
    },
    List,
    Version,
    Scan {
        #[arg(short, long, default_value_t = 0)]
        iface_index: u32,
    },
    Connect {
        #[arg(short = 'i', long, default_value_t = 0)]
        iface_index: u32,

        #[arg(long)]
        ssid: String,

        #[arg(long)]
        psk: Option<String>,

        #[arg(short = 's', long, value_enum, default_value_t = ConnectSecurityArg::Psk)]
        security: ConnectSecurityArg,

        #[arg(long, default_value_t = 10000)]
        timeout_ms: u32,
    },
    Disconnect {
        #[arg(short = 'i', long, default_value_t = 0)]
        iface_index: u32,
    },
    Interfaces {
        #[arg(short = 'i', long, default_value_t = 0)]
        iface_index: u32,

        #[arg(short = 't', long, value_enum, default_value_t = InterfaceTypeArg::All)]
        iface_type: InterfaceTypeArg,
    },
    /// Regulatory domain management (NET_REQUEST_WIFI_REG_DOMAIN)
    RegDomain {
        #[command(subcommand)]
        command: RegDomainCmd,
    },
    /// Wi-Fi power save management (NET_REQUEST_WIFI_PS / WIFI_PS_CONFIG)
    PowerSave {
        #[command(subcommand)]
        command: PowerSaveCmd,
    },
    /// Get the Wi-Fi interface status (state, SSID, BSSID, RSSI, ...)
    IfaceStatus {
        #[arg(short = 'i', long, default_value_t = 0)]
        iface_index: u32,
    },
    /// Wi-Fi access point management (NET_REQUEST_WIFI_AP_ENABLE/DISABLE)
    Ap {
        #[command(subcommand)]
        command: ApCmd,
    },
    /// Bluetooth HCI test commands (raw HCI over the common link protocol)
    Hci {
        #[command(subcommand)]
        command: HciCmd,
    },
    /// Wi-Fi scan interleaved with a Bluetooth HCI command, to demonstrate
    /// multiprotocol coexistence over the shared serial link.
    Coexist {
        #[arg(short, long, default_value_t = 0)]
        iface_index: u32,
    },
}

#[derive(Subcommand)]
enum RegDomainCmd {
    /// Set the regulatory domain (country code)
    Set {
        /// Wi-Fi interface index
        #[arg(short = 'i', long, default_value_t = 0)]
        iface_index: u32,
        /// 2-letter ISO 3166-1 alpha-2 country code (e.g. US, GB)
        #[arg(short = 'c', long)]
        country: String,
        /// Force update even if the domain is already active
        #[arg(long)]
        force: bool,
    },
    /// Get the current regulatory domain
    Get {
        /// Wi-Fi interface index
        #[arg(short = 'i', long, default_value_t = 0)]
        iface_index: u32,
    },
}

#[derive(Subcommand)]
enum PowerSaveCmd {
    /// Set the power save configuration
    Set {
        /// Wi-Fi interface index
        #[arg(short = 'i', long, default_value_t = 0)]
        iface_index: u32,
        /// Enable power save (default: disable)
        #[arg(long)]
        enable: bool,
        /// Listen interval in beacon periods
        #[arg(long, default_value_t = 10)]
        listen_interval: u32,
        /// Post-TX timeout in ms (0 = driver default)
        #[arg(long, default_value_t = 0)]
        timeout_ms: u32,
        /// Use WMM (U-APSD) power save instead of legacy (PSPOLL)
        #[arg(long)]
        wmm: bool,
    },
    /// Get the current power save configuration
    Get {
        /// Wi-Fi interface index
        #[arg(short = 'i', long, default_value_t = 0)]
        iface_index: u32,
    },
}

#[derive(Subcommand)]
enum HciCmd {
    /// Reset the Bluetooth controller
    Reset,
    /// Read local version information
    Version,
    /// Read local supported commands (8-byte bitmap)
    Commands,
    /// Read local supported features (8-byte bitmap)
    Features,
    /// Read the controller Bluetooth device address
    Addr,
    /// Convenience: reset, then read local version information
    Info,
    /// Send a raw HCI command and print the Command Complete event
    Raw {
        /// 16-bit opcode in hex (e.g. 0103)
        opcode: String,
        /// Command parameters in hex (e.g. 00)
        #[arg(short = 'p', long)]
        params: Option<String>,
    },
}

#[derive(Subcommand)]
enum ApCmd {
    /// Start the Wi-Fi access point (AP mode)
    Enable {
        /// Wi-Fi interface index
        #[arg(short = 'i', long, default_value_t = 0)]
        iface_index: u32,
        /// SSID of the access point (1..=32 characters)
        #[arg(long)]
        ssid: String,
        /// Channel number (0 = driver's choice)
        #[arg(short = 'c', long, default_value_t = 0)]
        channel: u32,
        /// Frequency band
        #[arg(short = 'b', long, value_enum, default_value_t = ApBandArg::Any)]
        band: ApBandArg,
        /// Security mode
        #[arg(short = 's', long, value_enum, default_value_t = ApSecurityArg::None)]
        security: ApSecurityArg,
        /// Pre-shared key (required for PSK/SAE)
        #[arg(long)]
        psk: Option<String>,
    },
    /// Stop the Wi-Fi access point (tear down AP mode)
    Disable {
        /// Wi-Fi interface index
        #[arg(short = 'i', long, default_value_t = 0)]
        iface_index: u32,
    },
}

#[derive(Clone, Copy, Debug, ValueEnum)]
enum ApBandArg {
    /// Any band supported by the driver
    #[value(name = "any")]
    Any,
    /// 2.4 GHz band
    #[value(name = "2g4")]
    Band2g4,
    /// 5 GHz band
    #[value(name = "5g")]
    Band5g,
    /// 6 GHz band
    #[value(name = "6g")]
    Band6g,
}

impl ApBandArg {
    fn to_proto(self) -> i32 {
        match self {
            ApBandArg::Any => 0,
            ApBandArg::Band2g4 => 1,
            ApBandArg::Band5g => 2,
            ApBandArg::Band6g => 3,
        }
    }
}

#[derive(Clone, Copy, Debug, ValueEnum)]
enum ApSecurityArg {
    /// Open (no authentication)
    None,
    /// WEP
    Wep,
    /// WPA/WPA2 PSK
    Psk,
    /// WPA3 SAE
    Sae,
}

impl ApSecurityArg {
    fn to_proto(self) -> WifiSecurityType {
        match self {
            ApSecurityArg::None => WifiSecurityType::None,
            ApSecurityArg::Wep => WifiSecurityType::Wep,
            ApSecurityArg::Psk => WifiSecurityType::Psk,
            ApSecurityArg::Sae => WifiSecurityType::Sae,
        }
    }
}

#[derive(Clone, Copy, Debug, ValueEnum)]
enum InterfaceTypeArg {
    All,
    Wifi,
    Openthread,
}

#[derive(Clone, Copy, Debug, ValueEnum)]
enum ConnectSecurityArg {
    None,
    Wep,
    Psk,
    Sae,
    Eap,
}

impl ConnectSecurityArg {
    fn to_proto(self) -> WifiSecurityType {
        match self {
            ConnectSecurityArg::None => WifiSecurityType::None,
            ConnectSecurityArg::Wep => WifiSecurityType::Wep,
            ConnectSecurityArg::Psk => WifiSecurityType::Psk,
            ConnectSecurityArg::Sae => WifiSecurityType::Sae,
            ConnectSecurityArg::Eap => WifiSecurityType::Eap,
        }
    }
}

impl InterfaceTypeArg {
    fn to_proto(self) -> NetIfType {
        match self {
            InterfaceTypeArg::All => NetIfType::Unknown,
            InterfaceTypeArg::Wifi => NetIfType::Wifi,
            InterfaceTypeArg::Openthread => NetIfType::Openthread,
        }
    }
}

fn init_serial_port(
    port_name: &str,
    baud_rate: u32,
) -> Result<Box<dyn serialport::SerialPort>, serialport::Error> {
    // Initialize the serial port here
    let port = serialport::new(port_name, baud_rate)
        .timeout(std::time::Duration::from_millis(100))
        .open()?;
    Ok(port)
}

fn die(err: &impl std::fmt::Display) -> ! {
    eprintln!("Error: {err}");
    std::process::exit(1);
}

fn is_wifi_mgmt_response(frame: &Frame) -> bool {
    decode_wifi_mgmt_response(frame).is_ok()
}

fn is_scan_done_notification(frame: &Frame) -> bool {
    decode_scan_done_notification(frame).is_some()
}

/// Matches AP result notification frames for the given direction
/// (`is_enable` selects ApEnableResultEvent vs ApDisableResultEvent).
fn is_ap_result_notification(is_enable: bool) -> impl Fn(&Frame) -> bool {
    move |frame: &Frame| decode_ap_result_notification(frame).is_some_and(|(en, _)| en == is_enable)
}

/// Matches HCI frames carrying a Command Complete event for `opcode`.
fn is_hci_command_complete(opcode: u16) -> impl Fn(&Frame) -> bool {
    move |frame: &Frame| {
        frame.rtype == SerialType::Hci
            && hci_command_complete(&frame.payload).is_some_and(|cc| cc.opcode == opcode)
    }
}

/// Parses a hex string (e.g. "0103" or "00") into a byte vector.
fn parse_hex(value: &str) -> Result<Vec<u8>, String> {
    let cleaned: String = value.chars().filter(|c| !c.is_whitespace()).collect();
    if !cleaned.len().is_multiple_of(2) {
        return Err(format!("hex value has odd length: {value}"));
    }
    let bytes = cleaned.as_str();
    let mut out = Vec::with_capacity(bytes.len() / 2);
    for chunk in bytes.as_bytes().chunks(2) {
        let hi = (chunk[0] as char)
            .to_digit(16)
            .ok_or_else(|| format!("invalid hex digit in {value}"))?;
        let lo = (chunk[1] as char)
            .to_digit(16)
            .ok_or_else(|| format!("invalid hex digit in {value}"))?;
        out.push(((hi << 4) | lo) as u8);
    }
    Ok(out)
}

/// Sends an HCI command and waits for its Command Complete event.
fn hci_request(
    session: &mut Session,
    deadline: Instant,
    opcode: u16,
    params: &[u8],
) -> HciCommandComplete {
    let pkt = hci_command_packet(opcode, params);
    let frame = match session.request(
        SerialType::Hci,
        &pkt,
        deadline,
        &is_hci_command_complete(opcode),
    ) {
        Ok(frame) => frame,
        Err(e) => die(&e),
    };
    match hci_command_complete(&frame.payload) {
        Some(cc) => cc,
        None => die(&"malformed HCI command complete event"),
    }
}

fn main() {
    let cli = Cli::parse();

    if matches!(cli.command, Commands::Test { .. } | Commands::List) {
        // Commands that do not talk to a device.
        match cli.command {
            Commands::Test { list } => {
                if list {
                    println!("Listing test values...");
                } else {
                    println!("Running tests...");
                }
            }
            Commands::List => {
                // This command could be used to list items, resources, etc.
                if let Ok(ports) = serialport::available_ports() {
                    println!("Available serial ports:");
                    for port in ports {
                        println!("{}", port.port_name);
                    }
                } else {
                    println!("No serial ports found.");
                }
            }
            _ => unreachable!(),
        }
        return;
    }

    let port = match init_serial_port(&cli.port, cli.baud_rate) {
        Ok(port) => port,
        Err(e) => die(&format!("Failed to open serial port: {e}")),
    };
    let mut session = Session::new(port);

    // Handshake: learn the device's next expected sequence number before
    // sending any command.
    let info = match session.sync(Instant::now() + RESPONSE_TIMEOUT) {
        Ok(info) => info,
        Err(e) => die(&e),
    };
    eprintln!(
        "Synced: protocol version {}, supported types 0x{:04X}, next seq {}",
        info.version, info.supported_types_mask, info.next_seq
    );

    let deadline = Instant::now() + RESPONSE_TIMEOUT;

    match cli.command {
        Commands::Version => {
            let payload = version_request_payload();
            let frame = match session.request(
                SerialType::WifiMgmt,
                &payload,
                deadline,
                &is_wifi_mgmt_response,
            ) {
                Ok(frame) => frame,
                Err(e) => die(&e),
            };
            println!("{}", format_version_response(&frame));
        }
        Commands::Scan { iface_index } => {
            let payload = scan_request_payload(iface_index);
            let status = match session.request(
                SerialType::WifiMgmt,
                &payload,
                deadline,
                &is_wifi_mgmt_response,
            ) {
                Ok(frame) => frame,
                Err(e) => die(&e),
            };
            match format_status_response(&status) {
                Ok(message) => println!("{message}"),
                Err(message) => die(&message),
            }
            eprintln!(
                "Waiting for scan results (up to {} s)...",
                SCAN_TIMEOUT.as_secs()
            );
            let done =
                match session.wait_for(Instant::now() + SCAN_TIMEOUT, &is_scan_done_notification) {
                    Ok(frame) => frame,
                    Err(e) => die(&e),
                };
            let (scan_status, results) = match decode_scan_done_notification(&done) {
                Some(done) => done,
                None => die(&"malformed scan done notification"),
            };
            println!("{}", format_scan_results(scan_status, &results));
        }
        Commands::Connect {
            iface_index,
            ssid,
            psk,
            security,
            timeout_ms,
        } => {
            if !matches!(security, ConnectSecurityArg::None) && psk.is_none() {
                eprintln!("--psk is required for selected security mode");
                std::process::exit(1);
            }

            let payload = connect_request_payload(
                iface_index,
                &ssid,
                psk.as_deref(),
                security.to_proto(),
                timeout_ms,
            );
            let frame = match session.request(
                SerialType::WifiMgmt,
                &payload,
                deadline,
                &is_wifi_mgmt_response,
            ) {
                Ok(frame) => frame,
                Err(e) => die(&e),
            };
            match format_status_response(&frame) {
                Ok(message) => println!("{message}"),
                Err(message) => die(&message),
            }
        }
        Commands::Disconnect { iface_index } => {
            let payload = disconnect_request_payload(iface_index);
            let frame = match session.request(
                SerialType::WifiMgmt,
                &payload,
                deadline,
                &is_wifi_mgmt_response,
            ) {
                Ok(frame) => frame,
                Err(e) => die(&e),
            };
            match format_status_response(&frame) {
                Ok(message) => println!("{message}"),
                Err(message) => die(&message),
            }
        }
        Commands::Interfaces {
            iface_index,
            iface_type,
        } => {
            let payload = interfaces_request_payload(iface_type.to_proto(), iface_index);
            let frame =
                match session.request(SerialType::NetIfMgmt, &payload, deadline, &|f: &Frame| {
                    f.rtype == SerialType::NetIfMgmt && !f.payload.is_empty()
                }) {
                    Ok(frame) => frame,
                    Err(e) => die(&e),
                };
            println!("{}", format_interfaces_response(&frame));
        }
        Commands::RegDomain { command: cmd } => match cmd {
            RegDomainCmd::Set {
                iface_index,
                country,
                force,
            } => {
                if country.len() != 2 {
                    die(&"--country must be a 2-letter ISO 3166-1 alpha-2 code");
                }
                let payload = set_reg_domain_request_payload(iface_index, &country, force);
                let frame = match session.request(
                    SerialType::WifiMgmt,
                    &payload,
                    deadline,
                    &is_wifi_mgmt_response,
                ) {
                    Ok(frame) => frame,
                    Err(e) => die(&e),
                };
                match format_status_response(&frame) {
                    Ok(message) => println!("{message}"),
                    Err(message) => die(&message),
                }
            }
            RegDomainCmd::Get { iface_index } => {
                let payload = get_reg_domain_request_payload(iface_index);
                let frame = match session.request(
                    SerialType::WifiMgmt,
                    &payload,
                    deadline,
                    &is_wifi_mgmt_response,
                ) {
                    Ok(frame) => frame,
                    Err(e) => die(&e),
                };
                println!("{}", format_reg_domain_response(&frame));
            }
        },
        Commands::PowerSave { command: cmd } => match cmd {
            PowerSaveCmd::Set {
                iface_index,
                enable,
                listen_interval,
                timeout_ms,
                wmm,
            } => {
                let payload =
                    set_ps_request_payload(iface_index, enable, listen_interval, timeout_ms, wmm);
                let frame = match session.request(
                    SerialType::WifiMgmt,
                    &payload,
                    deadline,
                    &is_wifi_mgmt_response,
                ) {
                    Ok(frame) => frame,
                    Err(e) => die(&e),
                };
                match format_status_response(&frame) {
                    Ok(message) => println!("{message}"),
                    Err(message) => die(&message),
                }
            }
            PowerSaveCmd::Get { iface_index } => {
                let payload = get_ps_request_payload(iface_index);
                let frame = match session.request(
                    SerialType::WifiMgmt,
                    &payload,
                    deadline,
                    &is_wifi_mgmt_response,
                ) {
                    Ok(frame) => frame,
                    Err(e) => die(&e),
                };
                println!("{}", format_ps_response(&frame));
            }
        },
        Commands::IfaceStatus { iface_index } => {
            let payload = iface_status_request_payload(iface_index);
            let frame = match session.request(
                SerialType::WifiMgmt,
                &payload,
                deadline,
                &is_wifi_mgmt_response,
            ) {
                Ok(frame) => frame,
                Err(e) => die(&e),
            };
            println!("{}", format_iface_status_response(&frame));
        }
        Commands::Ap { command: cmd } => match cmd {
            ApCmd::Enable {
                iface_index,
                ssid,
                channel,
                band,
                security,
                psk,
            } => {
                if ssid.is_empty() || ssid.len() > 32 {
                    die(&"--ssid must be 1..=32 characters");
                }
                if matches!(security, ApSecurityArg::Psk | ApSecurityArg::Sae) && psk.is_none() {
                    die(&"--psk is required for PSK/SAE AP security");
                }
                let payload = ap_enable_request_payload(
                    iface_index,
                    &ssid,
                    channel,
                    band.to_proto(),
                    security.to_proto(),
                    psk.as_deref(),
                );
                let frame = match session.request(
                    SerialType::WifiMgmt,
                    &payload,
                    deadline,
                    &is_wifi_mgmt_response,
                ) {
                    Ok(frame) => frame,
                    Err(e) => die(&e),
                };
                let status = match decode_wifi_status_response(&frame) {
                    Ok(status) => status,
                    Err(message) => die(&message),
                };
                match format_status_response(&frame) {
                    Ok(message) => println!("{message}"),
                    Err(message) => die(&message),
                }
                if !status.success {
                    eprintln!("AP enable request rejected by device; no result event expected.");
                    return;
                }
                eprintln!(
                    "Waiting for AP enable result (up to {} s)...",
                    RESPONSE_TIMEOUT.as_secs()
                );
                let done = match session.wait_for(
                    Instant::now() + RESPONSE_TIMEOUT,
                    &is_ap_result_notification(true),
                ) {
                    Ok(frame) => frame,
                    Err(e) => die(&e),
                };
                let ap_status = match decode_ap_result_notification(&done) {
                    Some((true, s)) => s,
                    _ => die(&"malformed AP enable result notification"),
                };
                println!(
                    "AP enable result: status {ap_status}{}",
                    if ap_status == 0 { " (success)" } else { "" }
                );
            }
            ApCmd::Disable { iface_index } => {
                let payload = ap_disable_request_payload(iface_index);
                let frame = match session.request(
                    SerialType::WifiMgmt,
                    &payload,
                    deadline,
                    &is_wifi_mgmt_response,
                ) {
                    Ok(frame) => frame,
                    Err(e) => die(&e),
                };
                let status = match decode_wifi_status_response(&frame) {
                    Ok(status) => status,
                    Err(message) => die(&message),
                };
                match format_status_response(&frame) {
                    Ok(message) => println!("{message}"),
                    Err(message) => die(&message),
                }
                if !status.success {
                    eprintln!("AP disable request rejected by device; no result event expected.");
                    return;
                }
                eprintln!(
                    "Waiting for AP disable result (up to {} s)...",
                    RESPONSE_TIMEOUT.as_secs()
                );
                let done = match session.wait_for(
                    Instant::now() + RESPONSE_TIMEOUT,
                    &is_ap_result_notification(false),
                ) {
                    Ok(frame) => frame,
                    Err(e) => die(&e),
                };
                let ap_status = match decode_ap_result_notification(&done) {
                    Some((false, s)) => s,
                    _ => die(&"malformed AP disable result notification"),
                };
                println!(
                    "AP disable result: status {ap_status}{}",
                    if ap_status == 0 { " (success)" } else { "" }
                );
            }
        },
        Commands::Hci { command: cmd } => match cmd {
            HciCmd::Reset => {
                let cc = hci_request(&mut session, deadline, HCI_OP_RESET, &[]);
                println!("{}", format_hci_command_complete(&cc));
            }
            HciCmd::Version => {
                let cc = hci_request(&mut session, deadline, HCI_OP_READ_LOCAL_VERSION, &[]);
                println!("{}", format_hci_command_complete(&cc));
            }
            HciCmd::Commands => {
                let cc = hci_request(
                    &mut session,
                    deadline,
                    HCI_OP_READ_LOCAL_SUPPORTED_COMMANDS,
                    &[],
                );
                println!("{}", format_hci_command_complete(&cc));
            }
            HciCmd::Features => {
                let cc = hci_request(
                    &mut session,
                    deadline,
                    HCI_OP_READ_LOCAL_SUPPORTED_FEATURES,
                    &[],
                );
                println!("{}", format_hci_command_complete(&cc));
            }
            HciCmd::Addr => {
                let cc = hci_request(&mut session, deadline, HCI_OP_READ_BD_ADDR, &[]);
                println!("{}", format_hci_command_complete(&cc));
            }
            HciCmd::Info => {
                let _ = hci_request(&mut session, deadline, HCI_OP_RESET, &[]);
                let cc = hci_request(&mut session, deadline, HCI_OP_READ_LOCAL_VERSION, &[]);
                println!("{}", format_hci_command_complete(&cc));
            }
            HciCmd::Raw { opcode, params } => {
                let op_bytes = match parse_hex(&opcode) {
                    Ok(b) => b,
                    Err(e) => die(&e),
                };
                if op_bytes.len() != 2 {
                    die(&"opcode must be exactly 2 hex bytes (e.g. 0103)");
                }
                let op = u16::from_le_bytes([op_bytes[0], op_bytes[1]]);
                let p = match params {
                    Some(p) => match parse_hex(&p) {
                        Ok(b) => b,
                        Err(e) => die(&e),
                    },
                    None => Vec::new(),
                };
                let cc = hci_request(&mut session, deadline, op, &p);
                println!("{}", format_hci_command_complete(&cc));
            }
        },
        Commands::Coexist { iface_index } => {
            // 1. Start a Wi-Fi scan (traffic type WIFI_MGMT).
            let payload = scan_request_payload(iface_index);
            let status = match session.request(
                SerialType::WifiMgmt,
                &payload,
                deadline,
                &is_wifi_mgmt_response,
            ) {
                Ok(frame) => frame,
                Err(e) => die(&e),
            };
            match format_status_response(&status) {
                Ok(message) => println!("{message}"),
                Err(message) => die(&message),
            }

            // 2. While the scan is in flight on the device, issue a Bluetooth
            //    HCI command (traffic type HCI). The Command Complete arrives in
            //    milliseconds, well before the scan-done notification, so the
            //    two protocols' frames interleave on the shared serial link.
            eprintln!("Scan in progress; issuing HCI Read Local Version ...");
            let cc = hci_request(&mut session, deadline, HCI_OP_READ_LOCAL_VERSION, &[]);
            println!("{}", format_hci_command_complete(&cc));

            // 3. Finish waiting for the scan results.
            eprintln!(
                "Waiting for scan results (up to {} s)...",
                SCAN_TIMEOUT.as_secs()
            );
            let done =
                match session.wait_for(Instant::now() + SCAN_TIMEOUT, &is_scan_done_notification) {
                    Ok(frame) => frame,
                    Err(e) => die(&e),
                };
            let (scan_status, results) = match decode_scan_done_notification(&done) {
                Some(done) => done,
                None => die(&"malformed scan done notification"),
            };
            println!("{}", format_scan_results(scan_status, &results));
            eprintln!("Coexistence OK: Wi-Fi scan and HCI command completed on one link session.");
        }
        Commands::Test { .. } | Commands::List => unreachable!(),
    }
}
