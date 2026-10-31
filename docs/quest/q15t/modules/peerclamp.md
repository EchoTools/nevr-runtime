# Module `peerclamp` (row R36)

Files: `src/peerclamp.{h,cpp}`, `src/quest/tests/q15t/peerclamp_test.cpp`, `src/quest/tests/q15t/mod/peerclamp.sh`, `src/quest/tests/q15t/mutants.d/peerclamp.sh`.

Option macro for the main seat to wire: `Q15T_PEERCLAMP`, default **ON** (#1625: the wave19a on-list turns on `peerclamp.on`; source: "TEST ONLY", the source build's peerclamp:1; `Switch()` is
true only for `peerclamp.on` or `debug.<app>.peerclamp 1`, `:181-200`; `source-build peerclamp_install` `:210-211`). No file switch is
kept (the source's is an enabling switch). API: `InstallResult InstallPeerClamp(std::uintptr_t base)`; seam `InstallPeerClampAt(base,
slot, real, invalid_vaddr)`; `PeerClampCount()`. No tick. The module changes game state (writes a stamp): the plugin's capability
flags should include ALTERS_GAMEPLAY when it is on.

## Row covered

R36: `SBroadcasterData::CleanupPeers()`, JUMP_SLOT ELF 0x36efad8, real function ELF 0x24ef9b0 = disassembly 0x25ef9b0 (size 1260). Read:
- `R0_readelf_r.txt:131055`: `00000000036efad8  00018d3f00000402 R_AARCH64_JUMP_SLOT    00000000024ef9b0 _ZN10NRadEngine16SBroadcasterData12CleanupPeersEv + 0`
- Disassembly `functions`: `24ef9b0 | CleanupPeers | 1260`; `R1_functions.md:49` (1 caller; stamps 0x25efa74/94/e4 3/3 MATCH).
- All 26 of the source's pinned words equal disassembly; the ones the wrapper depends on:
```
24ef9d8 b945541b ldr w27, [x0, #0x554]         head index
24efa68 b9456268 ldr w8, [x19, #0x560]         entries per chunk
24efa6c f9428e69 ldr x9, [x19, #0x518]         chunk pointer table
24efa74 f9423e78 ldr x24, [x19, #0x478]        "now"
24efa30 5281291c mov w28, #0x948               entry stride
24efa90 b940091b ldr w27, [x8, #0x8]           next index
24efa94 f9411519 ldr x25, [x8, #0x228]         last-received stamp
24efa98 91006116 add x22, x8, #0x18            the entry's key
24efa84 52801002 mov w2, #0x80                 key length (memcmp against kInvalid)
24efaa4 b4fffd40 cbz x0, 0x025efa4c            key == kInvalid -> "disconnected" branch
24efae4 cb190319 sub x25, x24, x25             age = now - stamp, unsigned, no clamp
24efa14 d0009177 adrp x23, 0x381d000 ; 24efa18 f9429ef7 ldr x23, [x23, #0x538]   &SAddress::kInvalid (GOT word ELF 0x371d538)
24eeec8 f9400260 ldr x0, [x19] ; 24eeecc 97a90645 bl <PLT CleanupPeers>          the one caller (Receive(uint)), x0 = the SBroadcasterData
```
Source: constants the source build's peerclamp:39-44, `Clamp :83-111`, `CleanupHook :113-116`.

## What it does

Crash (source `:1-8`): the lobby -> match switch can destroy the new game-server peer as "communication timeout" 0.25 s after connect: a stamp
above "now" makes `age = now - stamp` wrap to ~2^64 us and every timeout branch fires.
Wrapper (`void (void* bd)`): one SafeRead of `bd[+0x470, +0x564)` (a failed read leaves it zero: `per == 0`, nothing to do), one SafeRead of
`kInvalid` (0x80 bytes; unreadable: nothing is clamped), then the stock walk: from the head index, each entry at `chunks[idx / per] + (idx %
per) * 0x948`; read the entry span `+0x8..+0x230` in one SafeRead (unreadable: the walk ends); an entry whose key (0x80 bytes at +0x18)
differs from `kInvalid` and whose stamp (+0x228) is **above** `now` gets its stamp set to `now` (an 8-byte store at an address just
read; `now` is never written; stamps <= now, including a real silence, are untouched); `idx = next` until the end marker
0xffffffff or **4096** entries (one WARNING `peerclamp walk capped`, once). Then the real function runs, always, with the same
argument. INFO per clamp (idx, stamp, now, diff, bd+0x470, count; the first 16, then a total every 100th at most 8 times).

Mechanism: at install the pointer word at `base + 0x371d538` is read (null/unreadable: refused, `invalid_unreadable`, ERROR), then
`InstallImportHook(symbol, base, 0x36efad8, &CleanupWrapper, &g_real, 0x24ef9b0)`: the slot must hold exactly `base + 0x24ef9b0`.
Same thread as the stock loop (it runs inside `Receive(uint)`); no lock, no allocation, no engine call.
Logging: INFO `peerclamp on (TEST ONLY)`, ERROR `peerclamp refused`, INFO per clamp (capped), WARNING `walk capped`.

## Gate: no MD5, no file hash

The only gates are: the plugin's build-id check in InitEx, the code-word pins below (`src/quest/plugins/q15t/src/word_pins.h`, `CheckWordPins`: every
word is read with SafeRead at `base + ELF vaddr` before any slot is touched) and the slot-target check (the slot must hold exactly
`base + the real function's vaddr`). **There is no MD5 gate and no whole-file hash of any kind**: a rebuilt, repacked or custom-server
build that still carries these words is not refused. A pin that fails refuses the module (nothing swapped, reason `word_pin_mismatch`)
with one ERROR line naming it: `<module> refused: code word pin mismatch, ... pin=0x<ELF vaddr> found=0x<word read, or unreadable>
expected=0x<word pinned>`. A slot-target refusal logs ERROR with `pin=<slot ELF vaddr> found=<word at the slot> expected=<base + real
vaddr>`. Pin addresses in this note are ELF vaddrs (disassembly VA = ELF + 0x100000). The 26 pins are the source's `kWords` (the source build's peerclamp:50-62).
All pins read in the disassembly equal the tables in `src/quest/plugins/q15t/src/peerclamp.h` (one query whose VALUES are built from the header tables:
`MATCH | 88` over the five modules' tables, no NOT_SHOWN, no MISMATCH). Host test: every pin flipped in turn is refused and named, an
unreadable text is refused and named, the table's fingerprint is pinned, the installer refuses on a changed word (`src/quest/tests/q15t/pin_check.h`).

## Prediction (written before any headset run)

With `Q15T_PEERCLAMP=1`: `peerclamp on (TEST ONLY) ... status=ok`. Across lobby -> match transitions no new game-server peer is destroyed with
"communication timeout" 0.25 s after connect. In most sessions **zero** clamp lines (the race is rare); when the race happens one line
`peerclamp pulled a peer's last-received stamp down to now idx=<n> stamp=<s> now=<t> diff=<small or huge>` and the peer survives.

## Falsifiers

- `peerclamp refused: code word pin mismatch` (ERROR; pin / found / expected named) on the build the plugin accepted by build id: the code at that
  address is not what the wrapper was derived from (or the pin table has a typo); nothing is swapped, the game runs stock.
- The 0.25 s "communication timeout" disconnect still occurs and no clamp line was logged: the stamp is not above `now` (`now` is
  stale / the age is computed elsewhere), or the walk missed the entry (layout wrong).
- Clamp lines in every session at steady state: `now` lags the stamps normally (the cached "now" is older than receive time); the clamp
  would then mask a real silence (peers that left never time out). Look for peers stuck after a real disconnect.
- `peerclamp refused ... invalid_unreadable`: the GOT word at 0x371d538 is not relocated at InitEx time.
- `walk capped` logged: the peer list is cyclic or longer than 4096 (stale `next`).

## Needs a word patch / not built

- **Needs a word patch:** none.
- Not built: the md5 gate (deliberately none: the source accepted three whole-file MD5s only); the `.on`/property switch (option). The 26 code-word pins are kept.
- Unverified: that `kInvalid` bytes are stable for the process (the wrapper re-reads them each call, the source compared against
  the live object too); that `bd+0x470` is the "sent" counter (log field only); the single-thread claim (source `:20-22`); the
  8-byte stamp store is a plain write to heap memory (the contract's SafeRead rule covers reads; there is no SafeWrite).
