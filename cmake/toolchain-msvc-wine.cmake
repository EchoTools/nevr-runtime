# CMake Toolchain File for MSVC via Wine (Linux to Windows cross-compilation)
# Usage: cmake -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-msvc-wine.cmake ..

cmake_minimum_required(VERSION 3.20)

# System settings
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_VERSION 10.0)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

# Paths to Windows installation (from mounted Windows partition)
set(WINOS_ROOT "" CACHE PATH "Path to mounted Windows installation (e.g. /mnt/winos)")
set(MSVC_VERSION "14.44.35207" CACHE STRING "MSVC toolset version")
set(SDK_VERSION "10.0.26100.0" CACHE STRING "Windows SDK version")

# MSVC and SDK paths
set(VS_ROOT "${WINOS_ROOT}/Program Files/Microsoft Visual Studio/2022/Community")
set(MSVC_ROOT "${VS_ROOT}/VC/Tools/MSVC/${MSVC_VERSION}")
set(SDK_ROOT "${WINOS_ROOT}/Program Files (x86)/Windows Kits/10")

# Wrapper scripts directory
get_filename_component(TOOLCHAIN_DIR "${CMAKE_CURRENT_LIST_FILE}" DIRECTORY)
get_filename_component(SOURCE_DIR "${TOOLCHAIN_DIR}/.." ABSOLUTE)
set(SCRIPTS_DIR "${TOOLCHAIN_DIR}/../scripts")

# Use wrapper scripts for MSVC tools
set(CMAKE_C_COMPILER "${SCRIPTS_DIR}/cl-wine.sh" CACHE FILEPATH "C compiler")
set(CMAKE_CXX_COMPILER "${SCRIPTS_DIR}/cl-wine.sh" CACHE FILEPATH "C++ compiler")
set(CMAKE_AR "${SCRIPTS_DIR}/lib-wine.sh" CACHE FILEPATH "Static library archiver")
set(CMAKE_LINKER "${SCRIPTS_DIR}/link-wine.sh" CACHE FILEPATH "Linker")

# Set CMAKE_C_COMPILER_ID and CMAKE_CXX_COMPILER_ID manually since detection won't work
set(CMAKE_C_COMPILER_ID "MSVC" CACHE STRING "")
set(CMAKE_CXX_COMPILER_ID "MSVC" CACHE STRING "")
# Use 19.28 which is well-known to CMake (VS2019 16.8)
set(CMAKE_C_COMPILER_VERSION "19.28.29334" CACHE STRING "")
set(CMAKE_CXX_COMPILER_VERSION "19.28.29334" CACHE STRING "")

# Set MSVC version to a known value
set(MSVC_VERSION_INTERNAL 1928 CACHE STRING "")

# Disable compiler checks (Wine + MSVC detection is complex)
set(CMAKE_C_COMPILER_WORKS TRUE CACHE BOOL "")
set(CMAKE_CXX_COMPILER_WORKS TRUE CACHE BOOL "")
set(CMAKE_C_COMPILER_FORCED TRUE CACHE BOOL "")
set(CMAKE_CXX_COMPILER_FORCED TRUE CACHE BOOL "")

# Set C++ standard compile features for MSVC
# MSVC 19.28 supports up to C++20, but we set C++17 as the standard
set(CMAKE_CXX_COMPILE_FEATURES
    cxx_std_11 cxx_std_14 cxx_std_17 cxx_std_20
    cxx_alias_templates cxx_alignas cxx_alignof cxx_attributes cxx_auto_type
    cxx_constexpr cxx_decltype cxx_decltype_auto cxx_default_function_template_args
    cxx_defaulted_functions cxx_defaulted_move_initializers cxx_delegating_constructors
    cxx_deleted_functions cxx_enum_forward_declarations cxx_explicit_conversions
    cxx_extended_friend_declarations cxx_extern_templates cxx_final cxx_func_identifier
    cxx_generalized_initializers cxx_generic_lambdas cxx_inheriting_constructors
    cxx_inline_namespaces cxx_lambdas cxx_lambda_init_captures cxx_local_type_template_args
    cxx_long_long_type cxx_noexcept cxx_nonstatic_member_init cxx_nullptr
    cxx_override cxx_range_for cxx_raw_string_literals cxx_reference_qualified_functions
    cxx_relaxed_constexpr cxx_return_type_deduction cxx_right_angle_brackets
    cxx_rvalue_references cxx_sizeof_member cxx_static_assert cxx_strong_enums
    cxx_template_template_parameters cxx_thread_local cxx_trailing_return_types
    cxx_unicode_literals cxx_uniform_initialization cxx_unrestricted_unions
    cxx_user_literals cxx_variable_templates cxx_variadic_macros cxx_variadic_templates
    CACHE STRING "")

