# Custom x64-windows triplet for cross-compilation from Linux
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)
set(VCPKG_CMAKE_SYSTEM_NAME Windows)

# Use our MSVC Wine toolchain for cross-compilation
set(VCPKG_CHAINLOAD_TOOLCHAIN_FILE "${CMAKE_CURRENT_LIST_DIR}/toolchain-msvc-wine.cmake")

# Configuration - only build release to speed things up
set(VCPKG_BUILD_TYPE release)
