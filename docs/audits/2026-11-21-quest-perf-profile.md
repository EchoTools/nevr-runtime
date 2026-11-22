# Quest performance profile: our hooks, the game, and the reference base

Measured 2026-11-21 on one Quest headset (Android 14, SDK 34), on development builds of v6.19.0 plus measurement commits that
are not on `main`. This record is dated by design (`docs/audits/README.md`): it cites the tree and the builds as they were when
measured. Level names are the game's, from `src/runtime/log/symcache_data.cpp`.

## How it was measured

- **The hook profile build** (`NEVR_QUEST_HOOK_PROFILE`, preset `android-arm64-hook-profile`, `just quest-hook-profile-apk`):
  every sentinel hook is timed on 1 call in 16, with a game-thread calibration of the measurement's own cost (bias about 84 to
  94 ns, wrapper about 12 to 15 ns), and the frame time; one set of `hook_profile_*` records every 10 s. Cost per frame below is
  the sampled handler time less the bias, plus the wrapper, per call, times the calls.
- **simpleperf**, the CPU profiler the headset's OS ships (`/system/bin/simpleperf`), sampling the whole game process with call
  graphs (`simpleperf record --app com.readyatdawn.r15 -g`). It needs an APK that is profileable from the shell
  (`<profileable android:shell="true"/>` under `<application>`); the store APK is not, and `simpleperf` refuses it with
  `Package com.readyatdawn.r15 doesn't exist or isn't debuggable/profileable`. Through `adb shell`, simpleperf prints its result
  lines (`Samples recorded: ...`) only when its output goes to a pipe.
- **The game's own log** for level loads: `[LEVELLOAD] Finished loading level '<id>' in <n> ms`, in logcat. On the Quest the
  runtime's own log (`nevr-<timestamp>.jsonl`) carries only the sentinel's records and none of the game's lines.

| run (pid) | base | variant | what was profiled |
|---|---|---|---|
| 12082 | reference (final-1116) | combat, the v6.19.0 release | not profiled: the release, the stall below |
| 18471 | store | arena | hook profile |
| 23002 | reference (final-1116) | the reference repack's empty stamp | hook profile |
| 8386 | store | arena | hook profile and a 30 s simpleperf sample in the lobby (293,148 samples, 5 lost) |

## Findings

1. **The sentinel log blocks its caller.** A log record is written to logcat, then under one process-wide mutex to the file on
   shared storage (`Emit`, `src/quest/sentinel/sentinel_log.cpp`). In the first 10 s of run 8386 (store base, before a frame
   was drawn) the game's threads spent 23.5 s in total inside our log calls, and one call took 1,066 ms. In run 23002 the
   development build's line per resource request (`resource_request`, 8,642 records in run 8386's log) made our
   `FillResourceFileRequest` handler cost 4,516 µs a call, against 90 µs on the store base and 2 µs of the game's own time in
   that function; that run's `mnu_master` took 24,890 ms against 761 ms on the store base.
2. **Our hooks are within the 200 µs per frame budget in steady play** (ADR 0038): 42 µs a frame at 119.7 fps in run 18471, 120
   to 150 µs a frame in run 8386's lobby. The clock hook is the most called (94 million calls in run 18471); its handler time
   equals the measurement's own bias, so its cost is the wrapper, about 12 ns a call. simpleperf puts `libnevr-runtime.so` at
   1.09% of the game process's samples in run 8386; the largest part of that, `__aarch64_ldadd8_relax` at 0.53%, is atomic
   adds, which the hook profile build makes on every call (not checked by caller).
3. **The game resolves platform functions by name on every call.** `NRadEngine::CNSProvider::UserProviderID` (libr15
   `0x1a2f330` in the reconstruction) loads the function's name, calls `CSysModule::FindMethod` (`bl 0x01008620`, which calls
   `dlsym`) and then calls the result (`blr x20`); it keeps no pointer. In run 8386 `linker64` is 1.60% of the samples and
   `do_dlsym` 0.77%. On the thread with the most of them, half the `dlsym` samples come from `UserProviderID` through
   `CR15NetGame::IsPartyMember` in `CR15NetSocialInteractCS::UpdatePersonalSpace` (once per player), about 20% from
   `CNSProvider::MicDetected` and about 18% from `CNSProvider::VoipDecode`. The sentinel already hooks `FindMethod`
   (`FindMethodHandler`, `src/quest/sentinel/vk_ext_window.cpp`), which only adds one `strcmp`; a cache there would remove the
   lookups, and would have to forget a module the game unloads (`pnsradmatchmaking` is loaded per session).
