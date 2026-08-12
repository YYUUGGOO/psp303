cmake_minimum_required(VERSION 3.11)

project(psp303)

option(PSP303_BUILD_HOST_TESTS "Build portable host tests" OFF)

if(PSP303_BUILD_HOST_TESTS)
    include(CTest)
    add_subdirectory(tests)
    return()
endif()

add_subdirectory(libpspsync)
include("${CMAKE_CURRENT_SOURCE_DIR}/psp-usb-midi/dist/PspUsbMidi/PspUsbMidiConfig.cmake")

add_executable(${PROJECT_NAME}
    main.c
    midi_output.c
    midi.c
    pattern.c
    randomizer.c
    storage.c
    transport.c
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    pspdebug
    pspdisplay
    pspge
    pspgu
    pspctrl
    pspaudio
    pspaudiolib
    pspsync
    PspUsbMidi::PspUsbMidi
    m
)

target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra)

# Create an EBOOT.PBP file
create_pbp_file(
    TARGET ${PROJECT_NAME}
    ICON_PATH NULL
    BACKGROUND_PATH NULL
    PREVIEW_PATH NULL
    TITLE "PSP-303"
    VERSION 01.00
)

add_custom_command(
    TARGET ${PROJECT_NAME}
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${PSP_USB_MIDI_DRIVER}"
        "${CMAKE_CURRENT_BINARY_DIR}/UsbMidiDriver.prx"
    COMMENT "Copying the prebuilt USB MIDI driver beside PSP-303"
)
