# test-quest-shared: the code the Quest shares with the PC runtime, and the Quest sentinel's own pure parts, on the host.
# src/quest/tests/service_redirect_test.cpp is also compiled for Android by src/quest/CMakeLists.txt, which cannot run it there.

nevr_ht_group(shared)
set(_sentinel src/quest/sentinel)

nevr_ht_test(service_redirect_test GROUP shared
  SRC src/runtime/lifecycle/service_redirect.cpp src/quest/tests/service_redirect_test.cpp)
# The shared crash report (#1605): what it prints, what it writes and in what order, with the platform seams faked.
nevr_ht_test(crash_report_test GROUP shared
  SRC src/core/crash_report.cpp src/quest/tests/crash_report_test.cpp)
# The sender's POSIX file functions (core/crash_send.h, #1983), which the sentinel uses in place of std::filesystem: age order, regular
# report files only, a bounded read, the .sent mark, the install id, and a whole pass with no Files given.
nevr_ht_test(crash_send_test GROUP shared HOST
  SRC src/core/crash_report.cpp src/quest/tests/crash_send_test.cpp)
# The owned-removal guard (core/owned_remove.h, POSIX): the entry grammar, the descriptor walk, and the cases the test file names.
nevr_ht_test(owned_remove_test GROUP shared
  SRC src/core/owned_remove_posix.cpp src/quest/tests/owned_remove_test.cpp)
# The order a test removes its recorded entries in (runtime/tests/run_root_order.h), and the POSIX guard run over that order.
nevr_ht_test(run_root_order_test GROUP shared
  SRC src/core/owned_remove_posix.cpp src/quest/tests/run_root_order_test.cpp)
# P5: the asset cache's index rules (runtime/patch/asset_cache_index.h): which names the cache records and removes, the index text,
# the legacy adoption decision. Header-only: the Win32 cache runs the same functions.
nevr_ht_test(asset_cache_index_test GROUP shared
  SRC src/quest/tests/asset_cache_index_test.cpp)
# The PC mic provider's capture policy (runtime/patch/mic_policy.h): no capture on a server or with -noaudio. Header-only rule.
nevr_ht_test(mic_policy_test GROUP shared
  SRC src/quest/tests/mic_policy_test.cpp)
# The PC DirectInput enumeration rule (runtime/patch/dinput_enumerate_rules.h): enumerated only in desktop mode. Header-only rule.
nevr_ht_test(dinput_enumerate_rules_test GROUP shared
  SRC src/quest/tests/dinput_enumerate_rules_test.cpp)
# P5: the run-log index rules (runtime/log/run_log_index.h) and the retention selection it feeds (runtime/log/log_retention.h).
nevr_ht_test(run_log_index_test GROUP shared
  SRC src/quest/tests/run_log_index_test.cpp)
# #2129: the report's summary names its final file after the rename (sentinel/crash_summary_rules.h). Header-only rule.
nevr_ht_test(crash_summary_rules_test GROUP shared
  SRC src/quest/tests/crash_summary_rules_test.cpp)
# The Quest side's address resolver (crash_report_quest.cpp, #1927): the test includes the file itself; the stub android/log.h is the
# host's, and the sentinel log and build-id reader link for real.
nevr_ht_test(crash_report_quest_test GROUP shared DL INCS src/quest/tests/stub
  SRC src/quest/tests/crash_report_quest_test.cpp src/core/crash_report.cpp src/quest/sentinel/elf_build_id.cpp
      src/quest/sentinel/sentinel_log.cpp)
# The self-update's crash marker (ADR 0024, section 6): a SIGSEGV in a forked child leaves update/crashed only while
# update/pending.json exists, and the marker appends one record to the run's log. The test includes crash_report_quest.cpp itself.
nevr_ht_test(update_crash_marker_test GROUP shared DL INCS src/quest/tests/stub
  SRC src/quest/tests/update_crash_marker_test.cpp src/core/crash_report.cpp src/quest/sentinel/elf_build_id.cpp
      src/quest/sentinel/sentinel_log.cpp)
# The load timing records (#2728): src/runtime/hook/load_timing_rules.h, the header the PC runtime compiles, seen from the Quest build.
nevr_ht_test(load_timing_rules_test GROUP shared HOST
  SRC src/quest/tests/load_timing_rules_test.cpp)
