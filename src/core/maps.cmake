# The custom-map channel's core (docs/adr/0048), in nevr_core, and its tests. Included from src/core/CMakeLists.txt
# after nevr_core is defined; each part of the channel adds its core sources and tests here.

# The PE rules a map's script DLL must meet (core/script_image_rules.h; ADR 0048 decision 6). The ELF half
# (script_image_rules_elf.cpp) is the Quest's: its Quest consumer compiles it, and test_script_image_rules below runs it.
if(WIN32)
  target_sources(nevr_core PRIVATE "${CMAKE_CURRENT_LIST_DIR}/script_image_rules.cpp")
endif()

# ConfigAlias's pure half (core/config_alias.h; ADR 0048 decision 5): the staged and armed tables, the winner rule and
# the interned snapshots the runtime's lookup detour reads.
target_sources(nevr_core PRIVATE "${CMAKE_CURRENT_LIST_DIR}/config_alias.cpp")

# The formats (ADR 0048 decision 1): a guild's signed manifest and a map's package index (core/map_manifest.h), the cache index
# (core/map_index.h), and the minisign verifier (core/minisign_verify.h) over the vendored Monocypher (third_party/monocypher).
# The body sha256 is the portable streaming SHA-256 the Quest data step ships (quest/data/sha256.cpp, ADR 0045).
set(_maps_monocypher_dir "${CMAKE_SOURCE_DIR}/third_party/monocypher/src")
set(_maps_format_sources
  "${CMAKE_CURRENT_LIST_DIR}/map_manifest.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/map_index.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/minisign_verify.cpp"
  "${CMAKE_SOURCE_DIR}/src/quest/data/sha256.cpp")
set(_maps_monocypher_sources "${_maps_monocypher_dir}/monocypher.c" "${_maps_monocypher_dir}/monocypher-ed25519.c")
# Vendored unmodified (third_party/monocypher/UPSTREAM.txt): its warnings are upstream's, so they are off for these two C files
# only, and -Werror stays on for everything of ours.
if(MSVC)
  set_source_files_properties(${_maps_monocypher_sources} PROPERTIES COMPILE_OPTIONS "/W0")
else()
  set_source_files_properties(${_maps_monocypher_sources} PROPERTIES COMPILE_OPTIONS "-w")