set(CMAKE_C_COMPILE_FEATURES
    c_std_11 c_std_17 c_function_prototypes c_restrict c_static_assert c_variadic_macros
    CACHE STRING "")

# Set output suffixes for Windows
set(CMAKE_EXECUTABLE_SUFFIX ".exe")
set(CMAKE_SHARED_LIBRARY_PREFIX "")
set(CMAKE_SHARED_LIBRARY_SUFFIX ".dll")
set(CMAKE_STATIC_LIBRARY_PREFIX "")
set(CMAKE_STATIC_LIBRARY_SUFFIX ".lib")
set(CMAKE_IMPORT_LIBRARY_PREFIX "")
set(CMAKE_IMPORT_LIBRARY_SUFFIX ".lib")

# Set C++ standard directly (since feature detection won't work)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Include paths (exposed for use by CMakeLists.txt)
set(MSVC_INCLUDE "${MSVC_ROOT}/include")
set(UCRT_INCLUDE "${SDK_ROOT}/Include/${SDK_VERSION}/ucrt")
set(UM_INCLUDE "${SDK_ROOT}/Include/${SDK_VERSION}/um")
set(SHARED_INCLUDE "${SDK_ROOT}/Include/${SDK_VERSION}/shared")

list(APPEND CMAKE_SYSTEM_INCLUDE_PATH
    "${MSVC_INCLUDE}"
    "${UCRT_INCLUDE}"
    "${UM_INCLUDE}"
    "${SHARED_INCLUDE}"
)

# Library paths
set(MSVC_LIB "${MSVC_ROOT}/lib/x64")
set(UCRT_LIB "${SDK_ROOT}/Lib/${SDK_VERSION}/ucrt/x64")
set(UM_LIB "${SDK_ROOT}/Lib/${SDK_VERSION}/um/x64")

list(APPEND CMAKE_SYSTEM_LIBRARY_PATH
    "${MSVC_LIB}"
    "${UCRT_LIB}"
    "${UM_LIB}"
)

# vcpkg installed packages path (use pre-built Windows packages)
set(VCPKG_INSTALLED_DIR "${SOURCE_DIR}/vcpkg_installed/x64-windows")

# Find root settings - include vcpkg packages
set(CMAKE_FIND_ROOT_PATH 
    "${VCPKG_INSTALLED_DIR}"
    "${WINOS_ROOT}"
)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)

# Set CMAKE_PREFIX_PATH for finding packages
list(APPEND CMAKE_PREFIX_PATH "${VCPKG_INSTALLED_DIR}/share")

# Don't try to run compiled programs
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# Set some useful defines
set(WIN32 TRUE)
set(MSVC TRUE)
set(_WIN64 TRUE)

# Disable precompiled headers for Wine cross-compilation (they can be problematic)
set(CMAKE_DISABLE_PRECOMPILE_HEADERS ON CACHE BOOL "Disable precompiled headers for Wine build")

# Suppress warnings about Windows-specific code and fix header issues
# WIN32_LEAN_AND_MEAN must be defined before any Windows headers to avoid winsock conflicts
add_compile_definitions(_WIN64 _AMD64_ WIN32 _WINDOWS WIN32_LEAN_AND_MEAN NOMINMAX _WINSOCK_DEPRECATED_NO_WARNINGS)
