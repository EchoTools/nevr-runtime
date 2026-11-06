# Module `custctx` (rows R31, R32, R33)

Files: `src/quest/combat/custctx.{h,cpp}`, `src/quest/combat/custctx_targets.h` (the pins), `src/quest/tests/custctx_test.cpp`,
`src/quest/combat/mutants.d/custctx.sh`. The module is a normal-phase row of the combat install table (`src/quest/combat/combat_install.h`): it
runs in the `q15t` and `combat` APKs and in no other. Switch in `config.yaml` (docs/adr/0046 rule 4): `combat.custctx` (default true). The q15t
plugin's `custctx.off` file is no longer read; one found in the files folder's `q15t` directory is a Warning (`combat_custctx_legacy_switch`) that names
the key.

Origin: Ducky's echoquest-combat reference build, `afk_custctx.cpp` (docs/audits/2026-11-05-q15t-origin-citations.md rows 29-33 and 148:
afk_custctx.cpp:1,63-65,73-95,100-101,168-285; afk_tap.cpp:174), reconstructed in the q15t plugin (imported in #1591) and moved here. The plugin's
v4 layout-pool refusal and its 34 code-word pins were additions to the source's.

## Rows covered

| row | function | slot (ELF) | real fn (ELF = disassembly - 0x100000) |
|---|---|---|---|
| R31 | `CR15NetCustomizationRenderer::LoadMeshData` | 0x36cf6f0 | 0x1247d90 (disassembly 0x1347d90, size 2284) |
| R32 | `CR15NetCustomizationRenderer::DestroyContext` | 0x36c22f8 | 0x12492fc (0x13492fc, 72) |
| R33 | `CTableXT<SR15NetCustomDrawData>::Grow` | 0x36ed7f0 | 0x124893c (0x134893c, 408) |
| (helper) | `CMemBlock::Resize(u64,u32)` | called directly | 0x11dc30c (0x12dc30c, 216; R34 is its JUMP_SLOT 0x36f68e8, not hooked) |

The store libr15 (sha256 `8dd9a961...`, build ID `b243509c...`), `readelf -rW`: `00000000036cf6f0 ... R_AARCH64_JUMP_SLOT 0000000001247d90 ...LoadMeshData...`,
`00000000036c22f8 ... 00000000012492fc ...DestroyContext...`, `00000000036ed7f0 ... 000000000124893c ..CTableXTINS_8NRadGame21SR15NetCustomDrawDataEE4GrowEv`.
All 34 code words (`kWords` in `custctx_targets.h`) equal the reference build's, including the PLT stubs `ef0010 f0013ef0 / ef0014 f9437a11` (LoadMeshData,
`ldr x17,[x16,#0x6f0]`) and `ed5820 b0013f70 / ed5824 f9417e11` (DestroyContext, `ldr x17,[x16,#0x2f8]`), `1249644 97f29a73 bl <PLT LoadMeshData>`
(CreateContext's single call) and `124965c 97f23071 bl <PLT DestroyContext>`. Grow, the part the guard replicates:

```
1248948 ldp x9, x8, [x0, #0x28]     x9 = capacity, x8 = count
1248950 cmp x8, x9 ; 1248954 b.cc room
1248958 ldr x8, [x19, #0x20]        step
1248964 cmp x8, x9 ; 1248968 csel x20, x9, x8, cc    add = (step < capacity) ? capacity : step
1248970 mov w9, #0x9718 ; 1248974 mul x1, x8, x9      bytes = (add + capacity) * 0x9718
1248978 bl <PLT Resize>             result never checked
1248984 str x9, [x19, #0x28]        capacity += add
1248998 str x8, [x19, #0x30]        count + 1
12489c8 bl <PLT memset>             x0 = data + count*0x9718 - 0x9718: the crash site
```

RegisterRTLayout (the render-target layout pool): `18829c4 ldr x8,[x19,#0x2c0]` (the pool pointer, the GOT word at 0x379b2c0), `18829c8 ldr w3,[x8,#0x84]`
(capacity), `18829cc ldr x9,[x8,#0x30]` (free count), `18829d0 sub x10,x3,x9`, `18829d4 cmp x10,x3`, then the log and `brk`. `DestroyContext` is not
called by a direct `bl` anywhere (all 10 callers go through the PLT stub).

## What it does

All three guards are `CallbackThunk` handlers installed by the hook manager on their JUMP_SLOTs.

- **R31 LoadMeshData.** Under one mutex the context becomes *live* and gets a *load in flight* count (the only creation path is CreateContext's single
  call). It reads the layout pool (the pointer word in the image at 0x379b2c0, `+0x84` capacity u32, `+0x30` free count u64); with fewer than 64 free
  slots the load is refused: the handler answers 1, the stock failure value (CreateContext then destroys the empty context and answers NULL, which its
  caller checks), and `combat_custctx_load_refused` is written (the first 16, then every 50th). Otherwise the game's function runs. An unreadable or
  null pool pointer never refuses. Every 32nd load (at most 32 lines) writes `combat_custctx_loads` with the pool use.
- **R32 DestroyContext.** A null context goes to the game's function (it returns at once). Otherwise, under the mutex, the game's function runs only for
  a context that is live, with no load in flight and not already being destroyed; the entry is freed when the destroy returns (so a finished context
  leaves the table). Every other destroy is dropped (answers 0) with `combat_custctx_dropped` (`why`: no context holds the address, its LoadMeshData is
  running, already being destroyed; the first 16, then `combat_custctx_drops_total` every 50th, at most 16). A dropped destroy changes nothing; the
  caller clears its own slot. More than 256 contexts tracked: the overflow is untracked (the game's behaviour for its load, its later destroy dropped as
  unknown), one Warning. The mutex is never held across a game call.
- **R33 Grow.** One read of the 0x38-byte table header. No room (count >= capacity): `add = (step < capacity) ? capacity : step`,
  `CMemBlock::Resize(table, (capacity + add) * 0x9718, 1)` is called here and checked (the data word is read again); on success capacity += add and the
  game's Grow runs (it then only adds the element). Add 0, a NULL data after the resize, or room left but `data == NULL` (a torn-down table): the table is
  made consistent and empty (data, size, capacity, count 0; its elements are leaked, not destroyed) and the game's Grow is run on a one-slot table of
  ours (`posix_memalign(64, 0x9718)`, or one of 4 spares allocated at install, round robin), so LoadMeshData gets an element that is in no table; that
  player's cosmetics are not drawn. `combat_custctx_draw_failure` Warnings, capped like the drops. An unreadable table goes to the game's function.
- **Install: all or none.** The patched build ID, the 34 code words, `CMemBlock::Resize` (found with dlsym on the loaded libr15 and accepted only at exactly
  `base + 0x11dc30c`, else `export_missing`), the 4 spares (else `spare_alloc_failed`), then the three slots in order, each of which must hold exactly
  `base + its function`. When a slot is refused (the manager has already put its own failed install back), the hooks installed before it are removed
  with the manager's `Remove`, their thunks are disarmed, and the line names the refused hook (`which`) and the removal's status (`rollback`).

## Arena cost and arena behaviour (arena and combat share the APK)

- **Behaviour.** It acts in every level, on purpose, and does not use `level_scope.h`. custctx guards the lobby cosmetics renderer, code arena shares, and its
  three faults are crashes (a destroy racing a load, a draw table written through NULL, a trap on a full layout pool). A scope by level would leave the lobby,
  where cosmetics load, unguarded. It changes arena's result in one place: LoadMeshData is refused, and that player's cosmetics are skipped, when the
  layout pool has fewer than 64 free slots, up to 63 slots before the game's own function would trap; in arena that happens only in a state where a nearby
  load would crash the process. Every other call passes the game's own arguments to the game's own function, and a destroy is dropped only when the game
  would destroy a context it is not tracking, or one whose load or destroy is running.
- **Cost.** Per cosmetic load (per player join or lobby change): one mutex lock and unlock pair around a scan of the 256-entry table, three never-faulting
  reads for the pool state, one counter. Per destroy: the same lock and scan. Per draw-table Grow: one read of the table header. The call rates are not known:
  `combat_custctx_stats` counts loads, refused loads, dropped destroys and Grow failures, Debug at every 1024th load and Info at the end of the process, so
  a headset run gives them (ADR 0038). Host cost of the common path, added to a direct call of the game's function (x86-64, -O2, 2 million calls a loop, best of three loops on a machine that was
  building at the time): LoadMeshData 2.3 us (its three `process_vm_readv` reads are almost all of it), Grow with room 0.6 us (one read), a destroy of a live
  context within the noise of the load beside it (no read: a lock, a scan and an unlock). Per ADR 0038 a load of 2.3 us times its rate per frame has to stay
  under 10 us, which holds up to four loads a frame; the rate is what the headset run reads from `combat_custctx_stats`. Not measured on a headset.

