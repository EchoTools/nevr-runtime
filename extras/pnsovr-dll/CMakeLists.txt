# pnsovr-dll CMakeLists.txt
#
# Self-contained PNSOvr DLL - Echo VR Platform Native Services
# Replacement for pnsovr.dll v34.4
# 
# This is a standalone DLL that provides Echo VR platform services:
# - Voice/Audio communication (Voip subsystem)
# - User management and authentication (Users subsystem)
# - Multiplayer room management (Rooms subsystem)
# - Rich presence broadcasting (Presence subsystem)
# - In-app purchase management (IAP subsystem)
# - Social notifications (Notifications subsystem)
#
# Integrates with:
# - Nakama for backend services
# - libopus for audio codec
# - Standard Windows APIs for system integration

cmake_minimum_required(VERSION 3.16)

project(PNSOvrDLL LANGUAGES CXX)

# =============================================================================
# Source Files
# =============================================================================

set(PNSOVR_SOURCES
    "src/pnsovr_voip.cpp"
    "src/pnsovr_users.cpp"
    "src/pnsovr_rooms.cpp"
    "src/pnsovr_presence.cpp"
    "src/pnsovr_iap.cpp"
    "src/pnsovr_notifications.cpp"
    "src/pnsovr_plugin.cpp"
    "src/PNSOVRSocialInterceptor.cpp"
    "src/CustomSocialManager.cpp"
)

set(PNSOVR_HEADERS
    "src/pnsovr_voip.h"
    "src/pnsovr_users.h"
    "src/pnsovr_rooms.h"
    "src/pnsovr_presence.h"
    "src/pnsovr_iap.h"
    "src/pnsovr_notifications.h"
    "src/pnsovr_plugin.h"
    "src/PNSOVRSocialInterceptor.h"
    "src/CustomSocialManager.h"
)

# =============================================================================
# DLL Target
# =============================================================================

add_library(PNSOvrDLL SHARED
    ${PNSOVR_SOURCES}
    ${PNSOVR_HEADERS}
)

# Output as pnsovr.dll (matches original binary naming)
# Note: This can be overridden to pnsnevr.dll if needed for game integration
set_target_properties(PNSOvrDLL PROPERTIES
    OUTPUT_NAME "pnsovr"
    SUFFIX ".dll"
    PREFIX ""
)

# =============================================================================
# Compile Definitions
# =============================================================================

target_compile_definitions(PNSOvrDLL PRIVATE
    PNSOVR_DLL_EXPORTS
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WIN32_WINNT=0x0A00  # Windows 10+
    UNICODE
    _UNICODE
)

# =============================================================================
# Include Directories
# =============================================================================

