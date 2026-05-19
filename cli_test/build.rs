use std::io::Result;
fn main() -> Result<()> {
    prost_build::compile_protos(
        &["../protos/wifi.proto", "../protos/network_interface.proto"],
        &["../protos/"],
    )?;
    Ok(())
}