# Module `heaps` (rows R06 R07 R08 R09, plus the B3 table-step words as word-patch rows)

Files: `src/heaps.{h,cpp}`, `src/quest/plugins/q15t/src/module_switch.h` (shared by `heaps` and `memsys`: the per-module off switch),
`src/quest/tests/q15t/heaps_test.cpp`, `src/quest/tests/q15t/mod/heaps.sh`, `src/quest/tests/q15t/mutants.d/heaps.sh`.
Option macro for the main seat to wire: `Q15T_HEAPS`, default **ON** (the source build's default: its on-list
lines 4041, 10645 carry `--resource-heap-mib=320`; the other three are in the source build's patch table and
`the source build's memsys`). API: `InstallResult InstallHeaps(std::uintptr_t base)`; no tick.
Per-module switch: `heaps.off` in `/sdcard/Android/data/<pkg>/files/q15t/`; `memsys.off` also leaves R07 stock (one switch for the
System heap, as in the source build where memsys owned it).

## 1. TIMING: does the plugin run before android_main reads the sizes?

**Answer: only with `early: true` on the plugin's `config.yaml` entry. With the default (normal) phase it is not safe to
assume it does, and the example config in `src/quest/plugins/q15t/README.md` does not set it. Until `early: true` is set (and P10
below passes on a headset) `heaps` cannot be claimed to work.** What is shown, and what is not:

Shown (file:line, nevr-runtime-work/plugin-repack, read-only):
- Two phases exist. The early pass is step "RunEarlyPlugins" of the sentinel constructor sequence: after the reporter starts,
  before the clock hook (`src/quest/integration/ctor_sequence.h:16-17`, run at `ctor_sequence.cpp:188-194`, bound to
  `plugin::RunEarlyPlugins()` at `src/quest/integration/production_steps.cpp:381-383`). It calls `NvrPluginInitEx` through
  `Loader::RunPhase(Early)` (`src/quest/plugin/plugin_runtime.cpp:82-89`; the init call is `plugin_loader.cpp:192`).
- The normal pass runs at "the first post-load call" (`production_steps.cpp:335-339`, `PluginsAction`), i.e. after the game's
  own first `dlopen` returns (`src/quest/integration/post_load.cpp:69-80`, called at `:160` after the login/matchmaking
  installs). ADR 0019 says it in words: "Normal init, at moment (b) ... after the game's own `dlopen` returns" (docs/quest/q15t/adr/0019,
  section "Recommended placement"), and "(a) Constructor time. The sentinel's constructor runs while the game's library is
  being brought in, before the game's own code".
- Which entries are early: only those marked `early: true` (`src/runtime/ext/plugin_loader.h:9`; `plugin_load_plan.h:38,
  52-53`: the normal pass takes `!early`, and the early ones only when no early pass ran). The example config has no `early`
  key, so q15t loads in the normal pass today.
