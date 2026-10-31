target_include_directories(app PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

#target_sources(app PRIVATE
#    uart.c
#    serial_usb.c
#    uart.c
#)

if(CONFIG_SERIAL OR CONFIG_USB_DEVICE_STACK OR CONFIG_USB_DEVICE_STACK_NEXT)
    target_sources(app PRIVATE
        serial.c
    )
endif()

if(CONFIG_USBD_CDC_ACM_CLASS)
    target_sources(app PRIVATE
        serial_usb.c
    )
endif()

if(CONFIG_SERIAL)
    target_sources(app PRIVATE
        uart.c
    )
endif()
