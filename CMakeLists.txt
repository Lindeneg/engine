cmake_minimum_required(VERSION 3.21)
project(ExploreApp LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS true)

message("-- Toolchain file: " ${CMAKE_TOOLCHAIN_FILE})

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(CL_BUILD_TESTS "Build the test runner" OFF)
option(CL_MEM_STATS "Log memory stats usage" OFF)

if (CL_MEM_STATS)
    add_compile_definitions(CL_MEM_STATS=1)
endif()

find_package(glfw3 CONFIG REQUIRED)
find_package(glad CONFIG REQUIRED)

set(ENGINE_SOURCES
        src/anim.cpp
        src/asset.cpp
        src/cgltf_impl.cpp
        src/config.cpp
        src/entity.cpp
        src/file.cpp
        src/gfx.cpp
        src/gltf.cpp
        src/logger.cpp
        src/memory.cpp
        src/path.cpp
        src/platform.cpp
        src/pool.cpp
        src/render.cpp
        src/result.cpp
        src/shader.cpp
        src/str.cpp
        src/strmap.cpp
        src/window.cpp
)

add_executable(ExploreApp
        src/main.cpp
        ${ENGINE_SOURCES}
)
target_include_directories(ExploreApp PRIVATE src third_party)
target_link_libraries(ExploreApp PRIVATE glfw glad::glad)

target_compile_definitions(ExploreApp PRIVATE CL_ASSET_ROOT="${CMAKE_SOURCE_DIR}/assets")

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
    target_include_directories(ExploreTests PRIVATE src tests third_party)
    target_link_libraries(ExploreTests PRIVATE glfw glad::glad)
    target_compile_definitions(ExploreTests PRIVATE CL_ASSET_ROOT="${CMAKE_SOURCE_DIR}/assets")

    # so `make test` alone still refreshes clangd's view of tests/
    add_custom_command(TARGET ExploreTests POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_BINARY_DIR}/compile_commands.json"
            "${CMAKE_SOURCE_DIR}/")
endif ()
