//! Test utilities: a mock serial port for driving [`protocol::Session`]
//! without physical hardware.
//!
//! Implement [`MockSerialState`] on an in-memory device model and wrap it in
//! [`MockPort`]; the port supplies the entire `serialport::SerialPort`
//! control plane (constant values) so tests only provide the data path.

use std::io::{Read, Write};
use std::sync::{Arc, Mutex};

/// The data path a [`MockPort`] delegates to.
pub trait MockSerialState {
    /// Copies up to `buf.len()` queued bytes into `buf` and returns the count.
    fn read_data(&mut self, buf: &mut [u8]) -> usize;
    /// Consumes `buf` as bytes arriving from the host.
    fn write_data(&mut self, buf: &[u8]);
    /// Number of bytes currently queued for the host to read.
    fn bytes_ready(&self) -> u32;
}

/// A `serialport::SerialPort` backed by a shared [`MockSerialState`].
pub struct MockPort<T: MockSerialState + Send + ?Sized>(pub Arc<Mutex<T>>);

impl<T: MockSerialState + Send + ?Sized> Read for MockPort<T> {
    fn read(&mut self, buf: &mut [u8]) -> std::io::Result<usize> {
        Ok(self
            .0
            .lock()
            .expect("mock port mutex poisoned")
            .read_data(buf))
    }
}

impl<T: MockSerialState + Send + ?Sized> Write for MockPort<T> {
    fn write(&mut self, buf: &[u8]) -> std::io::Result<usize> {
        self.0
            .lock()
            .expect("mock port mutex poisoned")
            .write_data(buf);
        Ok(buf.len())
    }

    fn flush(&mut self) -> std::io::Result<()> {
        Ok(())
    }
}

impl<T: MockSerialState + Send + ?Sized> serialport::SerialPort for MockPort<T> {
    fn name(&self) -> Option<String> {
        Some("mock".to_string())
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
        Ok(self
            .0
            .lock()
            .expect("mock port mutex poisoned")
            .bytes_ready())
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
