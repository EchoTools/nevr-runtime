# ADR 0002: T-26, a new profile starts with the gun on the right hand

Status: built and host-tested (2026-10-28). Not observed on a headset.

## Decision

The plugin carries the source build's T-26 (the source build's gunconnect "T-26", header lines 58-64, code 496-520, hunk in
`ExclusiveUpdateGame`) as plugin behaviour: it is part of what the source build does for players, so it is on by
default (`Q15T_T26`, ON) and is not an A/B arm. It runs on the existing game tick (`game_tick.h`, the data-page hook on
`CncaGame::ExclusiveUpdateGame`'s JUMP_SLOT) and writes no code word.

## Behaviour (src/quest/plugins/q15t/src/weapon_arm.cpp, src/quest/plugins/q15t/src/gamespace.cpp)

1 Hz on the game thread until done. Profile id = `CR15NetGame::ClientProfile()` "xplatformid". A marker file
`t26arm.<id>` in `/sdcard/Android/data/<pkg>/files/q15t/` (the plugin's own directory; the source build used its own data
directory) means the profile was handled in an earlier launch: nothing is done. Otherwise the local
`R14NetPlayerEquipment` component system is found in the two gamespace tables, held for 3 passes, and then:
- `GetWeaponArm == 1` or profile `weaponarm == 1`: marker only (the player's own right-hand choice stands);
- else `SetWeaponArm(cs, handle, 1)` once: direct when `[gs+0x368]` is set, else through the gamespace's deferred
  method queue (`gs+0x170`), with the functor layout of the station's hand swap;
- the marker is written once `GetWeaponArm` reads 1; after 10 s without that, a WARNING and no marker (the next launch retries).
`<dir>/t26arm.off` turns it off. A missing engine export is one ERROR line and no tick; an unwritable data directory is a
"waiting" line and no set (a set without a marker would repeat every launch).

Differences from the source build, all deliberate: the marker directory; log lines are the plugin host's structured
records (`t26 on|waiting|set|done|gave up|off`), not `T26 ...` text; the slot and vptr constants are the pinned build's
and the plugin refuses any other build id.

## Evidence

- Host test `src/quest/tests/q15t/t26_test.cpp` (13 cases): hold of 3 passes; direct and deferred routes (functor bytes); already-right and
  profile-right paths; marker present; give-up after 10 s; invalid component; wrong vptr, key and gamespace link; no
  netgame; `t26arm.off`; missing export; unwritable directory; one-shot per launch.
- Mutants (`src/quest/tests/q15t/mutants.sh`, 16 for T-26): all killed after one test was tightened (`t26_no_gs_link` first survived).
- NDK r26d build, `tools/check_quest_static_init.sh --plugin`: OK.

## Prediction (written before any headset run)

On a profile with no marker whose gun is on the left (player's profile `weaponarm` not 1):
1. The log shows `t26 on`, then within about 10 s of the local equipment component existing `t26 set` (`why=direct` or
   `deferred`), then `t26 done ... detail=...` (`weaponarm now 1`), and a file `t26arm.<id>` appears in the data directory.
   Falsifier: `t26 set` without `t26 done` and a `t26 gave up` WARNING (the engine call did not take, most likely the
   deferred queue layout), or no `t26 set` after the equipment component exists (`t26 waiting` says which gate).
2. In the match the player holds the gun in the right hand without touching the setting. Falsifier: left hand.
3. The second launch logs `t26 done ... marker present: profile never touched` and changes nothing, including after the
   player has set the gun to the left hand in the menu.
4. A profile that already has the gun on the right gets the marker and no `t26 set`.
5. No crash and no change to frame timing (one bool test per frame once done).
Not known: whether the deferred-queue path (`[gs+0x368] == 0`) is the one a headset takes; the source build logs both
(`direct` / `defer`), and so does the plugin.