# The shim <-> runtime ABI (core/quest_runtime_abi.h on core/runtime_abi_common.h): the AArch64 layout of both structs as
# static_asserts that restate every offset, the validators, and that the common header compiles with no <windows.h>.
# The self-update start's steps on the Linux host (ADR 0024, sections 4 and 6): a kill at every step of a swap and of a rollback,
# a journal outside the vocabulary, and a link in the update folders. The POSIX edge (fs_posix.cpp, ADR 0024 section 4) is one
# of its sources. The cases are gtest cases (GTEST), run on the POSIX edge.
nevr_ht_test(engine_start_steps_test GROUP shared HOST GTEST PTHREAD
  SRC src/core/update/engine_start.cpp src/core/update/names.cpp src/core/update/fs_posix.cpp src/quest/tests/engine_start_steps_test.cpp)
nevr_ht_test(quest_runtime_abi_test GROUP shared
  SRC src/quest/tests/quest_runtime_abi_test.cpp src/quest/sentinel/build_variant.cpp)
nevr_ht_test(elf_build_id_test GROUP shared
  SRC src/quest/sentinel/elf_build_id.cpp src/quest/tests/elf_build_id_test.cpp)
nevr_ht_test(evr_codec_test GROUP shared HOST
  SRC src/runtime/compat/evr_codec.cpp src/runtime/compat/login_profile.cpp src/quest/tests/evr_codec_test.cpp)
nevr_ht_test(quest_config_test GROUP shared HOST LIBS ${NEVR_YAML_LIB}
  SRC src/runtime/lifecycle/service_redirect.cpp src/quest/sentinel/quest_config.cpp src/core/nevr_config.cpp
      src/quest/tests/quest_config_test.cpp)
# NevrConfig::Set (#2060), the first config.yaml writer: the same nevr_config.cpp the PC compiles, tested through its interface.
nevr_ht_test(nevr_config_set_test GROUP shared HOST PTHREAD LIBS ${NEVR_YAML_LIB}
  SRC src/core/nevr_config.cpp src/quest/tests/nevr_config_set_test.cpp)

# The hardware dump (#335): field schema, procfs/sysfs readers against a fake root, the os section's never-omitted fields, and
# the record table (also under ThreadSanitizer: the libr15 hooks write it from game threads while the dump thread reads it).
set(_hwdump_core src/quest/diag/hwdump_field.cpp src/quest/diag/hwdump_fs.cpp src/quest/diag/hwdump_os.cpp
    src/quest/diag/hwdump_records.cpp src/quest/diag/hwdump_report.cpp src/quest/tests/hwdump_core_test.cpp)
nevr_ht_test(hwdump_core_test GROUP shared HOST PTHREAD SRC ${_hwdump_core})
nevr_ht_test(hwdump_core_test_tsan GROUP shared HOST PTHREAD TSAN SRC ${_hwdump_core})
# The libr15 hook handlers, built like the sentinel's copy (-fno-exceptions): arguments pass through bit for bit, results come back
# unchanged, nothing is read past the game's buffers.
nevr_ht_test(hwdump_handlers_test GROUP shared NOEXC
  SRC src/quest/diag/hwdump_records.cpp src/quest/diag/hwdump_handlers.cpp src/quest/tests/hwdump_handlers_test.cpp)

# Sentinel activation + logging against a stand-in liblog. The test's constructor has priority 102, so it runs before
# activation.cpp's static initializers whatever the link order. The files directory is made new before the test runs (under the
# recipe's run root, NEVR_TEST_TMP, read at configure time; unset, the build tree's sentinel-files/). An existing one is refused.
if(DEFINED ENV{NEVR_TEST_TMP})
  set(_sentinel_files "$ENV{NEVR_TEST_TMP}/sentinel-files")
else()
  string(TIMESTAMP _nevr_ht_stamp "%Y%m%dT%H%M%S" UTC)
  string(RANDOM LENGTH 6 _nevr_ht_rand)
  set(_sentinel_files "${CMAKE_BINARY_DIR}/sentinel-files-${_nevr_ht_stamp}-${_nevr_ht_rand}")
