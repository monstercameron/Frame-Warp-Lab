cmake_minimum_required(VERSION 3.24)
project(FrameWarpLab VERSION 0.32.0 LANGUAGES C CXX)

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

set(STREAMLINE_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/streamline")
set(STREAMLINE_INCLUDE_DIR "${STREAMLINE_SDK_DIR}/include")
if(CMAKE_GENERATOR_PLATFORM MATCHES "ARM64" OR CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|aarch64")
    set(STREAMLINE_ARCH_DIR "arm64")
else()
    set(STREAMLINE_ARCH_DIR "x64")
endif()
set(STREAMLINE_LIB_FILE "${STREAMLINE_SDK_DIR}/lib/${STREAMLINE_ARCH_DIR}/sl.interposer.lib")
set(STREAMLINE_BIN_DIR "${STREAMLINE_SDK_DIR}/bin/${STREAMLINE_ARCH_DIR}")
set(FRAME_WARP_HAS_STREAMLINE OFF)
if(EXISTS "${STREAMLINE_INCLUDE_DIR}/sl.h" AND EXISTS "${STREAMLINE_LIB_FILE}" AND EXISTS "${STREAMLINE_BIN_DIR}/sl.interposer.dll")
    set(FRAME_WARP_HAS_STREAMLINE ON)
    message(STATUS "NVIDIA Streamline SDK found (${STREAMLINE_ARCH_DIR}): ${STREAMLINE_SDK_DIR}")
else()
    message(STATUS "NVIDIA Streamline SDK not found in third_party/streamline; building without DLSS-G backend")
endif()

set(USE_STATIC_MSVC_RUNTIME_LIBRARY OFF CACHE BOOL "" FORCE)
set(INTERPROCEDURAL_OPTIMIZATION OFF CACHE BOOL "" FORCE)
set(OVERRIDE_CXX_FLAGS OFF CACHE BOOL "" FORCE)
set(ENABLE_ALL_WARNINGS OFF CACHE BOOL "" FORCE)
set(FLOATING_POINT_EXCEPTIONS_ENABLED OFF CACHE BOOL "" FORCE)
set(CPP_EXCEPTIONS_ENABLED ON CACHE BOOL "" FORCE)
set(DEBUG_RENDERER_IN_DEBUG_AND_RELEASE OFF CACHE BOOL "" FORCE)
set(PROFILER_IN_DEBUG_AND_RELEASE OFF CACHE BOOL "" FORCE)
set(ENABLE_OBJECT_STREAM OFF CACHE BOOL "" FORCE)
set(GENERATE_DEBUG_SYMBOLS OFF CACHE BOOL "" FORCE)
set(JPH_USE_DX12 OFF CACHE BOOL "" FORCE)
set(JPH_USE_VK OFF CACHE BOOL "" FORCE)
set(JPH_USE_MTL OFF CACHE BOOL "" FORCE)
set(JPH_USE_CPU_COMPUTE OFF CACHE BOOL "" FORCE)
set(TARGET_UNIT_TESTS OFF CACHE BOOL "" FORCE)
set(TARGET_HELLO_WORLD OFF CACHE BOOL "" FORCE)
set(TARGET_PERFORMANCE_TEST OFF CACHE BOOL "" FORCE)
set(TARGET_SAMPLES OFF CACHE BOOL "" FORCE)
set(TARGET_VIEWER OFF CACHE BOOL "" FORCE)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/JoltPhysics/Build/CMakeLists.txt")
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/third_party/JoltPhysics/Build" "${CMAKE_CURRENT_BINARY_DIR}/jolt_build" EXCLUDE_FROM_ALL)
else()
    include(FetchContent)
    FetchContent_Declare(
        JoltPhysics
        GIT_REPOSITORY https://github.com/jrouwe/JoltPhysics.git
        GIT_TAG v5.7.0
        GIT_SHALLOW TRUE
        SOURCE_SUBDIR Build
    )
    FetchContent_MakeAvailable(JoltPhysics)
endif()

file(GLOB LUA54_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/lua-5.4.7/src/*.c")
list(FILTER LUA54_SOURCES EXCLUDE REGEX "(lua|luac|onelua)\\.c$")
add_library(lua54 STATIC ${LUA54_SOURCES})
target_include_directories(lua54 PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/third_party/lua-5.4.7/src")
if(MSVC)
    target_compile_options(lua54 PRIVATE /W0)
    target_compile_definitions(lua54 PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

add_executable(frame_warp_lab WIN32
    src/main.cpp
    src/renderer.cpp
    src/pbr_assets.cpp
    src/job_system.cpp
    src/gltf_loader.cpp
    src/ecs.cpp
    src/event_bus.cpp
    src/lua_engine.cpp
    src/tactical_range.cpp
    src/range_mesh.cpp
    src/latency_monitor.cpp
    src/streamline_backend.cpp
)
add_executable(frame_warp_cli
    src/cli.cpp
    src/streamline_backend.cpp
)
target_compile_features(frame_warp_lab PRIVATE cxx_std_20)
target_compile_features(frame_warp_cli PRIVATE cxx_std_20)
target_include_directories(frame_warp_lab SYSTEM PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/asset_loaders"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/lua-5.4.7/src"
)
target_compile_definitions(frame_warp_lab PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_definitions(frame_warp_cli PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

if(FRAME_WARP_HAS_STREAMLINE)
    target_compile_definitions(frame_warp_lab PRIVATE FRAME_WARP_HAS_STREAMLINE=1)
    target_compile_definitions(frame_warp_cli PRIVATE FRAME_WARP_HAS_STREAMLINE=1)
    target_include_directories(frame_warp_lab SYSTEM PRIVATE "${STREAMLINE_INCLUDE_DIR}")
    target_include_directories(frame_warp_cli SYSTEM PRIVATE "${STREAMLINE_INCLUDE_DIR}")
    target_link_libraries(frame_warp_lab PRIVATE Jolt lua54 "${STREAMLINE_LIB_FILE}" dxguid advapi32 dwmapi)
    target_link_libraries(frame_warp_cli PRIVATE "${STREAMLINE_LIB_FILE}" dxguid)
else()
    target_compile_definitions(frame_warp_lab PRIVATE FRAME_WARP_HAS_STREAMLINE=0)
    target_compile_definitions(frame_warp_cli PRIVATE FRAME_WARP_HAS_STREAMLINE=0)
    target_link_libraries(frame_warp_lab PRIVATE Jolt lua54 d3d12 dxgi dxguid advapi32 dwmapi)
    target_link_libraries(frame_warp_cli PRIVATE d3d12 dxgi dxguid)
endif()

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
        COMMAND "${DXC_EXECUTABLE}" -nologo -T ${PROFILE} -E ${ENTRY_POINT} -I "${CMAKE_CURRENT_SOURCE_DIR}/shaders" -O3 -WX ${EXTRA_DEFINES} -Fo "${OUTPUT_PATH}" "${SOURCE_FILE}"
        DEPENDS "${SOURCE_FILE}" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/pbr_materials.hlsl" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/range_lighting.hlsl"
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
add_dxc_shader(COMPILED_SHADERS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/present.hlsl" pixel_main ps_6_0 present_dlss_inputs_ps.cso -DPRESENT_VARIANT=4 -DPRESENT_OUTPUT_DLSS_INPUTS=1)
add_dxc_shader(COMPILED_SHADERS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/selective_tiles.hlsl" vis_classify_main cs_6_0 tile_vis_classify_cs.cso)
add_dxc_shader(COMPILED_SHADERS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/selective_tiles.hlsl" tile_admit_main cs_6_0 tile_admit_cs.cso)
add_dxc_shader(COMPILED_SHADERS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/selective_tiles.hlsl" tile_shade_main cs_6_0 tile_shade_indirect_cs.cso)
add_dxc_shader(COMPILED_SHADERS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/range_mesh.hlsl" range_vs_main vs_6_0 range_vs.cso)
add_dxc_shader(COMPILED_SHADERS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/range_mesh.hlsl" range_ps_main ps_6_0 range_ps.cso)
add_dxc_shader(COMPILED_SHADERS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/range_mesh.hlsl" range_vis_ps_main ps_6_0 range_vis_ps.cso)
add_dxc_shader(COMPILED_SHADERS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/range_vis_par.hlsl" range_vis_classify_main cs_6_0 range_vis_classify_cs.cso)
add_dxc_shader(COMPILED_SHADERS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/range_vis_par.hlsl" range_vis_admit_main cs_6_0 range_vis_admit_cs.cso)
add_dxc_shader(COMPILED_SHADERS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/range_vis_par.hlsl" range_vis_shade_main cs_6_0 range_vis_shade_cs.cso)

add_custom_target(frame_warp_shaders ALL DEPENDS ${COMPILED_SHADERS})
add_dependencies(frame_warp_lab frame_warp_shaders)

add_custom_command(TARGET frame_warp_lab POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:frame_warp_lab>/shaders"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${COMPILED_SHADERS} "$<TARGET_FILE_DIR:frame_warp_lab>/shaders/"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/assets" "$<TARGET_FILE_DIR:frame_warp_lab>/assets"
)

if(FRAME_WARP_HAS_STREAMLINE)
    set(STREAMLINE_RUNTIME_DLLS
        "${STREAMLINE_BIN_DIR}/sl.interposer.dll"
        "${STREAMLINE_BIN_DIR}/sl.common.dll"
        "${STREAMLINE_BIN_DIR}/sl.pcl.dll"
        "${STREAMLINE_BIN_DIR}/sl.reflex.dll"
        "${STREAMLINE_BIN_DIR}/sl.dlss.dll"
        "${STREAMLINE_BIN_DIR}/sl.dlss_g.dll"
        "${STREAMLINE_BIN_DIR}/nvngx_dlss.dll"
        "${STREAMLINE_BIN_DIR}/nvngx_dlssg.dll"
    )
    add_custom_command(TARGET frame_warp_lab POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${STREAMLINE_RUNTIME_DLLS} "$<TARGET_FILE_DIR:frame_warp_lab>/"
    )
    add_custom_command(TARGET frame_warp_cli POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${STREAMLINE_RUNTIME_DLLS} "$<TARGET_FILE_DIR:frame_warp_cli>/"
    )
endif()
