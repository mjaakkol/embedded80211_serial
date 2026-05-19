use std::io::{Read, Write};
use clap::{Parser, Subcommand};

use cli_test::protocol::{create_scan_query, create_version_query, parse_scan_response, parse_version_response};
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
    }
}
