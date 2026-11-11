# test-quest-hooks: the Quest hook backend and the sentinel's hook handlers on the host. No NDK, no Android.
# The hook handlers are built as the sentinel builds them (-fno-exceptions); the callback_thunk.h header refuses otherwise.
# Fixture shared objects (one per way a hooked module can be laid out) are built beside the tests that load them, and the
# tests find them in their first argument, the build tree.

nevr_ht_group(hooks)
set(_sent src/quest/sentinel)
set(_fx_dir "${CMAKE_BINARY_DIR}/hooks")

# The patched libr15's identity (sentinel::pinned::LibR15PatchedBuildId, docs/adr/0021 section 7): a link of any translation unit
# that calls a pinned_targets.h factory takes these three objects.
nevr_ht_obj(o_hooks_id_libr15_identity GROUP hooks NOEXC INCS ${_sent} SRC src/quest/sentinel/libr15_identity.cpp)
nevr_ht_obj(o_hooks_id_libr15_patch_identity GROUP hooks NOEXC INCS ${_sent} SRC src/quest/patch/libr15_patch_identity.cpp)
nevr_ht_obj(o_hooks_id_sha256 GROUP hooks NOEXC INCS ${_sent} SRC src/quest/data/sha256.cpp)
set(_ident o_hooks_id_libr15_identity o_hooks_id_libr15_patch_identity o_hooks_id_sha256)
# The hook backend, which almost every test links.
foreach(f got_hook hook_log hook_report)
  nevr_ht_obj(o_hooks_${f} GROUP hooks NOEXC INCS ${_sent} SRC src/quest/sentinel/${f}.cpp)
endforeach()
set(_core o_hooks_got_hook o_hooks_hook_log o_hooks_hook_report)
# Flags of the plain handler tests: built without exceptions, with threads and dlopen.
set(_hf NOEXC PTHREAD DL INCS ${_sent})

# ── Type-level gates ────────────────────────────────────────────────────────────────────────────────────────────────────
# The control compiles; each snippet that breaks one rule must fail to compile, with the message that names the rule (not for
# some unrelated reason). The patterns are GCC's wording, which is why the quest-host presets pin g++ (clang words the same
# errors differently).
set(_cf src/quest/tests/compile_fail)
nevr_ht_obj(o_hooks_cf_control GROUP hooks NOEXC INCS ${_sent} SRC ${_cf}/control.cpp)
nevr_ht_obj(o_hooks_cf_row_control GROUP hooks NOEXC INCS ${_sent} SRC ${_cf}/row_control.cpp)
nevr_ht_obj(o_hooks_cf_asm_entry_control GROUP hooks NOEXC INCS ${_sent} SRC ${_cf}/asm_entry_control.cpp)
nevr_ht_compile_fail(cf_fake_thunk ${_cf}/fake_thunk.cpp "error: static assertion failed: InstallThunk requires a CallbackThunk"
  GROUP hooks NOEXC INCS ${_sent})
nevr_ht_compile_fail(cf_row_fake_thunk ${_cf}/row_fake_thunk.cpp "error: static assertion failed: InstallRowThunk requires a CallbackThunk"
  GROUP hooks NOEXC INCS ${_sent})
nevr_ht_compile_fail(cf_direct_record ${_cf}/direct_record.cpp "HookRecord.*is private within this context"
  GROUP hooks NOEXC INCS ${_sent})
nevr_ht_compile_fail(cf_plain_handler ${_cf}/plain_handler.cpp "error: invalid conversion from .*::Handler"
  GROUP hooks NOEXC INCS ${_sent})
nevr_ht_compile_fail(cf_asm_entry_direct_record ${_cf}/asm_entry_direct_record.cpp "AsmEntryRecord.*is private within this context"
  GROUP hooks NOEXC INCS ${_sent})
nevr_ht_compile_fail(cf_asm_entry_raw_install ${_cf}/asm_entry_raw_install.cpp "InstallEntry.*is private within this context"
  GROUP hooks NOEXC INCS ${_sent})

# ── The GOT hook backend ────────────────────────────────────────────────────────────────────────────────────────────────
# Three fixture shared objects (BIND_NOW with RELRO, BIND_NOW without RELRO, lazy) over one provider, and the production GotHook,
# CallbackThunk and core/hook_lifecycle.h driven against them and against images built in memory.
nevr_ht_so(hooks_gotfx_provider GROUP hooks OUTPUT_NAME gotfx_provider INCS ${_sent} SRC src/quest/tests/got_fixture_provider.cpp)
nevr_ht_so(hooks_gotfx_consumer_now GROUP hooks OUTPUT_NAME gotfx_consumer_now INCS ${_sent}
  LIBS hooks_gotfx_provider LOPTS -Wl,-z,now,-z,relro BUILD_RPATH "\$ORIGIN"
  SRC src/quest/tests/got_fixture_consumer.cpp)
nevr_ht_so(hooks_gotfx_consumer_norelro GROUP hooks OUTPUT_NAME gotfx_consumer_norelro INCS ${_sent}
  LIBS hooks_gotfx_provider LOPTS -Wl,-z,now,-z,norelro BUILD_RPATH "\$ORIGIN"
  SRC src/quest/tests/got_fixture_consumer.cpp)
