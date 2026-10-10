# platform_compat — Schannel TLS modernisation, CreateDirectory fixes,
# MSXML6 CoCreateInstance pass-through hook. Linked statically into
# BugSplat64.dll (2026-08-02: folded from separate DLL per owner decision
# — separate deployable DLLs are a failure mode when they must always ship
# together).

add_library(platform_compat STATIC
    src/platform_compat.cpp
)
target_compile_definitions(platform_compat PRIVATE NEVR_USE_MINHOOK NEVR_MODULE_EXPORTS)

target_include_directories(platform_compat PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/runtime
)

target_precompile_headers(platform_compat PRIVATE "${CMAKE_SOURCE_DIR}/src/core/pch.h")

target_link_libraries(platform_compat PRIVATE minhook nevr_core)
