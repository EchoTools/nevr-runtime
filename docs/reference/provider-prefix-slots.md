# Platform-provider prefix slots in echovr.exe

**Question this answers:** how many characters can a patched provider prefix
actually be? The XPID patch (`src/runtime/patch/xpid_patch.cpp`) rewrites PSN's
strings to `DSC`, and the working assumption was that every site is exactly 4
bytes — a 3-character ceiling. **That is true for PSN, but for the wrong reason,
and it is not true for every provider.**

Measured 2026-07-30 directly from the shipped binary
(`bin/win10/echovr.exe`, 35,397,120 bytes) by locating each string and computing
the distance to the next non-zero byte. RVA = file offset + `0x1A00`.

## The two tables

The game keeps provider names in **two parallel blocks**, and the XPID patch has
to rewrite both. Slot size is the distance to the next string, so capacity is
`slot − 1` characters (the null must fit).

### Dash-prefix block (`0x16D0F5C`–`0x16D0FA4`)

| RVA | String | Slot | Max chars |
| --- | --- | --- | --- |
| `0x16D0F5C` | `STM-` | 8 | 7 |
| `0x16D0F64` | `PSN-` | 8 | 7 |
| `0x16D0F6C` | `XBX-` | 12 | 11 |
| `0x16D0F78` | `OVR-ORG-` | 12 | 11 |
| `0x16D0F84` | `OVR-` | 8 | 7 |
| `0x16D0F8C` | `DMO-` | 8 | 7 |
| `0x16D0F94` | `BOT-` | 8 | 7 |
| `0x16D0F9C` | `???-` | 12 | 11 |

### Compact-name block (`0x16D7130`–`0x16D7158`)

| RVA | String | Slot | Max chars |
| --- | --- | --- | --- |
| `0x16D7130` | `BOT` | 4 | **3** |
| `0x16D7134` | `STM` | 4 | **3** |
| `0x16D7138` | `PSN` | 4 | **3** |
| `0x16D713C` | `XBX` | 4 | **3** |
| `0x16D7140` | `OVR-ORG` | 8 | 7 |
| `0x16D7148` | `OVR` | 4 | **3** |
| `0x16D714C` | `DMO` | 4 | **3** |
| `0x16D7150` | `???` | 8 | 7 |

## The answer

A provider's usable prefix length is the **minimum** across all of its sites,
because the patch rewrites them together. The five sites the XPID patch touches:

| Patch site | RVA | Current | Slot | Max |
| --- | --- | --- | --- | --- |
| `XPID_PLATFORM_SHORT_NAME` | `0x16D0EE0` | `PSN` | 8 | 7 |
| `XPID_PLATFORM_DASH_PREFIX` | `0x16D0F64` | `PSN-` | 8 | 7 |
| **`XPID_PLATFORM_COMPACT_NAME`** | **`0x16D7138`** | **`PSN`** | **4** | **3** |
| `XPID_PLATFORM_FALLBACK_PREFIX` | `0x16D0F9C` | `???-` | 12 | 11 |
| `XPID_PLATFORM_COMPACT_FALLBACK_NAME` | `0x16D7150` | `???` | 8 | 7 |

> **PSN's ceiling is 3 characters, and the binding site is the compact name at
> `0x16D7138` — not the dash prefix.** Four of the five sites have 7–11
> characters available. One 4-byte slot sets the budget for all of them.

**OVR-ORG's ceiling is 7 characters** — compact name 8 bytes, dash prefix 12.

So the choice is real and it costs more than one character:

| Slot | Max prefix | Fits `EVR` | Fits `EVRCE` | Fits `DSC-NVR` | Fits `DSC-NOVR` |
| --- | --- | --- | --- | --- | --- |
| PSN | 3 | yes | no | no | no |
| OVR-ORG | 7 | yes | yes | yes | no (8) |

`DSC-NOVR` is 8 characters and does **not** fit either slot. `DSC-NVR` (7) fits
OVR-ORG's.

## Two things already in the binary

**A native `BOT-` provider exists** — `0x16D0F94` (dash) and `0x16D7130`
(compact). The game also carries the string **`generating bot account id`** at
`0x16D7160`. So a bot/spectator identity namespace is not something that has to be
invented; the engine already has one. This is directly relevant to the
player-plus-own-spectator collision, and it matches the
`BOT-<snowflake>` device-ID convention already in use on the Nakama side.

**The XPID format string is `%s-%llu`** at `0x16D7158` — prefix, hyphen, unsigned
64-bit id. That is the shape any replacement identity has to keep: **the id half
must remain a uint64**, which rules out UUIDs without also patching the formatter.

## Caveats — read before writing more than 4 bytes

1. **The slack is zero fill.** Every gap measured above is `00` bytes, consistent
   with alignment padding. Writing into it is safe **only if nothing references an
   interior address**. Nothing should point at, say, `0x16D0F68` (mid-padding), but
   this has not been proven by exhaustive xref analysis — it is inferred from the
   layout. Confirm before relying on it for a production patch.
2. **The size constants are asserted.** `xpid_patch.cpp` `static_assert`s each
   replacement against `XPID_*_SIZE` in `src/runtime/hook/addresses.h`. Extending a
   prefix means updating those constants, not just the bytes.
3. **`???` is the fallback, not `UNK`.** `ws_bridge.cpp`'s `PlatformPrefix()`
   returns `"UNK"` for an unmapped platform code, which never matches anything the
   game renders. Cosmetic today because we only send mapped codes, but the two
   sides disagree about what "unknown" looks like.
4. **The prefixes are referenced by direct address**, not through a pointer table —
   the qwords following the dash block point elsewhere. That is why overwriting in
   place reaches all call sites, and why relocating a string to gain room is not a
   small change.