nevr_ht_so(hooks_gotfx_consumer_lazy GROUP hooks OUTPUT_NAME gotfx_consumer_lazy INCS ${_sent}
  LIBS hooks_gotfx_provider LOPTS -Wl,-z,lazy,-z,norelro BUILD_RPATH "\$ORIGIN"
  SRC src/quest/tests/got_fixture_consumer.cpp)
# The thunk fixture is built WITH exceptions; everything that includes callback_thunk.h is built without (the header refuses
# otherwise).
nevr_ht_obj(o_hooks_thunk_exception_fixture GROUP hooks INCS ${_sent} SRC src/quest/tests/thunk_exception_fixture.cpp)
nevr_ht_test(got_hook_test GROUP hooks NOEXC DL PTHREAD INCS ${_sent} TIMEOUT 300 ARGS "${_fx_dir}"
  DEPENDS hooks_gotfx_consumer_now hooks_gotfx_consumer_norelro hooks_gotfx_consumer_lazy
  OBJS ${_core} o_hooks_thunk_exception_fixture
  SRC src/quest/tests/got_hook_test.cpp src/quest/plugin/slot_hook.cpp)

# ── The sentinel's run-time half of the libr15 build-time patch (docs/adr/0021 test 4) ──────────────────────────────────
# Attach on the shipped patcher's output for a synthetic stock libr15 at the table's real addresses, laid out in host memory
# (text pages r-x); every refusal of section 7, each row check, the trampolines, the activation store, and a protection seam that
# must see no call on the image. The patcher and its identity are built with exceptions (nlohmann::json), the sentinel half and the
# test without. The patcher tests run with the standard library's assertions on.
set(_af HOST PTHREAD INCS ${_sent} DEFS _GLIBCXX_ASSERTIONS)
nevr_ht_obj(o_hooks_attach_patcher GROUP hooks ${_af} SRC src/quest/patch/libr15_patcher.cpp)
nevr_ht_obj(o_hooks_attach_extra_rows GROUP hooks ${_af} SRC src/quest/patch/libr15_extra_rows.cpp)
nevr_ht_obj(o_hooks_attach_patch_identity GROUP hooks ${_af} SRC src/quest/patch/libr15_patch_identity.cpp)
nevr_ht_obj(o_hooks_attach_sha256 GROUP hooks ${_af} SRC src/quest/data/sha256.cpp)
set(_attach_objs o_hooks_attach_patcher o_hooks_attach_extra_rows o_hooks_attach_patch_identity o_hooks_attach_sha256
    o_hooks_id_libr15_identity)
nevr_ht_obj(o_hooks_attach_sentinel GROUP hooks ${_af} NOEXC INCS src/quest/tests
  SRC src/quest/sentinel/libr15_attach.cpp src/quest/sentinel/process_end_hook.cpp src/quest/sentinel/arm64_reloc.cpp
      src/quest/sentinel/got_hook.cpp src/quest/sentinel/hook_log.cpp src/quest/sentinel/hook_report.cpp)
nevr_ht_test(libr15_attach_test GROUP hooks ${_af} NOEXC DL TIMEOUT 300 ARGS src/quest/tests/fixtures
  OBJS ${_attach_objs} o_hooks_attach_sentinel SRC src/quest/tests/libr15_attach_test.cpp)
# The three feature files that take their hooks through rows of the patched libr15 (docs/adr/0021 sections 5 and 7, test 6), and
# the credits install (section 8), each run against the same Attach-verified synthetic image (libr15_live_image.h): the slots the
# installs wrote, the log lines they emitted, the refusals that leave a row pass-through, and no protection change on the image.
nevr_ht_test(row_hooks_install_test GROUP hooks ${_af} NOEXC DL TIMEOUT 300 INCS src/quest/tests
  OBJS ${_attach_objs} o_hooks_attach_sentinel
  SRC src/quest/tests/row_hooks_install_test.cpp src/quest/sentinel/splash_skip_hook.cpp src/quest/sentinel/ui_texture_mips.cpp
      src/quest/sentinel/irradiance_default_hook.cpp src/quest/sentinel/irradiance_default_install.cpp
      src/quest/social/lobby_full_hooks.cpp)
# The lobby-full handlers themselves (#1160): the test includes lobby_full_hooks.cpp and drives OnJoin and OnFailure with a fake
# lobby client and failure message, then reads the game buffer and the captured log lines.
nevr_ht_test(lobby_full_hooks_test GROUP hooks ${_af} NOEXC DL TIMEOUT 60 INCS src/quest/tests
  OBJS ${_attach_objs} o_hooks_attach_sentinel SRC src/quest/tests/lobby_full_hooks_test.cpp)
nevr_ht_test(credits_text_test GROUP hooks ${_af} NOEXC DL TIMEOUT 300 INCS src/quest/tests
  OBJS ${_attach_objs} o_hooks_attach_sentinel
  SRC src/quest/tests/credits_text_test.cpp src/quest/sentinel/credits_text.cpp)

