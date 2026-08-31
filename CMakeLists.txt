cmake_minimum_required(VERSION 3.15)
project(ExploreApp LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS true)

message("-- Toolchain file: " ${CMAKE_TOOLCHAIN_FILE})

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(CL_BUILD_TESTS "Build the test runner" OFF)

add_compile_definitions(CL_MEM_STATS=1)

set(ENGINE_SOURCES
        src/file.cpp
        src/logger.cpp
        src/memory.cpp
        src/path.cpp
        src/result.cpp
        src/strmap.cpp
)

add_executable(ExploreApp
        src/main.cpp
        ${ENGINE_SOURCES}
)

add_custom_command(TARGET ExploreApp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/assets
        $<TARGET_FILE_DIR:ExploreApp>/assets
        COMMENT "Copying assets to output folder"
)

add_custom_command(TARGET ExploreApp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_BINARY_DIR}/compile_commands.json"
        "${CMAKE_SOURCE_DIR}/")

if (CL_BUILD_TESTS)
    file(GLOB TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/tests/*.cpp)

    add_executable(ExploreTests
            ${TEST_SOURCES}
            ${ENGINE_SOURCES}
    )
    target_include_directories(ExploreTests PRIVATE src tests)

    # so `make test` alone still refreshes clangd's view of tests/
    add_custom_command(TARGET ExploreTests POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_BINARY_DIR}/compile_commands.json"
            "${CMAKE_SOURCE_DIR}/")
endif ()
