# hello_plugin: the example plugin, built the way an out-of-tree plugin repository builds: against the installed SDK
# only (find_package(NevrPluginSdk CONFIG)), with nothing of nevr-runtime's tree on the path. One source, three targets:
# Windows (mingw-w64), Quest (arm64-android, NDK) and the build host. The commands are in
# .github/workflows/plugin-sdk-example.yml.
cmake_minimum_required(VERSION 3.21)
project(NevrHelloPlugin LANGUAGES CXX)

set(NEVR_HELLO_API_VERSION 9 CACHE STRING "the plugin API major the SDK package must match")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(NevrPluginSdk ${NEVR_HELLO_API_VERSION} CONFIG REQUIRED)

add_library(hello_plugin SHARED hello_plugin.cpp)
target_link_libraries(hello_plugin PRIVATE nevr::plugin_sdk)
# NEVR_PLUGIN_API exports the entry points on Windows only when this is set (sdk/include/nevr/plugin_interface.h).
target_compile_definitions(hello_plugin PRIVATE NEVR_PLUGIN_EXPORTS)
set_target_properties(hello_plugin PROPERTIES PREFIX "")
if(WIN32)
  target_link_options(hello_plugin PRIVATE -static-libgcc -static-libstdc++)
endif()
