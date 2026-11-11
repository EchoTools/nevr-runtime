# Script marker lines: `RE1_LOADED_` and `RE1_THREAD_START`

Status: the emission path is established by static reading (ReVault and the generator sources); rank 5 in `docs/standards/verification.md`.
Per-load counts are **not measured** (see "Counts: not measured"). The choice among the options below is open, under the umbrella tracker
#1737; this page costs them and does not choose.

Upstream is `kotorvr/echoquest-combat` at `c5a67e8`. Paths written `echoquest-combat:<path>` are in that clone; every other path is in
this repository. ReVault: project `echovr`, binary `libr15.so`, sha256 `8dd9a961...1d8b20`, build ID `b243509c...5230`, VA = ELF + 0x100000.
The build ID is the stock one (`src/quest/sentinel/pinned_targets.h:34`). The table at `docs/adr/0003-quest-networking-port.md:772` lists
the same sha256 and build ID for a local repack.

## What the lines are

- `RE1_LOADED_<h16> native=%d wrappers=%d nodes=%d`: one per graph init of a script library. It is written by the synthetic
  `LOAD_MARKER` autorun node: `echoquest-combat:tools/scripts/regen_so.py:273` (the constant), `:276-283` (`_load_marker_body`, the write
  at `:280`), `:8012` (`autorun_nodes=[LOAD_MARKER]`). Level 8.
- `RE1_THREAD_START slot=<slot> node=%08x`: one per thread a generated library starts from its lifecycle entries. It is written by
  `_start` (`echoquest-combat:tools/scripts/cppgen.py:397-402`, the write at `:401`), at level 8, behind `static int _n=0; if(_n<16)` at
  each call site.

Level 8 is ERROR in the engine's numbering (`docs/standards/logging.md`, "The level a record carries"). That numbering is stated for
`echovr.exe`. That libr15 uses the same numbering is an inference, from the shared engine and from the `[ERROR]` tag that upstream's
header quotes (`echoquest-combat:runtime/native/afkxr/afk_scriptlog.cpp:3`).

## Emission path in libr15.so

No file under `libsrc/` in the upstream clone contains `WriteLog`, and the script-library generators (`echoquest-combat:tools/scripts/`)
do not emit it. The generated libraries call `write_log`, a function pointer read from their binding context
(`echoquest-combat:tools/scripts/cppgen.py:528`, `write_log = ...(c+0x60)`; declared at `echoquest-combat:tools/scripts/regen_rt.h:202`).
The imports of the shipped `.so` files were not read.

| Hop | VA | Evidence (ReVault raw decompilation or disassembly) |
| --- | --- | --- |
| 1. `CScriptResource::Attach`, size 480 | 0x29cfe84 | stores the adapter at `param_1+0xd8` and calls `setup_bindings` with `param_1+0x78`. The adapter is therefore at bindings `+0x60`, the offset the generator reads. |
| 2. script-log adapter, size 280 | 0x29d0064 | ReVault lists it as `FUN_028d0064` and its decompilation names it `FUN_029d0064`. `VSNPrintF` into a 0x2000-byte buffer, then `WriteLog(level, "[SCRIPT] %s: %s", owner, buffer)`, where `owner` is the result of `CSymbol64::String()`. The level passes through unchanged. Upstream's filter calls the owner `ownerStr` (`echoquest-combat:runtime/native/afkxr/afk_scriptlog.cpp:10`). |
| 3. format string `[SCRIPT] %s: %s` | 0x3311b2c | the only occurrence in `libr15.so` (`revault_search_strings` on that binary). |
| 4. thunk | 0x1032cd0 | `adrp x16,0x37f0000; ldr x17,[x16,#0xd50]; add x16,x16,#0xd50; br x17`. It loads the GOT slot at 0x37f0d50 (ELF 0x36f0d50). |
| 5. `NWriteLog::WriteLog(level, fmt, ...)`, size 112 | 0x10886b0 | calls `WriteLogV(level, 0, fmt, &va)` through the thunk of hop 6. |
| 6. thunk | 0xfe7590 | `adrp x16,0x37cb000; ldr x17,[x16,#0x1b0]; add x16,x16,#0x1b0; br x17`. It loads the GOT slot at 0x37cb1b0 (ELF 0x36cb1b0), the `WriteLogV` slot. |
| 7. `NWriteLogV` | 0x1088720 (ELF 0xf88720) | the target of the slot in hop 6. Pinned at `src/quest/combat/ec_targets.h:91`. |

