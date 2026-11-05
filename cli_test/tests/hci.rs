//! Integration test for the HCI command path against a mocked device.
//!
//! The mock device implements the common link protocol just enough for a
//! session: PROTOCOL_INFO sync responses and HCI Command Complete events
//! for the opcodes it receives.

use std::io::{Read, Write};
use std::sync::{Arc, Mutex};

use cli_test::protocol::{
    Frame, HCI_OP_READ_LOCAL_VERSION, HCI_OP_RESET, SerialType, Session, hci_command_complete,
    hci_command_packet,
};

struct HciDeviceState {
    to_host: Vec<u8>,
    rx_seq: u8,
    tx_seq: u8,
    /// Opcodes of HCI commands received, in order.
    pub commands: Vec<u16>,
}

impl HciDeviceState {
    fn new() -> Self {
        Self {
            to_host: Vec::new(),
            rx_seq: 0,
            tx_seq: 0,
            commands: Vec::new(),
        }
    }

    fn emit(&mut self, rtype: SerialType, seq: u8, payload: &[u8], advance_tx: bool) {
        let mut out: Vec<u8> = Vec::with_capacity(4 + payload.len());
        out.push((1 << 5) | rtype.to_u8()); // host = 1, no error
        out.push((seq & 0x0F) << 4);
        out.extend_from_slice(&(payload.len() as u16).to_be_bytes());
        out.extend_from_slice(payload);
        self.to_host.extend_from_slice(&out);
        if advance_tx {
            self.tx_seq = (self.tx_seq + 1) & 0x0F;
        }
    }

    /// Builds an H:4 Command Complete event for `opcode` (no status field).
    fn command_complete(opcode: u16, params: &[u8]) -> Vec<u8> {
        let mut p = vec![
            0x04, // H4 event
            0x0E, // Command Complete
            (4 + params.len()) as u8,
            1,
            (opcode & 0xFF) as u8,
            (opcode >> 8) as u8,
            params.len() as u8,
        ];
        p.extend_from_slice(params);
        p
    }

    fn handle_host_frame(&mut self, frame: &Frame) {
        match frame.rtype {
            SerialType::ProtocolInfo => {
                let mask = 0u16.to_le_bytes(); // unused by the test
                let payload = [1u8, 0x9F, 0x01, self.rx_seq]; // v1, HCI bit set
                let _ = mask;
                self.emit(SerialType::ProtocolInfo, self.tx_seq, &payload, true);
            }
            SerialType::Hci => {
                // Record the opcode of the received H:4 command.
                if frame.payload.len() >= 3 && frame.payload[0] == 0x01 {
                    let op = u16::from_le_bytes([frame.payload[1], frame.payload[2]]);
                    self.commands.push(op);
                    let cc = Self::command_complete(op, &[]);
                    self.emit(SerialType::Hci, self.tx_seq, &cc, true);
                }
                self.emit(SerialType::Hci, frame.seq, &[], false);
                self.rx_seq = (self.rx_seq + 1) & 0x0F;
            }
            _ => {
                self.emit(frame.rtype, frame.seq, &[], false);
                self.rx_seq = (self.rx_seq + 1) & 0x0F;
            }
        }
    }
}

struct HciMockPort(Arc<Mutex<HciDeviceState>>);

impl Read for HciMockPort {
    fn read(&mut self, buf: &mut [u8]) -> std::io::Result<usize> {
        let mut state = self.0.lock().unwrap();
        let n = buf.len().min(state.to_host.len());
        buf[..n].copy_from_slice(&state.to_host[..n]);
        state.to_host.drain(..n);
        Ok(n)
    }
}

impl Write for HciMockPort {
    fn write(&mut self, buf: &[u8]) -> std::io::Result<usize> {
        let mut state = self.0.lock().unwrap();
        state.to_host.reserve(buf.len() * 4);
        // Buffer incoming bytes, process complete frames.
        let mut pending: Vec<u8> = Vec::new();
        pending.extend_from_slice(buf);
        while pending.len() >= 4 {
            let len = u16::from_be_bytes([pending[2], pending[3]]) as usize;
            if pending.len() < 4 + len {
                break;
            }
            let frame_bytes: Vec<u8> = pending.drain(..4 + len).collect();
            if let Some(frame) = Frame::parse(&frame_bytes) {
                state.handle_host_frame(&frame);
            }
        }
        Ok(buf.len())
    }

    fn flush(&mut self) -> std::io::Result<()> {
        Ok(())
    }
}

