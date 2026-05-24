#
# Copyright (c) 2022 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

cmake_minimum_required(VERSION 3.20.5)
cmake_policy(SET CMP0057 NEW)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

add_compile_options(-Wno-unused-function -Wno-unused-variable)


list(APPEND CMAKE_MODULE_PATH ${ZEPHYR_BASE}/modules/nanopb)
include(nanopb)

project(embedded80211_serial)

target_include_directories(app PUBLIC ${ZEPHYR_BASE}/subsys/net/ip)

set(PROTO_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../protos)
set(PROTO_FILES ${PROTO_DIR}/wifi.proto ${PROTO_DIR}/network_interface.proto)

# Generate C sources from .proto files
# The function will create targets for generating the .pb.c and .pb.h files.
zephyr_nanopb_sources(app ${PROTO_FILES})

# we need to be able to include generated protobuf header files
zephyr_library_include_directories(${CMAKE_CURRENT_BINARY_DIR})

target_include_directories(app PUBLIC src)

#src/wifi_data.c


target_sources(app PRIVATE
	src/main.c
	src/multiplexer.c
)

if(CONFIG_SERIAL_PROXY_WIFI_MGMT)
	target_sources(app PRIVATE src/wifi_mgmt_proxy.c)
endif()

if(CONFIG_SERIAL_PROXY_WIFI_DATA)
	target_sources(app PRIVATE src/wifi_data.c)
endif()

if(CONFIG_SERIAL_PROXY_NET_IF_MGMT)
	target_sources(app PRIVATE src/net_if_mgmt_proxy.c)
endif()

if(CONFIG_SERIAL_PROXY_BT)
	target_sources(app PRIVATE src/bt_proxy.c)
endif()

if(CONFIG_SERIAL_PROXY_OT)
	target_sources(app PRIVATE src/ot_proxy.c)
endif()

if(CONFIG_SERIAL_PROXY_VSERIAL)
	target_sources(app PRIVATE src/vserial.c)
endif()

add_subdirectory(src/serial)