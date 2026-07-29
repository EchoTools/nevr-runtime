# nevr_core — NEVR's own primitives.
#
# Membership test: "we wrote this". Logging, the CLI-flag globals, base64, the
# hooking abstraction, the auth-token model, the precompiled header.
#
# Depends on nevr_abi (PUBLIC): EchoVR::LogLevel is the game's enum, so logging
# genuinely sits on top of the ABI layer. The reverse dependency is forbidden.

set(CORE_SOURCES "logging.cpp" "globals.cpp" "base64.cpp" "system_info.cpp")
set(CORE_HEADERS
  "logging.h" "globals.h" "base64.h" "system_info.h"
  "hooking.h" "auth_token.h" "pch.h"
  # Header-only published C ABI for third-party DLLs. Listed here so it appears
  # in IDE/target views; it has no compiled sources of its own.
  "${CMAKE_SOURCE_DIR}/src/extension/plugin_interface.h"
  "${CMAKE_SOURCE_DIR}/src/extension/module_interface.h")

add_library(nevr_core STATIC ${CORE_SOURCES} ${CORE_HEADERS})

target_precompile_headers(nevr_core PRIVATE "pch.h")

target_include_directories(nevr_core PUBLIC ${CMAKE_SOURCE_DIR}/src)

find_package(nlohmann_json CONFIG REQUIRED)
target_link_libraries(nevr_core PUBLIC nevr_abi PRIVATE nlohmann_json::nlohmann_json)

if(MSVC)
  target_compile_options(nevr_core PRIVATE /wd4251)
endif()