impl serialport::SerialPort for HciMockPort {
    fn name(&self) -> Option<String> {
        Some("hci-mock".to_string())
    }
    fn baud_rate(&self) -> serialport::Result<u32> {
        Ok(115200)
    }
    fn data_bits(&self) -> serialport::Result<serialport::DataBits> {
        Ok(serialport::DataBits::Eight)
    }
    fn flow_control(&self) -> serialport::Result<serialport::FlowControl> {
        Ok(serialport::FlowControl::None)
    }
    fn parity(&self) -> serialport::Result<serialport::Parity> {
        Ok(serialport::Parity::None)
    }
    fn stop_bits(&self) -> serialport::Result<serialport::StopBits> {
        Ok(serialport::StopBits::One)
    }
    fn timeout(&self) -> std::time::Duration {
        std::time::Duration::from_millis(100)
    }
    fn set_baud_rate(&mut self, _baud_rate: u32) -> serialport::Result<()> {
        Ok(())
    }
    fn set_data_bits(&mut self, _data_bits: serialport::DataBits) -> serialport::Result<()> {
        Ok(())
    }
    fn set_flow_control(
        &mut self,
        _flow_control: serialport::FlowControl,
    ) -> serialport::Result<()> {
        Ok(())
    }
    fn set_parity(&mut self, _parity: serialport::Parity) -> serialport::Result<()> {
        Ok(())
    }
    fn set_stop_bits(&mut self, _stop_bits: serialport::StopBits) -> serialport::Result<()> {
        Ok(())
    }
    fn set_timeout(&mut self, _timeout: std::time::Duration) -> serialport::Result<()> {
        Ok(())
    }
    fn write_request_to_send(&mut self, _level: bool) -> serialport::Result<()> {
        Ok(())
    }
    fn write_data_terminal_ready(&mut self, _level: bool) -> serialport::Result<()> {
        Ok(())
    }
    fn read_clear_to_send(&mut self) -> serialport::Result<bool> {
        Ok(true)
    }
    fn read_data_set_ready(&mut self) -> serialport::Result<bool> {
        Ok(false)
    }
    fn read_ring_indicator(&mut self) -> serialport::Result<bool> {
        Ok(false)
    }
    fn read_carrier_detect(&mut self) -> serialport::Result<bool> {
        Ok(false)
    }
    fn bytes_to_read(&self) -> serialport::Result<u32> {
        Ok(self.0.lock().unwrap().to_host.len() as u32)
    }
    fn bytes_to_write(&self) -> serialport::Result<u32> {
        Ok(0)
    }
    fn clear(&self, _buffer_to_clear: serialport::ClearBuffer) -> serialport::Result<()> {
        Ok(())
    }
    fn try_clone(&self) -> serialport::Result<Box<dyn serialport::SerialPort>> {
        Err(serialport::Error::new(
            serialport::ErrorKind::Unknown,
            "mock port cannot be cloned",
        ))
    }
    fn set_break(&self) -> serialport::Result<()> {
        Ok(())
    }
    fn clear_break(&self) -> serialport::Result<()> {
        Ok(())
    }
}

/// A full session round trip over the mock: sync, then HCI reset and version
/// commands, verifying the opcodes the device saw and the parsed events.
#[test]
fn hci_command_round_trip() {
    let device = Arc::new(Mutex::new(HciDeviceState::new()));
    let mut session = Session::new(Box::new(HciMockPort(device.clone())));
    let deadline = std::time::Instant::now() + std::time::Duration::from_secs(2);

    let info = session.sync(deadline).expect("sync");
    assert_eq!(info.version, 1);
    assert_ne!(info.supported_types_mask & 0x0008, 0, "HCI type bit set");

    // Reset: H:4 packet [01 03 0C 00].
    let pkt = hci_command_packet(HCI_OP_RESET, &[]);
    assert_eq!(pkt, vec![0x01, 0x03, 0x0C, 0x00]);
    let frame = session
        .request(SerialType::Hci, &pkt, deadline, &|f: &Frame| {
            f.rtype == SerialType::Hci
                && hci_command_complete(&f.payload).is_some_and(|cc| cc.opcode == HCI_OP_RESET)
        })
        .expect("reset request");
    let cc = hci_command_complete(&frame.payload).expect("reset CC");
    assert_eq!(cc.opcode, HCI_OP_RESET);

    // Version: device echoes a CC with 11 params.
    let pkt = hci_command_packet(HCI_OP_READ_LOCAL_VERSION, &[]);
    let frame = session
        .request(SerialType::Hci, &pkt, deadline, &|f: &Frame| {
            f.rtype == SerialType::Hci
                && hci_command_complete(&f.payload)
                    .is_some_and(|cc| cc.opcode == HCI_OP_READ_LOCAL_VERSION)
        })
        .expect("version request");
    let cc = hci_command_complete(&frame.payload).expect("version CC");
    assert_eq!(cc.opcode, HCI_OP_READ_LOCAL_VERSION);

    let state = device.lock().unwrap();
    assert_eq!(
        state.commands,
        vec![HCI_OP_RESET, HCI_OP_READ_LOCAL_VERSION],
        "device saw both commands in order"
    );
}
