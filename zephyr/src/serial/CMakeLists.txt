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

# Focusing in USB now
#if(CONFIG_SERIAL)
#    target_sources(app PRIVATE
#        uart.c
#    )
#endif()
