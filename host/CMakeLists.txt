#
# Host application for the embedded80211 link protocol (Renode board profile).
#
# Runs on the app-local renode_cortex_m3 board (see boards/renode_cortex_m3/).
# uart1 is the bus port bridged to the device; uart0 is the console. The
# virtual Wi-Fi and BT HCI drivers are registered in app.overlay.
#
cmake_minimum_required(VERSION 3.20.5)
cmake_policy(SET CMP0057 NEW)

# Register the embedded80211 Zephyr module (sibling directory). Must be set
# before find_package(Zephyr) so the module is discovered during kernel-mode
# build configuration (mirrors nrf7002dk/CMakeLists.txt).
set(ZEPHYR_EXTRA_MODULES ${CMAKE_CURRENT_SOURCE_DIR}/../embedded80211)

# App-local board and SoC definitions. Prepend this directory to BOARD_ROOT
# and SOC_ROOT so `west build -b renode_cortex_m3` finds them without
# extra flags.
set(BOARD_ROOT ${CMAKE_CURRENT_SOURCE_DIR} ${BOARD_ROOT})
set(SOC_ROOT ${CMAKE_CURRENT_SOURCE_DIR} ${SOC_ROOT})

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(e80211_host)
# Expose the embedded80211 module public headers to the app sources.
target_include_directories(app PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}/../embedded80211/zephyr/include
)

target_sources(app PRIVATE src/main.c)

if(CONFIG_HOST_TEST_ENABLED)
	target_sources(app PRIVATE src/e2e_test.c)
endif()
