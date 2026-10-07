cmake_minimum_required(VERSION 3.25)
project(TacUI VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "M0 targets Windows + MSVC only (D3D12 backend).")
endif()

include(GNUInstallDirs)

# ---------------------------------------------------------------------------
# Options
#
# The library is the product; the apps are how we validate it. A package build
# wants neither, and neither does a project that embeds TacUI through
# add_subdirectory() — so both default to "yes only when we are the top level".
# ---------------------------------------------------------------------------
option(TACUI_BUILD_EXAMPLES "Build the examples, the validation harnesses and the control gallery" ${PROJECT_IS_TOP_LEVEL})
option(TACUI_INSTALL        "Generate install rules and the TacUI CMake package"                    ${PROJECT_IS_TOP_LEVEL})

# The C ABI is the surface host languages load, and bindings/python looks for a
# DLL — so a plain build produces tacui.dll. vcpkg always defines
# BUILD_SHARED_LIBS from the triplet, which is how the static triplets get a
# static library instead.
if(NOT DEFINED BUILD_SHARED_LIBS)
    set(BUILD_SHARED_LIBS ON)
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
    $<BUILD_INTERFACE:${TACUI_PUBLIC_INCLUDE}>
    $<BUILD_INTERFACE:${TACUI_INCLUDE_ROOT}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_compile_definitions(tacui_flags INTERFACE
    UNICODE _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)
# Usage requirements only. These reach every downstream target once the package
# is installed, so nothing here may decide how the *consumer* optimises —
# per-config /Zi /Od /O2 belong to the build, not to the interface.
target_compile_options(tacui_flags INTERFACE
    /W4 /permissive- /Zc:__cplusplus /EHsc /utf-8 /MP
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
#
# dxc ships inside the Windows SDK, but not always at the path the glob looks
# at — CI images and vcpkg's build machines differ. Hence the override:
#     -DTACUI_DXC_EXECUTABLE=C:/path/to/dxc.exe
# ---------------------------------------------------------------------------
set(TACUI_DXC_EXECUTABLE "" CACHE FILEPATH
    "dxc.exe used to compile the shaders (default: newest in the Windows SDK)")

if(TACUI_DXC_EXECUTABLE)
    set(DXC_EXECUTABLE "${TACUI_DXC_EXECUTABLE}")
else()
    file(GLOB _dxc_candidates "C:/Program Files (x86)/Windows Kits/10/bin/*/x64/dxc.exe")
    if(NOT _dxc_candidates)
        message(FATAL_ERROR
            "dxc.exe not found in the Windows SDK; install the SDK's HLSL compiler "
            "or pass -DTACUI_DXC_EXECUTABLE=<path>.")
    endif()
    list(SORT _dxc_candidates)
    list(GET _dxc_candidates -1 DXC_EXECUTABLE)
endif()
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
add_library(tacui
    capi/tacui.cpp
)
# TUI_API is __declspec(dllexport) under TUI_BUILD_DLL and plain otherwise.
# Defining it for a static library would be a lie the linker happily believes.
if(BUILD_SHARED_LIBS)
    target_compile_definitions(tacui PRIVATE TUI_BUILD_DLL)
endif()
# The C ABI is a C consumer's entire surface, and C consumers link this target
# rather than tacui_host — so it has to carry the public include directory
# itself. Deliberately not tacui_flags: /EHsc, /permissive- and the rest are
# C++ build settings and have no business reaching a C or a host-language
# consumer.
target_include_directories(tacui PUBLIC
    $<BUILD_INTERFACE:${TACUI_PUBLIC_INCLUDE}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(tacui PRIVATE tacui_host)

# Keep the import library stable and put the DLL where hosts look for it.
# CMAKE_CURRENT_BINARY_DIR, not CMAKE_BINARY_DIR: as a subproject the latter is
# the *consumer's* build root, and dropping a DLL into someone else's bin/ is
# how you get name collisions and confusing artifacts.
set_target_properties(tacui PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY         ${CMAKE_CURRENT_BINARY_DIR}/bin
    RUNTIME_OUTPUT_DIRECTORY_DEBUG   ${CMAKE_CURRENT_BINARY_DIR}/bin
    RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_CURRENT_BINARY_DIR}/bin
    LIBRARY_OUTPUT_DIRECTORY         ${CMAKE_CURRENT_BINARY_DIR}/bin
    ARCHIVE_OUTPUT_DIRECTORY         ${CMAKE_CURRENT_BINARY_DIR}/bin
)

# ---------------------------------------------------------------------------
# Applications — none of these are installed. A package build is a library
# build; the harnesses exist to validate the library, not to ship with it.
# ---------------------------------------------------------------------------
if(TACUI_BUILD_EXAMPLES)

    # -----------------------------------------------------------------------
    # M0 application — architecture validation harness
    # -----------------------------------------------------------------------
    add_executable(m0 apps/m0/main.cpp)
    # Built against the public entry point only, so the harness doubles as a
    # test that embedding via <tacui/tacui.hpp> is sufficient on its own.
    target_link_libraries(m0 PRIVATE tacui_host)

    # Text-stack probe: verifies shaping + rasterisation with no GPU involved.
    add_executable(textprobe apps/textprobe/main.cpp)
    target_link_libraries(textprobe PRIVATE tacui_text)

    # -----------------------------------------------------------------------
    # Examples
    # -----------------------------------------------------------------------
    add_executable(asset_browser examples/asset_browser/main.cpp)
    target_link_libraries(asset_browser PRIVATE tacui_host)

    # Control gallery — every L2 control in one window, and the headless
    # input harness that drives them without a window or a GPU.
    add_executable(controls_gallery examples/controls_gallery/main.cpp)
    target_link_libraries(controls_gallery PRIVATE tacui_host)

    set(_tacui_examples m0 asset_browser controls_gallery)
    set_target_properties(${_tacui_examples} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY         ${CMAKE_CURRENT_BINARY_DIR}/bin
        RUNTIME_OUTPUT_DIRECTORY_DEBUG   ${CMAKE_CURRENT_BINARY_DIR}/bin
        RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_CURRENT_BINARY_DIR}/bin
    )

