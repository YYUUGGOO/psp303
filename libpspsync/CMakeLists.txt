add_library(pspsync STATIC
    src/pspsync.c
)

target_include_directories(pspsync PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_options(pspsync PRIVATE -Wall -Wextra)
