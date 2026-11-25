cmake_minimum_required(VERSION 3.20)
project(BtVSRecompUWP LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

file(GLOB GENERATED_CHUNKS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/generated/*.cpp")

add_executable(BtVSRecompUWP
    src/uwp_main.cpp
    src/x86_cpu.cpp
    src/d3d11_bridge.cpp
    src/xaudio2_bridge.cpp
    src/host_bridges.cpp
    src/uwp_config_ui.cpp
    ${GENERATED_CHUNKS}
)

target_include_directories(BtVSRecompUWP PRIVATE include src)
target_compile_definitions(BtVSRecompUWP PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
if(MSVC)
    target_compile_options(BtVSRecompUWP PRIVATE /MP /O2 /Ob2 /Oi /Ot /bigobj)
    target_link_options(BtVSRecompUWP PRIVATE /STACK:8388608)
endif()
target_link_libraries(BtVSRecompUWP PRIVATE d3d11 dxgi d3dcompiler xaudio2 mfplat mfreadwrite mfuuid ole32 user32)
