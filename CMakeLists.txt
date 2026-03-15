# Ixalance Playback Plugin
#
# Plays IXS (Impulse Tracker eXtendable Sequencer) files using
# webixs by Juergen Wothke, reverse-engineered from Shortcut Software's player.

cmake_minimum_required(VERSION 3.16)
project(playback_ixalance C CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(PlaybackPlugin)

add_subdirectory(external/zlib)

# Build ixalance as a static library (vendored webixs source)
add_library(ixalance_static STATIC
    src/asmEmu.cpp
    src/FileMap.cpp
    src/FileMapSFXI.cpp
    src/FileSFXI.cpp
    src/Mixer1.cpp
    src/Mixer2.cpp
    src/Mixer3.cpp
    src/Mixer4.cpp
    src/MixerBase.cpp
    src/Module.cpp
    src/Packer.cpp
    src/PlayerCore.cpp
    src/PlayerIXS.cpp
    src/WaveGen.cpp
    src/WaveLibrary.c
)
target_include_directories(ixalance_static PUBLIC src)
target_link_libraries(ixalance_static PUBLIC ZLIB::ZLIB)
# The webixs codebase uses LINUX to mean "modern non-Emscripten build"
target_compile_definitions(ixalance_static PUBLIC LINUX)
suppress_external_warnings(ixalance_static)
set_target_properties(ixalance_static PROPERTIES POSITION_INDEPENDENT_CODE ON CXX_STANDARD 14)
target_compile_options(ixalance_static PRIVATE -fno-strict-aliasing)

# Build plugin shared library
add_library(ixalance_playback SHARED ixalance_plugin.cpp)
target_include_directories(ixalance_playback PRIVATE src)
target_link_libraries(ixalance_playback PRIVATE ixalance_static ZLIB::ZLIB)
if(NOT WIN32)
    target_link_libraries(ixalance_playback PRIVATE stdc++ m)
endif()
set_target_properties(ixalance_playback PROPERTIES CXX_STANDARD 14)
rv_set_plugin_properties(ixalance_playback)
