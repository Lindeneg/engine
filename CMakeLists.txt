cmake_minimum_required(VERSION 3.15)
project(ExploreApp LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS true)

message("-- Toolchain file: " ${CMAKE_TOOLCHAIN_FILE})

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_definitions(CL_MEM_STATS=1)

add_executable(ExploreApp
        src/main.cpp
        src/logger.cpp
        src/memory.cpp
        src/file.cpp
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
