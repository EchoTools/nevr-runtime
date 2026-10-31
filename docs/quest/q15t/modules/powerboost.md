# Module `powerboost` (rows R51..R56)

Files: `src/powerboost.{h,cpp}`, `src/quest/plugins/q15t/src/powerboost_core.h`, `src/quest/plugins/q15t/src/powerboost_thunk.inc`, `src/quest/plugins/q15t/src/pbhud_log.h` (shared log helper of
the powerboost / hudmeter / shieldgamma ports), `src/quest/tests/q15t/powerboost_test.cpp`, `src/quest/tests/q15t/pb_thunk_qemu.cpp`, `src/quest/tests/q15t/pbhud_image.h`,
`src/quest/tests/q15t/mod/powerboost.sh` (+ `src/quest/tests/q15t/mod/pbhud_ndk.sh`), `src/quest/tests/q15t/mutants.d/powerboost.sh`.

Port of `<source build>/the source build's powerboost` (598 lines) and the source build's powerboost_core (156 lines):
EC's POWER BOOST on the Quest client. A heal amount of 1,000,000 or more is never a heal: it is a marker the
EC server rules put in the heal channel ("I used it", "you are linked", "unlinked"). Without this module those
markers reach `CR15NetDamageableCS::Heal` as enormous heals.

## Option macro and default (for the main seat to wire; I wired nothing)

`Q15T_POWERBOOST` (an integer), default **3 = cone** (#1625: the wave19a on-list ships an empty `powerboost.on`, which the source reads as `swallow cone`). The source: "powerboost.on (TEST ONLY ... STAGE 0" (the source build's powerboost:1),
"Off (no powerboost.on in the source build's data directory): the switch is read, nothing else"; the on-file's first line selects `swallow` /
`cone` (`:36`, `:400`); QA turns it on with stage 0 = read-only (`powerboost1.on`, `:421`). Numbering offset to keep in mind:
the plugin's enum is `kPowerboostRead = 1` (the source's "stage 0", read-only), `kPowerboostSwallow = 2` (source `swallow`),
`kPowerboostCone = 3` (source `cone`). Values: `0` off, `1` read-only, `2` swallow, `3` cone (default).

API: `InstallResult InstallPowerboost(std::uintptr_t base, int stage, bool net_tick, const PowerboostFns* fns = nullptr)`;
`void PowerboostGameTick(std::uintptr_t game)` (from the ExclusiveUpdateGame tick: only stores the game pointer);
`void PowerboostNetTick()` (stage 3 only, see "Open questions"). `net_tick` says whether the main seat has a net tick wired for
stage 3 (without one stage 3 is refused). `fns == nullptr` resolves `DamageFromClient`, `ResolveBody` and `PrimaryBody` by
`dlsym` on `libr15.so` and checks each against base + its pinned address (`ResolvePowerboostFns`).

## Rows, mechanism, evidence

ELF vaddr = disassembly VA - 0x100000. All addresses below read in the disassembly this session (functions / instructions) and in
a source-build note, not in this tree (the relocation table of the pinned `libr15.so`, build id
`b243509c08ce677aeb95fa348016949b3fc45230`).