# The variant build (docs/adr/0021 section 11): the sentinel compiled with NEVR_LIBR15_EXTRA_ROWS and the source configure
# generates for the example fixture. The rows it reads back are the parser's, the patched ID it expects is the patcher's for them
# and not the public one, and its digest is the file's. Built without exceptions, as on the device. The generator is the one the
# Quest build runs (cmake/generate_libr15_extra_rows.cmake).
set(_x "${CMAKE_BINARY_DIR}/extra_rows")
file(MAKE_DIRECTORY "${_x}")
function(nevr_ht_extra_rows_source out input)
  add_custom_command(OUTPUT "${out}"
    COMMAND "${CMAKE_COMMAND}" "-DNEVR_EXTRA_ROWS_FILE=${input}" "-DNEVR_EXTRA_ROWS_OUT=${out}"
            -P "${NEVR_REPO_ROOT}/src/quest/cmake/generate_libr15_extra_rows.cmake"
    DEPENDS "${input}" "${NEVR_REPO_ROOT}/src/quest/cmake/generate_libr15_extra_rows.cmake"
    COMMENT "Generating ${out}" VERBATIM)
endfunction()
nevr_ht_extra_rows_source("${_x}/libr15_extra_rows_data.cpp"
  "${NEVR_REPO_ROOT}/src/quest/tests/fixtures/libr15_extra_rows_example.json")
nevr_ht_obj(o_hooks_x_identity GROUP hooks ${_af} NOEXC DEFS NEVR_LIBR15_EXTRA_ROWS=1 SRC src/quest/sentinel/libr15_identity.cpp)
nevr_ht_obj(o_hooks_x_extra_rows GROUP hooks ${_af} NOEXC SRC src/quest/patch/libr15_extra_rows.cpp)
nevr_ht_obj(o_hooks_x_data GROUP hooks ${_af} NOEXC SRC "${_x}/libr15_extra_rows_data.cpp")
nevr_ht_obj(o_hooks_x_hook_log GROUP hooks ${_af} NOEXC SRC src/quest/sentinel/hook_log.cpp)
nevr_ht_test(libr15_extra_rows_identity_test GROUP hooks ${_af} TIMEOUT 120 INCS src/quest/tests ARGS src/quest/tests/fixtures
  OBJS o_hooks_x_identity o_hooks_x_extra_rows o_hooks_x_data o_hooks_x_hook_log o_hooks_attach_patch_identity o_hooks_attach_sha256
  SRC src/quest/tests/libr15_extra_rows_identity_test.cpp)
# An embedded text that does not parse stops the process with the log line and never falls back to the public rows (SIGILL from
# __builtin_trap, exit status 132). Configure parses the file first, so this is a corrupted build; the generator alone embeds
# whatever it is given, which is how the case is made here.
file(WRITE "${_x}/unparsable.json" "{")
nevr_ht_extra_rows_source("${_x}/unparsable_data.cpp" "${_x}/unparsable.json")
nevr_ht_obj(o_hooks_x_unparsable_data GROUP hooks ${_af} NOEXC SRC "${_x}/unparsable_data.cpp")
nevr_ht_test(unparsable_identity_test GROUP hooks ${_af} NO_TEST INCS src/quest/tests
  OBJS o_hooks_x_identity o_hooks_x_extra_rows o_hooks_x_unparsable_data o_hooks_x_hook_log o_hooks_attach_patch_identity
       o_hooks_attach_sha256
  SRC src/quest/tests/libr15_extra_rows_identity_test.cpp)
nevr_ht_check(unparsable_identity_traps GROUP hooks TIMEOUT 60
  SRC ${CMAKE_COMMAND} -DCMD=$<TARGET_FILE:unparsable_identity_test> -DCMD_ARGS=src/quest/tests/fixtures -DEXPECT_RC=SIGILL
  "-DEXPECT_OUT=\"event\":\"libr15_extra_rows\"|\"status\":\"unparsable\"" -P ${NEVR_HT_DIR}/scripts/expect_exit.cmake)
# Every byte value survives the generated source: a file of bytes 0 to 255 is embedded and written back byte for byte.
add_custom_command(OUTPUT "${_x}/all_bytes.bin"
  COMMAND "${NEVR_PYTHON3}" -c "import sys; open(sys.argv[1], 'wb').write(bytes(range(256)))" "${_x}/all_bytes.bin"
  COMMENT "Generating ${_x}/all_bytes.bin" VERBATIM)
nevr_ht_extra_rows_source("${_x}/all_bytes_data.cpp" "${_x}/all_bytes.bin")
nevr_ht_test(embedding_check GROUP hooks NO_TEST HOST INCS ${_sent} "${_x}"
  SRC src/quest/tests/extra_rows_embedding_check.cpp "${_x}/all_bytes_data.cpp")
nevr_ht_check(extra_rows_embedding_is_byte_exact GROUP hooks TIMEOUT 60
  SRC ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:embedding_check> -DEXPECTED=${_x}/all_bytes.bin -DOUT=${_x}/all_bytes.out
  -P ${NEVR_HT_DIR}/scripts/embedding_check.cmake)

# ── Pure parts and handlers through the thunk entries ───────────────────────────────────────────────────────────────────
# The PC-relative relocator (arm64_reloc.h): each kind decoded and then run, original window against the relocated trampoline far
# away, on a small A64 interpreter. Pure; no device.
nevr_ht_test(arm64_reloc_test GROUP hooks NOEXC PTHREAD INCS ${_sent} TIMEOUT 120
  SRC src/quest/tests/arm64_reloc_test.cpp src/quest/sentinel/arm64_reloc.cpp)
