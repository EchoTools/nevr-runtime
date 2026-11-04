# The Quest host tests: C++ test programs for the Quest code, built with the host compiler (no NDK, no Android) and run by
# ctest (ADR 0041: native code, test programs included, is built by CMake targets, never by compiler lines in a recipe).
#
# Reached from src/quest/host/CMakeLists.txt (the host tools project, NEVR_QUEST_HOST_TOOLS) when NEVR_QUEST_HOST_TESTS is ON,
# which the quest-host-* configure presets in src/quest/CMakePresets.json set. One preset per `just test-quest-*` recipe: its own build tree (build/quest-host-<group>), so
# the recipes `just verify` runs in parallel never share a ninja directory; its build preset builds the target
# host-tests-<group>; its test preset runs the tests labelled <group>. Every group is defined in every tree, and a recipe
# builds only its own.
#
# The flags are the ones the recipes' compiler lines had, per target: -std=c++17 (no GNU extensions), -Wall -Wextra -Werror,
# -Isrc -Isdk/include, and what a line added (-fno-exceptions where the sentinel is built that way, -pthread, a sanitizer,
# -isystem for the host dependency tree). A plain build type: no -O, no -DNDEBUG, as before.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF) # no source is a module; a ccache launcher refuses a compile that carries the scan flags
set(THREADS_PREFER_PTHREAD_FLAG ON)

