# new_unlocks_clear: each rule of src/quest/combat/new_unlocks_clear.cpp removed or bent once; src/quest/tests/new_unlocks_clear_test.cpp must fail on
# every one. Sourced by ../mutants.sh, which defines `runm <name> <file> <sed> [<build dir> <test target>]`. The decisions themselves (held,
# re-armed, superseded, dropped) are the shared header's (runtime/patch/new_unlocks_clear_rules.h); its mutants are in its GTest's commit.
F=src/quest/combat/new_unlocks_clear.cpp
# the wrapper: the game's clear runs, with the game's argument, through the word it was called by
runm nuc_original_skipped   $F "s/^  original(netGame);\$/  (void)original;/"
runm nuc_count_offset       $F "s/ReadU64(game + rules::kNewUnlocksCountOffset, &before)/ReadU64(game + rules::kNewUnlocksCountOffset + 8, \&before)/"
runm nuc_user_offset        $F "s/ReadU64(game + rules::kNetGameUserOffset, &user)/ReadU64(game + rules::kNetGameUserOffset + 8, \&user)/"
runm nuc_held_net_game_lost $F "s/    g_heldNetGame = game;/    g_heldNetGame = 0;/"
# the game's emptiness test: asked about user + 0x28, and nonzero means empty
runm nuc_inflight_inverted  $F "s/fn(reinterpret_cast<void\*>(user + rules::kUserInFlightOffset)) == 0/fn(reinterpret_cast<void*>(user + rules::kUserInFlightOffset)) != 0/"
runm nuc_inflight_offset    $F "s/fn(reinterpret_cast<void\*>(user + rules::kUserInFlightOffset)) == 0/fn(reinterpret_cast<void*>(user + rules::kUserFlagsOffset)) == 0/"
# the re-arm: the dirty bit is set, once, on the user the clear was held for
runm nuc_rearm_not_written  $F "s/    __atomic_fetch_or(reinterpret_cast<std::uint32_t\*>(held + rules::kUserFlagsOffset), rules::kUserDirtyBit, __ATOMIC_SEQ_CST);/    (void)held;/"
runm nuc_rearm_wrong_bit    $F "s/rules::kUserDirtyBit, __ATOMIC_SEQ_CST/0x2U, __ATOMIC_SEQ_CST/"
runm nuc_user_not_rechecked $F "s/current != held/current == 0/"
runm nuc_write_not_checked  $F "s/rules::kUserFlagsOffset + sizeof(std::uint32_t), \/\*write=\*\/true);/rules::kUserFlagsOffset + sizeof(std::uint32_t), \/*write=*\/false);/"
# what ends a hold without a scan: the frame hook's net game (session end) and the user pointer in it (logout)
runm nuc_session_end_unseen_read $F "s/  if (liveGame != g_heldNetGame) {/  if (false) {/"
runm nuc_scan_per_frame     $F "s/  const std::uintptr_t liveGame = reinterpret_cast<std::uintptr_t>(invite::frame::NetGame());/  const std::uintptr_t liveGame = g_heldNetGame;/"
# the lines
runm nuc_held_count_lost    $F "s/LogEnd(\"combat_new_unlocks_clear_held\", rules::kMsgHeld, before)/LogEnd(\"combat_new_unlocks_clear_held\", rules::kMsgHeld, 0)/"
runm nuc_dropped_renamed    $F "s/LogEnd(\"combat_new_unlocks_clear_dropped\", rules::kMsgDropped, heldCount)/LogEnd(\"combat_new_unlocks_clear_drop\", rules::kMsgDropped, heldCount)/"
# the install: both words, the pins, the frame work
runm nuc_glob_dat_not_hooked $F "s/InstallWord<1>(lookup, image.base)}/sentinel::GotStatus::kOk}/"
runm nuc_pins_not_checked   $F "s/FirstPinMismatch(base)) {/(false ? FirstPinMismatch(base) : nullptr)) {/"
runm nuc_frame_not_registered $F "s/!invite::frame::RegisterFrameWork(&FrameWork, nullptr)/false \&\& !invite::frame::RegisterFrameWork(\&FrameWork, nullptr)/"
