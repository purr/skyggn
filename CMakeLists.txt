cmake_minimum_required(VERSION 3.28)

project(skyggn
    VERSION 0.1.0
    DESCRIPTION "explorer thumbnails for every video and audio format"
    LANGUAGES CXX)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "skyggn builds for 64-bit windows only")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# static c++ runtime: windows loads the engine into its own processes, so it must not depend on a
# visual c++ redistributable being installed.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# engine, ffmpeg and tools land in one folder, the same layout the installer ships.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_compile_options(/W4 /permissive- /utf-8 /external:W0)
add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)

include(cmake/dependencies.cmake)

add_subdirectory(engine)
add_subdirectory(ctl)
add_subdirectory(app)
add_subdirectory(installer)
