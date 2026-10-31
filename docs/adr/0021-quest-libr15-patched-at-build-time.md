# ADR 0021: libr15.so is patched at build time; no code page is written at run time

Status: proposed (#1132). In the tree and tested on the host: the patcher (`quest_patch_libr15`), the patch table, the patched
build ID and the stub codec (`just test-quest-patcher`, and `just test-quest-patcher-pinned` on the store libr15); the APK repack
step (`just android-repack-apk`); the sentinel's `Attach` and the activation (`libr15_attach.cpp`, run by the constructor's
`attach_libr15` step, `just test-quest-hooks` and `just test-quest-integration`); the credits slot; the plugin identity of
section 12 (`plugin_identity.h`, the loader's `plugin_view_build_id` and `libr15_patched` fields, the GOT backend's build-ID
alias); and the extra-rows variant build of section 11 (`-DNEVR_LIBR15_EXTRA_ROWS=<file>`). The source rule of test 6 is N151
in `AGENTS.md`. No headset run of the patched build has been made: the "Prediction" section is what the first runs must show,
and the "What this does not know" section lists what no measurement in this tree covers.

## Context

The sentinel (`libovrplatformloader.so`, our shim) changes libr15's behaviour in two ways that GOT hooks cannot reach, and
both write libr15's code at run time:

- inline hooks (`git show 9c7c29fccfc38858f797bb0f8da29e32c1844735:src/quest/sentinel/inline_hook.cpp`): the lobby-full join, failure and success handlers, the texture
  `CGTextureResource::Attach`, and the splash-skip widget update. Each overwrites the
  first 16 bytes of a function (`LDR x16,#8; BR x16; .quad detour`) after `mprotect(page, RW)` and puts the page back with
  `mprotect(page, RX)`;
- the credits page (`src/quest/sentinel/credits_text.cpp`, `WriteWord`): two `ADRP`+`ADD` pairs are rewritten the same way.

