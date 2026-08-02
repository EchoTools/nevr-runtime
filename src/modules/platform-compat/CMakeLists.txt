# platform_compat — Schannel TLS modernisation, CreateDirectory fixes,
# WinHTTP-to-libcurl CoCreateInstance hook. Linked statically into
# BugSplat64.dll (2026-08-02: folded from separate DLL per owner decision
# — separate deployable DLLs are a failure mode when they must always ship
# together).
#
# Includes winhttp_stub (libcurl-backed COM stub) since the CoCreateInstance
# hook redirects WinHTTP COM creation to it.

find_package(CURL REQUIRED)

add_library(platform_compat STATIC
    src/platform_compat.cpp
)
# winhttp_stub.cpp is already compiled directly into nevr_runtime
# (src/runtime/CMakeLists.txt:24) — linking it here would duplicate symbols.

target_compile_definitions(platform_compat PRIVATE USE_MINHOOK NEVR_MODULE_EXPORTS)

target_include_directories(platform_compat PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/runtime
)

target_precompile_headers(platform_compat PRIVATE "${CMAKE_SOURCE_DIR}/src/core/pch.h")

target_link_libraries(platform_compat PRIVATE minhook nevr_core CURL::libcurl)
