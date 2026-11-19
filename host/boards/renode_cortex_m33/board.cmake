# Renode virtual Cortex-M33 board (host application).

# Zephyr emulation platform support: `west build -t run` launches Renode via
# support/renode_cortex_m33.resc.
set(SUPPORTED_EMU_PLATFORMS renode)
set(RENODE_SCRIPT ${CMAKE_CURRENT_LIST_DIR}/support/renode_cortex_m33.resc)
set(RENODE_UART sysbus.uart0)
