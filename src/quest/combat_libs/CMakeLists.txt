# The combat script libraries, built on the game's NDK (r21b, tools/game_ndk.pin).
#
# A separate CMake project, like the rest of src/quest: it is configured by `just build-quest-combat-libs` with the pinned NDK's own
# toolchain file, never by the Quest preset (the sentinel and the plugins build with the r26d NDK). The store APK's script
# libraries were built with r21b, and a library is only comparable with them when it is compiled by the same clang.
#
# Inputs, both without a default, both passed with -D:
#   NEVR_COMBAT_LIBSRC  the libsrc directory: LIBS.json and one directory per library, <hash16>/ with <hash16>.cpp and, when the
#                       row's link.asm is true, <hash16>.donor.s. Nothing from it is copied into this repository. The sources
#                       include regen_rt.h from tools/scripts in the directory's parent.
#   NEVR_STORE_APK      the unmodified store APK; its lib/arm64-v8a/libr15.so is what the rows with link.libr15 link.
#
# LIBS.json's "libs" object has one key per library, lib<hash16>.so, and each row has "link" with the booleans "asm" (add the
# donor object), "libr15" (link the stock libr15.so) and "ldl" (add -ldl). No library is built unless LIBS.json names it.
#
# Outputs in the build directory: lib<hash16>.so per library, SHA256SUMS over them, stock/libr15.so (taken from the store APK).
# Each library is checked by tools/check_quest_static_init.sh --plugin after it links.
#
# A refusal is a FATAL_ERROR that names what is wrong. The libsrc directory and the store APK are checked before the compiler is
# looked at, so a bad input is reported without an NDK.

cmake_minimum_required(VERSION 3.23)

set(NEVR_COMBAT_LIBSRC "" CACHE PATH "The libsrc directory: LIBS.json and one directory per library (no default)")
set(NEVR_STORE_APK "" CACHE FILEPATH "The unmodified store APK, for its lib/arm64-v8a/libr15.so (no default)")

function(nevr_combat_refuse message)
  message(FATAL_ERROR "build-quest-combat-libs: ${message}")
endfunction()

