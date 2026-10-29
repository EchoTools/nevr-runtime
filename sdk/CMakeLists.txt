# nevr_plugin_sdk: the plugin SDK, published. The plugin C ABI (nevr/plugin_interface.h) and the plugin-side helpers.
#
# Header-only, and it names nothing outside sdk/: no ${CMAKE_SOURCE_DIR}/src, no vcpkg package. Two ways in:
#   - configured on its own (cmake -S sdk), `cmake --install` puts the headers and the NevrPluginSdk package under a
#     prefix, for find_package(NevrPluginSdk <api> CONFIG);
#   - added as a subdirectory (this repository's build, src/quest, or FetchContent with SOURCE_SUBDIR sdk), it
#     defines the same nevr::plugin_sdk target and installs nothing.
#
#   include/nevr/      every target, the Quest included
#   include/nevr/pc/   Windows only: <windows.h>, the host's BugSplat64.dll exports, echovr.exe addresses, MinHook
#                      (hook_manager.h includes <MinHook.h>; the plugin that includes it supplies MinHook)
cmake_minimum_required(VERSION 3.21)

# The package's major version is the plugin API version, read from the one line that defines it.
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/include/nevr/plugin_interface.h" _nevr_api_line
     REGEX "^#define NEVR_PLUGIN_API_VERSION [0-9]+$")
if(NOT _nevr_api_line MATCHES "^#define NEVR_PLUGIN_API_VERSION ([0-9]+)$")
  message(FATAL_ERROR "sdk: include/nevr/plugin_interface.h has no single '#define NEVR_PLUGIN_API_VERSION <n>' line")
endif()

project(NevrPluginSdk VERSION ${CMAKE_MATCH_1}.0.0 LANGUAGES NONE)

add_library(nevr_plugin_sdk INTERFACE)
add_library(nevr::plugin_sdk ALIAS nevr_plugin_sdk)
set_target_properties(nevr_plugin_sdk PROPERTIES EXPORT_NAME plugin_sdk)

include(GNUInstallDirs)
target_include_directories(nevr_plugin_sdk INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

if(PROJECT_IS_TOP_LEVEL)
  include(CMakePackageConfigHelpers)
  set(_nevr_sdk_cmake_dir "${CMAKE_INSTALL_DATADIR}/cmake/NevrPluginSdk")

  install(DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/include/nevr" DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
  install(TARGETS nevr_plugin_sdk EXPORT NevrPluginSdkTargets)
  install(EXPORT NevrPluginSdkTargets NAMESPACE nevr:: FILE NevrPluginSdkConfig.cmake
          DESTINATION "${_nevr_sdk_cmake_dir}")

  # A plugin built against API N loads on a host of API N; a new major is a new ABI.
  write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/NevrPluginSdkConfigVersion.cmake"
    COMPATIBILITY SameMajorVersion ARCH_INDEPENDENT)
  install(FILES "${CMAKE_CURRENT_BINARY_DIR}/NevrPluginSdkConfigVersion.cmake" DESTINATION "${_nevr_sdk_cmake_dir}")
endif()
