cmake_minimum_required(VERSION 3.11)

project(psp_usb_midi VERSION 1.0.0 LANGUAGES C)

option(PSP_USB_MIDI_BUILD_EXAMPLE "Build the minimal example EBOOT" ON)

set(PSP_USB_MIDI_WARNINGS
    -Wall
    -Wextra
    -Wpedantic
    -Werror
)

add_prx_module(UsbMidiDriver
    driver/module.c
    driver/usb_core.c
    driver/usb_descriptors.c
    driver/usb_midi_packets.c
    driver/usb_midi_ipc.c
    driver/usb_midi_psp_transport.c
    driver/usb_midi_tx.c
    driver/exports.exp
)
target_compile_features(UsbMidiDriver PRIVATE c_std_11)
target_include_directories(UsbMidiDriver PRIVATE driver shared)
target_compile_options(UsbMidiDriver PRIVATE
    ${PSP_USB_MIDI_WARNINGS}
    -mno-gpopt
)
target_link_options(UsbMidiDriver PRIVATE -nostdlib)
target_link_libraries(UsbMidiDriver PRIVATE
    pspusbbus_driver
    pspmodinfo
    pspsdk
    pspkernel
)

add_library(PspUsbMidi STATIC
    library/psp_usb_midi.c
    library/usb_midi_state.c
    driver/usb_midi_packets.c
)
add_library(PspUsbMidi::PspUsbMidi ALIAS PspUsbMidi)
target_compile_features(PspUsbMidi PRIVATE c_std_11)
target_include_directories(PspUsbMidi
    PUBLIC include
    PRIVATE driver library shared
)
target_compile_options(PspUsbMidi PRIVATE ${PSP_USB_MIDI_WARNINGS})
target_link_libraries(PspUsbMidi PUBLIC pspusb pspsdk)

set(PSP_USB_MIDI_BUNDLE_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/dist/PspUsbMidi")
add_custom_target(PspUsbMidiBundle ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "${PSP_USB_MIDI_BUNDLE_DIR}/include"
        "${PSP_USB_MIDI_BUNDLE_DIR}/lib"
        "${PSP_USB_MIDI_BUNDLE_DIR}/kernel"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/include/psp_usb_midi.h"
        "${PSP_USB_MIDI_BUNDLE_DIR}/include/psp_usb_midi.h"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:PspUsbMidi>"
        "${PSP_USB_MIDI_BUNDLE_DIR}/lib/libPspUsbMidi.a"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_BINARY_DIR}/UsbMidiDriver.prx"
        "${PSP_USB_MIDI_BUNDLE_DIR}/kernel/UsbMidiDriver.prx"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/PspUsbMidiConfig.cmake"
        "${PSP_USB_MIDI_BUNDLE_DIR}/PspUsbMidiConfig.cmake"
    DEPENDS PspUsbMidi UsbMidiDriver
    COMMENT "Creating the standalone PSP USB MIDI bundle"
)

if(PSP_USB_MIDI_BUILD_EXAMPLE)
    add_executable(UsbMidiExample examples/basic/main.c)
    add_dependencies(UsbMidiExample UsbMidiDriver)
    target_compile_features(UsbMidiExample PRIVATE c_std_11)
    target_compile_options(UsbMidiExample PRIVATE ${PSP_USB_MIDI_WARNINGS})
    target_link_libraries(UsbMidiExample PRIVATE
        PspUsbMidi
        pspdebug
        pspdisplay
        pspge
        pspctrl
        pspsdk
    )

    set(PSP_USB_MIDI_EXAMPLE_DIR
        "${CMAKE_CURRENT_BINARY_DIR}/dist/UsbMidiExample")
    create_pbp_file(
        TARGET UsbMidiExample
        ICON_PATH NULL
        BACKGROUND_PATH NULL
        PREVIEW_PATH NULL
        TITLE "PSP USB MIDI Example"
        VERSION 01.00
        OUTPUT_DIR "${PSP_USB_MIDI_EXAMPLE_DIR}"
    )
    add_custom_command(
        TARGET UsbMidiExample
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_BINARY_DIR}/UsbMidiDriver.prx"
            "${PSP_USB_MIDI_EXAMPLE_DIR}/UsbMidiDriver.prx"
        COMMENT "Copying the USB MIDI driver beside the example EBOOT"
    )
endif()
