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

# The gate on the folded combat modules (#2170, docs/adr/0046 rule 5): combat::Active() for every variant, both build kinds and both
# combat.force answers (the whole table), the answer once per process, and its one log line with its reason and consequence.
nevr_ht_test(combat_active_test GROUP combat ${_cf} TIMEOUT 120 OBJS ${_ccore}
  SRC src/quest/tests/combat_active_test.cpp src/quest/combat/combat_active.cpp src/quest/sentinel/build_variant.cpp)
# The two install steps (#2171, docs/adr/0046 rule 6): one registration table per phase in its own order, only when combat::Active() says
# combat runs in this process, one `combat_install` line per phase, a plain release that installs nothing and says so once, the
# not_started record, and the `combat_config` line.
nevr_ht_test(combat_install_test GROUP combat ${_cf} TIMEOUT 120 OBJS ${_ccore} ${_cident}
  SRC src/quest/tests/combat_install_test.cpp src/quest/combat/combat_install.cpp src/quest/combat/combat_active.cpp
      src/quest/combat/heaps.cpp src/quest/combat/memsys.cpp src/quest/combat/sendguard.cpp src/quest/sentinel/build_variant.cpp)
# Sendguard (#2179): the real wrapper through the thunk's entry into the one JUMP_SLOT of a libr15 image built in memory, against models of
# CSendEventNode::Enter and the GameSpaceMT lookup and the node's context. A send to a game space that is not loaded dropped and the
# stock function not called, a loaded one passed with its own arguments, no level and each null link left to the stock function, the drop
# line cap and the exact count, each of the 13 pinned words refusing the module before the slot is touched, a foreign word and the
# other refusals, the one changed word (no other byte of the image, the unwind record the plugin took for a slot included), the off key
# and the old switch file's Warning, and the table's row reading the resolved configuration. got_pinned_test resolves the relocation
# and the pins in the real libr15.
nevr_ht_test(sendguard_test GROUP combat ${_cf} TIMEOUT 120 OBJS ${_ccore} ${_cident}
  SRC src/quest/tests/sendguard_test.cpp src/quest/combat/sendguard.cpp src/quest/combat/combat_install.cpp
      src/quest/combat/combat_active.cpp src/quest/combat/heaps.cpp src/quest/combat/memsys.cpp src/quest/sentinel/build_variant.cpp)
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
      src/quest/combat/heaps.cpp src/quest/combat/memsys.cpp src/quest/combat/sendguard.cpp src/quest/sentinel/build_variant.cpp)
