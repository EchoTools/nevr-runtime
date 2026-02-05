# Task 5 Root Cause Analysis: Server mode does not start

## 1. Executive Summary (what was wrong)
Server mode never started because the launcher (`evr-mcp_echovr_start` in evr-test-harness) **never passes the required `-server` flag**. The game patches only enable dedicated server behavior when `-server` is present, so launching with `-mp -level` alone always boots the game as a client.

## 2. Root Cause (why server mode failed)
The **activation condition for server mode is a CLI flag gate** in `gamepatches`:
- `-server` sets `isServer = TRUE`.
- The dedicated server patches (`PatchEnableServer`) only run when `isServer` is true.

`evr-mcp_echovr_start` launches the game with client-style args (`-mp`, `-gametype`, optional `-level`) and **omits** `-server`, so `isServer` remains false and all server-mode patches are skipped. As a result, the game remains in client mode and the HTTP API shows client restrictions (e.g., err_code -6).

## 3. Technical Details (how `-server` flag works)
`gamepatches/patches.cpp`:
- Registers `-server` as a valid argument.
- Parses it in `PreprocessCommandLineHook` and sets `isServer = TRUE`.
- If `isServer`:
  - `PatchEnableServer()` applies binary patches:
    - **SERVER_FLAGS_CHECK**: Forces dedicated server flags (load sessions from broadcast + dedicated server mode).
    - **ALLOW_INCOMING**: Forces `allow_incoming` to true.
    - **SPECTATORSTREAM_CHECK**: Bypasses `-spectatorstream` requirement to enter load-lobby state.
  - `PatchDisableLoadingTips()` avoids resource-heavy loading tips.

Legacy gamepatches have the same gating logic.

## 4. Fix Required (exact command to start server)
Add the **`-server`** flag when launching Echo VR. Minimum command (based on harness args):

```
wine ./ready-at-dawn-echo-arena/bin/win10/echovr.exe \
  -noovr -windowed -httpport 6721 -gametype echo_arena -mp \
  -server \
  [-level <level>] \
  [-headless] [-noconsole] \
  [-config-path <path-to-config.json>]
```

If using the harness, add a new input field and pass `-server` (and optional `-headless`, `-noconsole`, `-config-path`) through `StartProcess`.

## 5. Impact on Original Problem (player join error)
The original player-join error **cannot be validated** because the game never enters server mode. Without `-server`, the server patches never apply, so HTTP API endpoints remain restricted and the server does not register with ServerDB. This blocks any meaningful testing of player join behavior.
