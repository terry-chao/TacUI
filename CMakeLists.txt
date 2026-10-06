cmake_minimum_required(VERSION 3.25)
project(TacUI LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "M0 targets Windows + MSVC only (D3D12 backend).")
endif()

# Two include roots:
#   include/   the public API — "#include <tacui/tacui.hpp>" is the whole surface
#   <root>/    internal headers, referenced by their path from implementation
#              files only ("rhi/d3d12/d3d12_device.h")
set(TACUI_PUBLIC_INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/include)
set(TACUI_INCLUDE_ROOT   ${CMAKE_CURRENT_SOURCE_DIR})

# ---------------------------------------------------------------------------
# Shared compile settings
# ---------------------------------------------------------------------------
add_library(tacui_flags INTERFACE)
target_include_directories(tacui_flags INTERFACE
    ${TACUI_PUBLIC_INCLUDE}
    ${TACUI_INCLUDE_ROOT}
)
target_compile_definitions(tacui_flags INTERFACE
    UNICODE _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)
target_compile_options(tacui_flags INTERFACE
    /W4 /permissive- /Zc:__cplusplus /EHsc /utf-8 /MP
    $<$<CONFIG:Debug>:/Zi /Od>
    $<$<CONFIG:Release>:/O2>
)

# ---------------------------------------------------------------------------
# Framework layer — the retained tree and the rebuild/reconcile/paint cycle
# ---------------------------------------------------------------------------
add_library(tacui_core STATIC
    core/framework/vnode.cpp
    core/framework/element.cpp
    core/framework/ui.cpp
    core/framework/builder.cpp
    core/theme/theme.cpp
    core/controls/controls.cpp
)
target_link_libraries(tacui_core PUBLIC tacui_flags tacui_rhi tacui_text)

# ---------------------------------------------------------------------------
# Text layer — shaping + glyph rasterisation.
#
# M0 uses DirectWrite for the whole stack; HarfBuzz/ICU are the plan's target
# and slot in behind the same interface in M1 (architecture.md §3.4).
# ---------------------------------------------------------------------------
add_library(tacui_text STATIC
    core/text/text_system.cpp
    core/text/glyph_atlas.cpp
)
target_link_libraries(tacui_text PUBLIC tacui_flags dwrite)

# ---------------------------------------------------------------------------
# Platform layer
# ---------------------------------------------------------------------------
add_library(tacui_platform STATIC
    platform/windows/win32_window.cpp
)
target_link_libraries(tacui_platform PUBLIC tacui_flags)

# ---------------------------------------------------------------------------
# Shaders: compiled to DXIL headers at build time by dxc (-Fh).
# ---------------------------------------------------------------------------
file(GLOB _dxc_candidates "C:/Program Files (x86)/Windows Kits/10/bin/*/x64/dxc.exe")
if(NOT _dxc_candidates)
    message(FATAL_ERROR "dxc.exe not found in the Windows SDK; install the SDK's HLSL compiler.")
endif()
list(SORT _dxc_candidates)
list(GET _dxc_candidates -1 DXC_EXECUTABLE)
message(STATUS "dxc: ${DXC_EXECUTABLE}")

set(SHADER_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/core/renderer/shaders)
set(SHADER_OUT_DIR ${CMAKE_BINARY_DIR}/generated/shaders)

function(tacui_shader SOURCE NAME ENTRY PROFILE)
    add_custom_command(
        OUTPUT  ${SHADER_OUT_DIR}/${NAME}.h
        COMMAND ${CMAKE_COMMAND} -E make_directory ${SHADER_OUT_DIR}
        COMMAND "${DXC_EXECUTABLE}" -T ${PROFILE} -E ${ENTRY}
                -Fh ${SHADER_OUT_DIR}/${NAME}.h
                ${SHADER_SRC_DIR}/${SOURCE}.hlsl
        DEPENDS ${SHADER_SRC_DIR}/${SOURCE}.hlsl
        COMMENT "dxc ${NAME} (${PROFILE})"
        VERBATIM
    )
endfunction()

tacui_shader(sdf_rect sdf_rect_vs VSMain vs_6_0)
tacui_shader(sdf_rect sdf_rect_ps PSMain ps_6_0)
tacui_shader(glyph    glyph_vs    VSMain vs_6_0)
tacui_shader(glyph    glyph_ps    PSMain ps_6_0)

