use std::time::{Duration, Instant};

use clap::{Parser, Subcommand, ValueEnum};

use cli_test::protocol::network_interface::NetIfType;
use cli_test::protocol::wifi_mgmt::WifiSecurityType;
use cli_test::protocol::{
    Frame, SerialType, Session, connect_request_payload, decode_scan_done_notification,
    decode_wifi_mgmt_response, disconnect_request_payload, format_iface_status_response,
    format_interfaces_response, format_ps_response, format_reg_domain_response,
    format_scan_results, format_status_response, format_version_response,
    get_ps_request_payload, get_reg_domain_request_payload, iface_status_request_payload,
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
        Commands::Test { .. } | Commands::List => unreachable!(),
    }
}