4. **The game's own work dominates the lobby.** libr15 is 75.6% of run 8386's samples; `CTaskKernel::ExecuteTask` alone 12.7%,
   then physics, skinning and scripts; the GPU driver 2.4%. By thread: `Thread-3` 43%, `OVRA Update #0` 28%, `NativeThread`
   26%. `OVRA Update #0` runs `CTaskKernelThread::Main`, so it is one of the game's task workers, whatever its name.
5. **Two loads of about 24.9 s on the reference base.** Run 12082 (the v6.19.0 release): `mnu_master_mp_ingame` took 24,879 ms,
   and the run's log has one `Deadlock detected` line and one communication timeout line. Run 23002: `mnu_master` took 24,890 ms. Two different
   levels, about the same time.
6. **The command line on the Quest is the launch intent's `args` extra.** `android_main` fills a 256-byte buffer through
   `CSysJava::GetArgs` (the intent's `getStringExtra("args")`), passes it to `CSysInfo::SetCommandLineArgs`, and `Main_` splits it
   with `CMainArgs::Set`. `adb shell am start -n com.readyatdawn.r15/com.oculus.gles3jni.MainActivity -e args "-mp"` started run
   3719 with `CL Args: -mp` and `Beginning multiplayer`. A launch the OS holds behind one of its own dialogs (`Launch is blocked
   because: a Reprojected OS dialog is currently showing`) starts later without the extra (run 8386: `CL Args: `).
7. **The stock Quest lobby has no combat sub-level.** Run 8386 loaded `mpl_lobby_b2` (with `mpl_lobby_b2_lowspec` inside it) and
   `mpl_lobby_b_arena`, and never `mpl_lobby_b_combat`; the lobby shows an empty space where that sub-level would sit. The
   lobby script `libf808c072bb49da47.so` (the store's, md5 `98330bed1f3992cccc1fdd91612688a6`) adds it only when the headset is
   not a Quest (`src/quest/combat/combat_level.h`).
8. **The reference base's `libvrapi.so` is not Meta's.** Store: md5 `71ed8e36`, 265,856 bytes, clang 7.0.2, NEEDED liblog,
   libdl, libc. Reference base: md5 `8b7eab3382f2a848559b7f97667a403c`, 10,249,136 bytes, clang 18.0.3 and LLD 18.0.3,
   NEEDED adds libopenxr_loader, libvulkan, libandroid, libz and libm, and 159 strings name `EchoQuestCombat`; it identifies
   itself as `1.1.40.0 (EchoQuestXR on OpenXR)`. It is the wave-2 runtime (rev 22), not the final-1116 one (`98a1fde8`). Its
   modules patch libr15 in memory, so no hook of ours sees them; only sampling does. On a reference-base run they act whatever
   the variant's combat gate says (`combat_active` was 0 in run 23002, and the base wrote 660 `EchoQuestCombat` lines).

## Not shown

- Why run 8386's hook cost per frame (120 to 150 µs) is higher than run 18471's (42 µs); the player count was not read.
- What the two 24.9 s loads waited on; neither run was sampled.
- Whether `CNSProvider::MicDetected` and `VoipDecode` look their functions up the same way as `UserProviderID`; only
  `UserProviderID` was read in the disassembly.
- Any combat match: every sample here is the menus and the lobby.

## Open work this record points at

- An asynchronous log writer for the sentinel, with a host test that shows a slow file write does not hold the caller, and no
  record per resource request outside development builds.
- A `FindMethod` result cache in the sentinel's existing hook, cleared when a module is unloaded, with a test.
- A recipe that starts the game with `-e args "-mp"`, so a run reaches the lobby with no input.
- Making a development APK profileable from the shell, and a recorder for simpleperf runs: written and tested at `748aca872`,
  `50056722f` and `3d8a5c96b`, not on `main` when this record was written.
