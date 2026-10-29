# nevr_abi — the echovr.exe ABI surface.
#
# Everything here encodes knowledge OF THE GAME BINARY: its types, its struct
# layouts, the addresses we call through, and the symbol IDs it hashes. If a
# fact in this directory is wrong, the reconstruction is wrong.
#
# Membership test: "the binary told us this". Our own primitives live in
# src/core/. This layer SHALL NOT depend on src/core/ — the dependency runs
# core -> abi, never the reverse, and `just verify` enforces it.

set(ABI_SOURCES "echovr_functions.cpp")
set(ABI_HEADERS "echovr.h" "echovr_functions.h" "symbols.h" "game_level.h")

add_library(nevr_abi STATIC ${ABI_SOURCES} ${ABI_HEADERS})

target_precompile_headers(nevr_abi PRIVATE "${CMAKE_SOURCE_DIR}/src/core/pch.h")

# src/ only — headers are included path-qualified, as "abi/echovr.h".
target_include_directories(nevr_abi PUBLIC ${CMAKE_SOURCE_DIR}/src)

# The CSymbol64 hash and the plugin C ABI are published in the plugin SDK (nevr/symbol_hash.h,
# nevr/plugin_interface.h), because plugins need them too. This layer and everything above it reads them from there.
target_link_libraries(nevr_abi PUBLIC nevr::plugin_sdk)

if(MSVC)
  target_compile_options(nevr_abi PRIVATE /wd4251)
endif()
