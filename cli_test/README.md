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
- Start/stop the Wi-Fi access point (AP mode) and read its result status
- Transmit a raw 802.11 frame on the air interface (data connectivity testing)


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

### 8. Manage the Wi-Fi access point (AP mode)

```bash
# Start the AP (open network, driver-chosen channel)
cargo run -- --port /dev/ttyACM0 ap enable --ssid TestAp

# Start the AP with WPA2-PSK on a 2.4 GHz channel
cargo run -- --port /dev/ttyACM0 ap enable --ssid TestAp --security psk --psk TestApPass1 --band 2g4 --channel 6

# Tear the AP down
cargo run -- --port /dev/ttyACM0 ap disable
```

Supported security values: `none`, `wep`, `psk`, `sae`.
Supported band values: `any` (default), `2g4`, `5g`, `6g`.
`--channel` is 0 for the driver's choice, otherwise a concrete channel
number.

Both subcommands print the request status response. When the device accepts
the request they then wait for the matching `ApEnableResultEvent` /
`ApDisableResultEvent` notification and print the AP status (0 = success,
negative errno-style code on failure). Use `iface-status` to verify the
interface mode (`mode=AP` while the AP is up). Note: the nrf70 driver in the
current NCS release does not implement AP mode, so the device answers
`error_code=-ENOTSUP` until a driver with AP support is used.

### 9. Transmit a raw 802.11 frame (data connectivity testing)

The device injects a complete IEEE 802.11 MAC frame on the air interface,
bypassing the TCP/IP stack. The frame is read from a binary file (or stdin
with `-`) and must be 24..=2346 bytes (FCS not included; the driver appends
it). The command prints the device's status response.

```bash
# Frame from a file
cargo run -- --port /dev/ttyACM0 raw-tx --file frame.bin

# Frame from stdin
cat frame.bin | cargo run -- --port /dev/ttyACM0 raw-tx
```

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

### Data connectivity (`tests/data_connectivity.rs`)

Verifies raw 802.11 data connectivity (MAC layer only, no TCP/IP). The
device is placed in monitor mode on a channel and transmits a raw 802.11
**broadcast** data frame (a fixed marker plus an iteration sequence
number). Test case 2 repeats the same sequence with the device's Wi-Fi
power save enabled before the packet sequence.

The test runs in two modes:

- **Witness mode** (requires root): a host Wi-Fi adapter (default
  `wlo1`) is placed in monitor mode on the same channel and captures the
  frame on the air via an `AF_PACKET` socket. The test verifies the exact
  frame content arrived. The nRF7002 radio does not loop back its own
  transmissions to the RX path in monitor mode, so a separate on-air
  receiver is required.
- **TX-only mode** (no root needed): the device's own diagnostic counters
  (`tx_frames_sent`, `tx_last_ret`, `tx_last_len`) verify that each frame
  was accepted by the driver and injected. This validates the TX path
  but not on-air delivery.

The test automatically selects witness mode when root is available and
falls back to TX-only mode otherwise.

| Variable       | Meaning                               | Default      |
|----------------|---------------------------------------|--------------|
| `INTEG_PORT`   | serial port (required to run the test) | *(skip if unset)* |
| `INTEG_SSID`   | network SSID                          | `HappyHouse` |
| `INTEG_SECURITY` | `none` / `wep` / `psk` / `sae` / `eap` | `psk`       |
| `INTEG_PSK`    | pre-shared key (required unless `none`) | *(none)*    |
| `INTEG_WITNESS` | host Wi-Fi interface for monitor capture | `wlo1`    |

```bash
# TX-only mode (no root needed)
INTEG_PORT=/dev/ttyACM2 INTEG_SSID=HappyHouse INTEG_SECURITY=psk \
     INTEG_PSK='<ap-psk>' \
    cargo test --test data_connectivity -- --nocapture

# Witness mode (requires root for AF_PACKET + monitor interface)
sudo INTEG_PORT=/dev/ttyACM2 INTEG_SSID=HappyHouse INTEG_SECURITY=psk \
     INTEG_PSK='<ap-psk>' INTEG_WITNESS=wlo1 \
    cargo test --test data_connectivity -- --nocapture
```

In witness mode, a frame not captured within the 5 s window fails the
test. In TX-only mode, a frame whose TX counter does not increment or
whose `tx_last_ret` is non-zero fails the test.

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
