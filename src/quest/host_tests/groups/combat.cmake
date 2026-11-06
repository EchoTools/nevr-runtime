# test-quest-combat: the combat area's host tests (src/quest/combat, docs/adr/0046-combat-is-part-of-the-quest-runtime.md). Built like the
# hooks group: the combat code is compiled without exceptions, as the sentinel links it, against the sentinel's hook log and GOT backend.
# No NDK, no Android.
#
# One program per module, named for the module (`just test-quest-combat <module>` runs the tests whose name holds it), and the area's
# own programs beside them: the gate (combat_active_test) and the two install steps (combat_install_test). A module's pinned-slot test
# is its own file (docs/adr/0046 rule 4 of the module rules in src/quest/combat/README.md); the arm64 checks of a module are
# src/quest/combat/check/<module>.sh, which the recipe runs after the host tests.

nevr_ht_group(combat)
set(_csent src/quest/sentinel)
# The patched libr15's identity (sentinel::pinned::LibR15PatchedBuildId, docs/adr/0021 section 7): a link of any translation unit that
# calls a pinned_targets.h factory takes these three objects.
nevr_ht_obj(o_combat_id_libr15_identity GROUP combat NOEXC INCS ${_csent} SRC src/quest/sentinel/libr15_identity.cpp)
nevr_ht_obj(o_combat_id_libr15_patch_identity GROUP combat NOEXC INCS ${_csent} SRC src/quest/patch/libr15_patch_identity.cpp)
nevr_ht_obj(o_combat_id_sha256 GROUP combat NOEXC INCS ${_csent} SRC src/quest/data/sha256.cpp)
set(_cident o_combat_id_libr15_identity o_combat_id_libr15_patch_identity o_combat_id_sha256)
# The hook backend, which every module test links.
foreach(f got_hook hook_log hook_report)
  nevr_ht_obj(o_combat_${f} GROUP combat NOEXC INCS ${_csent} SRC src/quest/sentinel/${f}.cpp)
endforeach()
# The files folder, weakly (a test that names a folder of its own defines it and wins): the modules that moved from the q15t plugin name it.
nevr_ht_obj(o_combat_files_dir GROUP combat NOEXC INCS ${_csent} SRC src/quest/tests/files_dir_stub.cpp)
set(_ccore o_combat_got_hook o_combat_hook_log o_combat_hook_report o_combat_files_dir)
set(_cf NOEXC PTHREAD DL INCS ${_csent})
# The crash and timing guards (#2178) and the level scope they share: every program that links the registration table
# (combat_install.cpp) links these, because the table names them (and the tick and the level answer, which the scope needs).
set(_cguards src/quest/combat/fxtrail.cpp src/quest/combat/voipdrop.cpp src/quest/combat/custctx.cpp src/quest/combat/droneguard.cpp
    src/quest/combat/peerclamp.cpp src/quest/combat/level_scope.cpp)

# The gate on the folded combat modules (#2170, docs/adr/0046 rule 5): combat::Active() for every variant, both build kinds and both
# combat.force answers (the whole table), the answer once per process, and its one log line with its reason and consequence.
nevr_ht_test(combat_active_test GROUP combat ${_cf} TIMEOUT 120 OBJS ${_ccore}
  SRC src/quest/tests/combat_active_test.cpp src/quest/combat/combat_active.cpp src/quest/sentinel/build_variant.cpp)
# The two install steps (#2171, docs/adr/0046 rule 6): one registration table per phase in its own order, only when combat::Active() says
# combat runs in this process, one `combat_install` line per phase, a plain release that installs nothing and says so once, the
# not_started record, and the `combat_config` line.
nevr_ht_test(combat_install_test GROUP combat ${_cf} TIMEOUT 120 OBJS ${_ccore} ${_cident} ${_cident}
  SRC src/quest/tests/combat_install_test.cpp src/quest/combat/combat_install.cpp ${_cguards} src/quest/combat/combat_active.cpp
      src/quest/combat/heaps.cpp src/quest/combat/memsys.cpp src/quest/sentinel/build_variant.cpp
      src/quest/combat/combat_tick.cpp src/quest/combat/combat_level.cpp src/quest/combat/refresh_rate.cpp)
# Combat name tags on by default (#2090): the real handlers through the two constructor thunks' entries against a stand-in for the
# CR15Game constructor that lays the default settings down at this+0x8c08 and this+0x8ce4. Bit 13 only where a copy holds the stock
# 0x1003, the original first, no other byte changed, a saved false not undone, the golden lines. got_pinned_test resolves both slots in
# the real libr15.
nevr_ht_test(match_tag_default_test GROUP combat ${_cf} TIMEOUT 120 OBJS ${_ccore} ${_cident}
  SRC src/quest/tests/match_tag_default_test.cpp src/quest/combat/match_tags.cpp)
