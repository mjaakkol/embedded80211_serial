//! Host-side implementation of the cfg80211_serial common link protocol:
//! frame encoding and parsing, session sequencing, and Wi-Fi management,
//! network interface, and Bluetooth HCI protobuf helpers. See [`protocol`]
//! for the wire format and session API; [`mock`] provides a test serial
//! port.
pub mod mock;
pub mod protocol;
