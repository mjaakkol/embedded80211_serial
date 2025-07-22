//! Protocol definitions for the the wire protocol used between host and device.
//!

use bytes::{Bytes, BytesMut, BufMut};

enum SerialType {
    WiFiMgmt,
    WiFiData,
    HCI,
    Thread,
    AtCmd,
    Unknown,
}

impl SerialType {
    #[inline]
    fn to_u8(&self) -> u8 {
        match self {
            SerialType::WiFiMgmt => 0x01,
            SerialType::WiFiData => 0x02,
            SerialType::HCI => 0x03,
            SerialType::Thread => 0x04,
            SerialType::AtCmd => 0x05,
            SerialType::Unknown => panic!("Unknown SerialType cannot be converted to u32"),
        }
    }
}


fn create_header(stype: SerialType, no_ack: bool, length: u16) -> BytesMut {
    let mut header = BytesMut::with_capacity(4 + length as usize);

    header.put_u8(
        stype.to_u8() << 4 | (no_ack as u8) << 3,
    );
    header.put_u8(0); // reserved byte
    header.put_u8(length as u8);
    header.put_u8((length >> 8) as u8); // length high byte

    header
}