add_custom_target(tacui_shaders DEPENDS
    ${SHADER_OUT_DIR}/sdf_rect_vs.h
    ${SHADER_OUT_DIR}/sdf_rect_ps.h
    ${SHADER_OUT_DIR}/glyph_vs.h
    ${SHADER_OUT_DIR}/glyph_ps.h
)

# ---------------------------------------------------------------------------
# RHI layer (backend-agnostic interface + D3D12 implementation)
# ---------------------------------------------------------------------------
add_library(tacui_rhi STATIC
    rhi/d3d12/d3d12_device.cpp
    rhi/d3d12/d3d12_sdf_batch.cpp
    rhi/d3d12/d3d12_text_batch.cpp
)
add_dependencies(tacui_rhi tacui_shaders)
target_include_directories(tacui_rhi PRIVATE ${SHADER_OUT_DIR})
target_link_libraries(tacui_rhi PUBLIC
    tacui_flags
    d3d12 dxgi dxguid
)

# ---------------------------------------------------------------------------
# Host — the ready-made Win32 + D3D12 shell. The core stays embeddable; this is
# the convenience path the examples use (host/host.h).
# ---------------------------------------------------------------------------
add_library(tacui_host STATIC
    host/host.cpp
)
target_link_libraries(tacui_host PUBLIC
    tacui_core tacui_platform tacui_rhi tacui_text tacui_tools
)

# ---------------------------------------------------------------------------
# Tools (image output, later the golden-image harness)
# ---------------------------------------------------------------------------
add_library(tacui_tools STATIC
    tools/image_writer.cpp
)
target_link_libraries(tacui_tools PUBLIC tacui_flags windowscodecs)

# ---------------------------------------------------------------------------
# C ABI — the only cross-language boundary (plan.md D7).
#
# Built as a DLL so host languages can load it directly (Python/ctypes today,
# Swift and friends via plain C linkage later).
# ---------------------------------------------------------------------------
add_library(tacui SHARED
    capi/tacui.cpp
)
target_compile_definitions(tacui PRIVATE TUI_BUILD_DLL)
target_link_libraries(tacui PRIVATE tacui_host)

# Keep the import library stable and put the DLL where hosts look for it.
set_target_properties(tacui PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY         ${CMAKE_BINARY_DIR}/bin
    RUNTIME_OUTPUT_DIRECTORY_DEBUG   ${CMAKE_BINARY_DIR}/bin
    RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/bin
    LIBRARY_OUTPUT_DIRECTORY         ${CMAKE_BINARY_DIR}/bin
    ARCHIVE_OUTPUT_DIRECTORY         ${CMAKE_BINARY_DIR}/bin
)

# ---------------------------------------------------------------------------
# M0 application — architecture validation harness
# ---------------------------------------------------------------------------
add_executable(m0 apps/m0/main.cpp)
# Built against the public entry point only, so the harness doubles as a test
# that embedding via <tacui/tacui.hpp> is sufficient on its own.
target_link_libraries(m0 PRIVATE tacui_host)

# Text-stack probe: verifies shaping + rasterisation with no GPU involved.
add_executable(textprobe apps/textprobe/main.cpp)
target_link_libraries(textprobe PRIVATE tacui_text)

# ---------------------------------------------------------------------------
# Examples
# ---------------------------------------------------------------------------
add_executable(asset_browser examples/asset_browser/main.cpp)
target_link_libraries(asset_browser PRIVATE tacui_host)

# Control gallery — every L2 control in one window, and the headless
# input harness that drives them without a window or a GPU.
add_executable(controls_gallery examples/controls_gallery/main.cpp)
target_link_libraries(controls_gallery PRIVATE tacui_host)

set_target_properties(controls_gallery PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY         ${CMAKE_BINARY_DIR}/bin
    RUNTIME_OUTPUT_DIRECTORY_DEBUG   ${CMAKE_BINARY_DIR}/bin
    RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/bin
)
set_target_properties(asset_browser PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY         ${CMAKE_BINARY_DIR}/bin
    RUNTIME_OUTPUT_DIRECTORY_DEBUG   ${CMAKE_BINARY_DIR}/bin
    RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/bin
)
set_target_properties(m0 PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY         ${CMAKE_BINARY_DIR}/bin
    RUNTIME_OUTPUT_DIRECTORY_DEBUG   ${CMAKE_BINARY_DIR}/bin
    RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/bin
)