# The lobby-full policy (#1001): the #889 cases on the 16-byte lobby key. Pure; no libr15 here.
nevr_ht_test(lobby_full_test GROUP hooks PTHREAD INCS ${_sent} TIMEOUT 60 SRC src/quest/tests/lobby_full_test.cpp)
# The frame dispatcher (#966 S7): posted work runs only on Tick, once, in order; posting from another thread; try-lock contention
# skips without blocking. Built as the sentinel builds it (no exceptions).
nevr_ht_test(frame_dispatch_test GROUP hooks NOEXC PTHREAD INCS ${_sent} TIMEOUT 60
  SRC src/quest/tests/frame_dispatch_test.cpp src/quest/invite/frame_dispatch.cpp src/quest/sentinel/hook_log.cpp)
# The login-prompt hook (#239): the real handler and prompt board, built as the sentinel builds them (no exceptions), driven
# through the thunk entry.
nevr_ht_test(login_prompt_hook_test GROUP hooks ${_hf} TIMEOUT 120 OBJS ${_core} ${_ident}
  SRC src/quest/tests/login_prompt_hook_test.cpp src/quest/sentinel/login_prompt_hook.cpp src/quest/invite/frame_dispatch.cpp
      src/quest/auth/prompt_board.cpp)
# The login-failed page's title (#1137): the real handler and prompt board driven through the thunk entry, against a stand-in for
# the script library's string_t constructor.
nevr_ht_test(login_title_hook_test GROUP hooks ${_hf} TIMEOUT 120 OBJS ${_core} ${_ident}
  SRC src/quest/tests/login_title_hook_test.cpp src/quest/sentinel/login_title_hook.cpp src/quest/auth/prompt_board.cpp)
# The login_host fallback (#1737 C7): the pure rules and the TString handler's fallback step, driven through the thunk entry with
# fakes, built as the sentinel builds the thunk unit (no exceptions).
nevr_ht_test(login_fallback_test GROUP hooks ${_hf} TIMEOUT 60 OBJS ${_core} ${_ident}
  SRC src/quest/tests/login_fallback_test.cpp src/quest/redirect/tstring_thunks.cpp)
# The whole no-credentials chain (#581): the real prerequisite handlers and the real prompt hook together, token auth awaiting the
# player -> the answers delivered -> the game's local failure -> the code written.
nevr_ht_obj(o_hooks_chain_login_prerequisites GROUP hooks NOEXC DEFS NEVR_QUEST_TESTING SRC src/quest/login/login_prerequisites.cpp)
nevr_ht_obj(o_hooks_chain_login_standin GROUP hooks NOEXC DEFS NEVR_QUEST_TESTING SRC src/quest/login/login_standin.cpp)
nevr_ht_test(login_signin_code_chain_test GROUP hooks ${_hf} DEFS NEVR_QUEST_TESTING TIMEOUT 120
  OBJS o_hooks_chain_login_prerequisites o_hooks_chain_login_standin ${_core} ${_ident}
  SRC src/quest/tests/login_signin_code_chain_test.cpp src/quest/sentinel/login_prompt_hook.cpp src/quest/invite/frame_dispatch.cpp
      src/quest/auth/prompt_board.cpp)
# The OBB-mount skip (#319): the real handlers driven through the thunk entries, against a model of the game's callback and wait
# loop.
nevr_ht_test(obb_skip_hook_test GROUP hooks ${_hf} TIMEOUT 120 OBJS ${_core} ${_ident}
  SRC src/quest/tests/obb_skip_hook_test.cpp src/quest/sentinel/obb_skip_hook.cpp)
# The data-path rewrite of the pinned service-only libr15 (#1737): afk prefix to the running package, exact segment only.
nevr_ht_test(path_rewrite_test GROUP hooks NOEXC INCS ${_sent} TIMEOUT 60 SRC src/quest/tests/path_rewrite_test.cpp)
# The process ends when the game's android_main returns (#621): the real handler through the thunk entry, and a real process ended
# under a writer that saves the way the Quest code saves.
nevr_ht_so(hooks_pefx_provider GROUP hooks OUTPUT_NAME pefx_provider INCS ${_sent} SRC src/quest/tests/process_end_fixture_provider.cpp)
nevr_ht_so(hooks_pefx_consumer GROUP hooks OUTPUT_NAME pefx_consumer INCS ${_sent}
  LIBS hooks_pefx_provider LOPTS -Wl,-z,now BUILD_RPATH "\$ORIGIN" SRC src/quest/tests/process_end_fixture_consumer.cpp)
nevr_ht_test(process_end_hook_test GROUP hooks ${_hf} TIMEOUT 300 ARGS "${_fx_dir}" DEPENDS hooks_pefx_consumer
  OBJS ${_core} ${_ident}
  SRC src/quest/tests/process_end_hook_test.cpp src/quest/sentinel/process_end_hook.cpp src/quest/auth/atomic_write.cpp)
# Our crash handler stays first (#1605): the real handler and the sigaction/signal hooks, driven by real signals and a fixture
# library that calls libc's sigaction and signal through PLT slots as libr15 does. The same fixture under the names of the provider
# libraries the late install looks for (loaded after the constructor).
nevr_ht_so(hooks_cffx_consumer GROUP hooks OUTPUT_NAME cffx_consumer INCS ${_sent} LOPTS -Wl,-z,now
  SRC src/quest/tests/crash_first_fixture_consumer.cpp)
