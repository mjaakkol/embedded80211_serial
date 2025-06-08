#
# Copyright (c) 2022 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
find_package(nanopb REQUIRED)

project(embedded80211_serial)

target_include_directories(app PUBLIC ${ZEPHYR_BASE}/subsys/net/ip)

set(PROTO_DIR ${CMAKE_CURRENT_SOURCE_DIR}/protos)
set(PROTO_FILES ${PROTO_DIR}/wifi.proto)

# Generate C sources from .proto files
# The function will create targets for generating the .pb.c and .pb.h files.
nanopb_generate_c(
    SOURCES ${PROTO_FILES}
    PROTO_PATH ${PROTO_DIR}
    OPTIONS_FILE ${PROTO_DIR}/wifi.options
)

target_sources(app PRIVATE
	src/main.c
	src/process_requests.c
)
