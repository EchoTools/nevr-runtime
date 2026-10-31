# Arm's ASTC encoder, the core library only. Its own CMake installs the command-line tools and the shared library, never
# the static core library, so this port builds the static core and installs it and astcenc.h itself, with a small CMake
# config (unofficial-astcenc::astcenc).
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO ARM-software/astc-encoder
    REF "${VERSION}"
    SHA512 10d074a8bf81d6ce8f7948684423d925ee843f6b85444088a729ca5ca0be1c52968c9e930744eb1c8902a561f11820a6126260e111c6326b2e8ad94f8a6d7485
    HEAD_REF main
)

if(VCPKG_TARGET_ARCHITECTURE STREQUAL "arm64")
    set(ASTCENC_ISA "neon")
    set(ASTCENC_ISA_OPTION -DASTCENC_ISA_NEON=ON)
else()
    set(ASTCENC_ISA "none")
    set(ASTCENC_ISA_OPTION -DASTCENC_ISA_NONE=ON)
endif()

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        ${ASTCENC_ISA_OPTION}
        -DASTCENC_CLI=OFF
        -DASTCENC_SHAREDLIB=OFF
        -DASTCENC_UNITTEST=OFF
        -DASTCENC_UNIVERSAL_BUILD=OFF
)
vcpkg_cmake_build()

set(LIB_NAME "${VCPKG_TARGET_STATIC_LIBRARY_PREFIX}astcenc-${ASTCENC_ISA}-static${VCPKG_TARGET_STATIC_LIBRARY_SUFFIX}")
foreach(config IN ITEMS rel dbg)
    if(config STREQUAL "rel")
        set(DEST "${CURRENT_PACKAGES_DIR}/lib")
    else()
        if(DEFINED VCPKG_BUILD_TYPE AND VCPKG_BUILD_TYPE STREQUAL "release")
            continue()
        endif()
        set(DEST "${CURRENT_PACKAGES_DIR}/debug/lib")
    endif()
    file(GLOB_RECURSE BUILT "${CURRENT_BUILDTREES_DIR}/${TARGET_TRIPLET}-${config}/${LIB_NAME}")
    list(LENGTH BUILT FOUND)
    if(NOT FOUND EQUAL 1)
        message(FATAL_ERROR "astc-encoder: expected one ${LIB_NAME} in the ${config} build, found ${FOUND}")
    endif()
    file(INSTALL ${BUILT} DESTINATION "${DEST}" RENAME "${VCPKG_TARGET_STATIC_LIBRARY_PREFIX}astcenc${VCPKG_TARGET_STATIC_LIBRARY_SUFFIX}")
endforeach()
file(INSTALL "${SOURCE_PATH}/Source/astcenc.h" DESTINATION "${CURRENT_PACKAGES_DIR}/include")

file(WRITE "${CURRENT_PACKAGES_DIR}/share/unofficial-astcenc/unofficial-astcenc-config.cmake" [=[
get_filename_component(_astcenc_root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
if(NOT TARGET unofficial-astcenc::astcenc)
    add_library(unofficial-astcenc::astcenc STATIC IMPORTED)
    set_target_properties(unofficial-astcenc::astcenc PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${_astcenc_root}/include"
        IMPORTED_LOCATION "${_astcenc_root}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}astcenc${CMAKE_STATIC_LIBRARY_SUFFIX}")
    if(EXISTS "${_astcenc_root}/debug/lib/${CMAKE_STATIC_LIBRARY_PREFIX}astcenc${CMAKE_STATIC_LIBRARY_SUFFIX}")
        set_target_properties(unofficial-astcenc::astcenc PROPERTIES
            IMPORTED_LOCATION_DEBUG "${_astcenc_root}/debug/lib/${CMAKE_STATIC_LIBRARY_PREFIX}astcenc${CMAKE_STATIC_LIBRARY_SUFFIX}")
    endif()
endif()
unset(_astcenc_root)
]=])
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE.txt")