foreach(provider pnsrad pnsradmatchmaking pnsovr)
  nevr_ht_so(hooks_cffx_${provider} GROUP hooks OUTPUT_NAME ${provider} INCS ${_sent} LOPTS -Wl,-z,now
    SRC src/quest/tests/crash_first_fixture_consumer.cpp)
endforeach()
nevr_ht_test(crash_first_hook_test GROUP hooks ${_hf} TIMEOUT 120 ARGS "${_fx_dir}"
  DEPENDS hooks_cffx_consumer hooks_cffx_pnsrad hooks_cffx_pnsradmatchmaking hooks_cffx_pnsovr OBJS ${_core} ${_ident}
  SRC src/quest/tests/crash_first_hook_test.cpp src/quest/sentinel/crash_first_hook.cpp)
# The game waits for the storage permission it just asked for (#640): the real handler through the thunk entry, against a stand-in
# for RequestPermissions and a model of CheckPermission.
nevr_ht_test(permission_wait_hook_test GROUP hooks ${_hf} TIMEOUT 120 OBJS ${_core} ${_ident}
  SRC src/quest/tests/permission_wait_hook_test.cpp src/quest/sentinel/permission_wait_hook.cpp)
# The Vulkan extension window (#868): the real wrapper chain through the thunk entries against a fake driver, with the Quest 3
# driver's 137-entry list as the central case.
nevr_ht_test(vk_ext_window_test GROUP hooks ${_hf} TIMEOUT 120 OBJS ${_core} ${_ident}
  SRC src/quest/tests/vk_ext_window_test.cpp src/quest/sentinel/vk_ext_window.cpp)
# The OS build floor (#945): the real handler through the thunk entry against a stand-in for CSysInfo::OSBuildVersion, a number
# below the floor, above it, none, and an unknown floor.
nevr_ht_test(os_build_gate_hook_test GROUP hooks ${_hf} TIMEOUT 120 OBJS ${_core} ${_ident}
  SRC src/quest/tests/os_build_gate_hook_test.cpp src/quest/sentinel/os_build_gate_hook.cpp)
# The platform provider is libpnsrad (#1220): the four real handlers through their thunk entries against stand-ins for
# CNSProvider::Create, Destroy, ProviderID and UserProviderID, CSysModule::Unload and CJson::TString: the name rule, the appid
# kept, the OVR answer after a rename, and ShutdownEngine's three Destroys shutting libpnsrad down once.
nevr_ht_test(pns_provider_hook_test GROUP hooks ${_hf} TIMEOUT 120 OBJS ${_core} ${_ident}
  SRC src/quest/tests/pns_provider_hook_test.cpp src/quest/sentinel/pns_provider_hook.cpp)
# The main-menu QUIT (#1276): the real handler through the thunk entry against stand-ins for CVR::ShowSystemUI and
# ANativeActivity_finish (the activity finished; no app, no activity or no finisher leaves the game's call).
nevr_ht_test(quit_hook_test GROUP hooks ${_hf} TIMEOUT 120 OBJS ${_core} ${_ident}
  SRC src/quest/tests/quit_hook_test.cpp src/quest/sentinel/quit_hook.cpp src/quest/sentinel/process_end_hook.cpp)
# The player's choices are written to config.yaml by NevrConfig::Set (src/core/nevr_config.cpp, built with exceptions against the host
# yaml-cpp, like the shared group): the Texture Resolution control (#2072) and the Controller Hand Flip toggle (#2082, hand_flip.h,
# which the hand pose handler and the texture menu both include). A test that includes hand_flip.h links it and the host yaml-cpp.
nevr_ht_obj(o_hooks_nevr_config GROUP hooks HOST SRC src/core/nevr_config.cpp)
# The controller type and raw pose log (#1986): the real handlers through the thunk entries against stand-ins for
# vrapi_EnumerateInputDevices, vrapi_GetInputTrackingState and vrapi_GetInputDeviceCapabilities. The result and every byte the
# runtime wrote come back untouched; only type 4 enters the table; the lines are golden. got_pinned_test resolves both new slots in
# the real libr15.
nevr_ht_test(hand_pose_log_hook_test GROUP hooks ${_hf} TIMEOUT 120 LIBS ${NEVR_YAML_LIB} OBJS o_hooks_nevr_config ${_core} ${_ident}
  SRC src/quest/tests/hand_pose_log_hook_test.cpp src/quest/sentinel/hand_pose_log_hook.cpp)
# The variant stamp the sentinel reads beside its own library before its first line (#2170, build_variant.h): each word, the empty file,
# a missing file, an unknown or padded word, a file too long, a link and a directory, read from a temporary directory.
nevr_ht_test(build_variant_test GROUP hooks ${_hf} TIMEOUT 120
  SRC src/quest/tests/build_variant_test.cpp src/quest/sentinel/build_variant.cpp)