- Where the sizes are read, the disassembly (`functions`: `10784c4 | android_main | 1176`): the first call of android_main is
  `10784e4 bl 0x0100ddb0` (the CSysJava::Init PLT stub), and the four reads sit at `1078524 ldr x1,[x8]` (R07), `1078590 ldr x1,[x8]`
  (R06), `1078614 ldr x1,[x8]` (R08), each followed by `bl 0x0100e890` (the allocator constructor: `1078578`, `10785a4`, ...).
  Between the function start and the first read there are seven `bl` (`10784e4 .. 107850c`), all PLT thunks. R09 is read in
  `12ffde4 GameSpaceAllocatorSize` (size 16: `adrp x8,0x3815000; ldr x8,[x8,#0x198]; ldr x0,[x8]; ret`, at `12ffdec`).
  The source build says the same about R07: "reads the System heap size once, after every library constructor"
  (the source build's memsys:6-7).

Not shown (labelled unverified):
- That no game `dlopen` happens in those seven calls. A normal-phase init is "after the first `dlopen`"; the heap reads are a few
  dozen instructions into android_main. I did not trace the seven callees (the disassembly call graph does not reach
  `CSysModule::Load` even from android_main, so it cannot answer this). It is very likely a normal-phase init is after the reads;
  the R38 witness in `memsys` records the fact on a headset (see `docs/quest/q15t/modules/memsys.md`).
- ADR 0019 **P10**: whether a `dlopen` from the sentinel constructor completes, and whether libr15's relocations are applied
  at that point (ADR 0019 "The two moments"). If P10 fails the loader refuses `early` entries as `early_unsupported`
  (`plugin_plan.cpp:69-70`) and `heaps` never runs. The constructor does already GOT-hook libr15 on a real device (ADR 0021
  capture `steps_ok=22 steps_failed=1`, the one failure being `credits_text`), which is evidence the libr15 image is mapped
  at constructor time; it is not proof the slots are relocated.

What `heaps` does about the second point: the slot swap refuses unless the slot already holds `base + <rodata address>`
(`got_swap.h`'s expected-original rule). A GLOB_DAT slot that the dynamic linker has not relocated yet holds 0 (RELA), so
the module logs `heaps row refused ... why=unexpected_original observed=0x0` (ERROR) rather than swapping a word the linker
would overwrite a moment later. A refusal at constructor time is therefore loud, not silent.

**What is needed for the plugin to be sure of running first:**
1. `early: true` on the q15t entry in `config.yaml` (and `src/quest/plugins/q15t/README.md`, main seat), AND
2. P10 passes on a headset: the early init runs, `plugin_load` reports `outcome` ok in the early phase, and the four
   `heaps row swapped` lines appear. If the loader refuses the entry as `early_unsupported`, an early-run hook in the
   sentinel is needed (a plugin entry point the sentinel calls from its constructor without a `dlopen`, or relocation-complete
   signalling) and `heaps` cannot work through the normal phase. A late swap does nothing: libr15 has already passed the
   sizes to the allocator constructor `0x100e890` and the arena is reserved (the source build's memsys:6-9).

## 2. Mechanism (no code write)

Each reader is `ldr x8,[x8,#<got>]` (the GLOB_DAT word) then `ldr x1,[x8]` (or `ldr x0,[x8]` for R09): the stock value lives in
`.rodata`, reached through the GOT word. The module does one data-page store per row: it points the GOT word at a plugin-owned
`u64` (`g_value[row]`, static storage, so the pointer stays valid for the process). `.rodata` is untouched. A checked swap
(`SwapWord`, `got_swap.h`) refuses an executable page and any slot word that is not `base + rodata`. Before the swap: the
reader word at the pinned address must be the stock load, and the `.rodata` value must be the stock size; any mismatch refuses
that row only (WARNING), the other rows go on.

| Row | Symbol | GLOB_DAT slot (ELF) | `readelf -rW` line (R0_readelf_r.txt) | `.rodata` word | reader (disassembly VA, word) | stock | set to |
|---|---|---|---|---|---|---|---|
| R06 | `_ZN10NRadEngine22kResourceMemoryAndroidE` | 0x370b720 | 89726 | 0x2b9caa0 | `1078590` `010140f9` ldr x1,[x8]; slot via `ldr x8,[x8,#0x720]` at `1078580` | 206 MiB | 320 MiB |
| R07 | `_ZN10NRadEngine20kSystemMemoryAndroidE` | 0x371d020 | 98594 | 0x2b9ca90 | `1078524` `010140f9` (slot via `1078514 ldr x8,[x8,#0x20]`) | 400 MiB | 640 MiB, 512 MiB when `ro.product.device` is `hollywood` |
| R08 | `_ZN10NRadEngine17kGpuMemoryAndroidE` | 0x3710188 | 92082 | 0x2b9ca98 | `1078614` `010140f9`; slot via `1078604 ldr x8,[x8,#0x188]` | 160 MiB | 512 MiB |
| R09 | `_ZN10NRadEngine24kR15GameSpaceMemoryQuestE` | 0x3715198 | 94611 | 0x2b9e1c0 | `12ffdec` `000140f9` ldr x0,[x8] (GameSpaceAllocatorSize); slot via `12ffde8 ldr x8,[x8,#0x198]` | 40 MiB | 128 MiB |

Slot lines quoted verbatim from a source-build note (not in this tree), e.g. line 89726:
`000000000370b720  0000c85300000401 R_AARCH64_GLOB_DAT     0000000002b9caa0 _ZN10NRadEngine22kResourceMemoryAndroidE + 0`.
Disassembly VA = ELF vaddr + 0x100000 (the module's `reader` and `ro` columns are ELF: 0xf78590, 0xf78524, 0xf78614, 0x11ffdec).
The reader words and instruction text above were read from the disassembly `instructions` this session.

Target values and where each comes from (all from the source build):
- R06 320 MiB: `the source build's tools,163-170` (default `--mib 320`, accepted range 207..1024, measured peaks gauss 265.5 MB,
  fission 237.4 MB; stock 206 MiB = 0x0CE00000); the source build's on-list.
- R07 640 / 512 MiB: `the source build's memsys:9` and `:253` (`strcmp(g_dev,"hollywood")==0 ? 512ull : 640ull`);
  the source build's tools (planner GO, 640 MiB). Quest 2 is `hollywood`. The device string is read through
  `__system_property_get("ro.product.device")`; an empty string (not Android, property absent) gives 640.
- R08 512 MiB: the source build's patch table (160 -> 512, "combat spawn crash"); `res_heap_patch.py:11` ("which W-1's G12 raised to 512 MiB").
- R09 128 MiB: `the source build's patch table,67-68` ("the dyson HUD canvas overruns 40 MiB").
Whether a GPU-heap or GameSpace value above these is safe on a Quest 2 (memory pressure) is the source build's decision; I did not
re-derive it. When the reader runs a second time (not expected for R06-R08), it reads the same u64 again: harmless.
When each reader runs: R06-R08 once in android_main (above). R09: `GameSpaceAllocatorSize` is a function returning the value; its
callers and their time were not traced (unverified), so R09 may be read later than R06-R08 and might work from the normal
phase. Do not rely on it.

## 3. B3: the "4 table step words" are CODE, so they are word-patch rows, not built here

Decision (from the disassembly, `instructions`, read this session): each is a `mov wN,#0x20` immediate inside a function body
(the CTableXT growth step, 32 -> 1; `res_heap_patch.py:14-22`), i.e. an instruction word on an executable page. A plugin never
writes a code page (MODULE_CONTRACT), and these cannot be reached through a data slot. They stay with the main seat / word-patch
row list. They are only wanted together with R06 (`--resource-table-step-1` beside `--resource-heap-mib=320`,
`the source build's on-list`).

```
Disassembly VA  ELF vaddr  bytes (LE)  stock word   instruction   replacement          function (disassembly)
1a07130     1907130    08048052    0x52800408   mov w8,#0x20  0x52800028 mov w8,#1  CreateInstance  1a0707c size 292 (CFactory<CGShaderSetResource>)
1a07000     1907000    08048052    0x52800408   mov w8,#0x20  0x52800028 mov w8,#1  ConstructInstance 1a06f4c size 304 (CFactory<CGShaderSetResource>)
1a07958     1907958    14048052    0x52800414   mov w20,#0x20 0x52800034 mov w20,#1 CGInstancedModelResource 1a078dc size 376
196784c     186784c    14048052    0x52800414   mov w20,#0x20 0x52800034 mov w20,#1 CGMeshListResource 19677cc size 516
```
Query output (verbatim): `word | 196784c | 14048052 | mov | w20, #0x20`, `word | 1a07000 | 08048052 | mov | w8, #0x20`,
`word | 1a07130 | 08048052 | mov | w8, #0x20`, `word | 1a07958 | 14048052 | mov | w20, #0x20`;
`fn | 19677cc | CGMeshListResource | 516`, `fn | 1a06f4c | ConstructInstance | 304`, `fn | 1a0707c | CreateInstance | 292`,
`fn | 1a078dc | CGInstancedModelResource | 376`. Replacement encodings are the source's (`res_heap_patch.py:46-55`, `0x52800028`, `0x52800034`);
`mov w8,#1` = `0x52800028` and `mov w20,#1` = `0x52800034` is consistent with the stock encodings (imm16 at bits 20:5: 0x20 -> 0x1).
The 1a07958 row (CGInstancedModelResource) is not a word row any more: the instmodel module swaps its constructor's GOT word for a
wrapper that sets the same eight steps to 1 (`docs/quest/q15t/modules/instmodel.md`, `Q15T_INSTMODEL`). The other three rows stay as above.
Each function also has a stock sha256 in the source (`res_heap_patch.py:46-55`); I did not recompute those.

## 4. Logging (AGENTS.md)

One structured line per row, by `Log` (the loader prefixes `[NEVR.PLUGIN] plugin=q15t`): `heaps row swapped` (INFO) with fields
`row, slot_vaddr, why, observed, value_mib, device`, where `observed` is what the game will read through the slot now (slot -> pointer ->
u64, read back, not the intended value), and `heaps row refused` (WARNING for a stock-word mismatch, ERROR for a swap refusal)
with the same fields; `heaps not installed` (ERROR, reason `no_base`); `heaps off` / `heaps row off` (INFO). No secret is logged.
The host test asserts every line and field against the real code.

## 5. Prediction (written before any headset run) and falsifiers

Prediction, on a Quest 3 with the plugin loaded `early: true`, pinned build b243509c: logcat shows four INFO lines
`heaps row swapped` with `observed` 320, 640, 512 and 128 MiB (R06, R07, R08, R09) and no `heaps row refused`; on a Quest 2
(`hollywood`) R07 reads 512. The heap-exhaustion crashes of the testers (System arena dry; spawn crash) do not recur;
the process's largest anonymous reservation for the System arena is about 640 MiB (`/proc/<pid>/smaps`).

Falsifiers (any one disproves it):
- No `heaps row swapped` at all: the init did not run before the reads (P10 failed, or `early` not set); the lines never appear.
- `heaps row refused ... why=unexpected_original observed=0x0`: the slots are not relocated at constructor time; an early hook
  that runs after relocation is needed.
- Four swapped lines but `observed` differs from the table, or the System arena is still 400 MiB: the reader is not what the
  pinned words say, or another reader exists.
- A swapped line from a late phase (after `memsys: android_main reached CSysJava::Init` was NOT logged and the sizes stayed
  stock): the swap landed after the reads. The R38 witness in `memsys` is the instrument for this.

## 6. Host tests and mutants

`Q15T_MODULE=heaps bash src/quest/tests/q15t/run_host_tests.sh` -> `heaps_test: all checks passed` (12 case groups against a fake image: the pins equal
the readelf table; Quest 3, Quest 2 and an empty device; a foreign slot; a non-stock `.rodata` word; a bad reader word; an
executable page; `heaps.off`; `memsys.off` skipping only R07; base 0; no chaining on a second install). 23 mutants in
`src/quest/tests/q15t/mutants.d/heaps.sh`, one per rule; each is killed. The numbers from the full `src/quest/tests/q15t/mutants.sh` are in the report.

## 5. Wiring (main seat)

Built into its own library, `libq15t_early.so` (`src/quest/plugins/q15t/src/plugin_early.cpp`, CMake option `Q15T_EARLY`, default ON, OFF in the test arms), together
with `memsys`, and listed as its own `early: true` entry (`src/quest/plugins/q15t/README.md`). `heaps.off` / `memsys.off` still apply. Host test:
`src/quest/tests/q15t/early_test.cpp` (`src/quest/tests/q15t/mod/early.sh`). Unverified until a headset runs it: ADR 0019 P10 (does the early phase run, is the slot
relocated at that point). The repack recipe takes ONE plugin file (`just android-repack-apk ... <plugin .so>`), so shipping this library
next to `libq15t.so` needs a repack that takes two; until then the early library is built and tested but not in any APK.
