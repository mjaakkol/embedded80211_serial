target_include_directories(app PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

#target_sources(app PRIVATE
#    uart.c
#    serial_usb.c
#    uart.c
#)

if(CONFIG_SERIAL OR CONFIG_USB_DEVICE_STACK)
    target_sources(app PRIVATE
        serial.c
    )
endif()

if(CONFIG_USB_DEVICE_STACK)
    target_sources(app PRIVATE
        serial_usb.c
    )
endif()

if(CONFIG_SERIAL)
    target_sources(app PRIVATE
        uart.c
    )
endif()

#target_sources_ifdef(CONFIG_USB_DEVICE_STACK app PRIVATE
#     serial_usb.c
#)
#target_sources_ifdef(CONFIG_SERIAL app PRIVATE
#     uart.c
#)