endif()

# ---------------------------------------------------------------------------
# Namespaced aliases
#
# find_package(TacUI CONFIG) hands out TacUI::tacui_host and friends — the
# export set's NAMESPACE supplies that prefix. FetchContent and
# add_subdirectory see the raw target names instead, which would make the link
# line depend on how TacUI arrived. These aliases close the gap, so a
# consumer writes TacUI::tacui_host either way.
# ---------------------------------------------------------------------------
add_library(TacUI::tacui_flags    ALIAS tacui_flags)
add_library(TacUI::tacui_text     ALIAS tacui_text)
add_library(TacUI::tacui_platform ALIAS tacui_platform)
add_library(TacUI::tacui_rhi      ALIAS tacui_rhi)
add_library(TacUI::tacui_core     ALIAS tacui_core)
add_library(TacUI::tacui_tools    ALIAS tacui_tools)
add_library(TacUI::tacui_host     ALIAS tacui_host)
add_library(TacUI::tacui          ALIAS tacui)

# ---------------------------------------------------------------------------
# Install & CMake package
#
# This is what makes the library consumable at all: `cmake --install` has to
# leave behind the headers, the binaries and a TacUIConfig.cmake, or neither
# vcpkg nor a plain find_package(TacUI CONFIG) user can see it. Every target
# except the applications goes in one export set, because a consumer that
# links TacUI::tacui_host needs TacUI::tacui_core and friends to resolve.
# ---------------------------------------------------------------------------
if(TACUI_INSTALL)

    include(CMakePackageConfigHelpers)

    set(TACUI_INSTALL_CMAKEDIR ${CMAKE_INSTALL_LIBDIR}/cmake/TacUI
        CACHE STRING "Install destination for the TacUI CMake package files")

    # tacui_flags is an INTERFACE library: its destinations are ignored, only
    # the export matters.
    install(TARGETS tacui_flags tacui_text tacui_platform tacui_rhi tacui_core
                    tacui_tools tacui_host tacui
            EXPORT  TacUITargets
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})

    install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/TacUI)

    install(EXPORT TacUITargets
            NAMESPACE   TacUI::
            FILE        TacUITargets.cmake
            DESTINATION ${TACUI_INSTALL_CMAKEDIR})

    configure_package_config_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/TacUIConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/TacUIConfig.cmake
        INSTALL_DESTINATION ${TACUI_INSTALL_CMAKEDIR})

    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/TacUIConfigVersion.cmake
        VERSION       ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion)

    install(FILES
                ${CMAKE_CURRENT_BINARY_DIR}/TacUIConfig.cmake
                ${CMAKE_CURRENT_BINARY_DIR}/TacUIConfigVersion.cmake
            DESTINATION ${TACUI_INSTALL_CMAKEDIR})

endif()
