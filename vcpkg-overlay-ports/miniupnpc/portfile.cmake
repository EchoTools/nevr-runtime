string(REPLACE "." "_" MINIUPNPC_VERSION "${VERSION}")
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO miniupnp/miniupnp
    REF "miniupnpc_${MINIUPNPC_VERSION}"
    SHA512 2dd60632a65f7114d942a6361b5ab5f2e98109141dbfb9627a45536b473f691d390e16f79ffee4f76a6a3d9cb2b15536f592b0647f846000c61a10acc36f0c1d
    HEAD_REF master
)

string(COMPARE EQUAL "${VCPKG_LIBRARY_LINKAGE}" "dynamic" MINIUPNPC_BUILD_SHARED)
string(COMPARE EQUAL "${VCPKG_LIBRARY_LINKAGE}" "static" MINIUPNPC_BUILD_STATIC)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}/miniupnpc"
    OPTIONS
    -DUPNPC_BUILD_STATIC=${MINIUPNPC_BUILD_STATIC}
    -DUPNPC_BUILD_SHARED=${MINIUPNPC_BUILD_SHARED}
    -DUPNPC_BUILD_TESTS=OFF
    -DUPNPC_BUILD_SAMPLE=OFF
)

vcpkg_cmake_install()
vcpkg_copy_pdbs()

vcpkg_cmake_config_fixup(CONFIG_PATH "/lib/cmake/${PORT}")
vcpkg_fixup_pkgconfig()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/share")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
if(VCPKG_LIBRARY_LINKAGE STREQUAL "static")
    file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/bin" "${CURRENT_PACKAGES_DIR}/debug/bin")
else()
    file(GLOB RELEASE_TOOLS "${CURRENT_PACKAGES_DIR}/bin/*.exe")
    if(${RELEASE_TOOLS})
        vcpkg_copy_tools(TOOL_NAMES ${RELEASE_TOOLS} AUTO_CLEAN)
    endif()
endif()
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