set(NEVR_HT_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

# ── The host dependency tree ────────────────────────────────────────────────────────────────────────────────────────────
# nlohmann-json, yaml-cpp and zlib come from the tools/quest-host-deps manifest, installed for the x64-linux triplet by
# `just _vcpkg-quest-host` into build/quest-host-vcpkg; never a system package. libcurl, openssl and zstd are the host's
# development files (pkg-config libcurl; -lssl -lcrypto; -lzstd), as the recipes always took them.
set(NEVR_HOST_PREFIX "" CACHE PATH "The x64-linux vcpkg tree of tools/quest-host-deps (the quest-host-* presets set it)")
# NEVR_HOST_JSON_INCLUDE (the directory that holds nlohmann/json.hpp) is the host tools' cache variable, declared in
# src/quest/host/CMakeLists.txt, which enters this directory; a recipe may pass another tree.
set(NEVR_HOST_INCLUDE "${NEVR_HOST_PREFIX}/include")
get_filename_component(NEVR_JSON_INCLUDE "${NEVR_HOST_JSON_INCLUDE}" ABSOLUTE BASE_DIR "${NEVR_REPO_ROOT}")
find_library(NEVR_ZLIB_LIB NAMES libz.a PATHS "${NEVR_HOST_PREFIX}/lib" NO_DEFAULT_PATH)
find_library(NEVR_YAML_LIB NAMES libyaml-cpp.a PATHS "${NEVR_HOST_PREFIX}/lib" NO_DEFAULT_PATH)
foreach(dep ZLIB YAML)
  if(NOT NEVR_${dep}_LIB OR NOT EXISTS "${NEVR_JSON_INCLUDE}/nlohmann/json.hpp")
    message(FATAL_ERROR "quest host tests: the host dependency tree ${NEVR_HOST_PREFIX} is not installed "
            "(libz.a, libyaml-cpp.a and nlohmann/json.hpp under ${NEVR_JSON_INCLUDE}); 'just _vcpkg-quest-host' installs it")
  endif()
endforeach()

find_package(PkgConfig)
if(PkgConfig_FOUND)
  pkg_check_modules(NEVR_CURL IMPORTED_TARGET libcurl)
endif()
find_library(NEVR_ZSTD_LIB NAMES zstd)
if(NOT NEVR_ZSTD_LIB)
  set(NEVR_ZSTD_LIB zstd) # the link then fails with the linker's own message, as -lzstd did
endif()
find_program(NEVR_READELF readelf)
find_program(NEVR_PYTHON3 python3)

add_library(nevr_ht_base INTERFACE)
target_include_directories(nevr_ht_base INTERFACE "${NEVR_REPO_ROOT}/src" "${NEVR_REPO_ROOT}/sdk/include")
target_compile_options(nevr_ht_base INTERFACE -Wall -Wextra -Werror)

# ── Helpers ─────────────────────────────────────────────────────────────────────────────────────────────────────────────
# Paths of sources and include directories are given relative to the repository root, as the recipes' lines gave them.
set(_NEVR_HT_BOOL NOEXC PTHREAD TSAN HOST CURL DL NO_TEST EXCLUDE_FROM_GROUP)
set(_NEVR_HT_ONE GROUP TIMEOUT OUTPUT_NAME FIXTURE_SETUP FIXTURE_REQUIRED RESOURCE_LOCK)
set(_NEVR_HT_MULTI SRC OBJS DEFS INCS SYSINCS OPTS COPTS LOPTS LIBS ARGS ENV DEPENDS BUILD_RPATH)

# One aggregate target per recipe: the build preset of quest-host-<group> builds it.
function(nevr_ht_group group)
  add_custom_target(host-tests-${group})
endfunction()

function(_nevr_ht_sources out)
  set(list "")
  foreach(s IN LISTS A_SRC)
    if(IS_ABSOLUTE "${s}")
      list(APPEND list "${s}")
    else()
      list(APPEND list "${NEVR_REPO_ROOT}/${s}")
    endif()
  endforeach()
  foreach(o IN LISTS A_OBJS)
    list(APPEND list "$<TARGET_OBJECTS:${o}>")
  endforeach()
  set(${out} "${list}" PARENT_SCOPE)
endfunction()

# The per-target flags of a compiler line. kind is EXE, SO or OBJ.
function(_nevr_ht_apply tgt kind)
  target_link_libraries(${tgt} PRIVATE nevr_ht_base)
  if(A_NOEXC)
    target_compile_options(${tgt} PRIVATE -fno-exceptions)
  endif()
  if(A_PTHREAD)
    target_compile_options(${tgt} PRIVATE -pthread)
  endif()
  if(A_TSAN)
    target_compile_options(${tgt} PRIVATE -fsanitize=thread -O1 -g)
  endif()
  if(A_COPTS)
    target_compile_options(${tgt} PRIVATE ${A_COPTS})
  endif()
  if(A_OPTS)
    target_compile_options(${tgt} PRIVATE ${A_OPTS})
  endif()
  if(A_DEFS)
    target_compile_definitions(${tgt} PRIVATE ${A_DEFS})
  endif()
  foreach(inc IN LISTS A_INCS)
    if(NOT IS_ABSOLUTE "${inc}")
      set(inc "${NEVR_REPO_ROOT}/${inc}")
    endif()
    target_include_directories(${tgt} PRIVATE "${inc}")
  endforeach()
  if(A_SYSINCS)
    target_include_directories(${tgt} SYSTEM PRIVATE ${A_SYSINCS})
  endif()
  if(A_HOST)
    target_include_directories(${tgt} SYSTEM PRIVATE "${NEVR_HOST_INCLUDE}" "${NEVR_JSON_INCLUDE}")
  endif()
  if(A_CURL)
    if(TARGET PkgConfig::NEVR_CURL)
      target_link_libraries(${tgt} PRIVATE PkgConfig::NEVR_CURL)
    elseif(NOT kind STREQUAL "OBJ")
      target_link_libraries(${tgt} PRIVATE curl) # no pkg-config file: the plain -lcurl
    endif()
  endif()
  if(NOT kind STREQUAL "OBJ")
    # Programs and fixture libraries of a recipe sit together in build/quest-host-<tree>/<group>/, where the tests that load
    # fixtures are told to look (their first argument).
    set_target_properties(${tgt} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/${A_GROUP}"
                          LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/${A_GROUP}")
    if(A_PTHREAD)
      target_link_options(${tgt} PRIVATE -pthread)
    endif()
    if(A_TSAN)
      target_link_options(${tgt} PRIVATE -fsanitize=thread)
    endif()
    if(A_OPTS)
      target_link_options(${tgt} PRIVATE ${A_OPTS})
    endif()
    if(A_LOPTS)
      target_link_options(${tgt} PRIVATE ${A_LOPTS})
    endif()
    if(A_LIBS)
      target_link_libraries(${tgt} PRIVATE ${A_LIBS})
    endif()
    if(A_DL)
      target_link_libraries(${tgt} PRIVATE ${CMAKE_DL_LIBS})
    endif()
    if(A_BUILD_RPATH)
      set_target_properties(${tgt} PROPERTIES BUILD_RPATH "${A_BUILD_RPATH}")
    endif()
  endif()
  if(A_OUTPUT_NAME)
    set_target_properties(${tgt} PROPERTIES OUTPUT_NAME "${A_OUTPUT_NAME}")
  endif()
  if(A_DEPENDS)
    add_dependencies(${tgt} ${A_DEPENDS})
  endif()
  if(A_GROUP AND NOT A_EXCLUDE_FROM_GROUP)
    add_dependencies(host-tests-${A_GROUP} ${tgt})
  endif()
endfunction()

function(_nevr_ht_register name)
  if(NOT A_TIMEOUT)
    set(A_TIMEOUT 600) # a hang is a failure, not a stuck gate
  endif()
  add_test(NAME ${name} COMMAND ${ARGN} ${A_ARGS} WORKING_DIRECTORY "${NEVR_REPO_ROOT}")
  set_tests_properties(${name} PROPERTIES LABELS "${A_GROUP}" TIMEOUT ${A_TIMEOUT})
  if(A_ENV)
    set_tests_properties(${name} PROPERTIES ENVIRONMENT "${A_ENV}")
  endif()
  if(A_FIXTURE_SETUP)
    set_tests_properties(${name} PROPERTIES FIXTURES_SETUP "${A_FIXTURE_SETUP}")
  endif()
  if(A_FIXTURE_REQUIRED)
    set_tests_properties(${name} PROPERTIES FIXTURES_REQUIRED "${A_FIXTURE_REQUIRED}")
  endif()
  if(A_RESOURCE_LOCK)
    set_tests_properties(${name} PROPERTIES RESOURCE_LOCK "${A_RESOURCE_LOCK}")
  endif()
endfunction()

# A test program: one executable, one ctest test of the same name run from the repository root.
function(nevr_ht_test name)
  cmake_parse_arguments(A "${_NEVR_HT_BOOL}" "${_NEVR_HT_ONE}" "${_NEVR_HT_MULTI}" ${ARGN})
  _nevr_ht_sources(srcs)
  add_executable(${name} ${srcs})
  _nevr_ht_apply(${name} EXE)
  if(NOT A_NO_TEST)
    _nevr_ht_register(${name} ${name})
  endif()
endfunction()

# A second ctest test of an executable that is already a target (the same program with other arguments).
function(nevr_ht_rerun test_name exe)
  cmake_parse_arguments(A "${_NEVR_HT_BOOL}" "${_NEVR_HT_ONE}" "${_NEVR_HT_MULTI}" ${ARGN})
  _nevr_ht_register(${test_name} ${exe})
endfunction()

# An object library: sources compiled once with one set of flags and linked into several programs.
function(nevr_ht_obj name)
  cmake_parse_arguments(A "${_NEVR_HT_BOOL}" "${_NEVR_HT_ONE}" "${_NEVR_HT_MULTI}" ${ARGN})
  _nevr_ht_sources(srcs)
  add_library(${name} OBJECT ${srcs})
  _nevr_ht_apply(${name} OBJ)
endfunction()

# A fixture shared object (lib<OUTPUT_NAME>.so with a build ID, as the tests that load it expect).
function(nevr_ht_so name)
  cmake_parse_arguments(A "${_NEVR_HT_BOOL}" "${_NEVR_HT_ONE}" "${_NEVR_HT_MULTI}" ${ARGN})
  _nevr_ht_sources(srcs)
  add_library(${name} SHARED ${srcs})
  _nevr_ht_apply(${name} SO)
  target_link_options(${name} PRIVATE -Wl,--build-id=sha1)
endfunction()

# A check that runs a script or program that is not itself a test target.
function(nevr_ht_check name)
  cmake_parse_arguments(A "${_NEVR_HT_BOOL}" "${_NEVR_HT_ONE}" "${_NEVR_HT_MULTI}" ${ARGN})
  _nevr_ht_register(${name} ${A_SRC})
endfunction()

# A snippet that breaks one rule must fail to compile, with errors that are all the rule's own (scripts/compile_fail.cmake).
# It is excluded from the build: the ctest test builds it and reads the diagnostics.
function(nevr_ht_compile_fail name src pattern)
  cmake_parse_arguments(A "${_NEVR_HT_BOOL}" "${_NEVR_HT_ONE}" "${_NEVR_HT_MULTI}" ${ARGN})
  set(A_SRC "${src}")
  set(A_EXCLUDE_FROM_GROUP ON)
  _nevr_ht_sources(srcs)
  add_library(${name} OBJECT EXCLUDE_FROM_ALL ${srcs})
  _nevr_ht_apply(${name} OBJ)
  _nevr_ht_register(${name}_must_not_compile ${CMAKE_COMMAND}
    -DBUILD_DIR=${CMAKE_BINARY_DIR} -DTARGET=${name} "-DPATTERN=${pattern}" -DSOURCE=${src}
    -P ${NEVR_HT_DIR}/scripts/compile_fail.cmake)
endfunction()

include(${NEVR_HT_DIR}/groups/data.cmake)
include(${NEVR_HT_DIR}/groups/invite.cmake)
include(${NEVR_HT_DIR}/groups/shared.cmake)
include(${NEVR_HT_DIR}/groups/router.cmake)
include(${NEVR_HT_DIR}/groups/tls.cmake)
include(${NEVR_HT_DIR}/groups/hooks.cmake)
include(${NEVR_HT_DIR}/groups/redirect.cmake)
include(${NEVR_HT_DIR}/groups/patcher.cmake)
include(${NEVR_HT_DIR}/groups/social.cmake)
include(${NEVR_HT_DIR}/groups/integration.cmake)
include(${NEVR_HT_DIR}/groups/plugins.cmake)
include(${NEVR_HT_DIR}/groups/autopilot.cmake)