# The equipment re-fire (#2121): the real wrappers through the six thunks, installed by the real GotHook into the six words of a libr15
# image built in memory (JUMP_SLOT and GLOB_DAT of the three setters), against models of the setters, the change handlers, NetGameMT,
# CR15NetGame::UserId and the component. The shared rule is the header's GTest; here are the words hooked, each word's own original
# with the game's three arguments, the slot's own handler, the net game probe, a word that is not the setter refused alone, a pin that
# differs refusing the module before any word is touched, the other refusals, and the golden lines. got_pinned_test resolves the six
# relocations and the code pins in the real libr15.
nevr_ht_test(equip_refire_test GROUP combat ${_cf} TIMEOUT 120 OBJS ${_ccore} ${_cident}
  SRC src/quest/tests/equip_refire_test.cpp src/quest/combat/equip_refire.cpp src/quest/invite/frame_dispatch.cpp)
# The four heap sizes (#2175): the real module writing through the real GotDataSlot into the four GLOB_DAT words of a libr15 image built in
# memory, against a model of the reader (`ldr x8,[slot]; ldr x1,[x8]`). All four slots, the Quest 2's System heap, each refusal alone, the two
# switches, once, the golden lines. got_pinned_test resolves the four relocations, their symbol types and the words in the real libr15.
nevr_ht_test(heaps_test GROUP combat ${_cf} TIMEOUT 120 OBJS ${_ccore} ${_cident}
  SRC src/quest/tests/heaps_test.cpp src/quest/combat/heaps.cpp)
# The render-target pool and the android_main witness (#2175): the real wrappers through the two kCaller thunks, installed by the real GotHook
# into the two JUMP_SLOTs of a libr15 image built in memory, called through the slot against a fake Alloc, InitCommon's inline fill and a fake
# CSysJava::Init. The pool is 0..1023 exactly once, the wrong caller and capacity pass through, each unrecognised layout leaves the stock 512,
# the witness once with the System heap the game will read, each refusal alone, the switch, once, the golden lines.
nevr_ht_test(memsys_test GROUP combat ${_cf} TIMEOUT 120 OBJS ${_ccore} ${_cident}
  SRC src/quest/tests/memsys_test.cpp src/quest/combat/memsys.cpp)
# The heap step as the sentinel runs it (#2175): the shipped early table on a libr15 image built in memory against a model of the start of
# android_main. Before the first allocation it governs (the witness precedes the model's heap reads; a late run is the control), once, only in
# the variants the gate allows (and a development build forced on), and each shipped row reads its switch.
nevr_ht_test(heap_step_test GROUP combat ${_cf} TIMEOUT 120 OBJS ${_ccore} ${_cident}
  SRC src/quest/tests/heap_step_test.cpp ${_cguards} src/quest/combat/combat_install.cpp src/quest/combat/combat_active.cpp
      src/quest/combat/heaps.cpp src/quest/combat/memsys.cpp src/quest/sentinel/build_variant.cpp
      src/quest/combat/combat_tick.cpp src/quest/combat/combat_level.cpp src/quest/combat/refresh_rate.cpp)
# The game-thread tick (#2172, docs/adr/0046; docs/adr/0039): the real thunk and GotHook against the JUMP_SLOT of a libr15 image built in
# memory. The callback table, the vptr check (read once per object, not once per frame), and who owns the slot on each variant (plain release,
# combat, q15t) in both orders against the q15t plugin's own tick code (game_tick.cpp, import_hook.cpp, got_swap.cpp, linked from the plugin):
# the plugin first and then the runner's normal step leaves nothing to refuse (`unexpected_original` is never logged); the reverse order is the
# plugin's refusal.
nevr_ht_test(combat_tick_test GROUP combat ${_cf} TIMEOUT 120 OBJS ${_ccore} ${_cident}
  INCS src/quest/plugins/q15t/src
  SRC src/quest/tests/combat_tick_test.cpp ${_cguards} src/quest/combat/combat_install.cpp src/quest/combat/combat_active.cpp
      src/quest/combat/heaps.cpp src/quest/combat/memsys.cpp src/quest/sentinel/build_variant.cpp
      src/quest/combat/combat_tick.cpp src/quest/combat/combat_level.cpp src/quest/combat/refresh_rate.cpp
      src/quest/plugins/q15t/src/game_tick.cpp src/quest/plugins/q15t/src/import_hook.cpp src/quest/plugins/q15t/src/got_swap.cpp
      src/quest/plugins/q15t/src/safe_read.cpp src/quest/plugins/q15t/src/plugin_log.cpp)
