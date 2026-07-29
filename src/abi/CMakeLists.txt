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
set(ABI_HEADERS "echovr.h" "echovr_functions.h" "symbols.h" "symbol_hash.h")

add_library(nevr_abi STATIC ${ABI_SOURCES} ${ABI_HEADERS})

target_precompile_headers(nevr_abi PRIVATE "${CMAKE_SOURCE_DIR}/src/core/pch.h")

# src/ only — headers are included path-qualified, as "abi/echovr.h".
target_include_directories(nevr_abi PUBLIC ${CMAKE_SOURCE_DIR}/src)

if(MSVC)
  target_compile_options(nevr_abi PRIVATE /wd4251)
endif()
