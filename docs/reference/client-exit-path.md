# How the client leaves the process when its window goes away

After the window is closed, a Wine client sometimes stays alive (issues #53 and #341). This pins the paths that
exist and which of them the measurements cover: static (ReVault `echovr.exe`, runtime source) plus the
outcomes recorded on those issues.

## The runtime's part

`GameMainWrapperHook` (`src/runtime/lifecycle/crash_recovery.cpp`) calls the game's main loop. When the
loop returns on a client it logs `[NEVR.PATCH] game loop returned: the client is exiting (no server hold
outside server mode)` and returns, so the process exits through the game's normal teardown (the
`[NEVR.PATCH] ExitProcess(0) called — not suppressed, process exiting` line). A server that
sees the loop return exits through `PerformGracefulShutdown`: code 0 when a console shutdown is
pending, code 1 otherwise. The crash-recovery hold above it is reached only after a recovered crash
(`crashCount > 0`), which the exception handler arms for servers; it sleeps until a console shutdown is
pending and then exits with code 1.

## The game's part

The window procedure with the keyboard/IME handling is `FUN_1400eaa10` (`0x1400eaa10`, called from
`fcn.1400e9ef0`). For `WM_CLOSE` (`0x10`) it calls `DestroyWindow(hwnd)` and returns 0. For `WM_DESTROY`
(`2`) it looks the window up in the game's window table (`DAT_14209faa0`) and ORs `0x10` into that
window's flag word; the same table receives `0x1`, `0x2`, `0x8` for other messages. Nothing in this
procedure posts a quit message (`PostQuitMessage` has no call site in `echovr.exe` per
`revault search code`), so the game loop must observe the `0x10` flag to end. The consumer of that flag is not
traced.

## Measured outcomes (issues #53 and #341; Wine under a nested Xephyr)

| Close method | Window manager | Result |
| --- | --- | --- |
| `WM_DELETE_WINDOW` (`xdotool windowquit`) | present | clean exit within 10 s; both runtime lines above in the game log |
| `XDestroyWindow` (`xdotool windowclose`) | none | the process stays alive and keeps logging; no shutdown lines |

The first path goes `WM_CLOSE` -> `DestroyWindow` -> `WM_DESTROY` in the table above. The second never shows the
runtime's `game loop returned` line, so the game loop does not return. The window handle stays valid under
Wine after an outside `XDestroyWindow` (`IsWindow` and `IsWindowVisible` both stay true over 371 samples,
Wine's "destroyed from the outside"), so a poll of the handle cannot detect it; #341 stays open, and the
candidate signal is the swapchain/present results after the destroy, which is not measured.

## Not established

Whether `FUN_1400eaa10` receives `2` (`WM_DESTROY`) for a foreign `XDestroyWindow` under Wine, and what
consumes the window table's `0x10` flag. A breakpoint-free way to settle it is a runtime detour on `0x1400eaa10`
that logs `param_2` for messages `0x10` and `2`, and the window-table flag word after `2`. Received and flag set
but no exit: trace the flag's consumer. Not received: the cause is Wine's handling of a foreign `XDestroyWindow`,
and the `wineserver -k` in `launch-client.sh` is the only cleanup.