# The grey default irradiance texture (#2091, #1737 U25): the real handlers through the two thunk entries against models of
# CGEngineResources::Initialize and of the image upload helper. The texel is substituted for the default 3D texture during
# Initialize only; every other call of the helper, and Initialize's status, pass through. got_pinned_test pins the slot and the
# call site in the real libr15; row_hooks_install_test activates the row.
nevr_ht_test(irradiance_default_test GROUP hooks ${_hf} TIMEOUT 120 OBJS ${_core} ${_ident}
  SRC src/quest/tests/irradiance_default_test.cpp src/quest/sentinel/irradiance_default_hook.cpp)
# The Controller Hand Flip toggle, pure half (#1986, KNOWN-WRONG #2043): the roll (y, -x, w, -z), the start from the configuration's
# graphics.hand_flip (#2082), the save through NevrConfig::Set and the golden lines, against a temporary files directory.
nevr_ht_test(hand_flip_test GROUP hooks ${_hf} TIMEOUT 120 LIBS ${NEVR_YAML_LIB} OBJS o_hooks_nevr_config ${_core} ${_ident}
  SRC src/quest/tests/hand_flip_test.cpp)
# libr15's array of free GPU ranges has no bound check (#1759): the real handler through the thunk entry against a model of
# CGRawInterfaceVK::Free laid out as the game's (512 entries, the count after the last), the pinned words refused when one differs,
# and the fill and the leaks in the log.
nevr_ht_test(vk_free_guard_test GROUP hooks ${_hf} TIMEOUT 120 OBJS ${_core} ${_ident}
  SRC src/quest/tests/vk_free_guard_test.cpp src/quest/sentinel/vk_free_guard.cpp)
# A GPU allocation libr15 reports as made with no memory (#2262): the real handler through the thunk entry against a model of
# CGRawInterfaceVK::Allocate that does not read vkAllocateMemory's result and a model of CreateDeviceResource that binds what it
# returns; the pinned words refused when one differs; the refusals in the log with the free-range table's fill.
nevr_ht_test(vk_alloc_check_test GROUP hooks ${_hf} TIMEOUT 120 OBJS ${_core} ${_ident}
  SRC src/quest/tests/vk_alloc_check_test.cpp src/quest/sentinel/vk_alloc_check.cpp src/quest/sentinel/vk_free_guard.cpp)
# The Quest main menu shows the runtime version (#747): the real handler through the thunk entry against a stand-in for
# CCanvasUICS::SetText (the lobby_version item replaced, every other item, an empty text and a null text left alone). The handler
# includes the build-tree header generated/nevr_build_info.h; a stub stands in for it here.
set(_mv "${CMAKE_BINARY_DIR}/menu_version_gen")
file(WRITE "${_mv}/generated/nevr_build_info.h" "namespace nevr_build { inline constexpr const char* kVersion = \"5.17.0\"; }\n")
nevr_ht_test(menu_version_label_test GROUP hooks ${_hf} "${_mv}" TIMEOUT 120 OBJS ${_core} ${_ident}
  SRC src/quest/tests/menu_version_label_test.cpp src/quest/social/menu_version_label.cpp)
# The texture-quality hook (#943): the real handler through the thunk entry against a stand-in for vrapi_GetSystemPropertyInt,
# every level's eye size, the pass-through of every other id, the latch, `stock`, and the shared slot's observer.
nevr_ht_test(texture_quality_hook_test GROUP hooks ${_hf} TIMEOUT 120 OBJS ${_core} ${_ident}
  SRC src/quest/tests/texture_quality_hook_test.cpp src/quest/sentinel/texture_quality_hook.cpp)
# The headset identity (ADR 0031): the rules on device names, the settings each profile holds against the values the code gave before
# the table, and the identity step on a host (no property: the generic headset, one headset_profile line).
nevr_ht_test(headset_profile_test GROUP hooks ${_hf} TIMEOUT 120 OBJS ${_core} ${_ident}
  SRC src/quest/tests/headset_profile_test.cpp src/quest/integration/headset_identity_step.cpp src/quest/sentinel/texture_quality_hook.cpp)
# The reference libr15's eye-size route, pure half (#1737 C4): the RefreshRTSize body classifier, the reference start index's
# parse, the index-to-level map and the float truncation. No game, no hook.
nevr_ht_test(refresh_eye_size_test GROUP hooks NOEXC INCS ${_sent} TIMEOUT 120 SRC src/quest/tests/refresh_eye_size_test.cpp)
# The Texture Resolution control (#1012), pure half: the two settings documents rewritten in memory, the saved value read back, the
# level <-> menu index mapping. Built with exceptions, like quest_config.cpp; no game header.
nevr_ht_test(texture_menu_test GROUP hooks HOST INCS ${_sent} TIMEOUT 120
  SRC src/quest/tests/texture_menu_test.cpp src/quest/sentinel/texture_menu.cpp)
# The control's five handlers and the live apply (#1012) through their thunk entries, against a stand-in engine, resize, VR flags,
# settings, validator and JSON reader: the engine's order, what it waits for, what it refuses, a failed rebuild, the first
# delivery, the saved level, the two documents, the clamp and the log. The hook is built without exceptions as on the device; the
# rewrite it calls keeps them (it runs before the game is called: NEVR_OUTSIDE_GAME_CALL).
nevr_ht_obj(o_hooks_texture_menu_hook GROUP hooks NOEXC PTHREAD HOST INCS ${_sent} SRC src/quest/sentinel/texture_menu_hook.cpp)
nevr_ht_obj(o_hooks_texture_menu GROUP hooks HOST INCS ${_sent} SRC src/quest/sentinel/texture_menu.cpp)
# (o_hooks_nevr_config is defined with the hand pose tests above.)
nevr_ht_test(texture_menu_hook_test GROUP hooks ${_hf} TIMEOUT 120 LIBS ${NEVR_YAML_LIB}
  OBJS o_hooks_texture_menu_hook o_hooks_texture_menu o_hooks_nevr_config ${_core} ${_ident}
  SRC src/quest/tests/texture_menu_hook_test.cpp src/quest/sentinel/texture_quality_hook.cpp)