| row | what | slot (ELF) | real fn (disassembly, size) | mechanism |
|---|---|---|---|---|
| R51 | `CR15NetDamageableHealNode::Enter` | JUMP_SLOT 0x36e3868 -> 0x23207b0 | 0x24207b0, 372 | data-page GOT swap, wrapper `NodeWrap` |
| R52 | `CR15NetDamageableCS::Heal` | JUMP_SLOT 0x36f7820 and GLOB_DAT 0x3725b80 -> 0x2137e74 | 0x2237e74, 688 | two GOT swaps: `HealWrap` (JUMP_SLOT), `QueuedHealWrap` (GLOB_DAT) |
| R53 | `CR15NetHealBubbleCS::SetTargetInsideBubble` | JUMP_SLOT 0x36d7d08 -> 0x2158954 | 0x2258954, 280 | GOT swap, wrapper `LinkWrap` |
| R54 | `CR15NetDamageableCS::DamageFromClient` | JUMP_SLOT 0x36e5778 (and GLOB_DAT 0x3719f60) -> 0x213e80c | 0x223e80c, 312 | not hooked: called (the sender). Its JUMP_SLOT is only checked |
| R55 | `CPhTarget::ResolveBody` | JUMP_SLOT 0x36ca098 -> 0x19a27ac | 0x1aa27ac, 104 | GOT swap to a naked arm64 thunk (`src/quest/plugins/q15t/src/powerboost_thunk.inc`) |
| R56 | `kConeCall`: the `bl` to ResolveBody's stub inside `UpdateActiveHealBubble` | (code, not a slot) ELF 0x2157998 | 0x2257998 inside 0x22572cc, 4940 | not patched: the thunk recognises it by return address `x30 == base + 0x215799c` |

`readelf` slot lines (R0_readelf_r.txt): JUMP_SLOT 0x36e3868 -> 0x23207b0; JUMP_SLOT 0x36f7820 and GLOB_DAT 0x3725b80 ->
0x2137e74; JUMP_SLOT 0x36d7d08 -> 0x2158954; JUMP_SLOT 0x36e5778 and GLOB_DAT 0x3719f60 -> 0x213e80c; JUMP_SLOT 0x36ca098
-> 0x19a27ac; also JUMP_SLOT 0x36f6110 -> 0x1677d34 (`CPhysicsCS::PrimaryBody`, 0x1777d34 in the disassembly, size 112). Entry words
read in the disassembly (little-endian as stored): Enter `ff0303d1 e84b00fd f5530aa9 f37b0ba9`; Heal `ffc301d1 e81300fd f96303a9
f75b04a9`; SetTargetInsideBubble `ff4303d1 fa4300f9 f96309a9 f75b0aa9`; DamageFromClient `ff8301d1 e81300fd f75b03a9
f55304a9`; ResolveBody `080840b9 1f050031 c0020054 49ee00d0`. The install is data-page only: the slot word must hold
`base + expected_vaddr` or the swap is refused (`InstallImportHook`), so no code word is read or written, and no code-word pin
check is made (the source pins entry words; the GOT-word check replaces it, see "not built").

### R1's unreconciled GLOB_DAT reader: resolved

The row note listed `GLOB_DAT 0x3725b80` with a reader at 0x2420918. In the disassembly the reader is **0x2420904** (ELF 0x2320904),
inside `HealNode::Enter` (0x24207b0, size 372):
```
2420900 adrp x8, 0x3825000
2420904 ldr  x8, [x8, #0xb80]        ; xref type `read` from 0x2420904 to 0x3825b80
```
0x2420918 is `stur q0, [x0, #8]`, the store of that loaded word into the queued functor (not a reader). The source's value
(`kGotHealData = 0x3725b80`, the source build's powerboost:80) is right. The other GLOB_DAT, 0x3719f60 (DamageFromClient), is read at
0x24201ec inside another Enter (0x24200b4, size 348); this module calls DamageFromClient directly and never swaps that word.

### What `HealNode::Enter` does (disassembly, 0x24207b0)

`ldr x8,[x20,#0x80]` (0x2420888), `ldr s8,[x19,#0x20]` (amount, 0x2420890), `ldr w19,[x19,#0x24]` (user, 0x2420894),
`ldr w9,[x8,#0x368]` (the direct flag, 0x2420898). Direct path (0x24208a0..0x24208b0): x0 = cs, x1 = handle, v0 = amount, x2 =
user, `bl 0x1040270` = Heal's PLT stub (`adrp 0x37f7000; ldr [x16,#0x820]`, i.e. JUMP_SLOT 0x36f7820; Heal's stub has 2 callers).
Queued path: stores handle (+0x18), amount (+0x1c), user (+0x20) into a functor (0x242090c / 0x2420910 / 0x242091c) whose
function word is the one loaded from GLOB_DAT 0x3725b80.