The slot of hop 6 is the one `src/quest/combat/ec_targets.h:10` pins (`JUMP_SLOT 0x36cb1b0 -> 0xf88720`). It is already hooked:
`LogVHandler` (`src/quest/combat/ec.cpp:238-247`) is installed on that slot at `ec.cpp:644-648` when the combat module installs, and
its outcome is logged as `writelogv_libr15` (`ec.cpp:513`).

What the xrefs and instruction search show:

- The thunk of hop 4 has five call sites, all of them calls to the two-argument overload of hop 5: `~CConsoleStream` at 0x12e8440 and
  0x12e84b8, `RegisterStreamingSystem` at 0x19e4d90, a tail jump in `IoErrorCb` at 0x2602c6c, and the adapter at 0x29d0160
  (`revault_search_instructions` for `0x01032cd0`, and `revault_xrefs`). One instruction reads the GOT slot at 0x37f0d50
  (`0x1032cd4 [read]`).
- A swap of the slot at 0x37f0d50 therefore reaches those five call sites, four of which are not script markers. The upstream filter
  swaps this slot and passes every other call through unchanged (`echoquest-combat:runtime/native/afkxr/afk_scriptlog.cpp`).
- A second overload, `NWriteLog::WriteLog(level, channel, fmt, ...)` at 0x1088c20 (size 100), reaches `WriteLogV` through the thunk of
  hop 6, so it passes the hooked slot as well. Its callers use the thunk at 0x104b960, which loads its own slot, 0x37fd398 (ELF
  0x36fd398). That thunk has at least 1,000 call sites: `revault_xrefs` stops at its 1,000-row cap, so they are not counted here. The
  overload is not on the slot at 0x37f0d50.
- No instruction names WriteLogV's VA 0x1088720 (`revault_search_instructions`). The stub at 0xfe7590 (ELF 0xee7590) has exactly two
  callers, the two `WriteLog` overloads (`src/quest/combat/ec_targets.h:29-30`).
- `revault_binary_info` reports a measured call graph for 155,738 of 178,802 functions (23,064 without). `revault_callers` and
  `revault_callees` omit calls from functions without a measured edge. The five-site list above comes from an instruction scan and does not
  depend on the call graph.

The upstream header calls the adapter location `ctx+0x60`. That is the same slot, counted from the bindings base (row 1).

## Counts that the emission path implies (static, not measured)

- `RE1_LOADED_`: one per graph init. Upstream's comment at `echoquest-combat:tools/scripts/regen_so.py:268` equates a graph init with a
  level load that uses the script. Its comment at `:1543` describes the SData pool as per instance. How many graph inits occur per load is
  not established here. The generated code has no cap (`regen_so.py:276-283`).
- `RE1_THREAD_START`: at most 16 per call site per loaded copy of a library (`cppgen.py:401`, a `static` counter in the generated `.so`).
  An unload and reload gives a new copy and a new counter. That reading of `static` is inferred from the code, not measured.
- Upstream states "~2,900 RAD [ERROR] [SCRIPT] lines per level load, 116 of 142 shipped libs"
  (`echoquest-combat:runtime/native/afkxr/afk_scriptlog.cpp:3-4`). The 116 matches the generated sources: 116 `.cpp` files in
  `libsrc/` call `write_log`. The 2,900 is upstream's claim and is not checked here.

## Counts: not measured

The required measurement is logcat for `RE1_LOADED_` and `RE1_THREAD_START` across one Dyson load and one lobby load on the combat APK,
from a live headset run. It has not been run.

Read-only observations from the combat headset, not reproducible from this tree:

- A logcat dump of the headset's buffers, all merged, has 42,066 lines and no line matching `RE1_LOADED_`, `RE1_THREAD_START` or `[SCRIPT]`.
  Its main buffer, where app output lands, holds 23,758 lines, and none of them names `com.readyatdawn` or `echovr`. The dump therefore
  shows no game run in its main buffer. It does not show whether the markers are emitted.
- `dumpsys package` reports `versionCode=61700999` for `com.readyatdawn.r15` and `versionCode=4987566` for `com.readyatdawn.afk`.
  `src/runtime/combat/veilnet.cpp:845` calls 61700999 the Quest combat APK players have, and `tools/quest_release_base.py:43` names
  4987566 the reference combat build. Neither is shown to be the `nevr-sentinel` build.

What settles it: the combat APK is named, then one Dyson load and one lobby load are logged with start and end marks. The counts size the
operator noise. They do not change the emission path or the options below.

## Options, costed (not chosen; umbrella tracker #1737)

The three options are (a) collapse the markers into one counted line per load, (b) build the libraries with the marker at Debug, and
(c) leave them. Option (a) needs one place that every marker line passes. Two exist.

**(a1) A new swap of the two-argument overload's slot (hop 4, 0x37f0d50).**

