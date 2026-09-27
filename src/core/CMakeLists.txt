# nevr_core — NEVR's own primitives.
#
# Membership test: "we wrote this". Logging, the CLI-flag globals, base64, the
# hooking abstraction, the auth-token model, the precompiled header.
#
# Depends on nevr_abi (PUBLIC): EchoVR::LogLevel is the game's enum, so logging
# genuinely sits on top of the ABI layer. The reverse dependency is forbidden.

set(CORE_SOURCES "logging.cpp" "globals.cpp" "base64.cpp" "system_info.cpp" "nevr_config.cpp" "build_identity.cpp" "mic_dsp.cpp" "mic_lifecycle.cpp" "mic_capture_drain.cpp")
set(CORE_HEADERS
  "logging.h" "globals.h" "base64.h" "system_info.h" "build_identity.h"
  "hooking.h" "auth_token.h" "pch.h" "nevr_config.h" "mic_dsp.h" "mic_lifecycle.h" "mic_capture_drain.h"
  # Header-only published C ABI for third-party DLLs. Listed here so it appears
  # in IDE/target views; it has no compiled sources of its own.
  "${CMAKE_SOURCE_DIR}/src/extension/plugin_interface.h"
  "${CMAKE_SOURCE_DIR}/src/extension/module_interface.h")

add_library(nevr_core STATIC ${CORE_SOURCES} ${CORE_HEADERS})

target_precompile_headers(nevr_core PRIVATE "pch.h")

target_include_directories(nevr_core PUBLIC ${CMAKE_SOURCE_DIR}/src)

find_package(nlohmann_json CONFIG REQUIRED)
target_link_libraries(nevr_core PUBLIC nevr_abi PRIVATE nlohmann_json::nlohmann_json)

# nevr_config.cpp compiles against yaml-cpp headers, but nevr_core must NOT LINK
# yaml-cpp: this is a static archive, so its yaml symbols stay unresolved until a
# consumer links them. Only nevr_runtime (and the nevr_config test) link yaml-cpp;
# the module DLLs link nevr_core but never reference nevr_config, so its object is
# not pulled and no yaml dependency reaches them. SYSTEM include keeps -Wall/-Wextra
# off yaml-cpp's own headers.
target_include_directories(nevr_core SYSTEM PRIVATE
  $<TARGET_PROPERTY:yaml-cpp::yaml-cpp,INTERFACE_INCLUDE_DIRECTORIES>)
# nevr_config.cpp includes <windows.h> with NOMINMAX before yaml-cpp; the core PCH
# includes windows.h WITHOUT NOMINMAX, whose min/max macros break yaml-cpp/<limits>.
# Compile this one file without the PCH.
set_source_files_properties(nevr_config.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
# mic_dsp.cpp is deliberately platform-independent (no windows.h) so a GTest
# target can compile it directly without the Windows PCH — see
# tests/test_mic_dsp.cpp.
set_source_files_properties(mic_dsp.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
set_source_files_properties(mic_lifecycle.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
set_source_files_properties(mic_capture_drain.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)

if(MSVC)
  target_compile_options(nevr_core PRIVATE /wd4251)
endif()
