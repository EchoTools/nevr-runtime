# Early quit: the lockout end to end

Vocabulary: the **game client** is `echovr.exe` on a player's machine (PC) or `libr15.so` (Quest); the
**game service** is nakama. **netGame** is the game client's `NRadEngine::NRadGame::CR15NetGame` object (Quest
symbols); every `+0x6xxxx` offset below is from it. **flags** is the u64 that `*(netGame+0x2da0)` points to.

## Sources and grades

Everything here comes from live ReVault reads, project `echovr` (1,041 binaries):

| Binary | ReVault id | sha256 | Notes |
| --- | --- | --- | --- |
| `libr15.so` (Quest) | 576 | `8dd9a961b9dca8566069a4f65b3ddee9c65682c4e9c91a6d41e3c5727b1d8b20` | C++ symbols. ReVault VAs are the ELF vaddr **+ 0x100000**; both are given, as `ReVault / real` |
| `echovr.exe` (PC) | 1 | `b6d08277e5846900c81004b64b298df6acba834b69700a640b758bda94a52043` | base `0x140000000`, no symbols; names are ReVault's, matched to the Quest twin |
| Quest script libraries `lib<16hex>.so` | | | symbols; VAs as ReVault gives them |
| PC script libraries `<16hex>.dll` | | | same script id as the Quest twin; used for strings and event tables |