## Gate: no file hash

The gates are the patched build ID, the 34 pinned code words (read before any slot is touched) and each slot's own expected word. A word that differs
is refused with one Error `combat_custctx_install` line (`reason` `pinned_word_mismatch`, `pin`, `found`, `expected`; `found` is `unreadable` for a page that cannot be read). The
pins are `kWords` in `custctx_targets.h`; `got_pinned_test` reads every one in the store libr15. `custctx_test` flips each in turn, reads one from an
unreadable page, and pins the table by a fingerprint.

## Lines (sentinel log)

- `combat_custctx_install`: `result` `installed|refused|off`, `reason`, the three hooks' status; Info for `installed` and `off` (reason `off_config`), Error for a refusal
  (`module_not_loaded`, `no_build_id`, `build_id_mismatch`, `pinned_word_mismatch`, `export_missing`, `spare_alloc_failed`, `already_installed`, `slot_refused` with `which`, `hook`, `rollback`).
- `combat_custctx_loads`, `combat_custctx_load_refused`, `combat_custctx_dropped`, `combat_custctx_drops_total`, `combat_custctx_draw_failure`,
  `combat_custctx_draw_failures_total`, `combat_custctx_full`: the capped lines above.
- `combat_custctx_stats`: `loads`, `loads_refused`, `destroys_dropped`, `grow_failures`, `final`.
- `combat_custctx_legacy_switch`: Warning, when the plugin's `custctx.off` is found.