endif()
add_test(NAME sentinel_files_fresh COMMAND ${CMAKE_COMMAND} -DDIR=${_sentinel_files} -P ${NEVR_HT_DIR}/scripts/fresh_dir.cmake)
set_tests_properties(sentinel_files_fresh PROPERTIES FIXTURES_SETUP sentinel_files LABELS shared TIMEOUT ${NEVR_TEST_CEILING_S})
nevr_ht_test(sentinel_host_test GROUP shared HOST FIXTURE_REQUIRED sentinel_files LIBS ${NEVR_YAML_LIB}
  INCS src/quest/tests/stub src/quest/sentinel DEFS "NEVR_QUEST_FILES_DIR=\"${_sentinel_files}\""
  SRC src/quest/tests/sentinel_host_test.cpp src/quest/sentinel/activation.cpp src/quest/sentinel/sentinel_log.cpp
      src/core/crash_report.cpp src/quest/sentinel/quest_config.cpp src/core/nevr_config.cpp src/core/owned_remove_posix.cpp
      src/runtime/lifecycle/service_redirect.cpp)

# The Quest update root and the shim's version record: the pure rules (quest/auth/app_files_rules.h, quest/shim/update_root_rules.h)
# and no other source. The Android side (quest/shim/update_root.cpp) is compiled into the sentinel and checked on a headset.
nevr_ht_test(update_root_test GROUP shared HOST
  SRC src/quest/tests/update_root_test.cpp)

# The platform-neutral token-auth core and the Quest session, under a fake HTTP server and a fake clock. Same sources the NDK
# build compiles (src/quest/CMakeLists.txt).
nevr_ht_test(auth_core_test GROUP shared HOST PTHREAD
  SRC src/core/auth_refresh.cpp src/core/device_auth_flow.cpp src/core/device_poll_response.cpp
      src/quest/auth/session.cpp src/quest/auth/atomic_write.cpp src/quest/auth/file_store.cpp
      src/quest/auth/prompt_presenters.cpp src/quest/auth/prompt_board.cpp
      src/quest/tests/auth_core_test.cpp)
# CurlHttpClient's trust handling against real TLS peers on loopback (test CA, Android-style old-hash CA directory). Host libcurl
# with an OpenSSL backend, libssl and libcrypto.
nevr_ht_test(tls_ca_test GROUP shared HOST PTHREAD LIBS curl ssl crypto
  SRC src/core/auth_refresh.cpp src/core/device_auth_flow.cpp src/core/device_poll_response.cpp
      src/quest/auth/session.cpp src/quest/auth/atomic_write.cpp src/quest/auth/file_store.cpp src/quest/auth/quest_token_auth.cpp
      src/quest/auth/prompt_presenters.cpp src/quest/auth/prompt_board.cpp
      src/quest/auth/curl_http.cpp src/quest/auth/ca_bundle.cpp src/quest/tests/tls_ca_test.cpp)

# The Quest crash report POST (integration/crash_send_step.h, CurlCrashPost) against a loopback HTTP server: the request, the status,
# the transport failures and the refusal of plain http in the sentinel's setting.
nevr_ht_test(crash_send_step_test GROUP shared HOST PTHREAD LIBS curl ssl crypto
  SRC src/quest/integration/crash_send_step.cpp src/quest/auth/curl_http.cpp src/quest/auth/ca_bundle.cpp
      src/quest/tests/crash_send_step_test.cpp)
# Quest login rewrite: the compose half (login_rewrite.cpp, exceptions enabled) and the apply half (login_apply.cpp, built
# -fno-exceptions exactly as on the device, because it runs while game code is live) plus the shared PCVR login builder, against a
# fake CJson. The Android adapter (login_hook.cpp) cannot run on the host; build-android compiles it. The login tests also link the
# prerequisite handlers and the stand-ins (both -fno-exceptions as on the device), because the send gate and the end-to-end flow
# test drive them together with the rewrite.
foreach(f login_apply login_prerequisites login_standin)
  nevr_ht_obj(o_shared_${f} GROUP shared NOEXC DEFS NEVR_QUEST_TESTING SRC src/quest/login/${f}.cpp)
endforeach()
nevr_ht_obj(o_shared_hook_log GROUP shared NOEXC SRC src/quest/sentinel/hook_log.cpp)
nevr_ht_test(login_rewrite_test GROUP shared HOST DEFS NEVR_QUEST_TESTING
  OBJS o_shared_login_apply o_shared_login_prerequisites o_shared_login_standin o_shared_hook_log
  SRC src/quest/login/login_rewrite.cpp src/runtime/compat/login_profile.cpp src/runtime/ext/plugin_manifest.cpp src/quest/tests/login_rewrite_test.cpp)