### The cone call (R55 + R56), disassembly `UpdateActiveHealBubble` (0x22572cc)

```
2257998 bl 0x00fe5360          ; word 7236b697 (bytes) = 0x97b63672 = the source's `want` from ELF 0xee5360
225799c str x24, [sp, #0xf0]
22579a4 ldr q0, [x0, #0xbd0]   ; the body's quaternion
```
`fe5360` is ResolveBody's PLT stub (`adrp x16,0x37ca000; ldr x17,[x16,#0x98]` = slot ELF 0x36ca098). Other `bl`s to that stub
inside UpdateActiveHealBubble (0x2257364, 0x2257294) are passed through. At the call: `ldr x24,[sp,#0x3c0]` (0x225794c) is
the target's damageable system and nothing writes x24 between there and 0x2257998; `add x10,x8,#0x38 ; str x10,[sp,#0xe8]`
(0x2257594) is the radius pointer = record + 0x38; `str x9,[sp,#0xa0]` (0x2257968) is the target handle word.
`SetTargetInsideBubble` reads its third argument at +0x48 (0x22589b0, 0x2258a24). ResolveBody's body table strides are 0xf38
and 0x1e20 (the stand-in is 0xf40 bytes).

## Behaviour by stage

All stages: one structured line per install (hook name, slot ELF vaddr, status, reason; refusal = ERROR). All words or none:
if the second or third swap is refused, the earlier ones are put back (`SwapWord`), one ERROR line says so, nothing stays
installed. Every read of game memory is a `SafeRead`; any doubt calls the real function; the real function is always called
with the caller's own arguments. No namespace-scope constructors (checked in the arm64 object: no `.init_array` / `.ctors`).

