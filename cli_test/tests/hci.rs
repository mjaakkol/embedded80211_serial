//! Integration test for the HCI command path against a mocked device.
//!
//! The mock device implements the common link protocol just enough for a
//! session: PROTOCOL_INFO sync responses and HCI Command Complete events
//! for the opcodes it receives.

use std::sync::{Arc, Mutex};

use cli_test::mock::{MockPort, MockSerialState};
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

    fn emit(&mut self, traffic_type: SerialType, seq: u8, payload: &[u8], advance_tx: bool) {
        let mut out: Vec<u8> = Vec::with_capacity(4 + payload.len());
        out.push((1 << 5) | traffic_type.to_u8()); // host = 1, no error
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
        match frame.traffic_type {
            SerialType::ProtocolInfo => {
                let payload = [1u8, 0x9F, 0x01, self.rx_seq]; // v1, HCI bit set
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
                self.emit(frame.traffic_type, frame.seq, &[], false);
                self.rx_seq = (self.rx_seq + 1) & 0x0F;
            }
        }
    }
}
impl MockSerialState for HciDeviceState {
    fn read_data(&mut self, buf: &mut [u8]) -> usize {
        let n = buf.len().min(self.to_host.len());
        buf[..n].copy_from_slice(&self.to_host[..n]);
        self.to_host.drain(..n);
        n
    }

    fn write_data(&mut self, buf: &[u8]) {
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
                self.handle_host_frame(&frame);
            }
        }
    }

    fn bytes_ready(&self) -> u32 {
        self.to_host.len() as u32
    }
}

/// A full session round trip over the mock: sync, then HCI reset and version
/// commands, verifying the opcodes the device saw and the parsed events.
#[test]
fn hci_command_round_trip() {
    let device = Arc::new(Mutex::new(HciDeviceState::new()));
    let mut session = Session::new(Box::new(MockPort(device.clone())));
    let deadline = std::time::Instant::now() + std::time::Duration::from_secs(2);

    let info = session.sync(deadline).expect("sync");
    assert_eq!(info.version, 1);
    assert_ne!(info.supported_types_mask & 0x0008, 0, "HCI type bit set");

    // Reset: H:4 packet [01 03 0C 00].
    let pkt = hci_command_packet(HCI_OP_RESET, &[]);
    assert_eq!(pkt, vec![0x01, 0x03, 0x0C, 0x00]);
    let frame = session
        .request(SerialType::Hci, &pkt, deadline, &|f: &Frame| {
            f.traffic_type == SerialType::Hci
                && hci_command_complete(&f.payload).is_some_and(|cc| cc.opcode == HCI_OP_RESET)
        })
        .expect("reset request");
    let cc = hci_command_complete(&frame.payload).expect("reset CC");
    assert_eq!(cc.opcode, HCI_OP_RESET);

    // Version: device echoes a CC with 11 params.
    let pkt = hci_command_packet(HCI_OP_READ_LOCAL_VERSION, &[]);
    let frame = session
        .request(SerialType::Hci, &pkt, deadline, &|f: &Frame| {
            f.traffic_type == SerialType::Hci
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