- A new hook slot in `src/quest/sentinel/hook_budget.h`.
- The slot is a GOT data word. `src/quest/sentinel/got_hook.cpp:375-376` says a RELRO page is made writable for the store and protected
  again. N151 lists that file among the four places that make protection calls. Whether this slot lies in a RELRO page is not verified.
- Pass-through must be exact for the four call sites of hop 4 that are not markers, with a mutant per changed line.
- Pins: the slot, the format string and the two-argument `WriteLog`, none of which is pinned now, in a `*_targets.h` and a pinned test
  against the store `libr15.so`.

**(a2) Inside the existing `LogVHandler` for index 0 (`src/quest/combat/ec.cpp:238-247`, installed at `ec.cpp:644-648`).**

- No new GOT slot. The existing hook is reused on hop 6, which both overloads reach. How `src/quest/sentinel/hook_budget.h` counts an
  entry was not read, so no budget cost is stated.
- Pins: the format string (ELF 0x3211b2c) for a pointer compare. The owner string and the text are read with `va_copy`, as the profile
  check already does (`ec.cpp:239-244`).
- The filter acts only while the existing hook reports `installed` (`ec.cpp:513`). Otherwise lines pass through, and the status says so.
- Risk: `LogVHandler` is the combat module's hook and also carries the profile path, so a change there changes that hook. Mutant per
  changed line.
- Cost per line: every libr15 log line passes the handler, so the filter adds one pointer compare before any text is read. This is an
  inference from the code; it is not measured.

Both placements need a load boundary, since "one line per load" needs one. The generated code has none: the `LOAD_MARKER` runs once per
graph init (upstream: once per level load that uses the script). `CR15Game::LoadLevelName` (ELF 0x1202ecc, ReVault VA 0x1302ecc,
`src/quest/combat/levelname.h:3`) is a candidate for a per-level-load boundary. It is not verified to fire once per load.

Standard: `docs/standards/logging.md` Rule 5 treats game-native lines without an nEVR tag as noise. The PC filter's N78 rule
(`src/runtime/log/builtin_filter.cpp`) collapses repeats into counts. Applying N78 to Quest lines is an inference. These lines carry the
`[SCRIPT]` tag and no `[NEVR.*]` tag.

**(b) Marker at Debug.** Change the level argument from 8 to 1 at `echoquest-combat:tools/scripts/regen_so.py:280` and
`echoquest-combat:tools/scripts/cppgen.py:401`.

- The generated libraries change bytes. Three upstream records would no longer match: `echoquest-combat:SOURCE_MD5.txt`, the md5 of every
  file in that tree (`echoquest-combat:README.md:21`), and the per-library md5 pins in `echoquest-combat:tools/trials/qa-1104-aj4-w2z22.json`
  and `echoquest-combat:libsrc/LIBS.json` (`echoquest-combat:OUTPUTS.md:11`, "the 117 script libs"). `echoquest-combat:tools/src/build_libs.py`
  checks each built library against those pins as SAME, DIFF or FAIL (line 74). `echoquest-combat:INPUTS_quest_libs.md5` holds the md5 of the
  stock Quest APK's libraries (`echoquest-combat:INPUTS.md:8`) and does not change.
- Whether a level-1 line reaches logcat is unknown. The level gate in `WriteLogV` (0x1088720) was not read. ReVault can answer that.
- Each of the 116 `.cpp` files in `libsrc/` that call `write_log` (above) would be rebuilt and re-pinned.

**(c) Leave them.** No code cost. The cost is the per-load error-level lines in logcat, whose count is not yet measured, and the Rule 5
noise.

## Not verified

- Live presence and counts of the markers on a headset: not run.
- Which installed Quest build is the combat build, and whether either carries the upstream filter.
- Whether the installed headset's `libr15.so` matches the ReVault copy. The ReVault copy has the stock build ID, and upstream's constants
  (`echoquest-combat:runtime/native/afkxr/afk_scriptlog.cpp:47-50`) equal the VAs here minus 0x100000.
- Whether the existing `LogVHandler` hook is installed in a combat run. The `writelogv_libr15` status line is that run's own evidence.
- A per-load boundary. `LoadLevelName` is a candidate, not verified.
- The level gate of `WriteLogV`, needed for option (b).
- Indirect call sites outside ReVault's measured call graph (155,738 of 178,802 functions).
- Pins: `WriteLogV` (0xf88720) and its slot (0x36cb1b0) are pinned at `src/quest/combat/ec_targets.h:10` and `:91`. The
  two-argument overload's slot (0x36f0d50), the format string and the two-argument `WriteLog` are not pinned in this tree (`git grep` of
  those addresses gives no hit).
