# Module `heaps` (rows R06 R07 R08 R09, plus the B3 table-step words as word-patch rows)

Files: `src/quest/combat/heaps.{h,cpp}`, `src/quest/combat/heaps_targets.h` (the pins), `src/quest/tests/heaps_test.cpp`,
`src/quest/tests/heap_step_test.cpp` (the step against a model of the start of `android_main`), `src/quest/tests/got_pinned_test.cpp` (the pins in the
real libr15). The module is the sentinel's early combat step (`src/quest/combat/combat_install.h`): it runs in the `q15t` and `combat` APKs and
in no other. Switches in `config.yaml` (docs/adr/0046 rule 4): `combat.heaps` (default true) turns the module off; `combat.memsys: false` leaves the
System heap (R07) as the game has it, because the System heap and the render-target pool are one memory change (`docs/quest/combat/memsys.md`).

Origins: the sizes 320 MiB (R06), 512 MiB (R08) and 128 MiB (R09) are the reference build's heap patches (res_heap_patch.py:7-12,163-170,
qcp_patches.py:62-71, and the trial list qa-w15c-1028.json:4041 of its tools, pinned in `docs/audits/2026-11-05-q15t-origin-citations.md`); the
System heap's 640 MiB, and 512 MiB on a Quest 2, are afk_memsys.cpp:9,253 of the same record.

## 1. TIMING: the step runs before android_main reads the sizes

The sentinel's constructor runs while the dynamic linker finishes loading libr15's dependency closure: after the relocations of the whole set were
applied (the GLOB_DAT slots hold `base + <rodata address>`, the JUMP_SLOTs hold their functions) and before the activity enters `android_main`. The
constructor sequence (`src/quest/integration/ctor_sequence.h`) runs the combat early step right after the build-time libr15 patch is verified
(`attach_libr15`) and before `install_clock_hook`, only where `combat::Active()`. So the four sizes are ours before `android_main` reads them; no
plugin phase is involved.

Shown:
- `android_main` (`10784c4`, size 1176) calls `CSysJava::Init` first (`10784e4 bl 0x0100ddb0`) and reads the three heap sizes at `1078524`
  (R07), `1078590` (R06) and `1078614` (R08), each followed by the allocator constructor `bl 0x0100e890`. Between the function start and the first
  read there are seven `bl`, all PLT thunks. R09 is read in `GameSpaceAllocatorSize` (`12ffde4`, size 16: `adrp x8,0x3815000; ldr x8,[x8,#0x198]; ldr x0,[x8]; ret`).
  R09's callers and the time of their first call were not traced, so R09 may be read later than the others.
- `src/quest/tests/heap_step_test.cpp` runs the shipped early table on a libr15 image built in memory, then a model of `android_main`'s start: the
  model reads the raised sizes, the witness line (`docs/quest/combat/memsys.md`) is written when it reaches Init, before the model reads any heap,
  and a step run after the model leaves every size stock and writes no witness.
- Each slot is checked before it is written: the write stores only over `base + <rodata address>` (a slot the loader has not relocated holds 0 and is
  refused, `original_mismatch`, with a line that names the row), so a step that ran too early writes nothing and says so.

Relocation: bionic links the whole dependency group of a `dlopen` (`find_libraries`, every library's `link_image`) before it runs any constructor
(`do_dlopen` calls `call_constructors` after `find_library` returns, children first), and one `relocate` call applies the GLOB_DAT words
(`.rela.dyn`) before the JUMP_SLOTs (`.rela.plt`, bound eagerly, no lazy binding). The sentinel is a DT_NEEDED child of libr15, so its constructor runs
after libr15's GLOB_DATs are applied. A headset has shown the same order: the sentinel's constructor-installed hooks on libr15's JUMP_SLOTs (the
match-tags default, `combat_match_tags_install` `installed` on a Quest 2) install from a later step of the same sequence, and a slot accepts
a hook only once it holds a mapped address. A slot that were not relocated would hold 0 and be refused (`original_mismatch`), writing nothing.

Not shown (needs a headset): that the game runs with the raised heaps, and the witness on a device. The headset run reads, in the
sentinel log, `combat_heaps_install result=installed` with `resource_mib` 320, `system_mib` 640 (512 on a Quest 2), `gpu_mib` 512 and
`game_space_mib` 128, then `combat_memsys_witness` with `system_heap` `raised`; a combat match plays.

## 1b. What an arena match on the combat APK gets