## Prediction (written before any headset run)

At start `combat_custctx_install` `installed`. In a lobby with cosmetics changing: no `memset(NULL)` crash in `Grow <- LoadMeshData` and no brk in
`CGRenderTargetState::Initialize` ("Exceeded the max number of registered render target layouts (512)"). Every 32 loads a `combat_custctx_loads` line with
`layouts_used` below 448 of 512 (1024 slots with `combat.memsys`). In a healthy lobby no `combat_custctx_dropped`, `combat_custctx_draw_failure` or
`combat_custctx_load_refused`. Under memory or layout-pool exhaustion the matching line appears instead of the crash and that player's cosmetics are
missing until the next load.

## Falsifiers

- `pinned_word_mismatch` on a build the sentinel accepted by build ID: the code at that address is not what the guards were derived from.
- `combat_custctx_dropped` with `why` "no context holds the address" in an ordinary lobby, repeatedly: a context is created through a path other than
  LoadMeshData (or the load slot is not reached): destroys are skipped and contexts and layouts leak (`layouts_used` climbs on every re-entry).
- `combat_custctx_load_refused` while `layouts_used` is well below capacity: the pool offsets or the pool pointer are wrong.
- The `memset(NULL)` crash in Grow with no `combat_custctx_draw_failure`: Grow was reached with data NULL by a path the count and capacity test does not see.
- Cosmetics missing for players on a roomy heap with draw failures: `Resize` is not what Grow calls, or its argument convention differs.
- `slot_refused` for one hook: that slot does not hold the pinned function (nothing stays swapped).

## Not established

- On a headset: that the install succeeds on a combat APK, the call rates of the three functions (`combat_custctx_stats`), the cost per call, and that the crashes are gone.
- `SR15NetCustomDrawData` size 0x9718 beyond Grow's `mov w9,#0x9718`; that `CMemBlock::Resize` leaves `data` NULL on failure (the reference build's claim); that CreateContext
  checks LoadMeshData's non-zero result and returns NULL (the reference build's claim, not re-read); the mesh-entry-per-layout assumption behind the margin of 64. The
  bodies read here are Grow's and RegisterRTLayout's words, the PLT stubs and the function sizes, not LoadMeshData's or DestroyContext's bodies.
