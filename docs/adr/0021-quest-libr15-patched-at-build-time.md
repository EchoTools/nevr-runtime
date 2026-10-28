# ADR 0021: libr15.so is patched at build time; no code page is written at run time

Status: proposed (#1132). Not implemented. The "Prediction" section is what the first headset runs must show; the
"What this does not know" section lists what no measurement in this tree covers.

## Context

The sentinel (`libovrplatformloader.so`, our shim) changes libr15's behaviour in two ways that GOT hooks cannot reach, and
both write libr15's code at run time:

- inline hooks (`src/quest/sentinel/inline_hook.cpp`): the lobby-full join, failure and success handlers, the texture
  `CGTextureResource::Attach`, the splash-skip widget update, and the menu version label's `SetText` (#747). Each overwrites the
  first 16 bytes of a function (`LDR x16,#8; BR x16; .quad detour`) after `mprotect(page, RW)` and puts the page back with
  `mprotect(page, RX)`;
- the credits page (`src/quest/sentinel/credits_text.cpp`, `WriteWord`): two `ADRP`+`ADD` pairs are rewritten the same way.

On a tester's Quest 3 (NEVR 5.10.0, libr15 build ID `b243509c08ce677aeb95fa348016949b3fc45230`, #1127) the second
`mprotect` is denied by policy: `avc: denied { execmod } ... path=".../libr15.so" scontext=u:r:untrusted_app_29:s0
tcontext=u:object_r:apk_data_file:s0 tclass=file permissive=0`. `execmod` is `PROT_EXEC` on a private file mapping that has
been written. The page is left writable and not executable, it stays copy-on-written even when the original bytes are put
back (so a second `mprotect(RX)` is the same denied call), and the first call into it faults: `SIGSEGV`, `pc == x17 ==
libr15+0x18e7b28`, on the page the Attach hook patched, in every launch of the capture. The same capture shows the
trampoline pages (anonymous memory, `mmap` then `mprotect(RX)`) being made without a denial (`"trampoline":"0x0000007b8d527000"`
in the `restore_protect_failed` line of the join hook, and the next hook's line likewise): `execmod` is a rule about file-backed
pages that were written, not about executable anonymous pages.

A Quest 2 on Horizon v207 (build 52242990035800150, factory reset to the current OS) fails the same way on NEVR 5.16.1: six
launches, each `Thread-2: potentially unexpected fatal signal 11` about a second in, with 14 `avc: denied { execmod }` lines
for `libr15.so` (`untrusted_app_29`). The denial is the current Horizon policy on both headsets, so every player on the current
OS is affected. A design that writes a code page at run time depends on that policy; a design that never writes one does not.

Two of the addresses the writers use were wrong when they met a headset (`kSetTextVaddr`, #1125; `kWidgetUpdateVaddr`, #1128):
each is a ReVault address with 0x100000 subtracted by hand. A refused hook shows up on a headset, not at build time.

## Decision

`libr15.so` is patched once, at build time, on the host, and the patched file is what the APK carries. At run time the sentinel
writes only data. One path for every headset, no flag.

### 1. One table, one typed address per site

A new header, libr15_patch_table.h, in a new directory under src/quest, is the only place a patch site is written down. A row holds:

- `id` (`lobby_full.join`), the game function's ReVault name, and the **ReVault address**, the only address typed by hand. The
  pinned vaddr is derived (`revaultVa - kRevaultBias`, `kRevaultBias = 0x100000`), so the subtraction that went wrong twice is
  code, not a comment;
- the **stock words** at the site (3 words for an inline hook; for the credits pairs the ADRP and ADD words), copied from
  ReVault with `revault_disassemble <revaultVa>`;
- the kind (`kInlineHook` or `kCreditsPair`) and the handler's name.

The sentinel's hook files (`lobby_full_hooks.cpp`, `ui_texture_mips.cpp`, `splash_skip_hook.cpp`, `menu_version_label.cpp`) keep
their handlers and lose their per-file `kXxxVaddr` and prologue constants; they name a table row. The seven rows are:
`lobby_full.join` (ReVault 0x1340ba8), `lobby_full.failure` (0x1340cc8), `lobby_full.success` (0x134024c), `ui_texture.attach`
(0x19e75c4), `splash_skip.widget_update` (0x209a7a0; #1128's correct address), `menu_version.set_text` (0x149111c; #1125's), and
`credits.pairs` (ReVault 0x1ea1284/0x1ea1290 and 0x1ea1d84/0x1ea1d88, the two pairs of #970).

### 2. The patcher: a host tool that reads the stock file and refuses on any doubt

`quest_patch_libr15` (new, C++ compiled by the justfile with the host `g++`, as the `test-quest-hooks` tests are; it includes the
table header and `arm64_reloc.h`, so there is no second encoding of anything). `quest_patch_libr15 --stock <libr15.so> --out
<file> --report <json>`:

1. refuses unless the stock file's GNU build ID is `kLibR15StockBuildId` (the current `kLibR15BuildId`) and its sha-256 is the
   pinned one;
2. for each row, refuses unless the words at `vaddr` are the row's stock words. A wrong ReVault address, or a typo in a word, is
   a build failure with the row's `id`, not a refused hook on a headset;
3. writes the stubs (below), the slot area (below), the credits pairs, and the new build ID (below). Every byte that differs from
   the stock file is accounted for in the report: `{"event":"libr15_patch_site","id":...,"revault_va":...,"vaddr":...,
   "stock":[...],"stub":[...],"slot":...}` per row, then a summary with the stock and patched sha-256. The output is a pure
   function of the stock file and the table: the same inputs give the same bytes;
4. validates its own output as an ELF (PT_LOAD sorted and non-overlapping, `p_filesz <= p_memsz`, the build-ID note intact, the
   section table consistent) and refuses to write a file that fails.

### 3. The stub: three words, no write at run time

Each inline site's first 12 bytes become

```
adrp x16, <page of this site's slot>
ldr  x16, [x16, #<slot offset in its page, scaled by 8>]
br   x16
```

For `lobby_full.join` (vaddr 0x1240ba8, slot 0): `0xd0012c90`, `0xf942a210`, `0xd61f0200`. `x16` (IP0) is the register the 16-byte
patch already uses; the AAPCS64 lets a call site treat it as clobbered. The stub is position independent: `ADRP` and the `LDR`
use only the site's own address and the slot's page, so a load bias changes nothing. The window shrinks from 16 to 12 bytes:
no caller may branch to site+4..site+11 (the same obligation as today, over fewer bytes). `arm64_reloc.h`'s window constant
(`kRelocWindowBytes`, 16 today) becomes a parameter set to 3 words for these sites.

### 4. Slots: the tail of `.bss`, one 8-byte slot per row, filled at run time

libr15's writable `PT_LOAD` (vaddr 0x35f9eb0, `p_memsz` 0x1d8688 in the pinned file) ends at 0x37d2538, inside a page the
linker already maps; nothing follows it. The patcher raises that segment's `p_memsz` (and `.bss`'s `sh_size`) so it ends at
`roundup(0x37d2538, 64) + 8 * rows = 0x37d2578` (`p_memsz` 0x1d86c8; `p_filesz` and every file offset unchanged). The slot area is
zero-initialised memory outside `GNU_RELRO` (which ends at 0x372c000), so a store to a slot needs no `mprotect` at all. Slot *i*
is at `0x37d2540 + 8 * i`. A row's stub names its own slot; the credits pairs share one slot.

**How a slot is found at run time.** There is no directory to read. For each row the sentinel computes `slot = base +
row.slotVaddr` from the table and checks it three ways before it stores anything: (a) the three words live at the site are
exactly the stub the table implies (so a stock site, or a stub from another table, is refused); (b) decoding the live `ADRP` and
`LDR` gives the same slot address; (c) the slot lies inside the live program headers' writable `PT_LOAD` and at or after
`p_vaddr + p_filesz` (it is bss). Any failure is a refusal that names the row and the check.

### 5. Trampolines: from the table's stock words, in anonymous memory

The original prologue is no longer at the site, so the trampoline is built from the table's stock words, never from live
bytes: `mmap(RW)` an anonymous page, write the relocated stock words (`BuildTrampoline`, unchanged but for the window) and
`LDR x16,#8; BR x16; .quad base+vaddr+12`, `mprotect(RX)`. An anonymous page has no file label and is never written after it
becomes executable, so `execmod` does not apply; the capture above shows this succeeding on the device that denies the other.
The trampoline's address is `*originalOut` of the row's `CallbackThunk`, as now.

### 6. Pass-through by default; activation is one atomic store

`Attach(image)` is the sentinel's first libr15 step. It verifies the image (section 7), builds every row's trampoline, and
stores each trampoline's address in its row's slot. From that moment every patched function behaves exactly as stock. A hook's
install (`InstallInlineThunk`) is reduced to `__atomic_store_n(slot, detour, __ATOMIC_RELEASE)`: no `mprotect`, no page write.
A hook that is refused, or not wanted, stays pass-through. `inline_hook.cpp`'s write, rollback and restore code is deleted.

If a trampoline cannot be made, the slot would stay null and the first call would fault, so the sentinel ends the process
with a named reason (`libr15_patch_trampoline_failed`) through the existing `EndWithExitCode` path (ADR 0020), after logging
the `mmap`/`mprotect` errno. A loud exit is the acceptable cost; a crash at the first call is not.

### 7. Identity: the patched file has its own build ID, and the sentinel refuses anything else

The patcher replaces the 20-byte descriptor of the GNU build-ID note with the first 20 bytes of `SHA-256("NEVR-LIBR15-PATCH-1" ||
stock build-ID bytes || canonical serialisation of the table)`. The note's size and place do not change. The sentinel computes
the same value from its compiled-in table (the sha-256 in `src/quest/data/sha256.h`) and compares it with the live note, so no
generated constant can drift from the patcher's. `kLibR15BuildId` splits: `kLibR15StockBuildId` (patcher, tests) and
`PatchedBuildId()` (everything that is checked at run time: the `GotTarget`s in `pinned_targets.h`, the hook files, the
integration, the hardware dump). The tombstone and the sentinel log then say which libr15 ran.

Refusal, each with one `libr15_patch` line at error level naming the reason:

| Live libr15 | Result |
| --- | --- |
| the stock build ID | `libr15_not_patched`: no libr15 hook is installed (the GOT targets carry the patched ID), the game runs unmodified |
| any other build ID | `build_id_mismatch`, as today |
| the patched ID, but a row fails check (a), (b) or (c) of section 4 | `libr15_patch_row_<check>` and the process ends (`EndWithExitCode`): the file is ours, so a mismatch is a build defect, and a stub with an unfilled slot would crash later |

### 8. credits_text: a data slot, no code write

For the two credits pairs, the patcher rewrites `adrp xN, <blob page>; add xN, xN, #<blob lo12>` to `adrp xN, <slot page>; ldr xN,
[xN, #<slot lo12 / 8>]` (`xN` is x8 and x12; same register, same two words, same site). The value in the slot is the address the
`ADD` used to produce, so everything after the pair is unchanged. `Attach` stores `base + kBlobVaddr` (the game's own text) in the
credits slot; `credits_text::Install` then does what it does today up to the write: validates the build's rendered block, builds
`block + the game's own bytes + NUL` in anonymous memory, makes it read-only, and stores its address in the slot. A refused
install leaves the game's text. `AllocateInReach` and `WriteWord` are deleted: a pointer in a slot has no ADRP reach limit. The
constraint on the instructions between each `ADRP` and its `ADD` (they must not use the page register for anything else) is
the one the run-time patch has today (#970).

### 9. The repack writes the patched file; the signed APK is checked against the patcher

`android-repack-apk` (justfile) extracts `lib/arm64-v8a/libr15.so` from the store APK, runs the patcher, and stages the result
with the shim, in place of the store's entry (the store entry is deflated; the patched one is too). `tools/quest_repack_libs.py
verify-signed` stops requiring libr15 to equal the store's bytes and instead requires it to equal the patcher's output for
that store APK (it runs the patcher again and compares sha-256), and prints both digests; every other library is still
compared with the store's. The patcher is built by the same recipe, so `package-dev` and the release recipe take the same path.

### 10. Logging

The patcher's per-row JSON (section 2) goes to the build log and `--report`. The sentinel writes, to logcat and its on-disk log:
`{"event":"libr15_patch","status":"verified","build_id":"<patched>","stock_build_id":"b243509c...","rows":7,"slot_area":"0x...."}`
once, then per row `{"event":"inline_hook","status":"ok","id":...,"target":...,"slot":...,"trampoline":...,"page_perms":"r-xp"}`
(`page_perms` is read from `/proc/self/maps` after the activation: it is the evidence that no protection changed), and
`credits_text` as today with the slot. The `mmap`/`mprotect` errno logging of the trampoline step (#1131) stays.

## Tests, failing first

Each is run once with the line it covers changed and must fail (the mutant discipline of `AGENTS.md`).

1. **Stub codec.** Encode and decode `ADRP`/`LDR`/`BR` against reference encodings (the join example above is a vector); an
   `ADRP` out of range is refused.
2. **Patcher on a synthetic stock ELF** built in the test (two `PT_LOAD`, a note, `.bss`): the output differs from the input only
   in the stub words, the build-ID descriptor, `p_memsz` and `.bss`'s `sh_size` (a byte-diff test that lists every differing
   range); the output is identical across two runs; a wrong stock word, a wrong build ID, an already-patched input, a site
   outside the text, and a growth that would overlap the next segment are each refused with the row's `id`.
3. **Relocator, 3-word window**, on each row's real stock words, with the existing A64 interpreter (original window against the
   relocated trampoline far away); the `SetText` row's leading `CBZ` is the case that matters.
4. **Sentinel `Attach` on a synthetic patched image in memory**: every refusal in the section 7 table and every check of section 4;
   a trampoline-creation failure ends the process with its reason; after `Attach` every slot holds its trampoline; activation
   stores the detour; and a counting seam on `mprotect` sees **no call on any page of the image** (the test that fails if a code
   write comes back).
5. **Pinned, on the real files** (`just test-quest-hooks-pinned`, given the store APK and the signed APK): the patcher run on the
   store APK's libr15 equals the signed APK's byte for byte; the build ID in the signed APK equals `PatchedBuildId()`; the live
   stubs decode to the slots the table names; every row's stock words are at its vaddr in the store file (this replaces the
   per-pin checks of `got_pinned_test.cpp`, and is how #1128 would have failed at build time).
6. **Source wiring** (`tools/tests`): no file under `src/quest` calls `mprotect` or `mmap` with `PROT_EXEC`/`PROT_WRITE` on a
   range that is not anonymous, except `got_hook.cpp` (data pages) and the trampoline maker; `credits_text.cpp` has no
   `mprotect`. Registered as an N-rule in `AGENTS.md` with a `just verify` sensor when the change lands.
7. **`verify-signed`** unit test: a signed APK whose libr15 is the store's, or a patcher output for another input, fails.
8. **Headset rows** in `docs/testing/smoke-checklist.md`, Quest 3 and Quest 2 (below).

## Prediction

Written before any run. Lines are from the sentinel's log; "no execmod" means the logcat capture for the same process holds no
`avc: denied { execmod }` for `libr15.so`.

**Quest 3 (the tester's headset, OS build 52641960030600520).** In the constructor's log, in this order:
1. `libr15_patch` `status=verified`, `build_id` = the patched ID, `stock_build_id` = `b243509c...`, `rows=7`;
2. seven `inline_hook`/`credits_text` lines `status=ok` (the six hooks, then the credits slot), each `page_perms":"r-xp"`;
3. the constructor's summary reads `steps_failed=0` (the capture's `steps_ok=22 steps_failed=1` becomes 23 and 0: its one failed
   step was `credits_text`);
4. the process stays up past the first minute and reaches the login (`to logged in`); there is no `SIGSEGV`, and no `execmod`
   anywhere in the capture. The menu's version label shows the build's version and the splash skip runs (their pins are now right).
Falsifiers: any `avc: denied { execmod }` for `libr15.so`; a `SIGSEGV` at a libr15 page; a `libr15_patch` line with a reason;
the process dying inside `dlopen` (the linker refused the patched ELF).

**Quest 2 (Horizon v207, build 52242990035800150).** The same four points as the Quest 3, and the same falsifiers.
Both headsets deny `execmod` today, so a Quest 2 run is a second device on the
same policy, and it is also the evidence that the patched ELF loads on the Quest 2's linker: a `dlopen` failure here that
the Quest 3 does not show is a defect of the patched file for that OS.

## What this does not know

- That Bionic's linker (on either OS) loads a libr15 whose writable `PT_LOAD` has a larger `p_memsz` and whose build-ID note
  descriptor is rewritten. Both edits keep every offset and the segment order; neither has been measured here.
- That anonymous executable pages stay allowed. The Quest 3 capture shows them allowed today.
- That the instructions between each credits `ADRP` and `ADD` do not use the page register (the same assumption as the
  existing patch).
- Whether shipping a modified copy of the game's library changes anything about distributing the APK, which already carries the
  unmodified copy. That is not an engineering question and is flagged for the owner.
- The sentinel's constructor runs before libr15's initialisers (a dependency's constructors run first), so `Attach` fills the slots
  before the game can call a patched function. That is the obligation `inline_hook.h` states today, and it is not measured here.

## Alternatives considered

- **A per-device ladder** (probe for `execmod` on a scratch page, else write through `/proc/self/mem`, else skip): two code paths
  chosen at run time, a `/proc/self/mem` permission nobody has measured on this OS, and a "skip everything" tier that drops
  features. Rejected; written and discarded.
- **Trampolines in a new executable segment of the patched file**, so no executable page is ever made at run time. It needs a
  position-independent mode of the relocator (a displaced `CBZ` cannot reach a cave 40 MB away) and two new `PT_LOAD`s
  (code, and data with static defaults), and the file has one spare `PT_NOTE`. Not needed while anonymous executable pages are
  allowed; the table and the stub keep this open (a row's trampoline address becomes a file offset instead of an `mmap`).
- **Patching in the sentinel at run time but through `/proc/self/mem` only**: keeps a code write on every headset.

## Consequences for open work

- #1131 (undo of a failed restore): the code-write path it protects is deleted by this change; the errno logging of the
  trampoline's `mmap`/`mprotect` stays.
- #1125 and #1128 become table rows (`menu_version.set_text`, `splash_skip.widget_update`); neither is merged separately.
- Work order, one PR because a patched libr15 with the old sentinel crashes and the old libr15 with the new sentinel runs
  unhooked: (1) table, stub codec, patcher, relocator window, tests 1 to 3; (2) repack and `verify-signed`, tests 5 and 7;
  (3) `Attach`, activation, the deletions, credits, tests 4 and 6; (4) the headset rows. A Quest 2 and a Quest 3 run precede any
  release that carries it.
