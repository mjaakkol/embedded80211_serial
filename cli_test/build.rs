use std::io::Result;
fn main() -> Result<()> {
    prost_build::compile_protos(&["../zephyr/protos/wifi.proto"], &["../zephyr/protos/"])?;
    Ok(())
}