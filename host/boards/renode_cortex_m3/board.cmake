# Renode virtual Cortex-M3 board (host application).

# Zephyr emulation platform support: `west build -t run` launches Renode via
# support/renode_cortex_m3.resc.
set(SUPPORTED_EMU_PLATFORMS renode)
set(RENODE_SCRIPT ${CMAKE_CURRENT_LIST_DIR}/support/renode_cortex_m3.resc)
set(RENODE_UART sysbus.uart0)