# Quest login prerequisites: the handler bodies driven through a fake Platform SDK and the game's four callbacks. The install
# (login_prerequisites_install.cpp) needs libpnsovr.so for real slots; build-android compiles it and test-quest-hooks-pinned
# resolves its slots in the real library.
nevr_ht_test(login_prerequisites_test GROUP shared HOST DEFS NEVR_QUEST_TESTING
  OBJS o_shared_login_prerequisites o_shared_login_standin o_shared_hook_log
  SRC src/quest/tests/login_prerequisites_test.cpp)
# The install against a host with no libpnsovr.so loaded: fully partial, fail-safe, idempotent. The thunk-defining TU and the
# backend are -fno-exceptions (callback_thunk.h); the test too. NEVR_QUEST_TESTING: the install's slot seam (login_prerequisites.h)
# is compiled for the test only.
nevr_ht_obj(o_shared_login_prerequisites_install GROUP shared NOEXC DEFS NEVR_QUEST_TESTING INCS ${_sentinel}
  SRC src/quest/login/login_prerequisites_install.cpp)
foreach(f got_hook hook_report)
  nevr_ht_obj(o_shared_${f} GROUP shared NOEXC INCS ${_sentinel} SRC src/quest/sentinel/${f}.cpp)
endforeach()
nevr_ht_test(login_prerequisites_install_test GROUP shared NOEXC DL PTHREAD DEFS NEVR_QUEST_TESTING INCS ${_sentinel}
  OBJS o_shared_login_prerequisites o_shared_login_prerequisites_install o_shared_login_standin
       o_shared_got_hook o_shared_hook_log o_shared_hook_report
  SRC src/quest/tests/login_prerequisites_install_test.cpp)
# The per-process stand-ins and their predicates. NEVR_QUEST_TESTING exposes SetForTest/ResetForTest, which exist only in the test
# build.
nevr_ht_test(login_standin_test GROUP shared NOEXC DEFS NEVR_QUEST_TESTING
  OBJS o_shared_login_standin SRC src/quest/tests/login_standin_test.cpp)
# CSymbol64Hash on bytes >= 0x80 (#657): the vectors are static_asserts and a run-time loop. The NDK compile of the same file is in
# test-android (plain char is unsigned there).
nevr_ht_test(symbol_hash_test GROUP shared SRC src/quest/tests/symbol_hash_test.cpp)
# The same file with plain char unsigned, as under the NDK: it runs the loop with an unsigned char, which the NDK compile in
# test-android only parses. A bare static_cast<uint64_t>(c) fails to compile here.
nevr_ht_test(symbol_hash_test_uchar GROUP shared COPTS -funsigned-char SRC src/quest/tests/symbol_hash_test.cpp)

# The POSIX UpdateFs (core/update/fs_posix.cpp, ADR 0024 section 4): each operation on a temporary nevr root, a link refused at each
# folder and at the file, the lock held by a second process until its timeout, a move that is not a copy, and a move across volumes.
# Not in CORE_SOURCES: the file is POSIX only, and the PC build (MinGW) does not compile it.
nevr_ht_test(fs_posix_test GROUP shared PTHREAD
  SRC src/core/update/fs_posix.cpp src/quest/tests/fs_posix_test.cpp)

# The worker and the check of the engine (ADR 0024, section 3) on the Linux host: the same gtest
# cases as the PC build, against the POSIX fs (fs_posix.cpp) rooted in the system's temporary directory. gtest and nlohmann come from
# the quest-host-deps manifest (HOST, GTEST).
nevr_ht_test(test_update_worker GROUP shared HOST GTEST PTHREAD
  SRC src/runtime/tests/test_update_worker.cpp src/core/update/engine_check.cpp src/core/update/engine_start.cpp
      src/core/update/policy.cpp src/core/update/worker.cpp src/core/update/names.cpp src/core/update/fs_posix.cpp)
