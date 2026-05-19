# cli_test

`cli_test` is a Rust command-line utility for exercising the cfg80211_serial wire protocol over a serial port.
It is intended for quick manual validation of host-to-device requests such as Wi-Fi management and network interface queries.

## What It Can Do

- List locally available serial ports
- Query Wi-Fi firmware/driver version from the device
- Trigger Wi-Fi scan requests
- Query network interfaces from the device
- Send Wi-Fi connect requests

## Build

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

## Notes

- The device side must be running compatible cfg80211_serial firmware and connected to the selected serial port.
- Request/response framing and protobuf payloads are generated from `../protos/wifi.proto` and `../protos/network_interface.proto`.

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
