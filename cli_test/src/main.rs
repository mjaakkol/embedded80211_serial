use std::io::{Read, Write};
use clap::{Parser, Subcommand, ValueEnum};

use cli_test::protocol::{
    create_connect_query, create_interfaces_query, create_scan_query, create_version_query,
    parse_connect_response, parse_interfaces_response, parse_scan_response, parse_version_response,
};
use cli_test::protocol::network_interface::NetIfType;
use cli_test::protocol::wifi_mgmt::WifiSecurityType;
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

fn main() {
    let cli = Cli::parse();

    let mut port = init_serial_port(&cli.port, cli.baud_rate).unwrap_or_else(|e| {
        eprintln!("Failed to open serial port: {e}");
        std::process::exit(1);
    });

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
        Commands::Version => {
            let version_query = create_version_query();
            if let Err(e) = port.write_all(&version_query) {
                eprintln!("Failed to write version query: {e}");
                return;
            }

            let mut raw_response = vec![0; 64];
            if let Err(e) = port.read(raw_response.as_mut_slice()) {
                eprintln!("Failed to read version response: {e}");
                return;
            }

            let response = parse_version_response(&raw_response);
            println!("Version: {response}");
        }
        Commands::Scan { iface_index } => {
            let scan_query = create_scan_query(iface_index);
            if let Err(e) = port.write_all(&scan_query) {
                eprintln!("Failed to write scan request: {e}");
                return;
            }

            let mut raw_response = vec![0; 512];
            match port.read(raw_response.as_mut_slice()) {
                Ok(bytes_read) => {
                    raw_response.truncate(bytes_read);
                    let response = parse_scan_response(&raw_response);
                    println!("{response}");
                }
                Err(e) => {
                    eprintln!("Failed to read scan response: {e}");
                }
            }
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
                return;
            }

            let connect_query = create_connect_query(
                iface_index,
                &ssid,
                psk.as_deref(),
                security.to_proto(),
                timeout_ms,
            );
            if let Err(e) = port.write_all(&connect_query) {
                eprintln!("Failed to write connect request: {e}");
                return;
            }

            let mut raw_response = vec![0; 512];
            match port.read(raw_response.as_mut_slice()) {
                Ok(bytes_read) => {
                    raw_response.truncate(bytes_read);
                    let response = parse_connect_response(&raw_response);
                    println!("{response}");
                }
                Err(e) => {
                    eprintln!("Failed to read connect response: {e}");
                }
            }
        }
        Commands::Interfaces {
            iface_index,
            iface_type,
        } => {
            let interfaces_query = create_interfaces_query(iface_type.to_proto(), iface_index);
            if let Err(e) = port.write_all(&interfaces_query) {
                eprintln!("Failed to write interfaces request: {e}");
                return;
            }

            let mut raw_response = vec![0; 1024];
            match port.read(raw_response.as_mut_slice()) {
                Ok(bytes_read) => {
                    raw_response.truncate(bytes_read);
                    let response = parse_interfaces_response(&raw_response);
                    println!("{response}");
                }
                Err(e) => {
                    eprintln!("Failed to read interfaces response: {e}");
                }
            }
        }
    }
}
