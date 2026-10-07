# TacUI — Windows/MSVC/x64 only: the renderer is D3D12 and the text stack is
# DirectWrite, so the port declares `supports: windows & x64 & !uwp` and vcpkg
# only asks it to build on the x64 Windows triplets.

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO            terry-chao/TacUI
    REF             "v${VERSION}"
    SHA512          a4a385a7212c3dd3248a7cb56d195e8fbef0bb707d0001a23f648ab424b5bef9710e07b4081749495813fef74614aa5d7e243a6d35f9cab01175e25c89eb0651
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
