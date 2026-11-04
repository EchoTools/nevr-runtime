# test-quest-plugins: the Quest plugin loader on the host (docs/adr/0019-quest-plugin-loader.md, slice 1): the plan, the ELF and
# build-ID checks, the loader against real shared objects built from the fixture plugin (one lib<variant>.so per way a plugin can be
# right or wrong, each with a GNU build ID), the record log, the platform, and the sentinel's binding of it (the counts the
# constructor sequence acts on, once-only planning, the finish that never waits on a running pass). x86-64 fixtures, so the
# loader's machine check is told AArch64 or x86-64 explicitly. No NDK, no Android.

nevr_ht_group(plugins)
set(_sent src/quest/sentinel)
set(_pc HOST INCS ${_sent} src/quest/tests/stub)

# One fixture plugin per way a plugin can be right or wrong: lib<stem>.so named <name>, with the macros it is built with.
function(_plugin_fixture stem name)
  set(defs "FX_NAME=\"${name}\"")
  set(threads "")
  foreach(a IN LISTS ARGN)
    if(a STREQUAL "PTHREAD")
      set(threads PTHREAD)
    else()
      list(APPEND defs "${a}")
    endif()
  endforeach()
  nevr_ht_so(plugins_${stem} GROUP plugins ${_pc} ${threads} OUTPUT_NAME ${stem} DEFS ${defs}
    SRC src/quest/tests/plugin_fixture.cpp)
  set(_plugin_fixtures ${_plugin_fixtures} plugins_${stem} PARENT_SCOPE)
endfunction()
_plugin_fixture(fx_ok ok)
_plugin_fixture(fx_second second)
_plugin_fixture(fx_noids noids FX_NO_IDS)
_plugin_fixture(fx_otherids otherids "FX_IDS=\"3333333333333333333333333333333333333333\"")
_plugin_fixture(fx_initfail initfail FX_INIT_RC=3)
_plugin_fixture(fx_noinfo noinfo FX_NO_INFO)
_plugin_fixture(fx_legacy legacy FX_LEGACY_INIT)
# The slot-hook probes (#2001, round 3): the handler lives in the fixture object, so unmapping it under a chained call crashes.
# slotclaim registers under another plugin's id and fails its init; slotself registers under its own id, and its Shutdown
# unregisters the handler while a call is inside it.
_plugin_fixture(fx_slotclaim slotclaim FX_SLOT_HOOK "FX_SLOT_CLAIM=\"impostor\"" FX_INIT_RC=3 PTHREAD)
_plugin_fixture(fx_slotself slotself FX_SLOT_HOOK FX_SLOT_WAIT FX_INIT_RC=3 PTHREAD)
# slotthread: the init starts a thread that registers under another id (probe d); slotshutdown registers in its Shutdown (B06).
_plugin_fixture(fx_slotthread slotthread FX_SLOT_HOOK FX_SLOT_THREAD "FX_SLOT_CLAIM=\"impostor\"" FX_INIT_RC=3 PTHREAD)
_plugin_fixture(fx_slotshutdown slotshutdown FX_SLOT_HOOK FX_SLOT_SHUTDOWN "FX_SLOT_CLAIM=\"impostor\"" FX_INIT_RC=3 PTHREAD)
# The GOT fixture the slot hook test chains onto: a real module with a PLT slot for fx_add (got_hook_test builds the same).
nevr_ht_so(plugins_gotfx_provider GROUP plugins ${_pc} OUTPUT_NAME gotfx_provider SRC src/quest/tests/got_fixture_provider.cpp)
nevr_ht_so(plugins_gotfx_consumer_now GROUP plugins ${_pc} OUTPUT_NAME gotfx_consumer_now LIBS plugins_gotfx_provider
  LOPTS -Wl,-z,now,-z,relro BUILD_RPATH "\$ORIGIN" SRC src/quest/tests/got_fixture_consumer.cpp)
list(APPEND _plugin_fixtures plugins_gotfx_provider plugins_gotfx_consumer_now)

# The files that include callback_thunk.h (through pinned_targets.h) are built without exceptions, as on the device.
foreach(f sentinel/got_hook sentinel/hook_report plugin/plugin_game_image plugin/input_seam_hook plugin/slot_hook
          plugin/script_redirect sentinel/libr15_identity)
  get_filename_component(n ${f} NAME)
  nevr_ht_obj(o_plugins_${n} GROUP plugins ${_pc} NOEXC SRC src/quest/${f}.cpp)