Script libraries are named by their 16-hex id. The ids used here: `17d77f27d465760b` (matchmaking: the LOCKOUT
bar's trigger), `f70460f9a7d466ab` (the bar itself), `18000621255c6ea5` (tablet menu: notice pages),
`23e9971060929eb3` and `4e4a1e636eb78c95` (notice popup), `333eebe580773d43` (notice buttons), `1f28bd4fa7d724fb`,
`333eece492710114`, `8515ab072bc095a8`, `d280bb8333ad45ed`, `f5a7322adb1df807` (Quit Info / EARLY QUIT INFO pages),
`e2559f42df722e87` (post-match tally), `57a56db2c35acc5a` (scoreboard), `68484f130d05a392` (lockout-timer
tooltip), `346ee5436c2e559e`, `b844180d600ad977`, `cbab485b9f607fd2`, `991bbec05cde117f`, `beaac7bfb1ce12bd`,
`c18bafab7b6faa62`, `6c96d7f9423b0ed3`, `3ded824fccc2bf6d`.

Searches marked "project-wide" covered all 1,041 binaries (ReVault's own scope line). The others name the one
binary they covered.

Each claim carries a grade:

- **H**: read in the function body (disassembly or decompilation) in that binary.
- **M**: from a search hit or an instruction pattern, matched by order, or read on one platform and matched
  on the other by shape.
- **L**: inferred, not read.

CSymbol64 hashes (`sdk/include/nevr/symbol_hash.h`) were recomputed from the names; each one given here matched the
constant in the binary.

## 1. The three messages

All three are registered in `CR15NetGame::Initialize` with `CTcpBroadcaster::Listen`. On Quest
(`0x1385f6c / 0x1285f6c`) they sit in the branch `(flags >> 2 & 1) == 0`; on PC the registrations are
in `0x140173820`, with Listen calls at `0x1401747bb`, `0x14017482f` and `0x1401748a4`. **H** (Quest), **H** (PC,
decoded from bytes).

| Message | Symbol id | Listen args | Quest callback | PC callback |
| --- | --- | --- | --- | --- |
| `SNSEarlyQuitConfig` | `0x080495a43a6b7251` | 1, 1, 1 | `EarlyQuitConfigCB` `0x136a970 / 0x126a970` | `0x1401613a0` |
| `SNSEarlyQuitFeatureFlags` | `0xd9a955895caccac3` | 1, 1, 1 | `EarlyQuitFeatureFlagsCB` `0x136ac24 / 0x126ac24` | `0x1401618b0` (no function record) |
| `SNSEarlyQuitUpdateNotification` | `0x1f81b54c35788eaa` | `0x38`, 8, 1 | `EarlyQuitUpdateCB` `0x136ac50 / 0x126ac50` | `0x1401618e0` |

The game service writes all three; the game client never sends them. The meaning of the Listen arguments
was not read. For the update, `0x38` is one past the last byte the callback reads (+0x35, then 2 bytes of
padding). **M**

### 1.1 SNSEarlyQuitConfig: the level tables

Payload: a u32 uncompressed size at bytes 0-3, then a zstd stream of JSON. PC `0x1401613a0` copies the u32
(`MemCopy(&hdr, payload, 4)`), allocates that many bytes from the thread's stack allocator
(`CStackAllocator::DirectAlloc` `0x1400d4fb0`, call `0x14016141d`), and decompresses into the buffer
(`0x14152ab40`, call `0x14016144d`). Quest goes through
`CSysCompression::Decompress<CZStdCompression,...>` `0x13ca95c` with the same header and allocator. **H**

`DirectAlloc` checks capacity; on failure it logs `"Stack allocator ran out of memory: %llu"` (string
`0x1416cd978`) and executes `INT3` at `0x1400d502b`. **H** Nothing bounds the u32 except that capacity check.
**H** A sent config has crashed the PC client at that INT3, and why is not established (section 7).

Two JSON keys, each a table on netGame (pointer, count, allocator):

| Key | Table | Row | Row fields (`Inspect`) |
| --- | --- | --- | --- |
| `penalty_levels` | `+0x647f0` / `+0x647f8` / `+0x64800` | 0x20 bytes, `SEarlyQuitPenaltyLevel::Inspect` `0x13e0d70 / 0x12e0d70` | +0x00 u8 `penalty_level`, +0x08 u32 `min_earlyquits`, +0x0c u32 `max_earlyquits`, +0x10 u32 `mm_lockout_sec`, +0x14 / +0x18 / +0x1c u32 with key strings at `0x32e3dcd`, `0x2cb1110`, `0x2cb111b` (text not resolved) |
| `steady_player_levels` | `+0x64808` / `+0x64810` / `+0x64818` | 0x10 bytes, `SEarlyQuitSteadyPlayerLevel::Inspect` `0x13e1588 / 0x12e1588` | +0x00 u8 `steady_player_level`, +0x08 u32 `min_num_matches`, +0x0c float `min_steady_ratio` |

**H** for the keys, offsets and strides (Quest), and the same strings and strides on PC (`<<5`, `<<4`). The
callback then fires `delegate_onearlyquitconfigupdate` (`0xb9108d99d6d9229d`). Quest calls it at `0x136abc8`,
PC dispatches it at `0x140161841`. **H**

Nothing on the screen reads these tables (section 3.1). The client keeps them; it never computes a lockout
from them.

### 1.2 SNSEarlyQuitFeatureFlags: one byte

| Platform | Code | Store |
| --- | --- | --- |
| Quest | `EarlyQuitFeatureFlagsCB` `0x136ac24`: `ldrb w8,[x8]; and w8,w8,#0xdddddddd; strb w8,[x0,x9]` with `x9 = 0x64847` | `+0x64847 = byte & 0xdd` |
| PC | bytes at `0x1401618b0`, decoded by hand: `MOV RAX,[RDX+8]; MOVZX EDX,byte [RAX]; AND DL,0xDD; MOV [RCX+0x64847],DL` | same; the mask's immediate is at `0x1401618b9` |

**H** on both platforms. `0xdd` clears bits 1 and 5 and keeps 0, 2, 3, 4, 6 and 7. Both platforms then fire
`delegate_onearlyquitfeatureflagsupdate` (`0xc690a8ff1cf8af99`). No script handles that event
(project-wide `search_code` for both spellings of the hash). **H**

The only other writer of `+0x64847` is construction: the netGame constructor zeroes the qword at `+0x64840`
(Quest ctor `0x1383db8`; PC `ConfigureEngine` `0x140146c71`). **M**

### 1.3 SNSEarlyQuitUpdateNotification: 0x38 bytes

| Offset | Type | Client field (netGame) | Quest `0x136ac50` | PC `0x1401618e0` |
| --- | --- | --- | --- | --- |
| +0x00 | 0x18 bytes | not read | | |
| +0x18 | i64 | `+0x64820` (`penaltyts`, the "newer than" stamp) | `ldr x0,[x10,#0x18]` | `MOV RDX,[RDX+0x18]` (2nd argument) |
| +0x20 | i64 | **none** | never loaded | `MOV R8,[RDX+0x20]` (3rd argument), never read by the setter |
| +0x28 | i32 | `+0x64830` numearlyquits | `ldp w12,w11,[x10,#0x28]` | R9D |
| +0x2c | i32 | `+0x64834` numsteadymatches | (same `ldp`) | stack |
| +0x30 | i32 | `+0x64838` numsteadyearlyquits | `ldr w8,[x10,#0x30]` | stack |
| +0x34 | u8 | `+0x64844` penaltylevel | `ldrb w9,[x10,#0x34]` | stack byte |
| +0x35 | u8 | `+0x64845` steadyplayerlevel | `ldrb w10,[x10,#0x35]` | stack byte |
| +0x36 | 2 bytes | not read | | |

**H** on both platforms. The game service writes the message. Nakama lays it out in
`server/evr/login_earlyquitupdatenotification.go`. What +0x18 and +0x20 *mean* is section 6.

## 2. What the client does with them

### 2.1 The setter and its "newer than" gate

Quest inlines the setter into `EarlyQuitUpdateCB` and `LoadEarlyQuitPenalty`. The standalone
`SetEarlyQuitPenaltyLevel` `0x136ad54 / 0x126ad54` has no code callers, only two data slots (`0x335388c`,
`0x3433bc0`). PC calls `SetEarlyQuitPenaltyLevel` `0x1401ae2e0` from both paths. **H**

The gate (PC `0x1401ae2e6`: `MOV RAX,[RCX+0x64820]; CMP RAX,-1; JZ apply; CMP RAX,RDX; JGE return`; Quest
`cmn x2,#1; b.eq apply; cmp x2,x0; b.ge skip`) applies the update only if the stored `+0x64820` is -1 or **less
than** the incoming value. Otherwise it returns before storing anything: counters and levels are dropped too.
**H**

On apply:

| Slot | Value |
| --- | --- |
| `+0x64820` | incoming stamp |
| `+0x64828` | **-1** (PC `MOV qword ptr [RCX+0x64828],-1` at `0x1401ae372`; Quest `= 0xffffffffffffffff`) |
| `+0x64830` / `+0x64834` / `+0x64838` / `+0x64844` / `+0x64845` | numearlyquits, numsteadymatches, numsteadyearlyquits, penaltylevel, steadyplayerlevel |
| `+0x6483c` / `+0x64840` / `+0x64846` | the *old* `+0x64834` / `+0x64838` / `+0x64845` ("previous" slots); on the first apply (stored was -1) they take the new values |

**H** on both platforms. If any of the five counters or levels changed, the setter clears flags bits 45 and 46
and fires `delegate_onearlyquitpenaltyupdate` (`0xa2e48b35c7cd078f`). Quest does both with one AND
`0xffff9fffffffffff`; PC uses two ANDs, at `0x1401ae3e4` and `0x1401ae3f8`, and dispatches at `0x1401ae409`. **H**
On the first apply (stored was -1) the old steady counters and level are set equal to the new ones before
the compare, so only numearlyquits and penaltylevel can trigger it then (Quest `0x136ade0`: `mov w8,w7; mov
w9,w5; mov w13,w4`; PC `0x1401ae397`..`0x1401ae3b1`). **H** The update callback then fires the same event
again, unconditionally (PC tail jump `0x140161932`; Quest `0x136ad50`). **H**

The setter's third argument is a long. The Quest symbol is
`SetEarlyQuitPenaltyLevel(long, long, unsigned int, unsigned int, unsigned int, unsigned char, unsigned char)`;
on Quest the second long (x2) is never read: the standalone setter tail-branches (`0x136ae1c`, thunk
`0x1021530`) to `SendComponentEventGlobal` `0x1344758`, which takes (netGame, event) and loads its own
`x2 = 0xffffffffffff` before each call it makes (`0x1344778`, `0x13447a0`, `0x13447d8`). PC overwrites R8D at
`0x1401ae313` without reading it. **H** So the argument reaches the setter on PC and is dropped on both
platforms.

### 2.2 The login path

Quest `LocalUserProfileReceivedCB` `0x136c310 / 0x126c310` calls `LoadEarlyQuitPenalty` `0x136c958 / 0x126c958`
on every path and then calls `LogInSuccess`. **H** PC `0x14017e330` is the same function with the load inlined:
it calls the setter at `0x14017e462`, then `LogInSuccess` `0x14017f630`. **M** (name), **H** (body).

It reads the **server** profile (`CNSUser::ServerProfile` of `*(netGame+0x28d0)`; Quest `ldr x0,[x0,#0x28d0]; bl
0xfe74b0`), not the client profile. **H**

| Key | Default | Slot |
| --- | --- | --- |
| `earlyquit\|penaltyts` | -1 | `+0x64820` (through the same gate) |
| `earlyquit\|numearlyquits` | 0 | `+0x64830` |
| `earlyquit\|numsteadymatches` | 0 | `+0x64834` |
| `earlyquit\|numsteadyearlyquits` | 0 | `+0x64838` |
| `earlyquit\|penaltylevel` | 0, and 0 if ≥ 0x10000; stored as a byte, so 0x100..0xffff truncate | `+0x64844` |
| `earlyquit\|steadyplayerlevel` | 0, and 0 if ≥ 0x10000; stored as a byte | `+0x64845` |

**H** (Quest strings `0x2caa9f5`..`0x2caaa71`; PC strings `0x1416e0870`..`0x1416e08f8`). The PC call passes **R8 =
-1** (`LEA R8,[RCX-1]` with RCX = 0, at `0x14017e453`). **H** So the login path brings no expiry on either
platform: Quest's inlined copy writes -1 to `+0x64828`, as the setter does. **H**

### 2.3 The expiry and the countdown

`+0x64828` (`lockoutexpiretimestamp`) is the only input to the countdown. Its writers in the stock game:

| Writer | Value |
| --- | --- |
| netGame construction: Quest ctor `0x1383db8`, PC `ConfigureEngine` `0x140146c58` | -1 |
| the setter / its inlined copies (update, login) | -1 |

No other writer was found. On PC, a scan of `.text` for the displacement bytes `28 48 06 00` finds exactly 4 sites:
`0x140146c5b` and `0x1401ae375` (the two writes above), `0x1401bc5ed` and `0x140d9622f` (the two reads below).
ReVault's instruction search for `0x64828` gives the same 4. On Quest, the decompiled-text search `0x64828) = `
gives 4 hits, all `= 0xffffffffffffffff`; Quest stores the pair with `stp x1,x14,[x11]` at `+0x64820`, which an
instruction search for `0x64828` cannot see. **M** (absence by search).
**In the stock game the expiry is always -1, so no message can start a countdown.**

Two readers:

- **`CR15NetGame::Update`** (Quest `0x1394b40`, test `tbz x8,#0x2d` at `0x1394ca4`; PC `0x1401bbdb0`, test at
  `0x1401bc5dc`): if flags bit 45 is set and `+0x64828` is negative or ≤ now, it clears bit 45 (Quest
  `0x1394cd0`, PC `0x1401bc60c`) and fires `delegate_onearlyquitpenaltyupdate`. **H** The block runs only when
  flags bit 14 is set and on one value of Update's argument (Quest `tbnz w8,#0xe,0x1394c8c` at `0x1394c40`, `cmp
  x20,#1` at `0x1394c44` and `0x1394c9c`; PC `SHR RAX,0xe; TEST AL,1` at `0x1401bbfa0`, `TEST RSI,RSI` / `CMP RSI,1`
  from `0x1401bc011`). **M**; what bit 14 and the argument mean was not read. The PC name is **M**, from the
  shape: ReVault also gives `CR15NetGame::Update` to `0x1401bf610`.
- **`CR15NetEarlyQuitPenaltyExpression::operator()`** (Quest `0x2421ba0 / 0x2321ba0`, PC `0x140d96190`),
  section 4. Its `lockoutcountdownsec` is `+0x64828 − now` when `+0x64828` is not -1 and later than now,
  otherwise 0 (PC `0x140d96308`: `CMP RBP,-1; JZ; CMP RBP,RAX; JLE; MOV RDI,RBP; SUB RDI,RAX`). **H**

The clock is **Unix seconds** on both platforms. PC `CSysTime::GetTime` `0x1400de820` is `XOR ECX,ECX; JMP
_time64` (`0x141315468`), and `0x141315398` converts the FILETIME: it subtracts `0x019DB1DED53E8000`, divides by
10^7, and fails above `0x793582aff`, when `_time64` returns -1. Both Update (`0x1401bc5e5`) and the expression (`0x140d96303`) call it.
**H** On Quest, Update calls `CSysTime::GetTime64` and the expression calls `CSysTime::GetTime`, through the PLT
thunks `0xfd18e0` and `0xff2ab0` (`0x1084d80 / 0xf84d80` and `0x1084d28 / 0xf84d28`). Both are `clock_gettime(0, &ts)`, where import thunk
`0xfd7a00` is `clock_gettime` and clock 0 is `CLOCK_REALTIME`; each returns `tv_sec`, or -1 on failure. **H**

Flags bit 45 ("countdown running") and bit 46 (`showearlyquitwarning`) have **no setter** in either binary.
Searched: Quest instruction search for the masks and constants `#0x200000000000|#0x400000000000|#0x600000000000|
#-0x200000000001|...` (82 results, the early-quit ones are the five AND sites above), bit-field ops on `#0x2d`/`#0x2e`
(10 results, none on flags). On PC, ReVault's instruction search covers only code with a function record, and it
misses `0x1401618b0` and `0x1401ae430`. So the 64-bit mask immediates were also scanned in the bytes of `.text`:
the AND masks occur only at `0x1401ae3dc`, `0x1401ae3e9`, `0x1401ae439` and `0x1401bc604`. The OR-style constants
occur at `0x140688bcf` (a particle-graph table), `0x1413bbd94` (a TEST), `0x1416514bb` / `0x1416514ca` (the mask
table at `0x141651380`) and in unrelated code. None of these was shown to touch the flags. **M** (absence by
search). Two limits: a mask built in a register would not match, and the flags u64 is not netGame's own. PC
`ConfigureEngine` copies the pointer from another object (`MOV RAX,[RBP+0x248]; MOV [RSI+0x2da0],RAX` at
`0x140146222`), so a writer need not go through `netGame+0x2da0`.

### 2.4 What clears or forgives

| Event | Effect in the stock game |
| --- | --- |
| an applied update or login whose counters changed (on the first apply: numearlyquits or penaltylevel) | clears bits 45 and 46, fires the penalty event |
| `CR15NetGame::Update` at expiry, under its precondition (section 2.3) | clears bit 45 when `+0x64828` < 0 or ≤ now, fires the event |
| `SetEarlyQuitWarningShown` (Quest `0x1392760 / 0x1292760`; PC `0x1401ae430`, no function record, bytes: `MOV RAX,[RCX+0x2da0]; MOV RDX,0xffffbfffffffffff; AND [RAX],RDX; ...; JMP 0x1401a9fe0`) | clears bit 46, fires the event. **H** on both |
| `SetSteadyPlayerShown` (Quest `0x1392784 / 0x1292784`) | copies the current steady level and counters into the "previous" slots, fires the event. **H**. No script runs it (section 3.1) |
| an update whose `+0x18` is not newer than `+0x64820` | **nothing**: the whole update is dropped |

The client has no "forgive" message of its own. To forgive, the service sends an update that passes the gate,
with lower counters or levels.

The script node `R15NetSetEarlyQuitWarningShownNode` runs `SetEarlyQuitWarningShown`. Quest Enter `0x2435724`
calls it directly when `*(netGame+0x2d18) != 0`, and otherwise queues it on `netGame+0x2b20`; Exit `0x2435790`
and Update `0x2435794` are no-ops. PC node `0x140d8e920` queues `0x1401ae430` on `netGame+0x2b20`. **H**

## 3. The feature flags

`CR15NetEarlyQuitFeatureFlagExpression` (Quest `0x2421b28 / 0x2321b28`, PC `0x140d960a0`) takes an input `index`
(`0xe32dc7d3d78b41a8`) and outputs `output` = `index <= 0x20 && ((1 << index) & byte +0x64847) != 0`. PC shifts
an 8-bit register (`SHL DIL,CL`), Quest a 32-bit value; both read one byte, so index 8 to 31 reads 0. Index 32
shifts by 0 on both (the count is masked to 5 bits) and reads bit 0; no script passes it. **H**

The scripts pass only **0, 1 and 5**. A project-wide scan of every `INTCONST` that feeds the expression found 84
Quest nodes in 26 libraries; nothing reads bits 2, 3, 4, 6 or 7. **M** (tabulated from search results;
`17d77f27d465760b` `expression_c76e59d9_shouldexecute` `0x167508` and `18000621255c6ea5`
`expression_303932df_shouldexecute` `0x13413c` re-read in full: **H**).

| Bit | Readers (Quest script libs) | What the player sees when it is set |
| --- | --- | --- |
| 0 | only `17d77f27d465760b` `c76e59d9` `0x167508` (gate of the penalty-update thread in `event_handler` `0x128d64`; false frees the thread) | the LOCKOUT bar can come up (section 4.1) **H** |
| 1 | `68484f130d05a392` lockout-timer tooltip (`85a2b16`, `32c43422`); FIND MATCH show nodes in `bc170a382fda5499`, `f36316e246f2ca21`, `555db38933fef859`, `5ddc3dcdad4a3ed5`; `991bbec05cde117f` `5f25da0c`; `beaac7bfb1ce12bd` `4b585d95` (pause-menu penalty-update gate, `0x133520`); `17d77f27d465760b` `bb3fac9a`, `bed64604` (further LOCKOUT-bar paths) | "Lockout Timer. Click to view Early Quit Info" tooltip; the lockout state of the FIND MATCH elements **M** |
| 5 | `303932df` penalty-update gate in 18 tablet libraries (`abc70245` in `991bbec05cde117f`); `dfb343f6` (`ReturnToHome`, `18000621255c6ea5` `0x131a00`, read as NOT bit 5); `346ee5436c2e559e` "View Early Quit and Loyalty Info" tooltip; `57a56db2c35acc5a` ten scoreboard values; `e2559f42df722e87` `c78f5f23_show`; `c18bafab7b6faa62` `3b982293`; `6c96d7f9423b0ed3` `3d06914f` | the tablet's early-quit pages and labels update on a penalty, the notice pages can be reached, the loyalty tooltip, the scoreboard's loyalty colours **M** (`303932df`, `dfb343f6`: **H**) |

**On a stock client bits 1 and 5 are always 0** (the `& 0xdd` store), so every bit-1 and bit-5 gate fails and every
`NOT bit 5` passes. Only bit 0 can be set, and only if the service sends it. **H**

What the game service sends: nakama's `DefaultEarlyQuitFeatureFlags()` is `0x0F` (bits 0-3) at the head of
EchoTools/nakama#712; that plan changes it to `0x23` (bits 0, 1, 5).

### 3.1 Engine expressions no script uses

The engine registers these, but no script library in the project references them: `R15NETEARLYQUITPENALTYLEVELEXPRESSION`
(`0x1754096d63cf20d8`), `...PENALTYLEVELCOUNTEXPRESSION` (`0xac5c052c9b3008ba`), `R15NETSTEADYPLAYERLEVELEXPRESSION`
(`0x296eca7b722b0f4a`), `...LEVELCOUNTEXPRESSION` (`0xe89a5e4de4783e86`) and `R15NETSETSTEADYPLAYERSHOWNNODE`
(`0x67f6d0803689b0d3`). They are registered in Quest `R15NetInstallComponentScriptNodeBindings` `0x24416ac` and PC
`0x140f4c480`. The project-wide `search_code` for each name and each hash hits only those two registration
functions. **M**

So `mm_lockout_sec` and the row's +0x14 bool have no script consumer. That bool is the PenaltyLevel output +0x10,
binding hash `0x2a52ecbbcfcc19dd`, which matched none of about 110 names tried. Nothing was found that gates
matchmaking or spawn on them (decompiled-text search only). **M**

`CR15NetEarlyQuitPenaltyLevelExpression` (Quest `0x2421d58`, PC `0x140d96430`) outputs one row: `penaltylevel`,
`minearlyquits`, `maxearlyquits`, `mmlockoutsec` and the bool. `CR15NetEarlyQuitPenaltyLevelCountExpression` (Quest
`0x2421d20`) outputs `+0x647f8`. **H**

## 4. The UI

### 4.0 The penalty expression: what the scripts can see

`CR15NetEarlyQuitPenaltyExpression` (script name `R15NETEARLYQUITPENALTYEXPRESSION`, `0x301554de38f43654`). The
binding names come from `SOutput::Inspect<bool>` `0x256c9e0` and `<int>` `0x256aeb0` (Quest), and the values from
`operator()`:

| Output | Binding | Value | Read by scripts |
| --- | --- | --- | --- |
| +0x00 bool | `lockoutcountdownactive` | **constant 0** (PC `MOV byte ptr [RBX],0x0` at `0x140d96319`; Quest `strb wzr,[x19]`) | the LOCKOUT bar, the "Matchmaking Lockout" label, the notice title, the notice-vs-lockout page choice |
| +0x01 bool | `showearlyquitwarning` | flags bit 46 (PC `SHR RSI,0x2e` at `0x140d9620e`; Quest `ubfx x10,x21,#0x2e,#0x1` at `0x2421ce8`) | the notice buttons' guards, the tablet page conditions |
| +0x08 i64 | `lockoutexpiretimestamp` | `+0x64828` | **no script** |
| +0x10 i64 | `lockoutcountdownsec` | `+0x64828 − now`, else 0 | the bar's timer text |
| +0x18 u32 | `steadyplayerlevel` | `+0x64845` | Loyalty pages, scoreboard |
| +0x1c u32 | `numsteadymatches` | `+0x64834` | Loyalty pages, scoreboard |
| +0x20 u32 | `numsteadyearlyquits` | `+0x64838` | no node found |
| +0x24 u32 | `prevsteadyplayerlevel` | `+0x64846` | no node found |
| +0x28 u32 | `prevnumsteadymatches` | `+0x6483c` | post-match tally |
| +0x2c u32 | `prevnumsteadyearlyquits` | `+0x64840` | no node found |

**H** for the values on both platforms and for the binding names on Quest. No expression exposes `penaltyts`,
`numearlyquits` or `penaltylevel`. **H**

Scripts read an output **by name** into their own copy. Each lambda calls
`R15NETEARLYQUITPENALTYEXPRESSION::output_<name>_func(buf, output_<name>_offset, obj+N)`, and `obj+N` is the
script's slot, not the native offset. In `f70460f9a7d466ab`, for example, `expression_90a74fc_value` `0x10e6fc`
with lambda `0x10e88c` puts `lockoutcountdownactive` at obj+8 and `lockoutcountdownsec` at obj+0x10. **H**

**On a stock client every script sees `lockoutcountdownactive` = 0, `lockoutcountdownsec` = 0 and
`showearlyquitwarning` = 0.** **H** (sections 2.3, 4.0).

### 4.1 The LOCKOUT bar

1. `17d77f27d465760b` `event_handler` `0x128d64`: `delegate_onearlyquitpenaltyupdate` starts thread `0x9eff591c`,
   gated by **feature bit 0** (`c76e59d9` `0x167508`) and one thread at a time. **H**
2. `9eff591c` (`0x16002c`) continues only if graph flag +0xe0 is false; +0xe0 is plausibly "the finding-match UI
   is up" (L). Then `928bcba2` (`0x15fb00`) reads `lockoutcountdownactive`. If true, `ce1603af` enables the actor
   and calls function `0x25e59b0025f0916f`; if false, `3f554ec5` calls it with 0 and `c8596957` disables the
   actor. **M** (traced from the node graph, condition bodies read).
3. That function reaches `execute_node_bc815c84` `0x14f068`, which calls script function `UpdateLockOutUI`
   (`0x56eafdbac88c5105`). Its symbol parameter (`0x1626c4`) picks `"player"` or `"party"` from graph flag +0xe2
   and `lockoutcountdownactive`. **M**
4. `f70460f9a7d466ab` (strings `LOCKOUT`, `PARTY LOCKOUT`, `lock_out_timer_text`): `UpdateLockOutUI` sets the text,
   then raises `ActivateTimer` (`0x4a31f6d44d33ccd2`, `execute_node_6263bdba` `0x10e2d4`). The timer thread loops
   while graph flag +0xc is set (`2ca27e1e` `0x10f01c`); `90a74fc` (`0x10e6fc`) takes `lockoutcountdownsec` (as a
   real) when `lockoutcountdownactive` is true, and `ad56231a` sets `lock_out_timer_text`. **M** (the loop's
   re-entry each tick: L).
5. `b844180d600ad977` (`76c96cbc_value` `0x10e500`) and `cbab485b9f607fd2` (`c753e550_value` `0x112390`; self-event
   `evt_update_timer` `0x9c1b2b246c02b97a`) read the same two outputs for the same shape of timer. **M**

So the bar needs feature bit 0, `lockoutcountdownactive` true, a future `+0x64828` for the digits, and the
penalty event, with no finding-match UI up. On a stock client `lockoutcountdownactive` is always 0, so the bar is
always hidden. **H**

Which input picks `LOCKOUT` and which picks `PARTY LOCKOUT` is not established. `17d77f27d465760b` also handles
`dbg_evt_lock_out_party_activate` (`0xd8a114aa7515d439`) and `..._deactivate` (`0x62b4a3bb453d652d`), which set
and clear graph flag +0xe2. No script raises them (project-wide search for both hashes). **M**

### 4.2 The notice: EARLY QUIT NOTICE / LOCKOUT NOTICE

- **Tablet pages** (`18000621255c6ea5`; pages `quit_notice_page`, `lock_out_notice_page`, `quit_info_page`,
  `home_page`). `ReturnToHome` (thread `0xdfb343f6`) branches on `dfb343f6` (`0x131a00`), which is NOT bit 5.
  With bit 5 set it reads `b46d4b72` (`0x130078`), NOT `lockoutcountdownactive`: true goes to
  `TransitionToPage(quit_notice_page)` (`6a1e8fcb` `0x1256d0`), false to `TransitionToPage(lock_out_notice_page)`
  (`181010cc` `0x122f04`). **H**
  `left_button_press` runs `SetEarlyQuitWarningShown` (`7f8e6b9c` `0x126100`), guards on NOT
  `showearlyquitwarning` (`e3e19d33` `0x131c68`), then `TransitionToPage(quit_info_page)` (`5dd60b5a` `0x1253f0`).
  `right_button_press` runs `24c048cd` `0x123610`, the same guard (`f79950b1`), then `ReturnToHome` (`300924a1`
  `0x1244b4`). Both button threads accept only an event from the graph's own page actor whose +0x28 is
  `0x3375a9c5ccdcc06a`; that hash's name is not resolved. **H**
- **Popup** (`23e9971060929eb3`, and `4e4a1e636eb78c95` with the same shape, **M**). The title is
  `STRINGCONDITION(..., lockoutcountdownactive)` (`4a39664e_params_0_stringvalue` `0x10d9ec`): `"LOCKOUT NOTICE"`
  or `"EARLY QUIT NOTICE"`. `evt_continue_btn_clicked` runs `SetEarlyQuitWarningShown` (`9ef1fb04` `0x10d7a0`),
  then enables the next page (`eb2515f3`); `evt_info_btn_clicked` enables another page (`aa7d6565`). **H**
- **Button widget** `333eebe580773d43` ("Continue to Menu", "Access the Early Quit Info screen"): `button_clicked`
  runs `SetEarlyQuitWarningShown` (`d4116f72` `0x11090c`). **H** `1f28bd4fa7d724fb` runs the node as well. **M**
- **`showearlyquitwarning` as a page condition.** Tablet-template `_name` nodes hide a page while a warning is
  pending. For example `3ded824fccc2bf6d` `d3ef85a2_name` `0x1413e0` is `+0xe5 AND NOT (battery_warning OR
  showearlyquitwarning)`. **H** The same nodes appear in 13 more libraries. **M**

Nothing sets bit 46 (section 2.3), so `showearlyquitwarning` is always false, on stock clients and with this
repo's runtime. A notice page never comes up by itself; it can only be reached through `ReturnToHome` and page
navigation. **M** (no setter by search; the `delegate_onpageenabled` entry of `18000621255c6ea5`, `0x3d3533a0`,
was not followed). The notice body text is in no binary's string table (section 7).

### 4.3 Loyalty: the steady-player level

| Library | Screen | Reads |
| --- | --- | --- |
| `333eece492710114` | Quit Info page ("LOYALTY TIER ", "QUIT: -- / COMPLETE: --") | `steadyplayerlevel` (`9e4dd3d7_text` `0x11a4c0`, colour `d9f47e83` `0x11b778`), `numsteadymatches` |
| `8515ab072bc095a8` | "EARLY QUIT INFO 2/2" | `32afe89a_text` `0x1318f8`: "LOYALTY TIER " + digit when graph int +0x11c ≥ 10, else "LOYALTY"; `53529e9f_text` `0x132c0c` builds "<n>/10 GAMES" from +0x11c (the operand order: L) |
| `d280bb8333ad45ed`, `f5a7322adb1df807` | "EARLY QUIT INFO 1/2" | strings only, **M** |
| `e2559f42df722e87` | post-match tally ("LOYALTY TIER ") | `steadyplayerlevel`, `numsteadymatches`, `prevnumsteadymatches` (`3282eb06` `0x15e494`) |
| `57a56db2c35acc5a` | scoreboard | `18553d96_value` `0x14223c`: colour by `steadyplayerlevel` 3 / 2 / 1 / other, gated by bit 5 |

**H** where an address is read; **M** for the copies. The tier names (Steadfast, Noble, Loyalist, The Lifer) and
the "/10" literal are in no string table in the project (project-wide `search_strings`, 0 results each). They
come from a resource that was not read. **M**

### 4.4 Events

| Event | Hash | Fired by | Handled by |
| --- | --- | --- | --- |
| `delegate_onearlyquitpenaltyupdate` | `0xa2e48b35c7cd078f` | the setter (on change), the update callback (always), `Update` at expiry, `SetEarlyQuitWarningShown`, `SetSteadyPlayerShown` | 22 Quest libraries (project-wide search, 148 results). Gates: bit 5 (`303932df`) in 18, bit 0 in `17d77f27d465760b`, bit 1 in `beaac7bfb1ce12bd`, none in `b844180d600ad977` and `cbab485b9f607fd2` **M** |
| `delegate_onearlyquitfeatureflagsupdate` | `0xc690a8ff1cf8af99` | the feature-flags callback | none **H** |
| `delegate_onearlyquitconfigupdate` | `0xb9108d99d6d9229d` | the config callback | none **H** |

The "Matchmaking Lockout" label (`9998a42_value`, in 17 tablet libraries; `18000621255c6ea5` `0x1299e4`, **H**)
shows when `lockoutcountdownactive` is true.

## 5. PC and Quest

| Item | Same on both | Differs |
| --- | --- | --- |
| Messages, ids, Listen args | yes **H** | |
| Config parse: u32 header, zstd, stack allocator, table strides | yes **H** | |
| Feature-flag store `& 0xdd`, event | yes **H** | PC's callback has no function record (`0x1401618b0`) |
| Update notification fields read | +0x18, +0x28..+0x35 **H** | PC loads +0x20 into R8 and passes it to the setter, which discards it; Quest never loads it **H** |
| Setter | gate, stores, -1 to the expiry, the second long dropped **H** | PC standalone (two callers), two ANDs; Quest inlined, one AND **H** |
| Login path | server profile keys, gate **H** | PC passes R8 = -1 explicitly **H** |
| `Update` expiry test | yes **H** | |
| Clock | Unix seconds **H** | PC `_time64`; Quest `clock_gettime(CLOCK_REALTIME)` |
| Expressions | layouts, constant-0 `lockoutcountdownactive` **H** | FeatureFlag shift width (8-bit vs 32-bit), no effect for indexes 0-7 **H** |
| `SetEarlyQuitWarningShown` | clears bit 46, fires the event **H** | |
| Scripts | | only the Quest libraries' bodies were read; the PC `.dll` twins carry the same strings and event tables (**M**); their graphs were not compared |
| This repo's runtime | | PC only: `src/runtime/patch/early_quit_lockout.cpp` hooks the PC setter and expression and replaces the PC feature-flag callback; nothing on Quest |

## 6. The runtime, and the two readings of +0x18

### 6.1 What the runtime changes (PC)

`src/runtime/patch/early_quit_lockout.cpp`:

- `FeatureFlagsCallbackHook` replaces the whole feature-flag callback at `0x1401618b0` (35 bytes; its 16 prologue
  bytes are checked first). It stores the service's byte as sent, where the stock callback masks it with `0xdd`, then
  dispatches the event word at `0x1416dcd78`, so the scripts see exactly the bits the service sends.
- `PenaltyExpressionHook` runs after `0x140d96190` and sets `lockoutcountdownactive` = `lockoutcountdownsec > 0`
  (`CountdownActive`, `early_quit_lockout_rules.h`).
- `SetPenaltyHook` runs after the setter. `AfterSetPenalty(incoming, stored, now)` writes `+0x64828` and bit 45:
  a cleared penalty (≤ 0) ends the lockout and resets `+0x64820` to -1; a stored value later than now becomes the
  expiry; anything else leaves -1. It then raises the penalty event. It logs
  `[NEVR.EARLYQUIT] penalty set: incoming penaltyts=... stored=... now=... level=... early_quits=... -> lockout
  expiry=... active=...`.

With those three changes everything in section 4 is reachable, except a notice that opens by itself: nothing
sets bit 46 (section 4.2). The countdown is the runtime's own value in `+0x64828`, written at the one slot both
readers use (section 2.3). Its digits reach 0 at the expiry by themselves, because the expression compares with
the clock and the hook sets `lockoutcountdownactive` from them. The bit-45 clear in `Update`, and the event it
fires, also need that block's precondition (section 2.3).

`src/runtime/scenario/scenario_control.cpp` `FireEarlyQuit` (scenario builds only) writes the slots and the
constant store directly. A run that uses it exercises the scripts, not the path from the service to the client.

### 6.2 Reading A: +0x18 is the expiry (the runtime and nakama as they are)

Nakama sends `PenaltyTimestamp` (the lockout's end) at +0x18 and zero at +0x20 (`Reserved`). The runtime uses the
stored +0x18 as the expiry. The setter's gate (section 2.1) then makes the expiry a value that may only grow
within a process. **H** (from the gate):

- a later, **longer** lockout applies;
- a **shorter** lockout (moderator, or a forgive that lowers the level) fails the gate: the countdown keeps the
  old end, and the counters and levels in that update are dropped;
- a **clear** (+0x18 of 0 or -1) fails the gate as well, and the runtime ends the countdown itself
  (`resetStoredTs`), but the update's counters and levels are still dropped;
- a counters-only update with the same +0x18 is dropped.

### 6.3 Reading B: +0x18 is a stamp, +0x20 the expiry (#907, EchoTools/nakama#712)

The service sends a per-player stamp that always increases at +0x18 and the expiry at +0x20. The hook takes
the setter's third argument (R8: +0x20 from the update, -1 from the login path) and writes it to `+0x64828`,
but only when the setter applied the update. As long as the service's stamps always rise, every update it
sends passes the gate: **H** for the gate and the R8 path. The rising stamp is the service's job, not something
the binary shows.

What the binary supports, against each reading:

| Fact | A | B |
| --- | --- | --- |
| the gate wants +0x18 to rise for every update to apply (section 2.1) **H** | an expiry does not always rise | a stamp always does |
| the profile key for the same slot is `earlyquit\|penaltyts`, "penalty timestamp" (section 2.2) **H** | | consistent with a stamp **L** |
| the setter takes a second long it never reads, on either platform; PC loads +0x20 into it (section 2.1) **H** | +0x20 unused | +0x20 is the argument the game passes and drops |
| the login path passes -1 for that long (section 2.2) **H** | | the login path brings no expiry; one has to come from an update |
| every stock writer of `+0x64828` stores -1 (section 2.3) **M** | | |

What the binary cannot show: what the shipped game ever meant to put in `+0x64828`. No stock code stores
anything but -1 there, from +0x20, from +0x18 or from `penaltyts + mm_lockout_sec`. Under either reading the
expiry reaches the scripts only through the runtime.

## 7. Not established

- The text of the penalty-row keys at row +0x14, +0x18 and +0x1c, and the name of the PenaltyLevel output +0x10
  (`0x2a52ecbbcfcc19dd`).
- Why a sent `SNSEarlyQuitConfig` reaches the allocator's INT3 (`0x1400d502b`). One idea, not measured: a payload
  without the 4-byte header has its first 4 JSON bytes read as the size. The allocator's capacity was not read.
  Nakama does not send the config.
- What flags bit 14 and `Update`'s argument mean (the precondition of the expiry block, section 2.3).
- Whether `LocalUserProfileReceivedCB` runs more than once in a session, and so whether a later profile can reach
  the setter.
- Which input picks `LOCKOUT` and which `PARTY LOCKOUT` in `f70460f9a7d466ab`; graph flags +0xe0 and +0xe2;
  whether the timer loop re-enters every tick.
- The gate hash `0x3375a9c5ccdcc06a` on the tablet notice buttons; the pages behind `23e9971060929eb3`'s
  `eb2515f3` and `aa7d6565`; the `delegate_onpageenabled` path of `18000621255c6ea5`.
- The notice body text and the tier names: on screen, in no binary's string table.
- The PC script `.dll` graphs: they were not compared node by node with the Quest twins.
- The absence of a setter for flags bits 45/46, and of a non -1 writer of `+0x64828`, rests on searches and a byte
  scan (section 2.3), not on a read of every writer of the flags u64. That u64 belongs to another object, so its
  writers need not go through netGame.
- The order in which the login profile and `SNSEarlyQuitFeatureFlags` arrive at login.

## 8. Verdict on #907 and EchoTools/nakama#712

**not confirmed: the layout agrees with every instruction read, but no live test of the lockout path exists
yet, so the gate dropping an update whose +0x18 is not newer, +0x20 arriving in the setter's R8, and the login
path's R8 = -1 are static reads only.**

From the binary:

- +0x18 must rise for every update the service sends to apply (section 2.1). Reading B meets that and
  reading A does not. **H**
- On PC, +0x20 is loaded at `0x1401618e0` as a 64-bit value and passed to the setter as its third argument.
  The setter does not read it, so a hook can (section 2.1). **H**
- The login path passes -1 there (`0x14017e453`), so after login the countdown has to come from an update,
  as #712's step 3 plans. **H**
- The flags the plans send (`0x23`: bits 0, 1, 5) are exactly the bits the scripts read (section 3). **M**
  (PC script twins not compared).
- Expiry and clock are both Unix seconds (section 2.3). **H**

Points the plans should carry. None of them changes the layout:

1. **No notice by itself.** Bit 46 (`showearlyquitwarning`) has no setter in the game or in the runtime
   (sections 2.3 and 4.2). A live test with bit 5 set should therefore expect the EARLY QUIT INFO pages, the
   loyalty screens and the tablet labels, but no notice page that opens on its own.
2. **The released-client claim holds only up to clock skew.** A released client (reading A) treats a stamp
   `max(prev + 1, now)` as an expiry. If its clock is k seconds behind the service's, or the stamp runs ahead of
   now within one second, it shows a countdown of about k seconds. **L** (from `AfterSetPenalty` and the setter;
   not run).
3. **A profile must not carry a newer stamp than the last update.** If a later profile delivery reaches the
   setter with an `earlyquit|penaltyts` above the last stamp sent, the setter applies it with R8 = -1, and the
   planned hook writes -1 and ends a running lockout. Whether the profile is delivered again in a session is
   not established (section 7). The service should keep the profile's `penaltyts` at or below the last stamp
   it sent.
4. **Send the feature flags first.** The bar's gate reads bit 0 when the penalty event fires, and nothing
   re-raises that event when the flags arrive (section 4.4). At login, the service should send
   `SNSEarlyQuitFeatureFlags` before anything that applies a penalty: under reading B, before the post-login
   update.
5. **Quest has no hook.** Quest inlines the setter in `EarlyQuitUpdateCB` (`0x136ac50`) and `LoadEarlyQuitPenalty`
   (`0x136c958`), so a Quest port would hook those two. Its expression is at `0x2421ba0` and its mask at
   `0x136ac24`. A Quest client sees no lockout under either reading.

### The live test that decides it

The setup is PC, the game service with early quit enabled, and one account. The client log lines are
`[NEVR.EARLYQUIT] penalty set ...` and `[NEVR.EARLYQUIT] lockoutcountdownactive=...`. The service's send log
gives the bytes of each update (+0x18, +0x20). Run the steps with reading A (main and nakama as they are), then with
reading B (#907 and #712):

| Step | A: +0x18 = expiry (main) | B: +0x18 stamp, +0x20 expiry (#907/#712) |
| --- | --- | --- |
| 1. Set a 600 s lockout | applies; bar ≈ 10:00; log `incoming penaltyts=now+600 ... expiry=now+600 active=1` | applies; bar ≈ 10:00; log shows stamp s1 and expiry now+600 |
| 2. Shorten it to 60 s | **dropped by the gate** (incoming now+60 < stored now+600); bar keeps counting from ≈ 9:xx; counters unchanged; log `incoming penaltyts=now+60 stored=now+600 ... expiry=now+600` | applies (s2 > s1); bar ≈ 1:00; log shows expiry now+60 and applied |
| 3. Forgive it (the service sends no lockout) | if +0x18 is 0 or -1: bar ends (`resetStoredTs`), log `(cleared by the game service)`, and the update's counters and levels are dropped, so the Quit Info and loyalty pages keep the old numbers. If +0x18 is a positive time at or before now: dropped, and the old countdown stays | applies (s3 > s2); bar ends; counters and levels update |
| 3a. Forgive to a lower level that still has a lockout | dropped (earlier expiry); old countdown and counters stay | applies; countdown shows the lower level's end |
| 4. Set a lockout, quit the game, start it and log in (see the note below) | the profile's `penaltyts` (an expiry) applies in the fresh process; the bar returns from the profile alone | the profile's stamp applies with R8 = -1: no countdown until the post-login update, then the bar returns. No bar after the update means #712 step 3 or the stamp order is wrong |

Step 4 has a precondition. The bar needs feature bit 0 when the penalty event fires, and the flags message's
own event is handled by no script. If the login profile is applied before `SNSEarlyQuitFeatureFlags` arrives, the
bar stays down until the next penalty event: an update, or `Update` at expiry. Note the order from the run's
log.

Steps 2 and 3a decide between the readings: A keeps the old end, B shows the new one. Step 4 checks the login
path's -1 on the live client. Under B, a log line at step 2 with the expiry the service sent proves that +0x20
reaches the setter's R8. When steps 1-4 behave as column B describes, the verdict above becomes **confirmed: the
layout in #907/#712 matches the game.**

## Prior work

Dualgame's [EchoRelay fork](https://github.com/Dualgame/EchoRelay) (`EchoRelay/docs/protocol/early-quit.md`) and
[echovr-modkit](https://github.com/Dualgame/echovr-modkit) (`mods/early-quit`) read +0x18 as a stamp, +0x20 as the
expiry, and bits 1 and 5 as here. The earlier field map in
`echovr-reconstruction/docs/earlyquit_field_analysis.md` is covered by sections 1-4 here.