The menu version label (#747) is neither: it hooks `CCanvasUICS::SetText`, which libr15 calls only through its own PLT stub,
so its call path is one GOT slot (`R_AARCH64_JUMP_SLOT` 0x36ff908, in `.got` inside `PT_GNU_RELRO`, in a writable `LOAD` that is
not executable; 40 `bl` to the stub, none to the function; `src/quest/tests/got_pinned_test.cpp`). The sentinel redirects that
slot like its other libr15 GOT hooks (`src/quest/social/menu_version_label.cpp`): a data page is written and no `PROT_EXEC` is
involved, so the `execmod` rule above does not meet it, it runs today, and it is not a table row.

On a tester's Quest 3 (NEVR 5.10.0, libr15 build ID `b243509c08ce677aeb95fa348016949b3fc45230`, #1127) the second
`mprotect` is denied by policy: `avc: denied { execmod } ... path=".../libr15.so" scontext=u:r:untrusted_app_29:s0
tcontext=u:object_r:apk_data_file:s0 tclass=file permissive=0`. `execmod` is `PROT_EXEC` on a private file mapping that has
been written. The page is left writable and not executable, it stays copy-on-written even when the original bytes are put
back (so a second `mprotect(RX)` is the same denied call), and the first call into it faults: `SIGSEGV`, `pc == x17 ==
libr15+0x18e7b28`, on the page the Attach hook patched, in every launch of the capture. The same capture shows the
trampoline pages (anonymous memory, `mmap` then `mprotect(RX)`) being made without a denial (`"trampoline":"0x0000007b8d527000"`
in the `restore_protect_failed` line of the join hook, and the next hook's line likewise): `execmod` is a rule about file-backed
pages that were written, not about executable anonymous pages. The Quest 2 capture below shows the same, with its addresses (see
"What this does not know").

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
- the kind (`kInlineHook`, `kCreditsPair` or `kWordPatch`, section 11) and the handler's name.

The sentinel's hook files (`lobby_full_hooks.cpp`, `ui_texture_mips.cpp`, `splash_skip_hook.cpp`) keep
their handlers and lose their per-file `kXxxVaddr` and prologue constants; they name a table row. The six rows are:
`lobby_full.join` (ReVault 0x1340ba8), `lobby_full.failure` (0x1340cc8), `lobby_full.success` (0x134024c), `ui_texture.attach`
(0x19e75c4), `splash_skip.widget_update` (0x209a7a0; #1128's correct address), and
`credits.pairs` (ReVault 0x1ea1284/0x1ea1290 and 0x1ea1d84/0x1ea1d88, the two pairs of #970).

### 2. The patcher: a host tool that reads the stock file and refuses on any doubt

`quest_patch_libr15` (new, C++ compiled by the justfile with the host `g++`, as the `test-quest-hooks` tests are; it includes the
table header and `arm64_reloc.h`, so there is no second encoding of anything). `quest_patch_libr15 --stock <libr15.so> --out
<file> --report <json> [--extra-rows <file>]` (the optional file is section 11):

1. refuses unless the stock file's GNU build ID is `kLibR15StockBuildId` (the current `kLibR15BuildId`) and its sha-256 is the
   pinned one;
2. for each row, refuses unless the words at `vaddr` are the row's stock words. A wrong ReVault address, or a typo in a word, is
   a build failure with the row's `id`, not a refused hook on a headset;
3. writes the stubs (below), the slot area (below), the credits pairs, and the new build ID (below). Every byte that differs from
   the stock file is accounted for in the report: `{"event":"libr15_patch_site","id":...,"revault_va":...,"vaddr":...,
   "stock":[...],"stub":[...],"slot":...}` per row, then a summary with the stock and patched sha-256. The output is a pure
   function of the stock file and the table: the same inputs give the same bytes. The report's vocabulary is fixed: the `kind` of
   a `libr15_patch_site` event is `inline_hook` or `credits_pair` (a word patch has its own `libr15_patch_word` event, which has no
   `kind`); each entry of the summary's list of changed byte ranges has a `what`
   that is the row id and a suffix, `:stub` (an inline site's three words), `:credits_adrp` and `:credits_ldr` (the two words of a
   credits pair) or `:word` (a word patch); `end_symbols` lists the six end symbols sorted by name in byte order. The summary's
   `status` says what is on disk: `patched` only once `--out` is in place and has been read back, `refused` for a refusal (exit 1,
   nothing written to `--out`), `io_error` when `--out` could not be written (exit 2), and `incomplete` in the record written
   before `--out` is touched, which a run that dies while writing leaves behind. The temporary file beside `--out` is created
   exclusively and never opened through a link; a name that is already taken stops the run;
4. validates its own output as an ELF (PT_LOAD sorted and non-overlapping, `p_filesz <= p_memsz`, the build-ID note intact, the
   section table consistent) and refuses to write a file that fails.

### 3. The stub: three words, no write at run time

Each inline site's first 12 bytes become

```
adrp x16, <page of this site's slot>
ldr  x16, [x16, #<slot offset in its page, scaled by 8>]
br   x16
```

For `lobby_full.join` (vaddr 0x1240ba8, slot 0): `0xd0012c90`, `0xf9432210`, `0xd61f0200`. `x16` (IP0) is the register the 16-byte
patch already uses; the AAPCS64 lets a call site treat it as clobbered. The stub is position independent: `ADRP` and the `LDR`
use only the site's own address and the slot's page, so a load bias changes nothing. The window shrinks from 16 to 12 bytes:
no caller may branch to site+4..site+11 (the same obligation as today, over fewer bytes). `arm64_reloc.h`'s window constant
(`kRelocWindowBytes`, 16 today) becomes a parameter set to 3 words for these sites.

### 4. Slots: the tail of `.bss`, one 8-byte slot per row, filled at run time

libr15's writable `PT_LOAD` (vaddr 0x35f9eb0, `p_memsz` 0x1d8688 in the pinned file) ends at 0x37d2538, inside a page the
linker already maps (the page ends at 0x37d3000); nothing follows it. Other patches of this libr15 keep their own state words in
the same slack right after the image end (the reference build's two at 0x37d2540 and 0x37d2560, the community patcher's latches
at 0x37d2540, 0x37d2560 and 0x37d2570), so the patcher leaves `kSlotAreaGap` (0x100) bytes free and puts the slot area at
`roundup(0x37d2538 + 0x100, 64) = 0x37d2640`. It raises that segment's `p_memsz` (and `.bss`'s `sh_size`) so it ends at
`0x37d2640 + 8 * rows = 0x37d2670` (`p_memsz` 0x1d87c0; `p_filesz` and every file offset unchanged). The slot area is
zero-initialised memory outside `GNU_RELRO` (which ends at 0x372c000), so a store to a slot needs no `mprotect` at all. Slot *i*
is at `0x37d2640 + 8 * i`. A row's stub names its own slot; the credits pairs share one slot.

**The input's own references.** The patcher refuses an input whose own code reaches the slot area (`slot_area_referenced`,
naming the site and the address). It reads every executable `PT_LOAD` as A64 words, follows each `ADRP` near the slot area
through the next 32 words (`ADD` immediate, `MOV`, and the base of load/store, `LDUR`/`STUR` and `LDP`/`STP`) and checks `ADR`
on its own; an address within 16 bytes of the area's bytes is a reference. It reads code only. On the pinned stock file and both
accepted reference files it finds none.

**What the scan does not detect.** The scan is a guard for other inputs, not the proof that the known reference words are clear.
It does not detect: the second register of an `LDP`/`STP` of Q registers (its access at address + 16, where only the first 16
bytes are checked); register-offset loads and stores (`LDR Xt, [Xn, Xm]`), whose address depends on `Xm`; index arithmetic
after a register-form `ADD` (the destination keeps its earlier tracked value); absolute addresses built by `MOVZ`/`MOVK`/`MOVN`;
and any use more than 32 words after the `ADRP` that starts it, or past a `B`, `BR` or `RET` (a `BL` does not end the window).
The known reference words (0x37d2540, 0x37d2560) stay clear of the slot area because of `kSlotAreaGap`, independent of the scan.
The pinned test `ReferenceLibr15sArePatchedWithTheirStateWordsClearOfTheSlots` checks those words with the gap in place.

**How a slot is found at run time.** There is no directory to read. For each row the sentinel computes `slot = base +
row.slotVaddr` from the table and checks it three ways before it stores anything: (a) the three words live at the site are
exactly the stub the table implies (so a stock site, or a stub from another table, is refused); (b) decoding the live `ADRP` and
`LDR` gives the same slot address; (c) the slot lies inside the live program headers' writable `PT_LOAD` and at or after
`p_vaddr + p_filesz` (it is bss). Any failure is a refusal that names the row and the check.

**The linker-defined end symbols are not updated.** libr15's `.dynsym` defines six absolute symbols at the end of its image:
`__end__`, `__bss_end__`, `_bss_end__` and `_end` (all 0x37d2538, the old end of `.bss`), and `_edata`, `__bss_start`
(0x3761178). Their names are in `.dynstr`. The patcher leaves them as they are: `.dynstr` is not touched, and the slot area
(section 4) lies after `_end`. Nothing reads them, measured on the pinned file (`readelf -rW` and `readelf --dyn-syms -W` on
the four libraries of the store APK, `strings -a` on libr15):
- no relocation in libr15 names any of the six, and none of the four libraries (`libr15.so`, `libpnsovr.so`,
  `libpnsradmatchmaking.so`, `lib7f2363b8976491aa.so`) has an undefined reference to one;
- each name occurs once as a string in libr15 (its `.dynstr` entry; `_bss_end__` is the tail of `__bss_end__`'s), so no literal
  `dlsym` argument names one (libr15 imports `dlsym`; a name built at run time is not excluded by this);
- no file under `src/` or `tools/` names one (`git grep` for the six).
A reader that took `_end` as the first free address would see the slots as free, and no such reader exists in these four
libraries; the patcher's report lists the six values unchanged so a future reader is a visible change. Pinned test: the
patched file's six symbol values equal the stock file's (test 5 below).

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
A hook that is refused, or not wanted, stays pass-through. The old inline hook's write, rollback and restore code (`git show 9c7c29fccfc38858f797bb0f8da29e32c1844735:src/quest/sentinel/inline_hook.cpp`) is deleted.

If a trampoline cannot be made, the slot would stay null and the first call would fault, so the sentinel ends the process
with a named reason (`libr15_patch_trampoline_failed`) through the existing `EndWithExitCode` path (ADR 0020), after logging
the `mmap`/`mprotect` errno. A loud exit is the acceptable cost; a crash at the first call is not.

### 7. Identity: the patched file has its own build ID, and the sentinel refuses anything else

The patcher replaces the 20-byte descriptor of the GNU build-ID note with the first 20 bytes of `SHA-256("NEVR-LIBR15-PATCH-1" ||
stock build-ID bytes || canonical serialisation of the table, extra rows included)`. The note's size and place do not change. The sentinel computes
the same value from its compiled-in table (the sha-256 in `src/quest/data/sha256.h`) and compares it with the live note, so no
generated constant can drift from the patcher's. `kLibR15BuildId` splits: `kLibR15StockBuildId` (patcher, tests) and
`PatchedBuildId()` (everything that is checked at run time: the `GotTarget`s in `pinned_targets.h`, the hook files, the
integration, the hardware dump). The tombstone and the sentinel log then say which libr15 ran.

**The canonical serialisation is these bytes.** Its public part is text, every number written as `0x` and lowercase hex without
leading zeros: the line `layout|bias=<kRevaultBias>|slot_area=<slot area>|slot_bytes=<slot size>` and a newline, then one line
per table row, in table order, `row|<id>|<kind>|slot=<n>` where `<kind>` is `inline`, `credits` or `word`, followed for each site by
`|<revault_va>:<word>,<word>,...` (the stock words, in order, each written as a number) and a newline. A build with extra rows
(section 11) continues with the line `extra|<count>` and a newline and then, with no separator, one binary record per extra
row, the rows sorted by `revault_va` and then by `id` in byte order: the id length (1 byte), the id, `revault_va` (8 bytes,
little-endian), the word count (1 byte), and for each word `offset` (2 bytes, little-endian), then `new_bytes` (4 bytes), then
`stock_bytes` (4 bytes), the word bytes in memory order as in the file. A build without extra rows has no `extra` line, so its ID
is the public one. The byte widths are fixed here so that every program that derives the ID (the patcher, the sentinel's
compile) gets the same value from the same rows; a test pins the ID of a fixed set of invented rows.

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
A build with extra rows (section 11) passes the same file to the patcher, to `verify-signed` and to the sentinel's compile;
the public recipes pass none.

### 10. Logging

The patcher's per-row JSON (section 2) goes to the build log and `--report`. The sentinel writes, to logcat and its on-disk log:
`{"event":"libr15_patch","status":"verified","build_id":"<patched>","stock_build_id":"b243509c...","rows":6,"slot_area":"0x...."}`
once, then per row `{"event":"inline_hook","status":"ok","id":...,"target":...,"slot":...,"trampoline":...,"page_perms":"r-xp"}`
(`page_perms` is read from `/proc/self/maps` after the activation: it is the evidence that no protection changed), and
`credits_text` as today with the slot. The `mmap`/`mprotect` errno logging of the trampoline step (#1131) stays.

### 11. Word patches, and rows that are not in the public table

`kWordPatch` is a row that replaces words inside a function and nowhere else. It has no slot, no trampoline, no handler and no
relocator window, and the sentinel installs nothing for it. A row is an `id`, a ReVault address and a list of
`{offset, stock word, replacement word}`; each offset is a multiple of 4 from the address, and each replacement is one
32-bit word that takes the place of the stock word at the same address. The patcher:

- refuses unless every stock word is what the stock file holds at that vaddr, naming the row `id` and the word's offset;
- refuses a word that lies in another row's bytes (an inline hook's first 12 bytes, a credits pair, another word patch), and a
  word outside the text;
- writes the replacements, and reports each as `{"event":"libr15_patch_word","id":...,"vaddr":...,"stock":...,"word":...}`.

A replacement is only what one word can do: a PC-relative word is encoded for its own address, and no word reaches code that
the file does not already hold (libr15 has no spare executable space; the executable-segment alternative below is not built).
The "no caller branches into site+4..+11" obligation of section 3 does not apply, because no entry is rewritten. At run time
`Attach` checks the live words equal the replacements; a difference is `libr15_patch_row_words`, ending the process like
the other row checks.

**Extra rows.** `--extra-rows <file>` is a JSON list of `kWordPatch` rows (and only that kind: they add no slot, so the slot
area and `p_memsz` of section 4 do not depend on them). The public table, the public release APK and the public recipes carry
none, and the repo holds no address or word of a row that came this way. The rows are merged into the table before the
serialisation of section 7, so a build with extra rows has its own patched build ID. The sentinel of such a build is compiled
with the same file, so it computes that ID; a public sentinel meeting such a libr15 reports `build_id_mismatch`, and a variant
sentinel meeting a public libr15 reports the same, so neither can pass as the other. The patcher's summary and the sentinel's
`libr15_patch` line carry `extra_rows` (a count) and `extra_rows_sha256` (the file's digest), never the rows. A variant sentinel is
built with `-DNEVR_LIBR15_EXTRA_ROWS=<file>` (an empty cache variable by default; configure parses the file with the patcher's
parser and fails on a malformed one) and its APK repacked with `just android-repack-apk ... extra_rows=<file>`, the same file.
`android-repack-apk` ties the two before it stages anything (`tools/quest_repack_libs.py check-shim-rows`): with `extra_rows` the shim must
carry that file's sha-256, and without it the shim must not be a variant build. A pair that does not match is refused there, because on
the device the sentinel would report `build_id_mismatch` and install nothing while the patched library's stubs branch through slots
only an attached sentinel fills.

**The extra-rows file, format 1.** One UTF-8 JSON object. Every field below is required, an unknown field at any level is a
refusal, and so is a duplicate key in one object (the patcher reads the file with a parser that sees duplicates). The example
uses invented values.

```json
{
  "format": "nevr-libr15-extra-rows",
  "version": 1,
  "stock_build_id": "b243509c08ce677aeb95fa348016949b3fc45230",
  "rows": [
    {
      "id": "x.example_guard",
      "kind": "word_patch",
      "revault_va": "0x1500010",
      "words": [
        { "offset": 0, "stock_bytes": "e0031faa", "new_bytes": "1f2003d5" },
        { "offset": 8, "stock_bytes": "00010054", "new_bytes": "1f2003d5" }
      ]
    }
  ]
}
```

- `format` is exactly `nevr-libr15-extra-rows` and `version` is the integer 1. `stock_build_id` is 40 lowercase hex digits and
  must equal the patcher's pinned stock ID: the file is bound to one libr15, and a file for another is refused before any row is
  read.
- A **row is identified by `id`**: `x.` followed by 1 to 62 characters of `[a-z0-9_.]`, unique in the file and never equal to a
  public row's id (public ids do not start with `x.`). Every refusal, report line and log line names the row by `id`.
- `kind` is exactly `word_patch` (the only kind a file may carry). `revault_va` is the ReVault address of the first word the row
  may touch, as `0x` followed by lowercase hex without leading zeros; the patcher derives the file address with
  `kRevaultBias` (section 1) and the typed value is the only address in the file. It is a multiple of 4.
- `words` holds 1 to 64 entries in strictly increasing `offset` order. `offset` is a JSON integer, a multiple of 4, 0 to 4092, the
  distance in bytes from `revault_va`. A file holds at most 256 words in all.
- **A word is written as bytes in memory order**: `stock_bytes` and `new_bytes` are each exactly 8 lowercase hex digits, the four
  bytes in the order they stand in the file and in `xxd` or `objdump -s` output, without a prefix. They are not the little-endian
  value of the instruction: the A64 instruction `0xd503201f` (NOP) is `1f2003d5`. The patcher's report prints both forms
  (`stock_bytes` and `stock_word`) for every word, so a reader can compare against a disassembler.
- A row is refused when any `stock_bytes` differs from the stock file at `revault_va + offset - kRevaultBias` (the message names
  the `id` and the `offset`), when a word lies outside the text segment, when it lies in a byte a public row or another extra row
  also writes, when it lies in the ELF header, the program header table, the section header table or the build-ID note, or in a
  byte the patcher writes itself (the `p_memsz` of the grown segment, the `sh_size` of the section that ends it; the build ID is
  in the note) (the refusal is `protected_region`, naming the `id` and the region), or when `new_bytes` equals `stock_bytes`. A
  public row's site is held to the same rule.
- Build identity: the extra rows are part of the canonical serialisation of section 7, which defines the bytes (the `extra` line,
  then one binary record per row, rows sorted by `revault_va` and then by `id`), so the build ID does not depend on the file's
  key order, spacing or the order of its rows. `extra_rows_sha256` in the report is the digest of the file's bytes as given.
- Public prose and the file: the file is supplied at build time by whoever builds the variant. It is not committed, and the
  tests use only invented addresses, in a fixture file beside the patcher tests that is the example above made into a file
  for the synthetic ELF of test 9.

### 12. Plugins keep the stock identity

ADR 0019 compares each plugin's declared build-ID list (`NEVR_DECLARE_SUPPORTED_BUILD_IDS`) with the running libr15's live note
(`SentinelPlatform::GameBuildId` into `DecideBuildId`, `plugin_loader.cpp`), and the GOT backend compares a target's `buildId`
with the live note (`Prepare` in `got_hook.cpp`). The GOT backend's slot registry is keyed by slot address (`g_slots` in
`got_hook.cpp`), not by build ID. After section 7 the live note is the derived ID, which no plugin lists: every plugin would be
refused `build_id_unsupported`, and every target that carries the stock ID `kBuildIdMismatch`. The patched file differs from the stock one only in the bytes the patcher reports (stubs, credits pairs, word
patches, the slot area, the note and the `p_memsz` fields), so a plugin's addresses are properties of the stock file and hold.

**Decision.** The loader hands plugins the **plugin identity**:

- it is `kLibR15StockBuildId` when the running libr15 is our patched file, and the live note otherwise;
- "is our patched file" needs both: (a) `Attach` recorded `libr15_patch status=verified` in this process, which means the live
  note equalled `PatchedBuildId()` recomputed from the compiled-in table (extra rows included, section 11) and every row passed
  checks (a) to (c) of section 4, so the live stub words are the table's; and (b) the live note, read again by the loader, still
  equals `PatchedBuildId()`. With either missing the identity is the live note, and a plugin built for the stock ID is refused
  as today. A file that only carries the derived ID in its note, without the verified stubs, gets no mapping;
- it is used in three places: `DecideBuildId` accepts a plugin whose list names the plugin identity (a list that names the live
  note is accepted only on the verified patched file; any other libr15 is refused `libr15_not_verified`, ADR 0019 section 7); `ctx.game_build_id` is the plugin identity, so a plugin that copies it into its targets works on
  both files; and the GOT backend accepts a target whose `buildId` equals the live note or the alias that
  `sentinel::SetBuildIdAlias` gives that note. The alias function the constructor registers answers with the stock ID when the
  plugin identity is the stock ID and with nothing otherwise. The registry is keyed by slot address, so the sentinel's claim on a
  slot (its target carries the derived ID, section 7) and a plugin's claim on the same slot (its target carries the stock ID)
  are one key, and the second claim is refused as already owned;
- the plugin host API of ADR 0019 slice 2, which would pass a plugin's targets to the GOT backend, is not in the tree. What
  exists of this decision on that side is the alias function; the first plugin that installs a hook finds it in place;
- the `plugin_load` record keeps `game_build_id` as the live note and gains `plugin_view_build_id` (the ID compared) and
  `libr15_patched` (true or false), so the log says which file ran and which ID decided.

A stock libr15 under a patched sentinel (`libr15_not_patched`, section 7) has the live note as its identity, and plugins are
decided against it exactly as today.

**What a plugin sees of the patched sites.** The patched file's bytes at the table's sites are the stubs, the credits pairs'
`LDR` and the word patches. `CheckedWrite` requires the expected stock word (ADR 0019), so a plugin write over a patched site is
refused loudly; a plugin that reads a patched entry reads the stub, not the stock prologue. The site list is the patcher's
report, and a plugin that needs a stock prologue at one of those addresses cannot have it from this file.

## Tests, failing first

Each is run once with the line it covers changed and must fail (the mutant discipline of `AGENTS.md`).

1. **Stub codec.** Encode and decode `ADRP`/`LDR`/`BR` against reference encodings (the join example above is a vector); an
   `ADRP` out of range is refused.
2. **Patcher on a synthetic stock ELF** built in the test (two `PT_LOAD`, a note, `.bss`): the output differs from the input only
   in the stub words, the build-ID descriptor, `p_memsz` and `.bss`'s `sh_size` (a byte-diff test that lists every differing
   range); the output is identical across two runs; a wrong stock word, a wrong build ID, an already-patched input, a site
   outside the text, and a growth that would overlap the next segment are each refused with the row's `id`, and an input whose
   own code forms an address in the slot area (each form the scan reads, and the window and width limits) is refused with the site.
3. **Relocator, 3-word window**, on each row's real stock words, with the existing A64 interpreter (original window against the
   relocated trampoline far away); the leading `CBZ` of `CCanvasUICS::SetText` is the case that matters (`TheQuestSetTextPrologueRelocates`,
   which stays a relocator case though the label no longer needs the relocator).
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
9. **Word patches and extra rows**, on the synthetic stock ELF: a wrong stock word, an overlapping word, a word outside the text
   and an offset that is not a multiple of 4 are each refused with the row's `id`; the output differs from the input in the
   named words and nowhere else; a file of extra rows changes the build ID and the same file twice gives the same bytes; a
   row of any kind but `kWordPatch` in the file is refused; each rule of the extra-rows format is refused in its own case (wrong
   `format`, `version` or `stock_build_id`; an unknown field; a duplicate key; a bad, duplicate or public `id`; a `revault_va`
   or `offset` not a multiple of 4; offsets out of order; 65 words in a row; a word written as a little-endian value instead of
   bytes; a stock mismatch; a word outside the text, overlapping a public row or another extra row, in the ELF header, either
   header table, the build-ID note or a byte the patcher writes itself, or unchanged); the fixture
   loads, and its report prints each word's `stock_bytes` and `stock_word`; the public table's ID is the one a build without the file computes;
   `Attach` on an image whose live word differs ends with `libr15_patch_row_words`.
10. **Plugin identity** (section 12), in `plugin_loader_test.cpp` with the fake platform, and `got_hook_test.cpp`:
    - a plugin declaring only the stock ID is `loaded` when the platform reports the live note `PatchedBuildId()` with
      `libr15_patch` verified; its `ctx.game_build_id` is the stock ID, its record has `game_build_id` = the derived ID,
      `plugin_view_build_id` = the stock ID and `libr15_patched` true;
    - the same plugin is refused `build_id_unsupported` when the live note is the derived ID but `Attach` did not record
      `verified`, when `verified` was recorded but the live note read now is another ID, and when the running libr15 is a third
      ID;
    - a plugin declaring only the derived ID loads on the patched file and is refused on a stock one;
    - on a stock libr15 the plugin declaring the stock ID loads, with `libr15_patched` false;
    - a plugin target carrying the stock ID resolves on the patched image and is refused (`kBuildIdMismatch`) on the same image
      when the patch is not verified; a target carrying the derived ID resolves on the patched image;
    - the sentinel's claim on a slot with the derived ID, then a plugin's claim on the same slot with the stock ID, is refused
      as already owned (one key: the slot address);
    - pinned, on the real files (test 5): the `plugin_load` decision for a fixture declaring the stock ID, given the note of
      the patcher's output for the store APK's libr15, is `loaded`.
    Mutants: map to the stock ID without the `verified` record; map without re-reading the note; compare the list with the live
    note only; `ctx.game_build_id` set to the live note; an alias function that answers the stock ID whatever it is asked. Each
    must fail one of the cases above.

## Prediction

Written before any run. Lines are from the sentinel's log; "no execmod" means the logcat capture for the same process holds no
`avc: denied { execmod }` for `libr15.so`.

**Quest 3 (the tester's headset, OS build 52641960030600520).** In the constructor's log, in this order:
1. `libr15_patch` `status=verified`, `build_id` = the patched ID, `stock_build_id` = `b243509c...`, `rows=6`;
2. five `inline_hook` lines `status=ok`, each `page_perms":"r-xp"` (one per inline row of the table), and one `credits_text`
   line `status=applied` (the credits slot);
3. the constructor's summary reads `steps_failed=0`, and `steps_ok + steps_failed + steps_skipped` is the number of constructor
   steps (32 in this tree: the `StepId` values before `kCount` in `ctor_sequence.h`, including `attach_libr15`). The earlier
   capture's one failed step was `credits_text`, which is now an `ok` step;
4. the process stays up past the first minute and reaches the login (`to logged in`); there is no `SIGSEGV`, and no `execmod`
   anywhere in the capture. The menu's version label shows the build's version (its GOT hook installs without the patch) and the splash skip runs (its pin is now right).
5. with the no-op plugin of ADR 0019 slice 1 (it declares the stock ID only) in `plugins:`: one `plugin_load outcome=loaded`
   record with `game_build_id` = the patched ID, `plugin_view_build_id` = `b243509c...`, `libr15_patched` true.
Falsifiers: any `avc: denied { execmod }` for `libr15.so`; a `SIGSEGV` at a libr15 page; a `libr15_patch` line with a reason;
the process dying inside `dlopen` (the linker refused the patched ELF); the plugin record with `build_id_unsupported`.

**Quest 2 (Horizon v207, build 52242990035800150).** The same five points as the Quest 3, and the same falsifiers.
Both headsets deny `execmod` today, so a Quest 2 run is a second device on the
same policy, and it is also the evidence that the patched ELF loads on the Quest 2's linker: a `dlopen` failure here that
the Quest 3 does not show is a defect of the patched file for that OS.

## What this does not know

- That Bionic's linker (on either OS) loads a libr15 whose writable `PT_LOAD` has a larger `p_memsz` and whose build-ID note
  descriptor is rewritten. Both edits keep every offset and the segment order; neither has been measured here.
- That anonymous executable pages stay allowed, and that code runs from them. What the captures show is the creation: the
  sentinel maps a page `PROT_READ|PROT_WRITE`, copies the displaced words in, and `mprotect`s it `PROT_READ|PROT_EXEC`
  (`git show 9c7c29fccfc38858f797bb0f8da29e32c1844735:src/quest/sentinel/inline_hook.cpp`: the `mmap`, the `mprotect` with the `trampoline_rx` failure path, then the page write that fails); the
  `trampoline` field of the `restore_protect_failed` line is logged only when both succeeded.
  Quest 3: `trampoline` `0x0000007b8d527000` (Context).
  Quest 2 (the run of 2026-10-28, v5.16.1 first launch, 07:09 to 07:41 local time): the sentinel log holds six launches between
  07:38:53 and 07:39:07 local time (`ts_unix_ms` 1793191133724 to 1793191147229), each with two `restore_protect_failed` lines
  and the same two trampolines: `0x000000748dfe9000` for the join hook (target page ending `ba8`) and `0x000000748df5d000` for
  `ui_texture.attach` (target page ending `5c4`), 12 lines in all, none with `trampoline_map` or `trampoline_rx`. The same log's
  later launch of a 5.16.2 development build writes no code and makes no trampoline (`code_write_skipped`). Limits: the sentinel
  log has no device field (`hwdump` is off), so "Quest 2" rests on the run's record and the timestamps; the logcat capture
  of that run has no `NEVR-Sentinel` and no libr15 line at all, so its silence on `execmem` or `execmod` for libr15 proves
  nothing; and neither device has shown a call through a trampoline, because the first call into the patched page faulted
  before reaching it. The first Quest 2 run of this design is the first execution from an anonymous executable page on that OS.
- That the instructions between each credits `ADRP` and `ADD` do not use the page register (the same assumption as the
  existing patch).
- Whether shipping a modified copy of the game's library changes anything about distributing the APK, which already carries the
  unmodified copy. That is not an engineering question and is flagged for the owner.
- The sentinel's constructor runs before libr15's initialisers (a dependency's constructors run first), so `Attach` fills the slots
  before the game can call a patched function. That is the obligation `git show 9c7c29fccfc38858f797bb0f8da29e32c1844735:src/quest/sentinel/inline_hook.h` stated, and it is not measured here.

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
- #1128 becomes a table row (`splash_skip.widget_update`) and #1125 is replaced by the label's GOT hook; neither is merged separately.
- Work order, one PR because a patched libr15 with the old sentinel crashes and the old libr15 with the new sentinel runs
  unhooked: (1) table, stub codec, patcher, relocator window, tests 1 to 3; (2) repack and `verify-signed`, tests 5 and 7;
  (3) `Attach`, activation, the deletions, credits, tests 4 and 6; (4) the headset rows. A Quest 2 and a Quest 3 run precede any
  release that carries it.
