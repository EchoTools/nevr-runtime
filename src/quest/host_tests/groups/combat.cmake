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
set(_ccore o_combat_got_hook o_combat_hook_log o_combat_hook_report)
set(_cf NOEXC PTHREAD DL INCS ${_csent})
# The resource table-step modules (#2177) and what they share. combat_install.cpp's table names them, so every program that links it takes them,
# and they read the game's levels through the combat tick and the level answer (table_step.cpp), which those programs already link.
set(_ctables src/quest/combat/meshstep.cpp src/quest/combat/instmodel.cpp src/quest/combat/shaderstep.cpp src/quest/combat/tablestep.cpp
             src/quest/combat/table_step.cpp)
# The three modules of #2185 and the component-system lookup they share: every program that links combat_install.cpp links them, its table names their installs.
set(_cmods src/quest/combat/gunconnect.cpp src/quest/combat/aibots.cpp src/quest/combat/weapon_arm.cpp src/quest/combat/gamespace.cpp)

# The gate on the folded combat modules (#2170, docs/adr/0046 rule 5): combat::Active() for every variant, both build kinds and both
# combat.force answers (the whole table), the answer once per process, and its one log line with its reason and consequence.
nevr_ht_test(combat_active_test GROUP combat ${_cf} TIMEOUT 120 OBJS ${_ccore}
  SRC src/quest/tests/combat_active_test.cpp src/quest/combat/combat_active.cpp src/quest/sentinel/build_variant.cpp)
# The two install steps (#2171, docs/adr/0046 rule 6): one registration table per phase in its own order, only when combat::Active() says
# combat runs in this process, one `combat_install` line per phase, a plain release that installs nothing and says so once, the
# not_started record, and the `combat_config` line.
nevr_ht_test(combat_install_test GROUP combat ${_cf} TIMEOUT 120 OBJS ${_ccore} ${_cident} ${_cident}
  SRC src/quest/tests/combat_install_test.cpp src/quest/combat/combat_install.cpp src/quest/combat/combat_active.cpp
      src/quest/combat/heaps.cpp src/quest/combat/pfx.cpp src/quest/combat/pfxtev.cpp src/quest/combat/memsys.cpp src/quest/sentinel/build_variant.cpp
      src/quest/combat/combat_tick.cpp src/quest/combat/combat_level.cpp src/quest/combat/refresh_rate.cpp
      src/quest/combat/headset_gates.cpp ${_ctables} ${_cmods})
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
  SRC src/quest/tests/heap_step_test.cpp src/quest/combat/combat_install.cpp src/quest/combat/combat_active.cpp
      src/quest/combat/heaps.cpp src/quest/combat/pfx.cpp src/quest/combat/pfxtev.cpp src/quest/combat/memsys.cpp src/quest/sentinel/build_variant.cpp
      src/quest/combat/combat_tick.cpp src/quest/combat/combat_level.cpp src/quest/combat/refresh_rate.cpp
      src/quest/combat/headset_gates.cpp ${_ctables} ${_cmods})
# The game-thread tick (#2172, docs/adr/0046; docs/adr/0039): the real thunk and GotHook against the JUMP_SLOT of a libr15 image built in
# memory. The callback table, the vptr check (read once per object, not once per frame), and who owns the slot on each variant (plain release,
# combat, q15t) in both orders against the q15t plugin's own tick code (game_tick.cpp, import_hook.cpp, got_swap.cpp, linked from the plugin):
# the plugin first and then the runner's normal step leaves nothing to refuse (`unexpected_original` is never logged); the reverse order is the
# plugin's refusal.
nevr_ht_test(combat_tick_test GROUP combat ${_cf} TIMEOUT 120 OBJS ${_ccore} ${_cident}
  INCS src/quest/plugins/q15t/src
  SRC src/quest/tests/combat_tick_test.cpp src/quest/combat/combat_install.cpp src/quest/combat/combat_active.cpp
      src/quest/combat/heaps.cpp src/quest/combat/pfx.cpp src/quest/combat/pfxtev.cpp src/quest/combat/memsys.cpp src/quest/sentinel/build_variant.cpp
      src/quest/combat/combat_tick.cpp src/quest/combat/combat_level.cpp src/quest/combat/refresh_rate.cpp
      src/quest/combat/headset_gates.cpp ${_cmods}
      src/quest/plugins/q15t/src/game_tick.cpp src/quest/plugins/q15t/src/import_hook.cpp src/quest/plugins/q15t/src/got_swap.cpp
      src/quest/plugins/q15t/src/safe_read.cpp src/quest/plugins/q15t/src/plugin_log.cpp ${_ctables})