**Stage 1 (read-only; the source's stage 0).** Swaps R51, R52 (JUMP_SLOT), R53. `NodeWrap` reads amount and user from the data
(one `SafeRead` of 8 bytes at +0x20, before the real call, never after), calls Enter, and logs a marker line afterwards. `HealWrap`
calls Heal with the float untouched and logs marker lines (kind, rez, v, dr_permille, seconds, where, index, user). `LinkWrap`
calls SetTargetInsideBubble first, then logs one line per (bubble, target) change or per second for the same pair. Decode:
[1e6,2e6) kind 1 "used"; [2e6,3e6) kind 2 "linked"; [3e6,4e6) kind 1 + rez; [4e6,5e6) kind 2 + rez; [6e6,7e6) kind 3 "unlinked";
anything else kind 0 (an `amount is not a marker; passed on` line). High-fan-in rule: Heal and Link keep counters only, no
per-call line; marker lines cap at 96, link lines at 160, each cap announced once by one INFO line.

**Stage 2 (swallow).** Additionally swaps Heal's GLOB_DAT word (the queued heal) to `QueuedHealWrap`. Markers never reach Heal.
A queued kind-1 marker is sent to the server as `DamageFromClient(cs, handle, -amount, 0, 0)` (the "I used it" message); kinds
2 / 3 are swallowed with nothing sent. A direct-path kind 1 (in `HealWrap`, while `NodeWrap`'s real Enter runs on that thread) is
held for up to 2 s and sent by the next queued heal through the same damageable system; an older or other-system held use is
dropped with a line. A marker that arrives through Heal outside the node (relayed by the server) is swallowed, never re-sent.
The swallow flag is set only after the GLOB_DAT swap succeeded; if DamageFromClient's slot does not hold DamageFromClient at its
pinned address, or the GLOB_DAT swap is refused, the module stays at stage 1 and logs an ERROR (`swallow OFF; stage 1 only`).

**Stage 3 (cone).** Needs a net tick, `ResolveBody` and `PrimaryBody` resolved, and the word at ELF 0x2157998 equal to `bl` of
ResolveBody's stub (0x97b63672). Stores `q15t_pb_cone_ret = base + 0x215799c` and `q15t_pb_resolve_real` first, then swaps
ResolveBody's JUMP_SLOT to the naked thunk (rolled back, both words cleared, if the swap is refused). The thunk acts only when
`x30 == q15t_pb_cone_ret`; every other of the 771 callers goes straight to the real function with x0, x1, x2, x30 and every
callee-saved register unchanged (x16 is the only register used, no stack touched). For the one call it runs
`q15t_pb_cone_at(x0 = CPhTarget*, x1 = UpdateActiveHealBubble's sp, x2 = its x24)`, which is plain C++ (`ConeAt`, host-tested):
it asks the real ResolveBody for the target's body, reads the bubble radius through `[sp+0xe8]` (a POWER BOOST bubble carries an
angle: radius = metres + angle / 100000), the owner through `PrimaryBody(record+0, record+8)` and the owner's body position
(+0x8f8) and quaternion (+0xbd0), and rejects by returning a far stand-in body (position x = 1e30, so the game's own distance test
takes its outside branch): outside the angle (5 degrees wider once linked), beyond floor(radius) metres, or within 0.3 m of the
owner (the owner is never self-linked). Not a POWER BOOST bubble (no angle, or above 180) or any unreadable word: the real body.
A change of link state is queued (16 entries; a full queue drops its oldest, logged) and `PowerboostNetTick` sends it as
`DamageFromClient(dcs, index, -marker, 0, 0)`: link = 2e6 + dr_permille * 1000 + whole seconds left (+ 2e6 if the use had rez),
unlink = 6e6; only when we used it within 30 s, only for a change younger than 2 s, only to a damageable system still alive
(`FindComponentSystems`, key + vptr). The cone diagnostic line is rate-limited (twice a second, cap 240).

### The naked thunk

`src/quest/plugins/q15t/src/powerboost_thunk.inc`, 15 instructions (`bti c`; `adrp/ldr` cone_ret; `cmp x30,x16`; `b.eq`; `adrp/ldr` real; `br x16`; then
`stp x29,x30,[sp,#-16]!; mov x29,sp; add x1,sp,#16; mov x2,x24; bl q15t_pb_cone_at; ldp x29,x30,[sp],#16; ret`). A
`__attribute__((naked))` function with inline asm, arm64 only (`#if defined(__aarch64__)` in `powerboost.cpp`; the host build
swaps in a pass-through stand-in). No code page is written; the one write is the data-page slot swap. It is the same text the
plugin compiles and the qemu test runs (`src/quest/tests/q15t/pb_thunk_qemu.cpp` includes the `.inc`).

## Tests, and what they do not show

`Q15T_MODULE=powerboost bash src/quest/tests/q15t/run_host_tests.sh` runs, in order: the host test against a fake image (a 0x3730000-byte lazy
anonymous mapping so real ELF vaddrs are slots; x86 trampolines stand in for the game functions; slot pages made read-only like
RELRO; real `mprotect`, real `/proc/self/maps`), the NDK r26d clang++ arm64 build (fails if the object has `.init_array` or
`.ctors`), a golden check of the compiled thunk with `llvm-objdump -dr` (exactly the 15 instructions and 5 relocations), and the
thunk under `qemu-aarch64` (freestanding static binary): the ordinary call (x0 / x1 / x2 / x30 / x19 / x20 / x24 / x25 intact, real
function returns to the caller), the armed call (target, sp, x24 delivered, result returned, real not called), and the exact compare
(a return address one instruction off is an ordinary call). Not shown: the thunk running inside the game on a headset; the byte
layout of the game's body / record structures (below); DamageFromClient's behaviour; the real disassembly code words of the
installed library (not read at run time by this module; every address is a GOT word check).

## Prediction (written before any headset run)

With `Q15T_POWERBOOST=1` on the pinned build in an EC match where somebody uses POWER BOOST:
1. the log shows three `powerboost: hook install ... status=ok` lines and `powerboost: on stage=1` at start, no ERROR;
2. a use shows `powerboost: marker passed on kind=1 ... where=node` and then `where=heal` lines (the game still sees the huge
   heal: that is stage 1's point), the linked allies show `kind=2` lines; `powerboost: link` lines name (bubble, target) pairs;
3. no per-call log lines: the log does not grow with the heal rate.
With `Q15T_POWERBOOST=2`: the heal marker no longer shows up as a heal on the HUD / health bar; `use sent to the server` lines
appear once per use and the server-side effect (the boost starting) happens; no `marker passed on ... where=heal` lines for
markers. With `Q15T_POWERBOOST=3`: allies outside the cone / range stop being linked (their buff ends); `cone target` lines with
verdict LINK / no at most twice a second; `link marker sent to the server` lines follow only a use of ours.

## Falsifiers

- Any `powerboost: hook install` ERROR (`unexpected_original`): a slot did not hold base + the pinned address; the module is
  inert. A different build, or a preempting symbol.
- Stage 1: a game crash or a changed heal value for an ordinary heal (the wrapper must be invisible to non-markers).
- Stage 2: markers still appear as `where=heal` in the log although `swallow on` was logged: the queued word is not the GLOB_DAT
  one, or Heal is reached some other way. Or `use sent` lines but no server-side boost: `DamageFromClient`'s argument
  convention (-amount, 0, 0) is wrong, or sending from the queue drain's context is not allowed.
- Stage 3: a crash in `UpdateActiveHealBubble` right after `cone on` (the stand-in body is too small or the frame offsets
  0xe8 / 0xa0 / x24 are wrong for this build); `cone target` lines with max_deg 0 on a POWER BOOST bubble (radius encoding);
  allies inside the cone dropped (quaternion convention, axis +z).
- Any frame-rate drop on stage 1 (771 callers of ResolveBody in stage 3 pass through the thunk: it must cost three instructions).

## Needs a word patch / not built

- **Needs a word patch:** none. Every row is a data-page GOT swap; R56 is recognised by return address, not patched.
- Not built: the source's entry-word pin checks (the source build's powerboost verifies code words); the plugin's rule is the exact
  GOT-word check (`base + expected_vaddr`), the contract's "no code word read or written".
- Not built: the source's drain-from-recvfrom net tick. `PowerboostNetTick()` is a function the main seat must call from a net
  context; without it stage 3 is refused (`net_tick == false` -> ERROR `cone OFF; stage 2 unchanged`).
- Not built: the source's on-file / setprop switches (the main seat maps `Q15T_POWERBOOST` to `stage`).

## Open questions (unverified)

1. **The net tick.** The plugin has only the ExclusiveUpdateGame tick. The source sends stage 3's link / unlink markers from a
   `recvfrom` wrapper on the game's own socket (the source build's ec:170-195, the module net ticks registered at `:117`, `:649`), not from
   the game tick. Whether `DamageFromClient` is safe to call from the game tick is unverified; stage 3 is refused until the main
   seat wires a net tick. Stage 2's sends come from the deferred-queue drain (QueuedHealWrap), as in the source.
2. The damageable system's key `0xc9dc08cfc2f5920f` is a source constant (the source build's flowdiag:91, `:93`); not verified in the disassembly.
   Its vptr symbol is `PTR_~CR15NetDamageableCS_03793ea0` (disassembly 0x3793ea0, ELF 0x3693ea0), which is verified.
3. The body layout offsets (+0x8f8 position, +0xbd0 quaternion), the bubble record offsets (+0, +8, +0x38) beyond the
   instructions quoted above, and the stand-in's size and semantics are from the source, not independently decoded.
4. The qemu run proves the thunk's register contract on arm64, not its behaviour inside the game.
