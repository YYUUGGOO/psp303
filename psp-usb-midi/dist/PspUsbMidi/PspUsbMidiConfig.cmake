if(NOT TARGET PspUsbMidi::PspUsbMidi)
    add_library(PspUsbMidi::PspUsbMidi STATIC IMPORTED)
    set_target_properties(PspUsbMidi::PspUsbMidi PROPERTIES
        IMPORTED_LOCATION
            "${CMAKE_CURRENT_LIST_DIR}/lib/libPspUsbMidi.a"
        INTERFACE_INCLUDE_DIRECTORIES
            "${CMAKE_CURRENT_LIST_DIR}/include"
        INTERFACE_LINK_LIBRARIES
            "pspusb;pspsdk"
    )
endif()

set(PSP_USB_MIDI_DRIVER
    "${CMAKE_CURRENT_LIST_DIR}/kernel/UsbMidiDriver.prx")