# The refresh-rate rule (#2172, #2000, #1648; docs/adr/0046 rule 3): the decision, the keys, the session from the arena to a combat map and back
# through the tick, the lobbies arena players use, the legacy kill files, the release case (the game's own rate, nothing logged or registered).
nevr_ht_test(refresh_rate_test GROUP combat ${_cf} TIMEOUT 120 OBJS ${_ccore} ${_cident}
  SRC src/quest/tests/refresh_rate_test.cpp src/quest/combat/refresh_rate.cpp src/quest/combat/combat_level.cpp src/quest/combat/combat_tick.cpp
      src/quest/combat/headset_gates.cpp ${_cmods}
      src/quest/combat/combat_install.cpp src/quest/combat/combat_active.cpp src/quest/combat/heaps.cpp src/quest/combat/pfx.cpp src/quest/combat/pfxtev.cpp src/quest/combat/memsys.cpp
      src/quest/sentinel/build_variant.cpp ${_ctables})
# What level is loaded (#2172): the answer every combat module asks, so arena (same APK as combat) is told apart by the level. A combat map, the
# combat lobby and an Echo Arena level, each id checked against the CSymbol64 of its name; the lobbies arena players use are no class; an arena
# level wins; a gamespace of another game, an absurd count and an unreadable game are no level.
nevr_ht_test(combat_level_test GROUP combat ${_cf} TIMEOUT 120
  SRC src/quest/tests/combat_level_test.cpp src/quest/combat/combat_level.cpp)
# The four resource table steps (#2177, docs/adr/0046; docs/quest/combat/meshstep.md and its three siblings): each module's real handler through the
# real thunk, installed by the real GotHook into the slot of a libr15 image built in memory (a JUMP_SLOT for meshstep and instmodel, an ABS64 factory
# word for shaderstep and tablestep), called the way the game calls it. In a combat match the object has growth step 1; in an Echo Arena level, a
# lobby arena players use, the combat lobby, a transition out of combat, with nothing loaded and with no game known it is the game's own; every
# refusal leaves the slot alone; the kill file, the switch, once, the golden lines. got_pinned_test resolves the four relocations and every word.
nevr_ht_test(meshstep_test GROUP combat ${_cf} TIMEOUT 120 OBJS ${_ccore} ${_cident}
  SRC src/quest/tests/meshstep_test.cpp src/quest/combat/meshstep.cpp src/quest/combat/table_step.cpp src/quest/combat/combat_tick.cpp
      src/quest/combat/combat_level.cpp)
nevr_ht_test(instmodel_test GROUP combat ${_cf} TIMEOUT 120 OBJS ${_ccore} ${_cident}
  SRC src/quest/tests/instmodel_test.cpp src/quest/combat/instmodel.cpp src/quest/combat/table_step.cpp src/quest/combat/combat_tick.cpp
      src/quest/combat/combat_level.cpp)
nevr_ht_test(shaderstep_test GROUP combat ${_cf} TIMEOUT 120 OBJS ${_ccore} ${_cident}
  SRC src/quest/tests/shaderstep_test.cpp src/quest/combat/shaderstep.cpp src/quest/combat/table_step.cpp src/quest/combat/combat_tick.cpp
      src/quest/combat/combat_level.cpp)
nevr_ht_test(tablestep_test GROUP combat ${_cf} TIMEOUT 120 OBJS ${_ccore} ${_cident}
  SRC src/quest/tests/tablestep_test.cpp src/quest/combat/tablestep.cpp src/quest/combat/table_step.cpp src/quest/combat/combat_tick.cpp
      src/quest/combat/combat_level.cpp)
# The two factory words together (#2177): shaderstep's ConstructInstance word and tablestep's CreateInstance word are neighbours in one vtable.
nevr_ht_test(shaderstep_tablestep_test GROUP combat ${_cf} TIMEOUT 120 OBJS ${_ccore} ${_cident}
  SRC src/quest/tests/shaderstep_tablestep_test.cpp src/quest/combat/shaderstep.cpp src/quest/combat/tablestep.cpp
      src/quest/combat/table_step.cpp src/quest/combat/combat_tick.cpp src/quest/combat/combat_level.cpp)