# The hardware dump on the shared vrapi_GetSystemPropertyInt slot (#943): owned by the texture-quality hook, the dump installs
# nothing there and records the runtime's own answers through the observer.
nevr_ht_test(hwdump_share_test GROUP hooks ${_hf} TIMEOUT 120 OBJS ${_core} ${_ident}
  SRC src/quest/tests/hwdump_share_test.cpp src/quest/sentinel/texture_quality_hook.cpp src/quest/diag/hwdump_install.cpp
      src/quest/diag/hwdump_handlers.cpp src/quest/diag/hwdump_records.cpp)
# The render-configuration log (#1139): the five real handlers through the thunk entries against stand-ins for the VrApi
# functions, the line at the first frame and on each forced change, the globals read only from the proved image, the poller, and the
# refresh-rate rule the refresh handler asks (#2172: the original gets the combat rule's answer; the tick's re-ask reaches the line).
nevr_ht_test(render_config_hook_test GROUP hooks ${_hf} TIMEOUT 120 OBJS ${_core} ${_ident}
  SRC src/quest/tests/render_config_hook_test.cpp src/quest/sentinel/render_config_hook.cpp
      src/quest/sentinel/texture_quality_hook.cpp src/quest/sentinel/clock_levels_hook.cpp
      src/quest/combat/combat_level.cpp src/quest/combat/refresh_rate.cpp src/quest/combat/combat_tick.cpp src/quest/combat/combat_install.cpp src/quest/combat/script_redirect.cpp src/quest/plugin/script_redirect.cpp src/quest/combat/levelname.cpp src/quest/combat/boosterpool.cpp
      src/quest/combat/combat_active.cpp src/quest/combat/heaps.cpp src/quest/combat/pfx.cpp src/quest/combat/pfxtev.cpp src/quest/combat/memsys.cpp src/quest/combat/sendguard.cpp src/quest/sentinel/build_variant.cpp
      src/quest/combat/headset_gates.cpp src/quest/combat/meshstep.cpp src/quest/combat/instmodel.cpp src/quest/combat/shaderstep.cpp
      src/quest/combat/tablestep.cpp src/quest/combat/table_step.cpp src/quest/combat/gunconnect.cpp src/quest/combat/aibots.cpp
      src/quest/combat/weapon_arm.cpp src/quest/combat/gamespace.cpp src/quest/combat/bvh.cpp src/quest/combat/wallfire.cpp
      src/quest/combat/music.cpp src/quest/combat/sensorwd.cpp src/quest/combat/impactsnd.cpp src/quest/combat/decals.cpp
      src/quest/combat/powerboost.cpp src/quest/combat/hudmeter.cpp src/quest/combat/shieldgamma.cpp src/quest/combat/combat_scope.cpp
      src/quest/combat/ec.cpp src/quest/combat/ecfx.cpp src/quest/sentinel/ec_page.cpp
      src/quest/integration/module_observers.cpp src/quest/combat/dronekill.cpp src/quest/combat/dronemuzzle.cpp src/quest/combat/bullet_velocity_guard.cpp
      src/quest/combat/sound_symbol_guard.cpp
      src/quest/combat/fxtrail.cpp src/quest/combat/voipdrop.cpp src/quest/combat/custctx.cpp src/quest/combat/droneguard.cpp src/quest/combat/peerclamp.cpp
      src/quest/combat/frozen_frame_exit.cpp src/quest/sentinel/quit_hook.cpp src/quest/sentinel/process_end_hook.cpp)
# The shared core of the CDN texture delivery that resource_request_hook.cpp links (docs/adr/0045-cdn-texture-delivery.md).
nevr_ht_obj(o_hooks_cargo_shadow GROUP hooks NOEXC INCS ${_sent} SRC src/core/resource_shadow.cpp)
nevr_ht_obj(o_hooks_cargo_resolver GROUP hooks NOEXC INCS ${_sent} SRC src/core/cargo_resolver.cpp)
# The log-only resource request hook (#871): the real handler through the thunk entry against a stand-in for
# FillResourceFileRequest that fills a request the way the pinned function does.
nevr_ht_test(resource_request_hook_test GROUP hooks ${_hf} TIMEOUT 120 OBJS ${_core} ${_ident} o_hooks_cargo_shadow o_hooks_cargo_resolver
  SRC src/quest/tests/resource_request_hook_test.cpp src/quest/sentinel/resource_request_hook.cpp)
