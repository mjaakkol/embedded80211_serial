#
# Copyright (c) 2025 Mikko Jaakkola
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

cmake_minimum_required(VERSION 3.20.5)
cmake_policy(SET CMP0057 NEW)

# Register the embedded80211 common link protocol library as an out-of-tree
# Zephyr module so its sources are compiled and linked into the image.
# Must be set before find_package(Zephyr) so the module is discovered during
# Zephyr's kernel-mode build configuration.
set(ZEPHYR_EXTRA_MODULES ${CMAKE_CURRENT_SOURCE_DIR}/../embedded80211)

# Rebalance the flash/SRAM partitions so the Wi-Fi co-processor appcore fits
# (see partitions.overlay.dts). Must be set before find_package(Zephyr): the
# variable is gathered during Zephyr package processing.
set(EXTRA_DTC_OVERLAY_FILE ${CMAKE_CURRENT_SOURCE_DIR}/partitions.overlay.dts)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(embedded80211_serial)

target_include_directories(app PUBLIC ${ZEPHYR_BASE}/subsys/net/ip)

target_sources(app PRIVATE
	src/main.c
)
