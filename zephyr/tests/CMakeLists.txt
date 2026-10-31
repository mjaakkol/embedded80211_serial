cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(cfg80211_serial_tests)

target_sources(app PRIVATE
    ../src/multiplexer.c
    protocol_test.c
    frame_parser_test.c
    sequence_number_test.c
    error_handling_test.c
    multiplexer_link_protocol_test.c
)

target_include_directories(app PRIVATE
    ${ZEPHYR_BASE}/include
    ${CMAKE_SOURCE_DIR}/../src
    ${CMAKE_SOURCE_DIR}/../src/serial
    ${CMAKE_SOURCE_DIR}/../build/zephyr
)

target_compile_definitions(app PRIVATE
    CONFIG_SERIAL_MUX_IFACE_UART=1
)
