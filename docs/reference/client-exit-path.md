# How the client leaves the process when its window goes away

Issue #53: after the window is closed, a Wine client sometimes stays alive. This pins the paths that
exist and which of them the measurements cover. Static (ReVault `echovr.exe`, runtime source) plus the
measurements recorded on the issue; no run was made for this document.

## The runtime's part

`GameMainWrapperHook` (`src/runtime/lifecycle/crash_recovery.cpp`) calls the game's main loop. When the
loop returns on a client it logs `[NEVR.PATCH] game loop returned: the client is exiting (no server hold
outside server mode)` and returns, so the process exits through the game's normal teardown (the
`[NEVR.PATCH] ExitProcess(0) called — not suppressed, process exiting` line). Only a server enters the
`while (true) Sleep(1000)` hold. The crash-recovery hold above it is reached only after a recovered
crash (`crashCount > 0`), which the exception handler arms for servers.

## The game's part

The window procedure with the keyboard/IME handling is `FUN_1400eaa10` (`0x1400eaa10`, called from
`fcn.1400e9ef0`). For `WM_CLOSE` (`0x10`) it calls `DestroyWindow(hwnd)` and returns 0. For `WM_DESTROY`
(`2`) it looks the window up in the game's window table (`DAT_14209faa0`) and ORs `0x10` into that
window's flag word; the same table receives `0x1`, `0x2`, `0x8` for other messages. Nothing in this
procedure posts a quit message (`PostQuitMessage` has no call site in `echovr.exe` per
`revault search code`), so the game loop must observe the `0x10` flag to end. I did not trace the
consumer of that flag.

## What was measured (issue #53, 2026-10-07, GE-Proton 11-3, nested Xephyr)

| Close method | Window manager | Result |
| --- | --- | --- |
| `WM_DELETE_WINDOW` (`xdotool windowquit`) | present | clean exit within 10 s; both runtime lines above in `nevr-2026-10-07T21-04-19.813.jsonl` |
| `XDestroyWindow` (`xdotool windowclose`) | none | process alive for the whole 60 s watched; no shutdown lines (`nevr-2026-10-07T21-01-10.287.jsonl`) |

The first path goes `WM_CLOSE` -> `DestroyWindow` -> `WM_DESTROY` in the table above. The second never
shows the runtime's `game loop returned` line, so the game loop did not return: either the destroy
produces no `WM_CLOSE`/`WM_DESTROY` in the game's procedure under Wine, or the flag it sets is not
consumed. Which of the two is not established.

## What a run must show to settle it

Under Wine without a window manager, destroy the X window and log, from inside the process, whether
`FUN_1400eaa10` receives `2` (a breakpoint-free way is a runtime detour on `0x1400eaa10` logging
`param_2` for messages `0x10` and `2`, and the window-table flag word after `2`). Received and flag set but
no exit: trace the flag's consumer. Not received: the cause is Wine's handling of a foreign `XDestroyWindow`
and the `wineserver -k` in `launch-client.sh` is the only cleanup.