endforeach()
set(_loader_noexc o_plugins_libr15_identity o_plugins_got_hook o_plugins_hook_report o_plugins_plugin_game_image
    o_plugins_input_seam_hook o_plugins_script_redirect o_plugins_slot_hook)
set(_loader_srcs src/quest/plugin/packaged_plugins.cpp src/quest/plugin/plugin_plan.cpp src/quest/plugin/plugin_image.cpp
    src/quest/plugin/plugin_loader.cpp src/quest/plugin/plugin_record_log.cpp src/quest/plugin/plugin_platform.cpp
    src/quest/data/sha256.cpp src/quest/sentinel/hook_log.cpp src/quest/sentinel/sentinel_log.cpp src/core/crash_report.cpp
    src/quest/patch/libr15_patch_identity.cpp)
nevr_ht_obj(o_plugins_loader GROUP plugins ${_pc} PTHREAD SRC ${_loader_srcs})
nevr_ht_test(plugin_loader_test GROUP plugins ${_pc} PTHREAD DL TIMEOUT 120 ARGS "${CMAKE_BINARY_DIR}/plugins" DEPENDS ${_plugin_fixtures}
  OBJS o_plugins_loader ${_loader_noexc} SRC src/quest/tests/plugin_loader_test.cpp)
nevr_ht_test(packaged_plugins_test GROUP plugins ${_pc} PTHREAD TIMEOUT 120 ARGS src/quest/tests/fixtures
  SRC src/quest/tests/packaged_plugins_test.cpp src/quest/plugin/packaged_plugins.cpp src/quest/plugin/plugin_plan.cpp
      src/runtime/ext/plugin_load_plan.cpp)
nevr_ht_test(plugin_runtime_test GROUP plugins ${_pc} PTHREAD DL TIMEOUT 120 ARGS "${CMAKE_BINARY_DIR}/plugins" DEPENDS ${_plugin_fixtures}
  OBJS o_plugins_loader ${_loader_noexc}
  SRC src/quest/tests/plugin_runtime_test.cpp src/quest/plugin/plugin_runtime.cpp src/runtime/ext/plugin_load_plan.cpp)
# The constructor's attach step (docs/adr/0021 sections 6, 7, 10, 12): the real Attach on the shipped patcher's output laid out in
# host memory, and the plugin identity and build-ID alias it registers. The patcher and its identity are built with exceptions
# (nlohmann::json); the sentinel half is built without, as on the device. The identity objects are the loader's.
foreach(f libr15_patcher libr15_extra_rows)
  nevr_ht_obj(o_plugins_step_${f} GROUP plugins ${_pc} DEFS _GLIBCXX_ASSERTIONS SRC src/quest/patch/${f}.cpp)
endforeach()
nevr_ht_obj(o_plugins_step_libr15_attach_step GROUP plugins ${_pc} NOEXC SRC src/quest/integration/libr15_attach_step.cpp)
foreach(f libr15_attach process_end_hook arm64_reloc)
  nevr_ht_obj(o_plugins_step_${f} GROUP plugins ${_pc} NOEXC DEFS _GLIBCXX_ASSERTIONS SRC src/quest/sentinel/${f}.cpp)
endforeach()
# The loader sources again, built as the test's line builds them: with the standard library's assertions and the tests directory.
nevr_ht_obj(o_plugins_loader_asserts GROUP plugins ${_pc} PTHREAD INCS src/quest/tests DEFS _GLIBCXX_ASSERTIONS SRC ${_loader_srcs})
nevr_ht_test(libr15_attach_step_test GROUP plugins ${_pc} PTHREAD DL INCS src/quest/tests DEFS _GLIBCXX_ASSERTIONS TIMEOUT 120
  ARGS src/quest/tests/fixtures
  OBJS o_plugins_step_libr15_attach_step o_plugins_step_libr15_patcher o_plugins_step_libr15_extra_rows o_plugins_step_libr15_attach
       o_plugins_step_process_end_hook o_plugins_step_arm64_reloc o_plugins_loader_asserts ${_loader_noexc}
  SRC src/quest/tests/libr15_attach_step_test.cpp)
