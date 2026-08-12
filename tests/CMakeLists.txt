cmake_minimum_required(VERSION 3.11)

project(psp303_host_tests C)

enable_testing()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(PSP303_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/..")
set(PSP303_TEST_CFLAGS -Wall -Wextra -Werror)

add_executable(test_groovebox
    test_groovebox.c
    "${PSP303_SOURCE_DIR}/pattern.c"
    "${PSP303_SOURCE_DIR}/transport.c"
)
add_executable(test_midi
    test_midi.c
    "${PSP303_SOURCE_DIR}/midi.c"
)
add_executable(test_storage
    test_storage.c
    "${PSP303_SOURCE_DIR}/pattern.c"
    "${PSP303_SOURCE_DIR}/storage.c"
)
add_executable(test_randomizer
    test_randomizer.c
    "${PSP303_SOURCE_DIR}/pattern.c"
    "${PSP303_SOURCE_DIR}/randomizer.c"
)
add_executable(test_transport
    test_transport.c
    "${PSP303_SOURCE_DIR}/pattern.c"
    "${PSP303_SOURCE_DIR}/transport.c"
)

foreach(test_target IN ITEMS
        test_groovebox
        test_midi
        test_storage
        test_randomizer
        test_transport)
    target_include_directories(${test_target} PRIVATE "${PSP303_SOURCE_DIR}")
    target_compile_options(${test_target} PRIVATE ${PSP303_TEST_CFLAGS})
    add_test(NAME ${test_target} COMMAND ${test_target})
endforeach()
