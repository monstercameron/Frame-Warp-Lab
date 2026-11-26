cmake_minimum_required(VERSION 3.24)
project(FrameWarpLab VERSION 0.1.2 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "Frame Warp Lab currently requires Windows and Direct3D 12.")
endif()

add_executable(frame_warp_lab WIN32 src/main.cpp)
target_compile_features(frame_warp_lab PRIVATE cxx_std_20)
target_compile_definitions(frame_warp_lab PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(frame_warp_lab PRIVATE d3d12 dxgi dxguid)
if(MSVC)
    target_compile_options(frame_warp_lab PRIVATE /W4 /permissive- /EHsc)
endif()