# The CDN texture delivery's decision in that hook (docs/adr/0045-cdn-texture-delivery.md): a key the published shadow holds is filled by
# the game's own function from the shadow, and the shadow of a texture's index 0 answers its index 1. The shadow builder and the pins
# are the shared core, built here as the sentinel builds it.
nevr_ht_test(cargo_hook_test GROUP hooks ${_hf} TIMEOUT 120 OBJS ${_core} ${_ident} o_hooks_cargo_shadow o_hooks_cargo_resolver
  SRC src/quest/tests/cargo_hook_test.cpp src/quest/sentinel/resource_request_hook.cpp)
# The clock-level override (#928): the decision, its switch and its counters. The handler that calls it is the
# render-configuration hook's, tested above.
nevr_ht_test(clock_levels_hook_test GROUP hooks ${_hf} TIMEOUT 120
  SRC src/quest/tests/clock_levels_hook_test.cpp src/quest/sentinel/clock_levels_hook.cpp src/quest/sentinel/hook_log.cpp
      src/quest/sentinel/hook_report.cpp)
# The Quest main-menu cards (#966): the real handler through the thunk entry against a stand-in callback block, a CAllocator with
# the slots FreeResource and the streaming worker use, and the game's callback.
nevr_ht_test(menu_cards_hook_test GROUP hooks ${_hf} TIMEOUT 120
  OBJS ${_core} o_hooks_id_libr15_identity o_hooks_id_libr15_patch_identity
  SRC src/quest/tests/menu_cards_hook_test.cpp src/quest/sentinel/menu_cards_hook.cpp src/quest/data/sha256.cpp)
# The CREATE GAME / JOIN VIA CODE observer on libr15's MetricLog (#966, ADR 0017 slice S2): the real observer through the thunk
# entry, built without exceptions, against a stand-in for the game's MetricLog. The shared parse needs nlohmann::json.
nevr_ht_test(wizard_metric_hook_test GROUP hooks ${_hf} HOST TIMEOUT 120
  OBJS ${_core} o_hooks_id_libr15_identity o_hooks_id_libr15_patch_identity
  SRC src/quest/tests/wizard_metric_hook_test.cpp src/quest/invite/wizard_metric_hook.cpp src/quest/data/sha256.cpp)
# The Quest join (#966 S4, ADR 0017): the staging, the route and repeat gates, and the begin order, against a fake game block at
# the Quest offsets (kQuestLayout). The service's two symbols are stubbed inside the test. Built without exceptions.
nevr_ht_test(invite_join_quest_test GROUP hooks ${_hf} HOST TIMEOUT 120
  OBJS ${_core} o_hooks_id_libr15_identity o_hooks_id_libr15_patch_identity
  SRC src/quest/tests/invite_join_quest_test.cpp src/quest/invite/join_game_quest.cpp src/quest/invite/ui_text_quest.cpp
      src/quest/invite/frame_dispatch.cpp src/quest/data/sha256.cpp)
# The controller-input seam (#1202): the VrApi layout rules, the registration's every refusal and its one owner, and the real
# handler through the thunk entry against stand-ins for vrapi_GetCurrentInputState and vrapi_GetInputDeviceCapabilities.
# got_pinned_test resolves both slots in the real libr15.
nevr_ht_test(input_seam_test GROUP hooks ${_hf} TIMEOUT 120 OBJS ${_core} ${_ident}
  SRC src/quest/tests/input_seam_test.cpp src/quest/plugin/input_seam_hook.cpp src/quest/plugin/script_redirect.cpp
      src/quest/plugin/slot_hook.cpp)
# The script redirect (#1737): the stock-name rules, the all-or-nothing registry against a real directory, the empty registry's
# pass-through and the log lines. Needs no pinned target: hook_log.cpp is its only sentinel dependency.
nevr_ht_test(script_redirect_test GROUP hooks ${_hf} TIMEOUT 120
  SRC src/quest/tests/script_redirect_test.cpp src/quest/plugin/script_redirect.cpp src/quest/sentinel/hook_log.cpp)

# ── Against a fake JNI ──────────────────────────────────────────────────────────────────────────────────────────────────
# The toast presenter (#807): a Java exception thrown at each JNI call in turn, null or not, is never left pending. And the
# permission request with a defined request code (#947): the array of names, the method and the int the variadic call carries.
# jni.h is the NDK's own, copied into the build tree; the tests are defined only when ANDROID_NDK_HOME holds it, and the recipe
# stops, naming the NDK, when it does not.
nevr_ht_group(hooks-jni)
set(_jni_src "$ENV{ANDROID_NDK_HOME}/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/include/jni.h")
if(NOT "$ENV{ANDROID_NDK_HOME}" STREQUAL "" AND EXISTS "${_jni_src}")
  configure_file("${_jni_src}" "${CMAKE_BINARY_DIR}/jni/jni.h" COPYONLY)
  nevr_ht_test(toast_presenter_test GROUP hooks-jni NOEXC PTHREAD DL INCS ${_sent} SYSINCS "${CMAKE_BINARY_DIR}/jni" TIMEOUT 60
    SRC src/quest/tests/toast_presenter_test.cpp src/quest/sentinel/toast_presenter.cpp src/quest/sentinel/hook_log.cpp)
  nevr_ht_test(permission_request_test GROUP hooks-jni NOEXC PTHREAD INCS ${_sent} SYSINCS "${CMAKE_BINARY_DIR}/jni" TIMEOUT 60
    SRC src/quest/tests/permission_request_test.cpp src/quest/sentinel/permission_request.cpp)
endif()
