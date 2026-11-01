use std::time::{Duration, Instant};

use clap::{Parser, Subcommand, ValueEnum};

use cli_test::protocol::{
    connect_request_payload, decode_scan_done_notification, decode_wifi_mgmt_response,
    format_interfaces_response, format_scan_results, format_status_response,
    format_version_response, interfaces_request_payload, scan_request_payload,
    version_request_payload, Frame, Session, SerialType,
};
use cli_test::protocol::network_interface::NetIfType;
use cli_test::protocol::wifi_mgmt::WifiSecurityType;

/// How long to wait for a normal request/response round trip.
const RESPONSE_TIMEOUT: Duration = Duration::from_secs(10);
/// How long to wait for scan results to complete.
const SCAN_TIMEOUT: Duration = Duration::from_secs(30);

#[derive(Parser)]
#[command(name = "cli-serial")]
#[command(version = "1.0")]
#[command(about = "cli test application for cfg80211_serial", long_about = None)]
struct Cli {
     #[arg(short, long, action = clap::ArgAction::Count)]
    debug: u8,

    #[arg(short, long,)]
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
    Interfaces {
        #[arg(short = 'i', long, default_value_t = 0)]
        iface_index: u32,

        #[arg(short = 't', long, value_enum, default_value_t = InterfaceTypeArg::All)]
        iface_type: InterfaceTypeArg,
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

fn init_serial_port(port_name: &str, baud_rate: u32) -> Result<Box<dyn serialport::SerialPort>, serialport::Error> {
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
            let frame = match session.request(SerialType::WifiMgmt, &payload, deadline, &is_wifi_mgmt_response) {
                Ok(frame) => frame,
                Err(e) => die(&e),
            };
            println!("{}", format_version_response(&frame));
        }
        Commands::Scan { iface_index } => {
            let payload = scan_request_payload(iface_index);
            let status = match session.request(SerialType::WifiMgmt, &payload, deadline, &is_wifi_mgmt_response) {
                Ok(frame) => frame,
                Err(e) => die(&e),
            };
            match format_status_response(&status) {
                Ok(message) => println!("{message}"),
                Err(message) => die(&message),
            }
            eprintln!("Waiting for scan results (up to {} s)...", SCAN_TIMEOUT.as_secs());
            let done = match session.wait_for(Instant::now() + SCAN_TIMEOUT, &is_scan_done_notification) {
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

            let payload =
                connect_request_payload(iface_index, &ssid, psk.as_deref(), security.to_proto(), timeout_ms);
            let frame = match session.request(SerialType::WifiMgmt, &payload, deadline, &is_wifi_mgmt_response) {
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
            let frame = match session.request(SerialType::NetIfMgmt, &payload, deadline, &|f: &Frame| f.rtype == SerialType::NetIfMgmt && !f.payload.is_empty()) {
                Ok(frame) => frame,
                Err(e) => die(&e),
            };
            println!("{}", format_interfaces_response(&frame));
        }
        Commands::Test { .. } | Commands::List => unreachable!(),
    }
}
