target_include_directories(app PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

#target_sources(app PRIVATE
#        *.c
#        )

target_sources_ifdef(CONFIG_USB_DEVICE_STACK app PRIVATE
     serial_usb.c
)

target_sources_ifdef(CONFIG_SERIAL app PRIVATE
     uart.c
)
