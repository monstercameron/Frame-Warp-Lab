cmake_minimum_required(VERSION 3.24)
project(FrameWarpLab VERSION 0.16.0 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "Frame Warp Lab currently requires Windows and Direct3D 12.")
endif()

find_program(DXC_EXECUTABLE
    NAMES dxc dxc.exe
    HINTS
        "C:/Program Files (x86)/Windows Kits/10/bin/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/arm64"
        "C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/arm64"
        "C:/Program Files (x86)/Windows Kits/10/bin/10.0.22621.0/arm64"
        "C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64"
)
if(NOT DXC_EXECUTABLE)
    message(FATAL_ERROR "Could not locate Windows SDK dxc.exe for Shader Model 6.0 compilation.")
endif()

add_executable(frame_warp_lab WIN32
    src/main.cpp
    src/renderer.cpp
    src/latency_monitor.cpp
)
add_executable(frame_warp_cli src/cli.cpp)
target_compile_features(frame_warp_lab PRIVATE cxx_std_20)
target_compile_features(frame_warp_cli PRIVATE cxx_std_20)
target_compile_definitions(frame_warp_lab PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_definitions(frame_warp_cli PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(frame_warp_lab PRIVATE d3d12 dxgi dxguid advapi32 dwmapi)
target_link_libraries(frame_warp_cli PRIVATE d3d12 dxgi dxguid)
if(MSVC)
    target_compile_options(frame_warp_lab PRIVATE /W4 /permissive- /EHsc)
    target_compile_options(frame_warp_cli PRIVATE /W4 /permissive- /EHsc)
endif()

set(SHADER_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/compiled_shaders")
file(MAKE_DIRECTORY "${SHADER_OUTPUT_DIR}")

function(add_dxc_shader OUT_VAR SOURCE_FILE ENTRY_POINT PROFILE OUTPUT_NAME)
    set(EXTRA_DEFINES ${ARGN})
    set(OUTPUT_PATH "${SHADER_OUTPUT_DIR}/${OUTPUT_NAME}")
    add_custom_command(
        OUTPUT "${OUTPUT_PATH}"
        COMMAND "${DXC_EXECUTABLE}" -nologo -T ${PROFILE} -E ${ENTRY_POINT} -O3 -WX ${EXTRA_DEFINES} -Fo "${OUTPUT_PATH}" "${SOURCE_FILE}"
        DEPENDS "${SOURCE_FILE}"
        COMMENT "Compiling ${OUTPUT_NAME} (${PROFILE})"
        VERBATIM
    )
    list(APPEND ${OUT_VAR} "${OUTPUT_PATH}")
    set(${OUT_VAR} "${${OUT_VAR}}" PARENT_SCOPE)
endfunction()

set(COMPILED_SHADERS)
add_dxc_shader(COMPILED_SHADERS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/motion_lab.hlsl" vertex_main vs_6_0 scene_vs.cso)
add_dxc_shader(COMPILED_SHADERS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/motion_lab.hlsl" pixel_main ps_6_0 scene_ps.cso)
add_dxc_shader(COMPILED_SHADERS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/present.hlsl" vertex_main vs_6_0 present_vs.cso)
add_dxc_shader(COMPILED_SHADERS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/present.hlsl" pixel_main ps_6_0 present_simple_ps.cso -DPRESENT_VARIANT=0)
add_dxc_shader(COMPILED_SHADERS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/present.hlsl" pixel_main ps_6_0 present_rot_warp_ps.cso -DPRESENT_VARIANT=1)
add_dxc_shader(COMPILED_SHADERS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/present.hlsl" pixel_main ps_6_0 present_depth_fast_ps.cso -DPRESENT_VARIANT=2)
add_dxc_shader(COMPILED_SHADERS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/present.hlsl" pixel_main ps_6_0 present_depth_full_ps.cso -DPRESENT_VARIANT=3)
add_dxc_shader(COMPILED_SHADERS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/present.hlsl" pixel_main ps_6_0 present_synthesis_ps.cso -DPRESENT_VARIANT=4)

add_custom_target(frame_warp_shaders ALL DEPENDS ${COMPILED_SHADERS})
add_dependencies(frame_warp_lab frame_warp_shaders)

add_custom_command(TARGET frame_warp_lab POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:frame_warp_lab>/shaders"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${COMPILED_SHADERS} "$<TARGET_FILE_DIR:frame_warp_lab>/shaders/"
)
