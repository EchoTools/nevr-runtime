# What the client does after `[SYSNET] Found Internet connection`

Issue #13 reports a native-Windows client whose log stops after two lines, the second being
`[SYSNET] Found Internet connection`. This is the ordered list of everything the game runs after
that line until its next log line, from ReVault (`echovr.exe`). It is static; no hang was observed
on a running client.

## Where the line comes from

`0x1401f6fa0` (`FUN_1401f6fa0`) is the internet check. It creates the Windows Network List Manager
(`CoCreateInstance`, CLSID bytes at `0x141d3bbc0` read as `{DCB00C01-570F-4A9B-8D69-199FDBA5723B}`,
interface bytes at `0x1416e9000` read as `{DCB00000-570F-4A9B-8D69-199FDBA5723B}`), calls vtable slot
`+0x68` (`GetConnectivity`) into `DAT_1420a29f0`, and logs `[SYSNET] Found Internet connection (IPv4)`
(bit `0x40`), `(IPv6)` (bit `0x400`) or, with neither bit, falls back to `InternetGetConnectedState`
and logs the plain line (strings at `0x1416e90e0`, `0x1416e9110`, `0x1416e9140`).

## The caller, in order

`0x1401f6fa0` has seven distinct callers (ReVault): `0x140157bd0`, `0x140157fb0` (the multiplayer init
orchestrator named in `src/runtime/patch/mode_patches.cpp`), `0x140172850`, `0x1401728a0`,
`0x14017c660`, `0x1401ac820` and `0x1401ac9d0`. The logged line does not say which of them ran, so
#13's log does not identify the caller. This section follows the first-connect step `0x140157bd0`
(called from `0x140dd9950`), the one whose next steps are listed below. It runs when
the NetGame state is 0 or 1. After the SYSNET line (gate: mode bits at `NetGame+0x2da0`,
`(bit2==0 && bit6!=0) || (bit41==0 && SYSNET())`), it does, in this order:

| # | Call | What it does | Logs on success? | Logs on failure |
| --- | --- | --- | --- | --- |
| 1 | `0x140f80720` | tests whether the broadcaster already has a local address (`0x1401fb2d0` on `broadcaster+0x280`) | no | n/a |
| 2 | `0x1401f4fb0`, `0x1405f6ae0`, `0x14151ee70` | load and decode `json/r14/config/netconfig_client.json` (client) or `netconfig_localserver.json` | no | `[NETGAME] broadcaster config decode failed` (level 8) |
| 3 | `0x140f7f8b0` (`CBroadcaster::InitializeFromJson`) then `0x140f7fe50` (`CBroadcaster::Initialize`) | read the config keys, then: `socket(AF_INET,SOCK_DGRAM)` (`0x1401f8290`), the bind loop `0x1401f5ba0` (up to `retries` = 20 in the shipped client config), `getaddrinfo` of the machine's own hostname (`0x1401f7ac0`), a compare against the unresolved defaults, `getnameinfo` (`0x1401f7090`) | no | `%s: Broadcaster port (%u) initialization failed` (level 8); `Could not resolve local host!` (level 4) |
| 4 | `0x14060c3d0` | tests whether the service connection exists | no | n/a |
| 5 | `0x1406193e0`, `0x140181730`, `0x140621ec0`, `0x140606190` -> `BeginConnection` (`0x140f759c0`) | build the connection description and start the connection to the game service | no | state set to `0xffffffa1` |
| 6 | `CNSIUsers::CreateUser` (`0x140606e40`) | create the local user | yes: `[NSUSER] Creating user %s` (level 2) | n/a |
| 7 | `0x140602550` | create the lobby object with the two callbacks `0x140161100` and `0x140160ee0` | no | n/a |
| 8 | `LogIn` (`0x14017ef10`), then `SetGameState(2)` | begin login | | |

`[NETGAME] broadcaster initialization failed` (level 8) follows a `0` return from `0x140f7f8b0`.

So the first line that can follow the SYSNET line on success is `[NSUSER] Creating user ...` from step 6.
A log that ends at the SYSNET line means the thread is inside steps 1 to 5 (or step 6 never logs),
and none of those steps logs on entry.

## The blocking calls in that window

| Call | Address | Blocks on | Config knob |
| --- | --- | --- | --- |
| `getaddrinfo("", port)` with flags 0, `AF_INET`, `SOCK_DGRAM`, `IPPROTO_UDP` | `0x1401f7ac0`, called unconditionally from `0x140f7fe50` | own-hostname resolution | none read; the call is not gated by `hostname_lookup` |
| `getnameinfo` | `0x1401f7090`, called from `0x140f7fe50` with `~(flags >> 1) & 1` as the numeric-host flag | reverse DNS only when `hostname_lookup` is true (flags value 0); with it false the call passes `NI_NUMERICHOST` (2) | `broadcaster_init.hostname_lookup`, bit 1 of the flags built in `0x140f7f8b0` |
| `BeginConnection` | `0x140f759c0` | starts the connection to the service; whether it blocks was not read | endpoint config |

The shipped `netconfig_client.json` and `netconfig_localserver.json` in the vanilla install
(`sourcedb/rad15/json/r14/config/`) both set `"hostname_lookup": false`, so `getnameinfo` there is
numeric-only and cannot do reverse DNS. The only name-resolution call left in the window is the
`getaddrinfo` in `0x1401f7ac0`, which matches the static finding recorded in #13.

## Not proven

- No hang has been reproduced; #13's own measurements show the `getaddrinfo` takes 5.9 ms on a
  healthy Windows 11 guest.
- Whether `BeginConnection` (`0x140f759c0`) can block on native Windows was not read. The
  `CoCreateInstance` in the SYSNET step returns before the line is logged, so it is not a blocker for
  a log that ends on that line.
- That #13's log came from `0x140157bd0` rather than another caller of `0x1401f6fa0`.
- In server/headless mode the runtime replaces `0x1401f6fa0` with a function that returns 1
  (`PatchAddresses::SYSNET_CHECK`, `src/runtime/patch/mode_patches.cpp`), so such a run never logs
  the SYSNET line; a log that ends on it is a client run. No other address in the table is hooked
  by the runtime (a search of `src/` for those addresses and for `getaddrinfo` finds only the
  SYSNET references).
- A stuck process's stacks (`tools/winvm/dump_stacks.py`
  after `just test-winvm --dump-on-fail`) name which of steps 1 to 5 holds the thread; the table above
  maps each frame to its step.