# The Quest runtime's start of the update worker (runtime/update_wiring.h): the worker's inputs, the start with a fake core, the frame
# call that reaches the confirmation, and the read of the running runtime.
nevr_ht_test(update_wiring_test GROUP shared HOST DL PTHREAD
  SRC src/quest/tests/update_wiring_test.cpp src/quest/runtime/update_wiring.cpp src/core/update/engine_check.cpp
      src/core/update/policy.cpp src/core/update/worker.cpp src/core/update/names.cpp src/core/update/fs_posix.cpp
      src/quest/data/sha256.cpp)
# How the Quest runtime library hands a record to the shim's log (runtime/host_log_forward.h): the level mappings, the lifted tag and
# message, integer fields as text, a keyless field, and the cap.
nevr_ht_test(host_log_forward_test GROUP shared
  SRC src/quest/tests/host_log_forward_test.cpp)
# The pending decision (ADR 0024, section 6, R2) over a fake UpdateFs: no file system, the same cases as the PC build.
nevr_ht_test(engine_pending_test GROUP shared HOST GTEST PTHREAD
  SRC src/runtime/tests/engine_pending_test.cpp src/core/update/engine_start.cpp src/core/update/names.cpp)
# The map channel's file seam on the Quest (cdn/posix_map_fs.h; docs/adr/0048): the stamp, a held file, the listing, the
# removal and the rename, on real files under the group's own build folder.
set(_shared_env NEVR_TEST_TMP=${CMAKE_BINARY_DIR}/shared)
nevr_ht_test(posix_map_fs_test GROUP shared NOEXC ENV ${_shared_env}
  SRC src/cdn/posix_map_fs.cpp src/quest/tests/posix_map_fs_test.cpp)
# The session map pin's pure part on the Quest (core/map_session_pin.cpp, ADR 0048 decision 9): the vectors and decision rows a Quest
# client meets, the view's replacement and the empty-state gate, over a counting game.
nevr_ht_test(map_session_pin_quest_test GROUP shared NOEXC INCS sdk/include src
  SRC src/quest/tests/map_session_pin_quest_test.cpp src/core/map_session_pin.cpp src/core/map_server_hold.cpp)
# The session's group and the groups the cache keeps (core/map_groups.h; ADR 0048 decisions 15 and 16; ADR 0045 decision 8): the group
# id of a session's game text, the nil group, the bound over each group's record, the latest-first order and the request hook.
nevr_ht_test(map_groups_quest_test GROUP shared NOEXC INCS sdk/include src
  SRC src/quest/tests/map_groups_quest_test.cpp)
# The Quest's libr15 half of the pin (quest/cdn/map_pin_game.cpp), compiled for the host: its layout, its pinned words and its
# call into the core. It is not linked here (it needs the sentinel's libr15 image), so this is a compile check of the Quest half.
nevr_ht_obj(o_map_pin_game GROUP shared NOEXC DEFS NEVR_QUEST_TESTING INCS ${_sentinel} sdk/include src
  SRC src/quest/cdn/map_pin_game.cpp)
# A map's set over a held descriptor on the Quest (quest/sentinel/map_sets.cpp; docs/adr/0048 decision 5): the shared registration
# (core/resource_set_registry.cpp, the registry the PC compiles) with the Android types, its records read back as the sentinel log
# writes them, and the file's mapping released before the registration returns.
nevr_ht_test(map_sets_test GROUP shared NOEXC ENV ${_shared_env} INCS sdk/include src/quest/sentinel
  SRC src/quest/tests/map_sets_test.cpp src/quest/sentinel/map_sets.cpp src/core/resource_set_registry.cpp
      src/core/resource_shadow.cpp src/quest/sentinel/hook_log.cpp src/quest/data/sha256.cpp
  LIBS ${NEVR_ZSTD_LIB})
# The Quest's stock source over a stock tree under the group's build folder (quest/cdn/stock_source.h; docs/adr/0048, the recipes),
# and the shared half (core/stock_source.cpp, which the PC compiles) under a memory figure the test chooses.
nevr_ht_test(stock_source_test GROUP shared HOST NOEXC ENV ${_shared_env} INCS sdk/include
  SRC src/quest/tests/stock_source_test.cpp src/quest/cdn/stock_source.cpp src/core/stock_source.cpp
      src/cdn/posix_map_fs.cpp src/core/device_memory.cpp src/quest/data/zip.cpp
  LIBS ${NEVR_ZSTD_LIB} ${NEVR_ZLIB_LIB})
