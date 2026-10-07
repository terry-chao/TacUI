# TacUI — Windows/MSVC only: the renderer is D3D12 and the text stack is
# DirectWrite, so the port declares `supports: windows` and vcpkg only asks it
# to build on the Windows triplets.

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO            terry-chao/TacUI
    REF             "v${VERSION}"
    SHA512          0   # <-- replace with the value vcpkg prints on the first run
    HEAD_REF        main
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        # A package build is a library build — the examples, `m0` and the
        # control gallery are validation harnesses, and none of them install.
        -DTACUI_BUILD_EXAMPLES=OFF
)

vcpkg_cmake_install()

# Merges the Debug and Release package files into one and moves them to
# share/tacui, which is where vcpkg expects them.
vcpkg_cmake_config_fixup(
    PACKAGE_NAME TacUI
    CONFIG_PATH  lib/cmake/TacUI
)

vcpkg_copy_pdbs()

# The sources are the same in both configurations; only one copy belongs in the
# packages dir.
file(REMOVE_RECURSE
    "${CURRENT_PACKAGES_DIR}/debug/include"
    "${CURRENT_PACKAGES_DIR}/debug/share"
)

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
