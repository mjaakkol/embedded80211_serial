#
# Copyright (c) 2022 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

cmake_minimum_required(VERSION 3.21.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

list(APPEND CMAKE_MODULE_PATH ${ZEPHYR_BASE}/modules/nanopb)
include(nanopb)

project(embedded80211_serial)

target_include_directories(app PUBLIC ${ZEPHYR_BASE}/subsys/net/ip)

set(PROTO_DIR ${CMAKE_CURRENT_SOURCE_DIR}/protos)
set(PROTO_FILES ${PROTO_DIR}/wifi.proto)

# Generate C sources from .proto files
# The function will create targets for generating the .pb.c and .pb.h files.
zephyr_nanopb_sources(app ${PROTO_DIR}/wifi.proto)


target_sources(app PRIVATE
	src/main.c
	src/wifi_mgmt_proxy.c
)
