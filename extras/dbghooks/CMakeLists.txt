# DbgHooks - Debugging and Diagnostic Hooks
# Implements hooks specified in:
#   - evr-reconstruction/docs/usage/QUICK_START_HASHING_HOOKS.md (hash discovery)
#   - evr-reconstruction/docs/hooks/gun2cr_visual_fix_hook.md (Gun2CR visual fix)

# Use vendored MinHook for function hooking
set(HOOKING_LIBRARY minhook::minhook)

# Find jsoncpp for weapon configuration (use pkg-config as fallback)
find_package(jsoncpp QUIET)
if(NOT jsoncpp_FOUND)
  find_package(PkgConfig)
  if(PKG_CONFIG_FOUND)
    pkg_check_modules(JSONCPP REQUIRED IMPORTED_TARGET jsoncpp)
    add_library(JsonCpp::JsonCpp ALIAS PkgConfig::JSONCPP)
  endif()
endif()

# DbgHooks sources
set(DBGHOOKS_SOURCES 
  "static_entry.cpp"
  "hash_hooks.cpp"
  "gun2cr_hook.cpp"
  "gun2cr_fix/weapon_enabler.cpp"
  "gamestart_hooks.cpp"
  "mesh_dump_hooks.cpp"
  "weapon_input_hook.cpp"
)

set(DBGHOOKS_HEADERS 
  "hash_hooks.h"
  "gun2cr_hook.h"
  "gun2cr_fix/weapon_enabler.h"
  "gamestart_hooks.h"
  "mesh_dump_hooks.h"
  "weapon_input_hook.h"
)

add_library(DbgHooksObj OBJECT 
  ${DBGHOOKS_SOURCES} 
  ${DBGHOOKS_HEADERS}
)

target_compile_definitions(DbgHooksObj PRIVATE USE_MINHOOK)
target_precompile_headers(DbgHooksObj PRIVATE "${NEVR_COMMON_INCLUDE}/pch.h")
target_include_directories(DbgHooksObj PRIVATE 
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}
)
# Ensure object library users get the include directories
target_include_directories(DbgHooksObj INTERFACE 
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}
)

# Link dependencies to the object library so it can compile (find headers)
target_link_libraries(DbgHooksObj PUBLIC ${HOOKING_LIBRARY} Common JsonCpp::JsonCpp)

add_library(DbgHooks SHARED 
  "dllmain.cpp"
  "exports.def"
)

target_link_libraries(DbgHooks PRIVATE DbgHooksObj ${HOOKING_LIBRARY} Common JsonCpp::JsonCpp)

target_link_directories(DbgHooks PRIVATE Common)

# Static link CRT to avoid api-ms-win-crt-* dependencies in Wine
if(MINGW)
  target_link_options(DbgHooks PRIVATE -static)
endif()

if(MSVC)
  target_compile_options(DbgHooks PRIVATE /wd4251)
endif()

# =============================================================================
# echovr_launcher - Early injection launcher for hash discovery
# =============================================================================

add_executable(echovr_launcher
  "echovr_launcher.cpp"
)

set_target_properties(echovr_launcher PROPERTIES
  OUTPUT_NAME "echovr_launcher"
)

target_compile_features(echovr_launcher PRIVATE cxx_std_17)

# Link DbgHooks statically
target_link_libraries(echovr_launcher PRIVATE DbgHooksObj ${HOOKING_LIBRARY} Common JsonCpp::JsonCpp)
target_link_directories(echovr_launcher PRIVATE Common)

# Static link CRT for standalone executable (testing)
if(MSVC)
  # MSVC: Use static CRT (/MT)
  target_compile_options(echovr_launcher PRIVATE /MT)
  target_link_options(echovr_launcher PRIVATE /SUBSYSTEM:CONSOLE)
endif()

if(MINGW)
  # MinGW: Static link all runtime libraries including MSVCRT
  target_compile_options(echovr_launcher PRIVATE -static-libgcc -static-libstdc++)
  target_link_options(echovr_launcher PRIVATE -static -static-libgcc -static-libstdc++ -lmsvcrt)
endif()

# No dependencies - standalone launcher
# Links only to Windows APIs (kernel32 is implicit)

# =============================================================================
# DllLoader - Loads echovr_asdll.dll with hooks installed first
# =============================================================================

add_executable(DllLoader
  "DllLoader.cpp"
)

set_target_properties(DllLoader PROPERTIES
  OUTPUT_NAME "DllLoader"
)

target_compile_features(DllLoader PRIVATE cxx_std_17)

# Static link CRT for standalone executable (testing)
if(MSVC)
  # MSVC: Use static CRT (/MT)
  target_compile_options(DllLoader PRIVATE /MT)
  target_link_options(DllLoader PRIVATE /SUBSYSTEM:CONSOLE)
endif()

if(MINGW)
  # MinGW: Static link all runtime libraries including MSVCRT
  target_compile_options(DllLoader PRIVATE -static-libgcc -static-libstdc++)
  target_link_options(DllLoader PRIVATE -static -static-libgcc -static-libstdc++ -lmsvcrt)
endif()

