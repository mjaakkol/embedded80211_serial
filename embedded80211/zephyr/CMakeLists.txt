#
# Copyright (c) 2022 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#
# embedded80211: common link protocol library for radio co-processors.
#
# Technology proxies (Wi-Fi mgmt/data, net_if mgmt, BT HCI, OpenThread
# Spinel, virtual serial) plus the serial transport layer, mapped onto
# one common link protocol. Consumed by board applications (device
# role) and by host applications (host role).
#
# This is a Zephyr module: it is registered by the consuming application
# via ZEPHYR_EXTRA_MODULES and processed in Zephyr kernel mode.

zephyr_library_named(embedded80211)

#
# Protobuf schemas (device + host roles)
#
list(APPEND CMAKE_MODULE_PATH ${ZEPHYR_BASE}/modules/nanopb)
include(nanopb)

set(PROTO_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../protos)

# Generated C sources are compiled into the library; the generated
# headers are exposed to consumers via the PUBLIC include dir added by
# zephyr_nanopb_sources().
zephyr_nanopb_sources(${ZEPHYR_CURRENT_LIBRARY}
	${PROTO_DIR}/wifi.proto
	${PROTO_DIR}/network_interface.proto)

# The nanopb runtime (pb_common/encode/decode) is provided by the
# modules__nanopb Zephyr library in NCS device builds. When that target is
# absent (e.g. the standalone host app build), compile the runtime sources
# directly so pb_encode/pb_decode/pb_istream are resolved.
if(NOT TARGET modules__nanopb)
zephyr_library_sources(
	${ZEPHYR_NANOPB_MODULE_DIR}/pb_common.c
	${ZEPHYR_NANOPB_MODULE_DIR}/pb_encode.c
	${ZEPHYR_NANOPB_MODULE_DIR}/pb_decode.c
)
endif()

#
# Include paths:
#  - include/  ->  #include <embedded80211/serial.h>, <embedded80211/multiplexer.h>, ...
#  - device/   ->  device-internal headers ("wifi_data.h", "vserial.h", ...)
#
target_include_directories(${ZEPHYR_CURRENT_LIBRARY} PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}/include
	${CMAKE_CURRENT_SOURCE_DIR}/device
	${ZEPHYR_NANOPB_MODULE_DIR}
)

#
# Device-role sources (RCP device): multiplexer, technology proxies, and the
# serial transport layer. Compiled only for the device role; the host role
# builds just the host-side drivers at the bottom of this file.
#
if(CONFIG_SERIAL_DEVICE_ROLE)

# Core multiplexer
zephyr_library_sources_ifdef(CONFIG_SERIAL_PROTOCOL_MULTIPLEXER device/multiplexer.c)

# Technology proxies
zephyr_library_sources_ifdef(CONFIG_SERIAL_PROXY_WIFI_MGMT device/wifi_mgmt_proxy.c)
zephyr_library_sources_ifdef(CONFIG_SERIAL_PROXY_WIFI_DATA device/wifi_data.c)
zephyr_library_sources_ifdef(CONFIG_SERIAL_PROXY_NET_IF_MGMT device/net_if_mgmt_proxy.c)
zephyr_library_sources_ifdef(CONFIG_SERIAL_PROXY_BT device/bt_proxy.c)
zephyr_library_sources_ifdef(CONFIG_SERIAL_PROXY_OT device/ot_proxy.c)
zephyr_library_sources_ifdef(CONFIG_SERIAL_PROXY_VSERIAL device/vserial.c)

# Serial transport layer
zephyr_library_sources_ifdef(CONFIG_SERIAL device/serial/serial.c)
zephyr_library_sources_ifdef(CONFIG_SERIAL device/serial/uart.c)
zephyr_library_sources_ifdef(CONFIG_USBD_CDC_ACM_CLASS device/serial/serial_usb.c)

endif() # CONFIG_SERIAL_DEVICE_ROLE

#
# Host-side drivers (host role only)
#
zephyr_library_sources_ifdef(CONFIG_EMBEDDED80211_HOST_BUS host/bus.c)
zephyr_library_sources_ifdef(CONFIG_EMBEDDED80211_HOST_WIFI host/host_wifi.c)
zephyr_library_sources_ifdef(CONFIG_EMBEDDED80211_HOST_BT host/host_bt.c)
