# Where the client builds the `EvrId` in `SNSRemoteLogSetv3`

Nakama logs `*evr.RemoteLogSet{..., evr_id=UNK-1, ...}` for a client session (#160). This traces
the send path in `echovr.exe` (ReVault) to the call that fills the id, and to the runtime code that
writes the fields it reads. Static only; the values of those fields in a run are not in this
document.

## The send path

| Step | Address | What it does |
| --- | --- | --- |
| Message id | `0x244b47685187eae1` | `CSymbol64("SNSRemoteLogSetv3")`, recomputed with `src/abi/symbol_hash.h`; the name string is at `0x1416d4fa3` |
| Periodic trigger | `0x1401c4170`, called from `CR15NetGame::Update`, `LogOut` and `EndMultiplayer` | Flushes remote logs; the senders below are throttled to one flush per 15 s unless the caller forces it (`15.0 <= now - last`) |
| Client sender | `0x140118d50` | When the mode bit that selects the server variant is clear and `NetGame+0x28d0` is non-null, calls `0x1401206c0` once per log buffer (four buffers at `FUN_1401110a0()+0`, `+0x8090`, `+0x10120`, `+0x181b0`) |
| Server sender | `0x140119070` -> `0x1401207c0` | Used when the mode bit is set; fills the first 8 bytes of the id with `config server_id`; the second 8 bytes are left unwritten in the caller's stack buffer (`local_58[16]` is not zeroed) |
| Message builder | `0x1401206c0` | Builds a `0x38`-byte struct and sends it with `0x140f8a460`: bytes `0x00`-`0x0f` = the EvrId (copied from the pointer in its third argument), `0x10`-`0x1f` = a 16-byte id (read through `0x14060bf50` from the object at `NetGame+0x40`, `+0x18`, or the default at `0x142100b20` when that object is null), `0x20` = the log text, `0x30` = the level, `0x34` = 0 (client variant) |
| **EvrId producer** | `get`, `0x140618690`, called by `0x140118d50` as `get(NetGame+0x28d0, buf)` | `buf[0] = user[0x12]` (the qword at `user+0x90`); `buf[1] = *user->vfunc_0x68(...)` (a pointer to the account id) |

So the id is the pair read from the local user object at `NetGame+0x28d0`, the same object
`CNSIUsers::CreateUser` (`0x140606e40`) returns, with the first qword taken whole from `user+0x90`.
`CreateUser` formats the same two values as `"%s-%llu"` with `GetProviderPrefix` (`0x14060d640`, which
switches on `qword & 0xf`) and logs `[NSUSER] Creating user <prefix>-<id>`. The callers already ruled out
in #160 (`AddPlayerUser`, `LogSocialAnalytic`) also call `get`; none of them builds the RemoteLogSet id.

## What writes those fields

`src/runtime/compat/ws_bridge.cpp` (the login-injection block that logs `CNSUser login state patched`)
rewrites the user object that pnsrad's `Users()` returns: `+0x88` = account id (the Discord id),
`+0x90` = login-state qword with only its low nibble replaced by `kBridgeLoginPlatform` (4), and
`+0x9c` = 0x04. The first qword of the EvrId is therefore the whole `+0x90` qword, not a platform
code: its low nibble is the provider, and any other bit set in it reaches Nakama as part of the
platform field. Before that patch runs, the low nibble is whatever the game left there, and
`GetProviderPrefix` returns `"???"` for a nibble outside 1-7.

The fields are consistent with `UNK`: Nakama's `PlatformCode.Abbrevation()` returns `"UNK"` for any value
outside 1-7 (#160), and a value with upper bits set or a zero nibble is outside it. That the two
`user` objects are the same object (the game's `NetGame+0x28d0` and pnsrad's `Users()` first entry)
is inferred from the shared offsets and the patch's documented effect ("unblocks LogInSuccessCB"); it
was not read from a run.

## Not proven

- The values in a real session. One client run answers it: read the runtime line
  `[NEVR.WS] CNSUser login state patched acct=A->B state=S->T ...` (it prints the fields before and
  after the patch), the game line `[NSUSER] Creating user <prefix>-<id>`, and Nakama's `evr_id=` for
  the same session. If `T` has bits above the low nibble, or the first flush precedes the patch
  (`S` unchanged), the platform field is not a clean 1-7 code.
- Whether the runtime should change it. That is a decision after those values are read; no change is
  proposed here.