set(NEVR_COMBAT_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../..")
get_filename_component(NEVR_COMBAT_REPO_ROOT "${NEVR_COMBAT_REPO_ROOT}" ABSOLUTE)

# ── The libsrc directory ────────────────────────────────────────────────
if(NOT NEVR_COMBAT_LIBSRC)
  nevr_combat_refuse("NEVR_COMBAT_LIBSRC names no libsrc directory (pass -DNEVR_COMBAT_LIBSRC=<dir>)")
endif()
get_filename_component(nevr_libsrc "${NEVR_COMBAT_LIBSRC}" REALPATH)
if(NOT IS_DIRECTORY "${nevr_libsrc}")
  nevr_combat_refuse("libsrc is not a directory: ${nevr_libsrc}")
endif()
set(nevr_libs_json "${nevr_libsrc}/LIBS.json")
if(NOT EXISTS "${nevr_libs_json}")
  nevr_combat_refuse("no LIBS.json in ${nevr_libsrc} (the libsrc directory names the libraries to build)")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${nevr_libs_json}")
file(READ "${nevr_libs_json}" nevr_json)

set(nevr_json_error "NOTFOUND")
string(JSON nevr_libs_type ERROR_VARIABLE nevr_json_error TYPE "${nevr_json}" libs)
if(nevr_json_error OR NOT nevr_libs_type STREQUAL "OBJECT")
  nevr_combat_refuse("LIBS.json in ${nevr_libsrc} has no non-empty \"libs\" object (or does not parse)")
endif()
string(JSON nevr_libs GET "${nevr_json}" libs)
string(JSON nevr_count LENGTH "${nevr_libs}")
if(nevr_count EQUAL 0)
  nevr_combat_refuse("LIBS.json in ${nevr_libsrc} has no non-empty \"libs\" object")
endif()

set(nevr_names "")
math(EXPR nevr_last "${nevr_count} - 1")
foreach(nevr_i RANGE ${nevr_last})
  string(JSON nevr_member MEMBER "${nevr_libs}" ${nevr_i})
  list(APPEND nevr_names "${nevr_member}")
endforeach()
list(SORT nevr_names)

set(nevr_hashes "")
set(nevr_any_asm FALSE)
foreach(nevr_name IN LISTS nevr_names)
  # CMake's regular expressions have no {16}: 16 lowercase hex digits are checked by character set and length.
  string(LENGTH "${nevr_name}" nevr_name_length)
  if(NOT nevr_name MATCHES "^lib[0-9a-f]+\\.so$" OR NOT nevr_name_length EQUAL 22)
    nevr_combat_refuse("LIBS.json key '${nevr_name}' is not named lib<hash16>.so (16 lowercase hex digits)")
  endif()
  string(SUBSTRING "${nevr_name}" 3 16 nevr_hash)

  set(nevr_link_message "LIBS.json row ${nevr_name} has no \"link\" object of booleans asm, libr15 and ldl")
  string(JSON nevr_row_type TYPE "${nevr_libs}" "${nevr_name}")
  if(NOT nevr_row_type STREQUAL "OBJECT")
    nevr_combat_refuse("${nevr_link_message}")
  endif()
  string(JSON nevr_row GET "${nevr_libs}" "${nevr_name}")
  set(nevr_json_error "NOTFOUND")
  string(JSON nevr_link_type ERROR_VARIABLE nevr_json_error TYPE "${nevr_row}" link)
  if(nevr_json_error OR NOT nevr_link_type STREQUAL "OBJECT")
    nevr_combat_refuse("${nevr_link_message}")
  endif()
  string(JSON nevr_link GET "${nevr_row}" link)
  foreach(nevr_key asm libr15 ldl)
    set(nevr_json_error "NOTFOUND")
    string(JSON nevr_key_type ERROR_VARIABLE nevr_json_error TYPE "${nevr_link}" ${nevr_key})
    if(nevr_json_error)
      set(nevr_row_${nevr_hash}_${nevr_key} FALSE) # an absent key is false
    elseif(nevr_key_type STREQUAL "BOOLEAN")
      string(JSON nevr_row_${nevr_hash}_${nevr_key} GET "${nevr_link}" ${nevr_key})
    else()
      nevr_combat_refuse("${nevr_link_message}")
    endif()
  endforeach()

  if(NOT EXISTS "${nevr_libsrc}/${nevr_hash}/${nevr_hash}.cpp")
    nevr_combat_refuse("source of ${nevr_name} is missing: ${nevr_libsrc}/${nevr_hash}/${nevr_hash}.cpp")
  endif()
  if(nevr_row_${nevr_hash}_asm)
    set(nevr_any_asm TRUE)
    if(NOT EXISTS "${nevr_libsrc}/${nevr_hash}/${nevr_hash}.donor.s")
      nevr_combat_refuse("${nevr_name} links its donor object, and ${nevr_libsrc}/${nevr_hash}/${nevr_hash}.donor.s is missing")
    endif()
  endif()
  list(APPEND nevr_hashes "${nevr_hash}")
endforeach()

# ── The store APK ───────────────────────────────────────────────────────
if(NOT NEVR_STORE_APK)
  nevr_combat_refuse("NEVR_STORE_APK names no store APK (pass -DNEVR_STORE_APK=<file>)")
endif()
get_filename_component(nevr_store_apk "${NEVR_STORE_APK}" ABSOLUTE)
if(NOT EXISTS "${nevr_store_apk}" OR IS_DIRECTORY "${nevr_store_apk}")
  nevr_combat_refuse("store APK not found: ${nevr_store_apk}")
endif()
file(READ "${nevr_store_apk}" nevr_zip_magic LIMIT 4 HEX)
if(NOT nevr_zip_magic STREQUAL "504b0304" AND NOT nevr_zip_magic STREQUAL "504b0506")
  nevr_combat_refuse("the store APK is not a zip file: ${nevr_store_apk}")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${nevr_store_apk}")
set(nevr_stock_member "lib/arm64-v8a/libr15.so")
set(nevr_stock_dir "${CMAKE_BINARY_DIR}/stock")
file(REMOVE_RECURSE "${nevr_stock_dir}")
file(MAKE_DIRECTORY "${nevr_stock_dir}/apk")
execute_process(COMMAND "${CMAKE_COMMAND}" -E tar xf "${nevr_store_apk}" -- "${nevr_stock_member}"
                WORKING_DIRECTORY "${nevr_stock_dir}/apk" RESULT_VARIABLE nevr_tar_rc ERROR_VARIABLE nevr_tar_err TIMEOUT 300)
if(NOT nevr_tar_rc EQUAL 0 OR NOT EXISTS "${nevr_stock_dir}/apk/${nevr_stock_member}")
  string(STRIP "${nevr_tar_err}" nevr_tar_err)
  nevr_combat_refuse("the store APK has no ${nevr_stock_member} (cmake -E tar exit ${nevr_tar_rc}: ${nevr_tar_err})")
endif()
file(COPY_FILE "${nevr_stock_dir}/apk/${nevr_stock_member}" "${nevr_stock_dir}/libr15.so")
file(REMOVE_RECURSE "${nevr_stock_dir}/apk")

# ── The toolchain: the pinned NDK's own ─────────────────────────────────
if(NOT CMAKE_TOOLCHAIN_FILE)
  nevr_combat_refuse("no CMAKE_TOOLCHAIN_FILE: configure with the game's NDK, <ndk>/build/cmake/android.toolchain.cmake (`just build-quest-combat-libs` does; `just toolchain-r21b` installs the NDK)")
endif()
# The NDK toolchain file reads these before project(). No STL is linked (-nostdlib++ below), but its headers stay on the include
# path, as they were when the libraries were compiled with the driver alone; undefined symbols are left to the stock libr15.so
# and the system at load (--allow-shlib-undefined below), so the toolchain's --no-undefined is off.
set(ANDROID_ABI arm64-v8a CACHE STRING "")
set(ANDROID_PLATFORM android-21 CACHE STRING "")
set(ANDROID_STL c++_shared CACHE STRING "")
set(ANDROID_ALLOW_UNDEFINED_SYMBOLS ON CACHE BOOL "")

project(nevr_quest_combat_libs LANGUAGES C CXX)
if(nevr_any_asm)
  enable_language(ASM)
endif()

# The compiler is the pinned clang: the first line of `clang --version` is the pin's CLANG_VERSION, exactly.
set(nevr_pin "${NEVR_COMBAT_REPO_ROOT}/tools/game_ndk.pin")
file(STRINGS "${nevr_pin}" nevr_pin_lines)
set(nevr_pin_clang "")
foreach(nevr_pin_line IN LISTS nevr_pin_lines)
  if(nevr_pin_line MATCHES "^CLANG_VERSION=(.+)$")
    set(nevr_pin_clang "${CMAKE_MATCH_1}")
  endif()
endforeach()
if(NOT nevr_pin_clang)
  nevr_combat_refuse("${nevr_pin} has no CLANG_VERSION")
endif()
# The C++ compiler compiles the rows, the C compiler links them (see "The flags" below) and the ASM compiler assembles the donor
# objects (108 of the 116 rows have one), so all three are checked.
set(nevr_gated_languages CXX C)
if(nevr_any_asm)
  list(APPEND nevr_gated_languages ASM)
endif()
foreach(nevr_lang IN LISTS nevr_gated_languages)
  execute_process(COMMAND "${CMAKE_${nevr_lang}_COMPILER}" --version
                  OUTPUT_VARIABLE nevr_clang_out ERROR_VARIABLE nevr_clang_err RESULT_VARIABLE nevr_clang_rc TIMEOUT 60)
  string(REGEX MATCH "^[^\n]*" nevr_clang_first "${nevr_clang_out}${nevr_clang_err}")
  if(NOT nevr_clang_rc EQUAL 0 OR NOT nevr_clang_first STREQUAL nevr_pin_clang)
    nevr_combat_refuse("the compiler ${CMAKE_${nevr_lang}_COMPILER} is not the game's NDK clang pinned in tools/game_ndk.pin (run `just toolchain-r21b`)\n  expected: ${nevr_pin_clang}\n  actual:   ${nevr_clang_first}")
  endif()
endforeach()
get_filename_component(nevr_llvm_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
set(nevr_check_script "${NEVR_COMBAT_REPO_ROOT}/tools/check_quest_static_init.sh")

# ── The flags ───────────────────────────────────────────────────────────
# The NDK's toolchain file adds flags of its own. Each one is kept only when the store APK's libraries show it
# (docs/reference/quest-game-ndk.md, "Flags", has the counts over its 465 script libraries and libr15.so):
#   -g                         overridden by -g0, and --strip-debug (the NDK's static libgcc unwinder brings its own .debug_*): none
#                              has a .debug_* section (they keep .symtab)
#   -D_FORTIFY_SOURCE=2        overridden by -U_FORTIFY_SOURCE: none imports a __*_chk function
#   (frame pointer)            -fomit-frame-pointer: they set one up in 53 of 22588 function prologues, the NDK default does in 95%
#   -Wl,--build-id             overridden by --build-id=none: none has an NT_GNU_BUILD_ID note
#   -lm                        dropped by linking with the C driver: none has NEEDED libm.so
# Kept, because the libraries show them or cannot settle them: -fstack-protector-strong (they all import __stack_chk_fail),
# -funwind-tables, -fdata-sections, -ffunction-sections, -DANDROID, -Wl,--fatal-warnings, -latomic.
set(CMAKE_C_STANDARD_LIBRARIES "-latomic")
# CMake also appends the C++ compiler's implicit libraries (-lc++ -lm) to a link whose objects are C++, even when the C driver
# links them; no row links the C++ runtime or libm.
set(CMAKE_CXX_IMPLICIT_LINK_LIBRARIES "")

# ── The libraries ───────────────────────────────────────────────────────
# ROOT is the parent of the libsrc directory: the generated sources include regen_rt.h from its tools/scripts.
get_filename_component(nevr_libsrc_parent "${nevr_libsrc}" DIRECTORY)
set(nevr_scripts_dir "${nevr_libsrc_parent}/tools/scripts")

add_library(nevr_stock_libr15 SHARED IMPORTED)
# The store's libr15.so has no SONAME: linked by path, its NEEDED entry would be that path. IMPORTED_NO_SONAME makes the link
# name it by file name, which is what `-L<dir> -l:libr15.so` gave.
set_target_properties(nevr_stock_libr15 PROPERTIES IMPORTED_LOCATION "${nevr_stock_dir}/libr15.so" IMPORTED_NO_SONAME TRUE)

set(nevr_lib_targets "")
set(nevr_lib_names "")
foreach(nevr_hash IN LISTS nevr_hashes)
  set(nevr_target "combat_lib_${nevr_hash}")
  set(nevr_sources "${nevr_libsrc}/${nevr_hash}/${nevr_hash}.cpp")
  if(nevr_row_${nevr_hash}_asm)
    list(APPEND nevr_sources "${nevr_libsrc}/${nevr_hash}/${nevr_hash}.donor.s")
  endif()
  add_library(${nevr_target} SHARED ${nevr_sources})
  # The link is done by the C driver, not the C++ one: the C++ driver adds -lm to every link, and none of the store's script
  # libraries has NEEDED libm.so.
  set_target_properties(${nevr_target} PROPERTIES OUTPUT_NAME "${nevr_hash}" LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
                        LINKER_LANGUAGE C)
  target_include_directories(${nevr_target} PRIVATE "${nevr_scripts_dir}")
  target_compile_options(${nevr_target} PRIVATE -O2 -fPIC -g0
                         $<$<COMPILE_LANGUAGE:CXX>:-fomit-frame-pointer -U_FORTIFY_SOURCE>)
  target_link_options(${nevr_target} PRIVATE -nostdlib++ -Wl,--gc-sections -Wl,--allow-shlib-undefined -Wl,--build-id=none
                      -Wl,--strip-debug)
  if(nevr_row_${nevr_hash}_libr15)
    target_link_libraries(${nevr_target} PRIVATE nevr_stock_libr15)
  endif()
  if(nevr_row_${nevr_hash}_ldl)
    target_link_libraries(${nevr_target} PRIVATE dl)
  endif()
  add_custom_command(TARGET ${nevr_target} POST_BUILD
    COMMAND bash "${nevr_check_script}" --plugin "${nevr_llvm_bin}" "$<TARGET_FILE:${nevr_target}>"
    COMMENT "static-init check: lib${nevr_hash}.so"
    VERBATIM)
  list(APPEND nevr_lib_targets ${nevr_target})
  list(APPEND nevr_lib_names "lib${nevr_hash}.so")
endforeach()

# SHA256SUMS: "<sha256>  <name>" per library, sorted by name, written after every library has linked and passed its check.
list(JOIN nevr_lib_names "\n" nevr_names_text)
file(WRITE "${CMAKE_BINARY_DIR}/combat_lib_names.txt" "${nevr_names_text}\n")
add_custom_target(combat_libs_sha256sums ALL
  COMMAND "${CMAKE_COMMAND}" "-DNEVR_NAMES_FILE=${CMAKE_BINARY_DIR}/combat_lib_names.txt" "-DNEVR_OUT_DIR=${CMAKE_BINARY_DIR}"
          -P "${CMAKE_CURRENT_LIST_DIR}/write_sha256sums.cmake"
  BYPRODUCTS "${CMAKE_BINARY_DIR}/SHA256SUMS"
  COMMENT "SHA256SUMS"
  VERBATIM)
add_dependencies(combat_libs_sha256sums ${nevr_lib_targets})

list(LENGTH nevr_hashes nevr_lib_count)
message(STATUS "build-quest-combat-libs: ${nevr_lib_count} libraries from ${nevr_libsrc}, compiler: ${nevr_clang_first}")