# The three headset gates (#2180, docs/adr/0046; docs/adr/0034 for the chain): the real entries through the real GotHook into the two JUMP_SLOTs of a
# libr15 image built in memory, called through the slot from functions the gates' ranges name. Each gate answers its caller and no other; the bot gate
# only while a combat map or the combat lobby is loaded (arena shares the APK); an entry above a handler that was on the slot first keeps answering and
# a handler that asks to join above an entry is refused (#2219); the switches and their reasons, a pinned word that differs, the legacy switch files,
# the golden lines. got_pinned_test resolves both relocations and every pinned word in the real libr15.
nevr_ht_test(headset_gates_test GROUP combat ${_cf} TIMEOUT 120 OBJS ${_ccore} ${_cident}
  SRC src/quest/tests/headset_gates_test.cpp src/quest/combat/headset_gates.cpp src/quest/combat/combat_level.cpp src/quest/combat/combat_tick.cpp)
# The menu gate's library name (headset_gates.cpp LibraryName, its dladdr refusal, called directly: the Bind frame is absent on the host).
nevr_ht_test(headset_gates_library_name_test GROUP combat ${_cf} TIMEOUT 120 OBJS ${_ccore} ${_cident}
  SRC src/quest/tests/headset_gates_library_name_test.cpp src/quest/combat/headset_gates.cpp src/quest/combat/combat_level.cpp
      src/quest/combat/combat_tick.cpp)
# Which of the three gates a run installs, from the four `combat:` keys.
nevr_ht_test(combat_gates_test GROUP combat NOEXC TIMEOUT 120
  SRC src/quest/tests/combat_gates_test.cpp)
# The particle-renderer gate (#2188, #1631; docs/adr/0046): the decision from `combat.pfx`, the gate that reads off until the install step has
# decided, the install line, the plugin's `pfx.off` file not read and warned about, and the scope: open on a combat map only, shut on every Echo Arena
# level, lobby and transition, against a fake CR15Game read by process_vm_readv. The module installs no hook and registers no tick.
nevr_ht_test(pfx_test GROUP combat ${_cf} TIMEOUT 120 OBJS ${_ccore}
  SRC src/quest/tests/pfx_test.cpp src/quest/combat/pfx.cpp src/quest/combat/combat_level.cpp)
# The event-effect decision (#2188, #1642; docs/adr/0046): the switch, the six ids, the non-event pass-through, one line per decision up to the cap,
# the plugin's `pfxtev.off` file not read and warned about, and the scope: an event id is created on a combat map only.
nevr_ht_test(pfxtev_test GROUP combat ${_cf} TIMEOUT 120 OBJS ${_ccore}
  SRC src/quest/tests/pfxtev_test.cpp src/quest/combat/pfxtev.cpp)

# Gun connect (#2185, docs/adr/0046; moved from the q15t plugin's gunconnect): the plugin test's fourteen sections with the same assertions against a fake
# CR15Game, then the switch (off by default, nothing registered), the plugin's kill file not read and warned about, the scope (no Echo Arena level, lobby
# or transition ever reaches the game's functions), the install on a libr15 image built in memory (build id, every pinned word, a name that resolves
# elsewhere, a full tick table) and the host cost of a pass in an arena level. got_pinned_test resolves the symbols and words in the real libr15.
nevr_ht_test(gunconnect_test GROUP combat ${_cf} TIMEOUT 120 OBJS ${_ccore} ${_cident}
  SRC src/quest/tests/gunconnect_test.cpp src/quest/combat/gunconnect.cpp src/quest/combat/gamespace.cpp src/quest/combat/combat_level.cpp
      src/quest/combat/combat_tick.cpp)
# Practice AI bots (#2185; moved from the q15t plugin's aibots): the plugin test's sections, then the switches and the scenario key, the request file in a
# development build only, the Echo Arena scope, the install on an image and the host cost of a look.
nevr_ht_test(aibots_test GROUP combat ${_cf} TIMEOUT 120 OBJS ${_ccore} ${_cident}
  SRC src/quest/tests/aibots_test.cpp src/quest/combat/aibots.cpp src/quest/combat/combat_level.cpp src/quest/combat/combat_tick.cpp)
# The gun hand default (#2185; moved from the q15t plugin's weapon_arm, T-26): the plugin test's sections, a marker the plugin wrote that stops the
# module, then the switch, the scope (a profile with a marker never scans the levels; no hand is set in an arena level), the install on an image.
nevr_ht_test(weapon_arm_test GROUP combat ${_cf} TIMEOUT 120 OBJS ${_ccore} ${_cident}
  SRC src/quest/tests/weapon_arm_test.cpp src/quest/combat/weapon_arm.cpp src/quest/combat/gamespace.cpp src/quest/combat/combat_level.cpp
      src/quest/combat/combat_tick.cpp)