# The refresh-rate rule (#2172, #2000, #1648; docs/adr/0046 rule 3): the decision, the keys, the session from the arena to a combat map and back
# through the tick, the lobbies arena players use, the legacy kill files, the release case (the game's own rate, nothing logged or registered).
nevr_ht_test(refresh_rate_test GROUP combat ${_cf} TIMEOUT 120 OBJS ${_ccore} ${_cident}
  SRC src/quest/tests/refresh_rate_test.cpp src/quest/combat/refresh_rate.cpp src/quest/combat/combat_level.cpp src/quest/combat/combat_tick.cpp
      ${_cguards} src/quest/combat/combat_install.cpp src/quest/combat/combat_active.cpp src/quest/combat/heaps.cpp src/quest/combat/memsys.cpp
      src/quest/sentinel/build_variant.cpp)
# What level is loaded (#2172): the answer every combat module asks, so arena (same APK as combat) is told apart by the level. A combat map, the
# combat lobby and an Echo Arena level, each id checked against the CSymbol64 of its name; the lobbies arena players use are no class; an arena
# level wins; a gamespace of another game, an absurd count and an unreadable game are no level.
nevr_ht_test(combat_level_test GROUP combat ${_cf} TIMEOUT 120
  SRC src/quest/tests/combat_level_test.cpp src/quest/combat/combat_level.cpp)

# The level scope (#2178): what the guards that act only in a combat level ask. Published from the combat tick, so a combat map, the combat
# lobby and an Echo Arena level (which wins) are told apart without a read in the handler.
nevr_ht_test(level_scope_test GROUP combat ${_cf} TIMEOUT 120 OBJS ${_ccore} ${_cident}
  SRC src/quest/tests/level_scope_test.cpp src/quest/combat/level_scope.cpp src/quest/combat/combat_level.cpp
      src/quest/combat/combat_tick.cpp src/quest/combat/combat_install.cpp ${_cguards} src/quest/combat/combat_active.cpp src/quest/combat/heaps.cpp
      src/quest/combat/memsys.cpp src/quest/combat/refresh_rate.cpp src/quest/sentinel/build_variant.cpp)
# fxtrail (#2178): the real handler through the thunk, installed into the JUMP_SLOT of a libr15 image built in memory, against a model of
# CParticleEffectCS; acts only while a combat map is loaded (the real level scope against a model of the game's gamespace tables).
nevr_ht_test(fxtrail_test GROUP combat ${_cf} TIMEOUT 120 OBJS ${_ccore} ${_cident}
  SRC src/quest/tests/fxtrail_test.cpp ${_cguards} src/quest/combat/combat_level.cpp src/quest/combat/combat_tick.cpp
      src/quest/combat/combat_install.cpp src/quest/combat/combat_active.cpp src/quest/combat/heaps.cpp src/quest/combat/memsys.cpp
      src/quest/combat/refresh_rate.cpp src/quest/sentinel/build_variant.cpp)

# voipdrop (#2178): the real handler through the thunk, installed into the JUMP_SLOT of a libr15 image built in memory, against a model of the
# receiver's voice table and allocator. The guard acts in every level (it is a crash guard of voice code arena shares), so no level model is asked.
nevr_ht_test(voipdrop_test GROUP combat ${_cf} TIMEOUT 120 OBJS ${_ccore} ${_cident}
  SRC src/quest/tests/voipdrop_test.cpp ${_cguards} src/quest/combat/combat_level.cpp src/quest/combat/combat_tick.cpp
      src/quest/combat/combat_install.cpp src/quest/combat/combat_active.cpp src/quest/combat/heaps.cpp src/quest/combat/memsys.cpp
      src/quest/combat/refresh_rate.cpp src/quest/sentinel/build_variant.cpp)

# custctx (#2178): its test program is added here by its own commit.

# droneguard (#2178): the real handler through the thunk, installed into the JUMP_SLOT of a libr15 image built in memory, against a model of
# CR15NetFollowPlayerCS and fake RefreshCache / IsComponentValid; stage 0 (the null target) and stage 1 (the cache), the switch, the pins, the stats.
nevr_ht_test(droneguard_test GROUP combat ${_cf} TIMEOUT 120 OBJS ${_ccore} ${_cident}
  SRC src/quest/tests/droneguard_test.cpp ${_cguards} src/quest/combat/combat_level.cpp src/quest/combat/combat_tick.cpp
      src/quest/combat/combat_install.cpp src/quest/combat/combat_active.cpp src/quest/combat/heaps.cpp src/quest/combat/memsys.cpp
      src/quest/combat/refresh_rate.cpp src/quest/sentinel/build_variant.cpp)

# peerclamp (#2178): its test program is added here by its own commit.
