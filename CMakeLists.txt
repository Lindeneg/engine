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
        src/core/config.cpp
        src/core/frametime.cpp
        src/core/logger.cpp
        src/core/memory.cpp
        src/core/pool.cpp
        src/core/result.cpp
        src/core/str.cpp
        src/core/strmap.cpp

        src/math/intersect.cpp

        src/platform/file.cpp
        src/platform/input.cpp
        src/platform/path.cpp
        src/platform/platform.cpp
        src/platform/window.cpp

        src/gfx/canvas.cpp
        src/gfx/debugdraw.cpp
        src/gfx/drawkey.cpp
        src/gfx/font.cpp
        src/gfx/gfx.cpp
        src/gfx/render.cpp
        src/gfx/shader.cpp
        src/gfx/stb_truetype_impl.cpp
        src/gfx/texture.cpp

        src/ui/textedit.cpp
        src/ui/ui.cpp

        src/asset/anim.cpp
        src/asset/asset.cpp
        src/asset/cgltf_impl.cpp
        src/asset/gltf.cpp

        src/scene/entity.cpp
        src/scene/motion.cpp
)

# Not the engine: one game's decisions, built alongside it until there is a
# scripting language to move them into. Nothing in ENGINE_SOURCES may include
# these, and the test runner does not link them.
set(GAME_SOURCES
        src/game/player.cpp
)

add_executable(ExploreApp
        src/main.cpp
        ${ENGINE_SOURCES}
        ${GAME_SOURCES}
)
target_include_directories(ExploreApp PRIVATE src third_party)
target_link_libraries(ExploreApp PRIVATE glfw glad::glad)

target_compile_definitions(ExploreApp PRIVATE CL_ASSET_ROOT="${CMAKE_SOURCE_DIR}/assets")

add_custom_command(TARGET ExploreApp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_BINARY_DIR}/compile_commands.json"
        "${CMAKE_SOURCE_DIR}/")

if (CL_BUILD_TESTS)
    # tests/ mirrors src/: the runner and framework at the root, one folder per src/ folder
    file(GLOB TEST_SOURCES CONFIGURE_DEPENDS
            ${CMAKE_SOURCE_DIR}/tests/*.cpp
            ${CMAKE_SOURCE_DIR}/tests/*/*.cpp)

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
