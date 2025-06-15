use clap::{Parser, Subcommand};

#[derive(Parser)]
#[command(version, about, long_about = None)]
struct Cli {
     #[arg(short, long, action = clap::ArgAction::Count)]
    debug: u8,

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
}

fn main() {
    let cli = Cli::parse();

    println!("Hello, world!");

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
    }
}
