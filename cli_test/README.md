# cli_test

`cli_test` is a Rust command-line utility for exercising the cfg80211_serial wire protocol over a serial port.
It is intended for quick manual validation of host-to-device requests such as Wi-Fi management and network interface queries.

## What It Can Do

- List locally available serial ports
- Query Wi-Fi firmware/driver version from the device
- Trigger Wi-Fi scan requests
- Query network interfaces from the device
- Send Wi-Fi connect requests
- Send Wi-Fi disconnect requests
- Send Bluetooth Low Energy HCI commands to the on-board controller

## Build

System prerequisites (Linux):

- `protoc` (Protocol Buffers compiler), found on `PATH` or via `PROTOC`
  - Fedora: `sudo dnf install -y protobuf-compiler`
  - Debian/Ubuntu: `sudo apt-get install -y protobuf-compiler`
- `libudev` development files (required by the `serialport` crate)
  - Fedora: `sudo dnf install -y libudev-devel`
  - Debian/Ubuntu: `sudo apt-get install -y libudev-dev`

From the `cli_test` directory:

```bash
cargo build
```

## Usage

General form:

```bash
cargo run -- --port <serial-port> [--baud-rate <rate>] <command> [command-options]
```

Default baud rate is `115200`.

## Example Commands

### 1. List local serial ports

```bash
cargo run -- --port dummy list
```

Note: `list` does not use the remote device, but the CLI still currently requires `--port`.

### 2. Query Wi-Fi version

```bash
cargo run -- --port /dev/tty.usbmodem14101 version
```

### 3. Trigger Wi-Fi scan

```bash
cargo run -- --port /dev/tty.usbmodem14101 scan
cargo run -- --port /dev/tty.usbmodem14101 scan --iface-index 1
```

### 4. Query network interfaces

```bash
cargo run -- --port /dev/tty.usbmodem14101 interfaces
cargo run -- --port /dev/tty.usbmodem14101 interfaces --iface-type wifi
cargo run -- --port /dev/tty.usbmodem14101 interfaces --iface-index 1 --iface-type openthread
```

Supported interface types:

- `all`
- `wifi`
- `openthread`

### 5. Send Wi-Fi connect request

```bash
cargo run -- --port /dev/tty.usbmodem14101 connect --ssid MyWiFi --psk MySecret123 --security psk
cargo run -- --port /dev/tty.usbmodem14101 connect --ssid OpenNetwork --security none
cargo run -- --port /dev/tty.usbmodem14101 connect --iface-index 1 --ssid MyWiFi --psk MySecret123 --timeout-ms 15000
```

Supported security values:

- `none`
- `wep`
- `psk`
- `sae`
- `eap`

### 6. Send Wi-Fi disconnect request

```bash
cargo run -- --port /dev/tty.usbmodem14101 disconnect
cargo run -- --port /dev/tty.usbmodem14101 disconnect --iface-index 1
```

The disconnect takes effect asynchronously: after `Success: Disconnect
initiated`, verify the link state with `interfaces --iface-type wifi`. The
Wi-Fi interface reports `dormant=true` once the association is down (the
carrier flag is not cleared by the driver).

### 7. Send Bluetooth HCI commands

The board runs the nRF5340 on-chip SoftDevice Controller (BLE) on the
network core. The application core exposes a **raw HCI** channel to the host
over the common link protocol (traffic type `HCI`), so the host is the
Bluetooth host and the board is a transparent HCI pipe to the controller.

Subcommands:

```bash
# Reset the controller (Host-to-Controller HCI_Reset, 0x0C03).
cargo run -- --port /dev/ttyACM0 hci reset

# Read local version information (0x1001).
cargo run -- --port /dev/ttyACM0 hci version

# Read the supported-commands bitmap (0x1002).
cargo run -- --port /dev/ttyACM0 hci commands

# Read the local features bitmap (0x1003).
cargo run -- --port /dev/ttyACM0 hci features

# Read the controller BD_ADDR (0x1009).
cargo run -- --port /dev/ttyACM0 hci addr

# Convenience: reset, then read version information.
cargo run -- --port /dev/ttyACM0 hci info

# Send an arbitrary HCI command by opcode and (optional) hex parameters.
cargo run -- --port /dev/ttyACM0 hci raw 1001
cargo run -- --port /dev/ttyACM0 hci raw 0c03
```

Each subcommand waits for the matching Command Complete event and prints the
decoded result. `raw` prints the raw parameter bytes for any command. The
controller has no public Bluetooth address programmed, so `addr` reports
`00:00:00:00:00:00` until a random address is set by the host.

## Integration Test

`tests/connect_disconnect.rs` repeats full connect/disconnect cycles against
the real device. Connection parameters are configured at run time through
environment variables:

| Variable         | Meaning                                        | Default        |
|------------------|------------------------------------------------|----------------|
| `INTEG_PORT`     | serial port (required to run the test)         | *(skip if unset)* |
| `INTEG_SSID`     | network SSID                                   | `HappyHouse`   |
| `INTEG_SECURITY` | `none` / `wep` / `psk` / `sae` / `eap`         | `psk`          |
| `INTEG_PSK`      | pre-shared key (required unless security is `none`) | *(none)*     |
| `INTEG_CYCLES`   | number of connect/disconnect cycles            | `5`            |

```bash
INTEG_PORT=/dev/ttyACM2 INTEG_SSID=HappyHouse INTEG_SECURITY=psk INTEG_PSK=secret \
    cargo test --test connect_disconnect -- --nocapture
```

Each cycle asserts: connect request accepted, interface reaches the
connected state (carrier present, not dormant), disconnect request accepted,
interface reaches the disconnected state (dormant). Without `INTEG_PORT` the
test skips, so `cargo test` stays green on hosts without the device attached.

## Notes

- The device side must be running compatible cfg80211_serial firmware and connected to the selected serial port.
- Request/response framing and protobuf payloads are generated from `../protos/wifi.proto` and `../protos/network_interface.proto`.
- `hci` commands carry raw H:4 HCI packets (type byte first) inside the common
  link protocol frame; they are not protobuf-encoded. The board forwards them
  to the nRF5340 SoftDevice Controller over HCI IPC and back.

## Troubleshooting

### Command Times Out Or No Response

- Confirm the selected `--port` is correct by running `list` first.
- Verify the device firmware is running and actively connected on the same serial interface.
- Ensure host and device baud rate match (`--baud-rate`, default `115200`).
- Retry the command after reconnecting the serial device.

### Failed To Open Serial Port

- Check that no other program is using the same serial port.
- Verify the path exists (for example `/dev/tty.usbmodem*` on macOS).
- Replug the device and retry.

### "Unable To Decode ... Response"

- This usually indicates host/device protobuf mismatch.
- Rebuild `cli_test` after pulling latest proto changes:

```bash
cargo clean
cargo build
```

- Ensure device firmware was built from a compatible revision.

### Connect Request Fails

- For secured modes (`wep`, `psk`, `sae`, `eap`), provide `--psk`.
- For open networks, use `--security none` and omit `--psk`.
- Verify `--iface-index` matches a valid interface (use `interfaces` command to confirm).

### Scan Or Interfaces Return Errors

- Verify the interface index exists and is active.
- If using filters (`--iface-type`, `--iface-index`), try removing filters first to confirm baseline behavior.
- Check firmware logs on the device side for detailed error codes.
