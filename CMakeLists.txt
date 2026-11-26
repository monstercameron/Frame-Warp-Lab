cmake_minimum_required(VERSION 3.24)
project(FrameWarpLab VERSION 0.4.0 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "Frame Warp Lab currently requires Windows and Direct3D 12.")
endif()

add_executable(frame_warp_lab WIN32 src/main.cpp)
add_executable(frame_warp_cli src/cli.cpp)
target_compile_features(frame_warp_lab PRIVATE cxx_std_20)
target_compile_features(frame_warp_cli PRIVATE cxx_std_20)
target_compile_definitions(frame_warp_lab PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_definitions(frame_warp_cli PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(frame_warp_lab PRIVATE d3d12 dxgi dxguid d3dcompiler)
target_link_libraries(frame_warp_cli PRIVATE d3d12 dxgi dxguid)
if(MSVC)
    target_compile_options(frame_warp_lab PRIVATE /W4 /permissive- /EHsc)
    target_compile_options(frame_warp_cli PRIVATE /W4 /permissive- /EHsc)
endif()

add_custom_command(TARGET frame_warp_lab POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:frame_warp_lab>/shaders"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/motion_lab.hlsl"
        "$<TARGET_FILE_DIR:frame_warp_lab>/shaders/motion_lab.hlsl")
set_property(TARGET frame_warp_lab APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/motion_lab.hlsl")