The sizes are read once at the start of the process and cannot be scoped by level, so an arena match on a q15t or combat APK runs with them too: heap
sizes totalling 1600 MiB against 806 MiB stock (+794 MiB; +666 MiB on a Quest 2), and a 1024-slot layout pool (`docs/quest/combat/memsys.md`). Both wrappers run about
once per process: libr15 has one call through each PLT stub (Alloc from InitCommon at 0x19818c8, `CSysJava::Init` from `android_main` at 0x10784e4). The cost
of the sizes is unmeasured: whether the allocator commits or only reserves the memory, and the headroom on a Quest 2, are open. The headset row
measures it: an arena match on the combat APK on a Quest 2, PSS and frame time with `combat.heaps: true` against `false`. A plain release is untouched (the step does not run).

## 2. Mechanism (no code write)

Each reader is `ldr x8,[x8,#<got>]` (the GLOB_DAT word) then `ldr x1,[x8]` (or `ldr x0,[x8]` for R09): the stock value lives in
`.rodata`, reached through the GOT word. The module does one data-page store per row: it points the GOT word at a
`u64` of ours (`g_value[row]`, static storage, so the pointer stays valid for the process). `.rodata` is untouched. A checked data write
(`sentinel::GotDataSlot::Write`, `src/quest/sentinel/got_hook.h`) refuses a slot whose symbol is not a data object, a slot in an executable mapping, a new value that is a code address and any slot word that is not `base + rodata`. Before the swap: the
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

From the disassembly: each is a `mov wN,#0x20` immediate inside a function body
(the CTableXT growth step, 32 -> 1; `res_heap_patch.py:14-22`), i.e. an instruction word on an executable page. The runtime writes
no code page at run time (N151), and these cannot be reached through a data slot. They are not this module's: the table-step modules (`docs/quest/combat/meshstep.md`, `tablestep.md`, `shaderstep.md`, `instmodel.md`) own them. They are only wanted together with R06 (`--resource-table-step-1` beside `--resource-heap-mib=320`,
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
The 1a07958 row (CGInstancedModelResource) is not a word row any more: the instmodel module hooks its constructor's JUMP_SLOT with a
handler that sets the same eight steps to 1 in a combat match (`docs/quest/combat/instmodel.md`). The other three rows stay as above.
Each function also has a stock sha256 in the source (`res_heap_patch.py:46-55`); I did not recompute those.

## 4. Logging (AGENTS.md)

One aggregate line per run, `combat_heaps_install` (Info; Error when a row was refused), with `result` (`installed`, `partial`, `refused`, `off`),
`reason`, the counts (`swapped`, `refused`, `left`), the size the game reads through each slot after the step (`resource_mib`, `system_mib`, `gpu_mib`,
`game_space_mib`: slot -> pointer -> u64, read back, not the intended value), the `device` and the consequence. One `combat_heaps_row` line per row:
Debug when swapped, Warning for a stock-word mismatch (`reader_word_not_stock`, `rodata_not_stock`), Error for a refused write (the manager's status,
for example `original_mismatch`, `executable_mapping`), Info for a row left alone by `combat.memsys: false`. The sentinel's `combat_install` line
(phase `early`) carries `heaps=<status>`. No secret is logged. `src/quest/tests/heaps_test.cpp` asserts every line and field against the real code.

## 5. Prediction (written before any headset run) and falsifiers

Prediction, on a Quest 3 in a combat APK (pinned build b243509c): the sentinel log has `combat_heaps_install result=installed` among the constructor's lines
with the four sizes above, then `combat_memsys_witness ... system_heap=raised`; on a Quest 2 (`hollywood`) `system_mib` is 512. The heap-exhaustion
crashes (System arena dry; spawn crash) do not recur.

Falsifiers (any one disproves it):
- `result=refused` or `partial` with `original_mismatch` and an observed 0: the slots are not relocated when the constructor runs; the step needs a later moment.
- `combat_memsys_witness` is missing, comes before `combat_heaps_install`, or reports `system_heap=stock`: the sizes were not in place when `android_main` started.
- `result=installed` but a size differs from the table, or the System arena is still 400 MiB: the reader is not what the pinned words say, or another reader exists.

## 6. Host tests

`just test-quest-combat heaps` runs `heaps_test` (all four slots, the Quest 2, a slot another writer owns, a non-stock `.rodata` word, a reader that is
not the stock load, a symbol that is a function, the two switches, the missing and the wrong image, once, the golden lines) and `heap_step_test`.
`just test-quest-hooks-pinned` runs `got_pinned_test`, which reads the four relocations, their symbol types, the `.rodata` sizes and the reader
words in the real libr15.
