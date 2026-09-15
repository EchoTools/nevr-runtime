# Quest "basics" hook — on-device verification plan and log

2026-09-15, Claude + Andrew. Written mid-task, after Andrew asked "where is
that written down" and the honest answer was nowhere — this is the fix, not
a retroactive summary.

## Task, as given

Andrew (2026-09-15, ~05:14 local): don't treat Quest as a separate hacking
project from PCVR nevr-runtime — same engine, probably the same offsets
under a different ISA. "Start with just the basics. Modify the quest build
and deploy it to my headset. I will load into a lobby and you should be
able to verify that your modifications took." In parallel: verify the
existing Quest crash-reporter work is actually reflected in ReVault, use
ReVault to find real hook points, contribute back what's found, fix PCVR
symbol names encountered along the way.

## What "the basics" turned out to be, and why

Checked ReVault first (`docs/design/2026-09-15-server-client-quest-roadmap.md`
already covers the crash-reporter inventory). Confirmed via `revault_projects`
→ `revault_binaries("echovr")` that `libr15.so`, `libpnsrad.so`,
`libpnsradmatchmaking.so`, `libpnsovr.so` are already indexed in ReVault
under the same `echovr` project as the PCVR binaries, named identically to
their PE counterparts — real support for Andrew's "same hacks, different
addresses" hypothesis.

But `revault_functions(libr15.so, named_only=true)` returns only
`sym.imp.*`/`aav.*` auto-names — no application-level reconstruction has
happened on this binary yet. Found the string `"Beginning multiplayer"` at
`libr15.so:0x2ba19ee` (identical wording to echovr.exe's `BeginMultiplayer`
@ `0x1400fda70`), but `revault_xrefs`/`revault_search_code` can't reach the
containing function — no indexed data-xrefs, no decompiled-source hit yet.
Recorded as a ReVault comment + bookmark (category `quest-porting`) rather
than guessed at further tonight.

Given that, hooking a specific *internal* libr15.so function (the direct
PCVR-offset-reuse experiment Andrew described) isn't reachable yet through
ReVault alone — it needs either deeper decompiler coverage or a raw
ADRP/ADD disassembly scan, both bigger asks than "start with the basics."

So "the basics" became: prove the injection mechanism can intercept a real
call the live game makes, using a technique that doesn't depend on that
missing reconstruction at all — **GOT/PLT import hooking**. `src/quest/
sentinel/got_hook.{h,cpp}` (commit `87c68d2`) parses a loaded module's
`PT_DYNAMIC`/`.rela.plt` via `dl_iterate_phdr` + `<elf.h>`, finds the GOT
slot for a named imported symbol, and swaps the pointer for a hook
function — the Bionic/arm64 analogue of the MinHook inline detours used on
Windows PCVR (pointer-swap in a writable data page, not code patching —
ARM64 code pages aren't reliably runtime-writable, and inline patching
needs an architecture-specific trampoline neither of which this needs).

Target: `clock_gettime`, an import `libr15.so` calls (confirmed via
`readelf -r` on the real extracted binary: a genuine `R_AARCH64_JUMP_SLOT`
relocation in `.rela.plt`; confirmed via `readelf -p .dynstr` that the raw
symbol name is exactly `"clock_gettime"`, no version suffix baked in — the
`@LIBC` readelf/nm show is display-only decoration from the version
tables). Chosen for an unambiguous POSIX signature (no risk of a
wrong-arity call corrupting engine state) and because it's called
continuously by any real-time engine loop — a climbing log counter is an
unambiguous "did it take" signal without needing a specific game event.

## Tonight's execution log

1. Built (`just build-android`) — clean, `nevr_sentinel_marker` present,
   correct `NEEDED`/`SONAME` shape.
2. Verified the GOT-hook logic against the real extracted `libr15.so`
   (`/var/tmp/r15_verify/`) before ever touching hardware — see commit
   `87c68d2` message for the exact `readelf` commands and output.
3. Committed (`87c68d2`) — repo convention: commit before build/deploy so
   the working state is deterministic.
4. Device connected (`adb devices` — Quest 2, `hollywood`,
   `1WMHH83A6Q1136`).
5. First repack attempt used the cached store APK from Spritz's July work
   (`/var/tmp/r15_goldmaster_store.apk`, versionCode 4987566) — **install
   refused**: `INSTALL_FAILED_VERSION_DOWNGRADE`, since the headset's
   actual installed version (4987570) is newer (an Oculus store update
   landed since July). Pulled the real installed APK instead
   (`adb pull .../base.apk` → `/var/tmp/r15_installed.apk`, single base
   APK, no split APKs) rather than force a downgrade onto a live install.
6. Repacked against the real installed APK — same verified ELF shape.
7. Install attempt failed as expected on signer mismatch
   (`INSTALL_FAILED_UPDATE_INCOMPATIBLE`, debug keystore vs. the store's
   signing key) — this exact failure mode, and that it requires an
   uninstall (losing local app data — cached login/settings; account/social
   data itself is server-side, not on-device), was surfaced to Andrew
   *before* attempting, as part of the earlier headset-choice question.
8. `adb uninstall com.readyatdawn.r15` → Success. Reinstall of the
   repacked APK → Success.
9. **Andrew said "pause" here, arriving concurrently with the install
   completing.** App is installed but **not launched**. No further adb
   commands issued after this point.

## Verification plan (not yet executed — holding for "go")

1. Clear logcat, start a filtered capture (`adb logcat -c` then
   `adb logcat NEVR-Sentinel:V *:S`) *before* launching, so the earliest
   lines aren't missed.
2. Launch the app.
3. Confirm, in order, in logcat: the constructor line ("constructor: arming
   crash reporter (pre-libr15)"), then the hook-install line ("hook:
   GOT-hooked libr15.so's clock_gettime import (basics proof)"). That's
   proof the shim loaded and the hook installed, before any gameplay.
4. Watch for the one known failure mode from the crash-reporter design doc:
   any `UnsatisfiedLinkError` / `cannot locate symbol ovr_*` line would mean
   the forwarding-to-real-loader trick broke on this OS build, and the game
   may fail to boot at all. If seen: stop, report, do not push further —
   the documented fallback is the codegen re-export forwarders (design doc
   §2.3), a bigger change than tonight's scope.
5. Once actually in a lobby: watch for the periodic "clock_gettime call #N
   (GOT hook live)" lines (every 300th call) climbing in real time — the
   actual "load into a lobby and verify" proof Andrew asked for.
6. Scope stop for tonight: one hook, confirmed live, nothing further
   (no crash trigger, no second hook) without checking in first.

## Standing follow-up items (not tonight's scope, recorded so they aren't lost)

- Find `libr15.so`'s `BeginMultiplayer` equivalent (ReVault bookmark at
  `0x2ba19ee`, category `quest-porting`) — needs deeper decompiler coverage
  or a raw disassembly scan, not yet started.
- General internal-function (non-imported-symbol) hooking on Quest — needs
  inline code patching with an arm64 trampoline, or a different mechanism
  entirely; GOT hooking only reaches imported-symbol call sites.
- Fix the design doc's stale git-sha citation for the deleted sideload
  runbook (`docs/design/2026-07-13-quest-crash-reporter-injection.md`'s
  header cites `e30efee`, which does not actually resolve to that path —
  the real content is at `56c5c3e:docs/quest-sideload-runbook.md`).