endif()
# nevr_core's precompiled header is C++ (pch.h includes <windows.h>): the C files cannot take it, and the format sources include no
# Windows header, as the Quest builds them.
set_source_files_properties(${_maps_format_sources} ${_maps_monocypher_sources} PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
target_sources(nevr_core PRIVATE ${_maps_format_sources} ${_maps_monocypher_sources})
target_include_directories(nevr_core PRIVATE "${_maps_monocypher_dir}")

# The recipe rebuild (core/map_derive.h; ADR 0048): a stock-derived asset rebuilt from the player's own install and merged into
# the map's set, with the one reader of a set file (core/map_set_tables.h). Each frame header is judged by the texture cargo's
# validator (core/cargo_validate.h, ADR 0045), and the memory a rebuild holds is a share of the device's (core/cargo_limits.h),
# read by the one memory reader (core/device_memory.h, whose Win32 half the PC's Env calls). The PC build has no other user of
# those three cargo files, so the map channel brings them into nevr_core. No PCH: device_memory.cpp includes <windows.h> itself,
# after <winsock2.h>, and the others include no Windows header.
set(_maps_derive_sources
  "${CMAKE_CURRENT_LIST_DIR}/map_set_tables.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/map_derive.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/cargo_validate.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/cargo_limits.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/device_memory.cpp")
set_source_files_properties(${_maps_derive_sources} PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
target_sources(nevr_core PRIVATE ${_maps_derive_sources})

# The fetch of one lobby group's maps and the mount of the cached maps (core/map_run.h; ADR 0048, ADR 0045 decisions 7 and 8),
# and the queue of group requests the worker answers (core/map_fetch_queue.h). Platform-free: no PCH.
set(_maps_run_sources
  "${CMAKE_CURRENT_LIST_DIR}/map_run.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/map_fetch_queue.cpp")
set_source_files_properties(${_maps_run_sources} PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
target_sources(nevr_core PRIVATE ${_maps_run_sources})

if(BUILD_TESTING)
  find_package(GTest QUIET)
  find_package(zstd CONFIG QUIET)
endif()
if(BUILD_TESTING AND GTest_FOUND)
  include(GoogleTest)

  # The table, the winner rule, the snapshot cap and the revalidated sublevel set, with Log() captured through the boot
  # sink and operator new counted. No game state.
  add_executable(test_config_alias "${CMAKE_CURRENT_LIST_DIR}/tests/config_alias_test.cpp")
  set_source_files_properties("${CMAKE_CURRENT_LIST_DIR}/tests/config_alias_test.cpp"
    PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
  target_include_directories(test_config_alias PRIVATE "${CMAKE_SOURCE_DIR}/src")
  target_link_libraries(test_config_alias PRIVATE GTest::gtest GTest::gtest_main nevr_core)
  gtest_discover_tests(test_config_alias DISCOVERY_MODE PRE_TEST)

  # The PE and the ELF rules, one must-fail case per rule, over images the test builds. Pure: both rule files are
  # compiled into the test, so the ELF half runs on the PC build too.
  set(_maps_rules_test_sources
    "${CMAKE_CURRENT_LIST_DIR}/tests/script_image_rules_test.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/script_image_rules.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/script_image_rules_elf.cpp")
  add_executable(test_script_image_rules ${_maps_rules_test_sources})
  set_source_files_properties(${_maps_rules_test_sources}
    PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
  target_include_directories(test_script_image_rules PRIVATE "${CMAKE_SOURCE_DIR}/src")
  target_link_libraries(test_script_image_rules PRIVATE GTest::gtest GTest::gtest_main)
  gtest_discover_tests(test_script_image_rules DISCOVERY_MODE PRE_TEST)

  # The manifest, the package index and the minisign verifier: manifests signed at run time with fixed-seed test keys, and one
  # signature the minisign tool made with a key made for the test (tests/vectors/maps/minisign). Reads only tests/vectors/maps;
  # no network, no game file.
  add_executable(test_map_manifest "${CMAKE_CURRENT_LIST_DIR}/tests/map_manifest_test.cpp")
  set_source_files_properties("${CMAKE_CURRENT_LIST_DIR}/tests/map_manifest_test.cpp" PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
  target_compile_definitions(test_map_manifest PRIVATE NEVR_MAP_VECTORS="${CMAKE_SOURCE_DIR}/tests/vectors/maps")
  target_include_directories(test_map_manifest PRIVATE "${CMAKE_SOURCE_DIR}/src" "${_maps_monocypher_dir}")
  target_link_libraries(test_map_manifest PRIVATE GTest::gtest GTest::gtest_main nevr_core nlohmann_json::nlohmann_json)
  gtest_discover_tests(test_map_manifest DISCOVERY_MODE PRE_TEST)

  # The cache index: its JSON round trip with per-guild floors, and its read bound from the device's memory, over the shared
  # fake Env (src/quest/tests/cargo_fake_env.h).
  add_executable(test_map_index "${CMAKE_CURRENT_LIST_DIR}/tests/map_index_test.cpp")
  set_source_files_properties("${CMAKE_CURRENT_LIST_DIR}/tests/map_index_test.cpp" PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
  target_include_directories(test_map_index PRIVATE "${CMAKE_SOURCE_DIR}/src")
  target_link_libraries(test_map_index PRIVATE GTest::gtest GTest::gtest_main nevr_core nlohmann_json::nlohmann_json)
  gtest_discover_tests(test_map_index DISCOVERY_MODE PRE_TEST)

  # The recipe rebuild over the shared fake Env and its MapFs (quest/tests/cargo_fake_env.h, map_fake_fs.h), a fake install, zstd
  # to read the merged set back and the SDK's set parser; the shared recipe vectors are read from the source tree.
  add_executable(test_map_derive "${CMAKE_CURRENT_LIST_DIR}/tests/map_derive_test.cpp")
  set_source_files_properties("${CMAKE_CURRENT_LIST_DIR}/tests/map_derive_test.cpp" PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
  target_include_directories(test_map_derive PRIVATE "${CMAKE_SOURCE_DIR}/src")
  target_compile_definitions(test_map_derive PRIVATE NEVR_RECIPE_VECTORS="${CMAKE_SOURCE_DIR}/tests/vectors/maps/recipes")
  target_link_libraries(test_map_derive PRIVATE GTest::gtest GTest::gtest_main nevr_core nevr::plugin_sdk
    nlohmann_json::nlohmann_json zstd::libzstd)
  gtest_discover_tests(test_map_derive DISCOVERY_MODE PRE_TEST)

  # The fetch, the mount and the request queue over the shared fake Env and its MapFs, a scripted server, a recording Mounter and
  # Deriver, and the real parsers fed group manifests the test signs with fixed-seed keys. No network, no Win32, no game file.
  add_executable(test_map_run "${CMAKE_CURRENT_LIST_DIR}/tests/map_run_test.cpp")
  set_source_files_properties("${CMAKE_CURRENT_LIST_DIR}/tests/map_run_test.cpp" PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
  target_include_directories(test_map_run PRIVATE "${CMAKE_SOURCE_DIR}/src" "${_maps_monocypher_dir}")
  target_link_libraries(test_map_run PRIVATE GTest::gtest GTest::gtest_main nevr_core nevr::plugin_sdk
    nlohmann_json::nlohmann_json)
  gtest_discover_tests(test_map_run DISCOVERY_MODE PRE_TEST)
endif()