target_include_directories(PNSOvrDLL PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Add nevr-common includes if available
if(DEFINED NEVR_COMMON_INCLUDE)
    target_include_directories(PNSOvrDLL PRIVATE ${NEVR_COMMON_INCLUDE})
endif()

# =============================================================================
# External Dependencies
# =============================================================================

# NOTE: Social features module is STANDALONE - no libOVRPlatform or MinHook dependency
# It provides static placeholder implementations of all social APIs

# Optional: libopus for audio codec (used by Voip subsystem)
# The Voip subsystem uses external libopus for actual codec work.
# For Wine cross-compilation, we cannot link Linux .so files into Windows DLLs.
# Instead, we provide stub implementations that return errors at runtime,
# allowing the DLL to load and other subsystems to function.

find_package(PkgConfig QUIET)
set(OPUS_FOUND FALSE)

# Try to find opus headers - these exist on Linux
if(PkgConfig_FOUND)
    pkg_check_modules(OPUS opus)
    if(OPUS_FOUND)
        message(STATUS "  libopus headers found via pkg-config")
        
        # Include directories from pkg-config - needed for compilation
        target_include_directories(PNSOvrDLL PRIVATE ${OPUS_INCLUDE_DIRS})
        
        # For Wine cross-compilation: We cannot link the actual opus library
        # The VOIP subsystem will use stub implementations at runtime
        # (See pnsovr_voip.cpp - functions return VOIP_E_NOT_AVAILABLE when opus is unavailable)
        message(STATUS "  opus linking: DISABLED for Wine cross-compilation")
        message(STATUS "  (VOIP functions will return E_NOT_AVAILABLE at runtime)")
        target_compile_definitions(PNSOvrDLL PRIVATE PNSOVR_VOIP_STUB_MODE=1)
        set(OPUS_FOUND TRUE)
    endif()
endif()

# If pkg-config didn't find it, try manual search for headers only
if(NOT OPUS_FOUND)
    message(STATUS "  Searching for libopus headers manually...")
    
    # Find opus headers only - we won't link the library on Wine
    find_path(OPUS_INCLUDE_DIR opus.h
        PATHS 
            /usr/include 
            /usr/local/include 
            /opt/include
            ${CMAKE_SYSTEM_PREFIX_PATH}/include
    )
    
    if(OPUS_INCLUDE_DIR)
        target_include_directories(PNSOvrDLL PRIVATE ${OPUS_INCLUDE_DIR})
        message(STATUS "  libopus headers found: ${OPUS_INCLUDE_DIR}")
        message(STATUS "  opus linking: DISABLED for Wine cross-compilation")
        message(STATUS "  (VOIP functions will return E_NOT_AVAILABLE at runtime)")
        target_compile_definitions(PNSOvrDLL PRIVATE PNSOVR_VOIP_STUB_MODE=1)
        set(OPUS_FOUND TRUE)
    else()
        message(WARNING "  libopus headers not found - VOIP compilation will fail")
        message(WARNING "  Install libopus dev headers: apt install libopus-dev")
    endif()
endif()

# Optional: Nakama for backend services
# When Nakama is available, the Voip subsystem can integrate with Nakama's
# voice channels, and other subsystems can use Nakama for persistence.
find_package(Nakama QUIET)
if(Nakama_FOUND)
    target_link_libraries(PNSOvrDLL PRIVATE Nakama::NakamaClient)
    target_compile_definitions(PNSOvrDLL PRIVATE PNSOVR_WITH_NAKAMA=1)
    message(STATUS "  Nakama found: enabling Nakama backend integration")
else()
    message(STATUS "  Nakama not found: building standalone pnsovr.dll")
    message(STATUS "  To enable Nakama: vcpkg install nakama:x64-windows-static")
endif()

# =============================================================================
# Platform Libraries
# =============================================================================

# Windows-only APIs (only link on Windows platform)
if(WIN32 OR MINGW OR CYGWIN)
    target_link_libraries(PNSOvrDLL PRIVATE
        # Windows API libraries
        ws2_32           # Winsock 2 (networking)
        winhttp          # HTTP client
        crypt32          # Cryptography (for tokens/verification)
        advapi32         # Advanced API (registry, security)
        kernel32         # Kernel (base API)
        user32           # User API (messages, windows)
    )
else()
    # On non-Windows platforms (Linux, macOS), provide stub implementations
    message(STATUS "  Building on non-Windows platform - Windows APIs will be stubbed")
    target_compile_definitions(PNSOvrDLL PRIVATE PNSOVR_STUB_WINDOWS_APIS=1)
endif()

# =============================================================================
# Compiler Options
# =============================================================================

# Set C++ standard
set_target_properties(PNSOvrDLL PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

# MSVC-specific options
if(MSVC)
    # Disable specific warnings
    target_compile_options(PNSOvrDLL PRIVATE
        /wd4251  # class needs to have dll-interface
        /wd4275  # non-dll-interface class used as base
        /wd4820  # padding added after data member
        /wd4100  # unreferenced formal parameter
        /wd4267  # conversion, possible loss of data
        /wd4365  # signed/unsigned mismatch
        /wd4710  # function not inlined
        /wd4514  # unreferenced inline function removed
        /wd4711  # function selected for inline expansion
        /Wall    # Enable all warnings
        $<$<CONFIG:Release>:/WX>  # Treat warnings as errors only in Release builds
    )
    
    # Release build optimizations
    target_compile_options(PNSOvrDLL PRIVATE
        $<$<CONFIG:Release>:/O2>     # Optimize for speed
        $<$<CONFIG:Release>:/Oi>     # Intrinsic functions
        $<$<CONFIG:Release>:/Ot>     # Favor speed
    )
    
    # Disable LTCG for this target (causes issues with C4711 warnings)
    target_link_options(PNSOvrDLL PRIVATE
        $<$<CONFIG:Release>:/LTCG:OFF>
    )
endif()

# =============================================================================
# DLL Export Configuration
# =============================================================================

# Set DLL characteristics for proper export symbol generation
set_target_properties(PNSOvrDLL PROPERTIES
    WINDOWS_EXPORT_ALL_SYMBOLS OFF  # Use explicit export declarations
    DEFINE_SYMBOL "PNSOVR_EXPORTS"
    INTERPROCEDURAL_OPTIMIZATION FALSE  # Disable LTCG (causes issues with C4711 warnings in code generation)
)

# =============================================================================
# Output Location
# =============================================================================

# Place output DLL in the main build bin directory
set_target_properties(PNSOvrDLL PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
)

# =============================================================================
# Build Information
# =============================================================================

message(STATUS "==============================================")
message(STATUS "  PNSOvrDLL (pnsovr.dll)")
message(STATUS "  Echo VR Platform Native Services")
message(STATUS "  Self-contained subsystem package")
message(STATUS "==============================================")
message(STATUS "  Subsystems:")
message(STATUS "    • Voip (voice communication with Opus codec)")
message(STATUS "    • Users (user management and authentication)")
message(STATUS "    • Rooms (multiplayer room management)")
message(STATUS "    • Presence (rich presence broadcasting)")
message(STATUS "    • IAP (in-app purchase management)")
message(STATUS "    • Notifications (social notifications)")
message(STATUS "==============================================")
message(STATUS "  Dependencies:")
message(STATUS "    • libopus (required for audio codec)")
message(STATUS "    • Nakama (optional for backend services)")
message(STATUS "==============================================")
