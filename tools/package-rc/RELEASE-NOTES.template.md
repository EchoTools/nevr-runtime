# nEVR runtime @VERSION@ (release candidate @RC@)

Commit `@COMMIT@`. Two artifacts, built the same way from the same commit:

| file | what |
|---|---|
| `@ZIP@` | Windows: `BugSplat64.dll`, `install.ps1`, `uninstall.ps1`, `README.txt`, `SHA256SUMS` |
| `@APK@` | Quest: the repacked, debug-signed APK |
| `SHA256SUMS` | SHA-256 of the two files above |

Neither build needs a config file. The service endpoints and the public client keys are built in; the
Windows DLL logs in with no `config.yaml`, and the Quest APK turns its login and social features on
with no `nevr-quest.json`.

## What to test
- Sign in (the device-code prompt on first run), reach the lobby, see your friends.
- Invite a friend and join a party; mute and ghost toggles.
- Leave and re-enter a match; voice chat (Windows: the game's own microphone path).
- Report anything odd with the newest log file (see below).

## Install and uninstall
- **Windows:** unzip, close the game, run `install.ps1` (PowerShell, `-ExecutionPolicy Bypass`). It backs up
  the original `BugSplat64.dll` with a timestamp and renames a legacy `dbgcore.dll` aside; `uninstall.ps1`
  restores both. Nothing is deleted.
- **Quest:** `adb install -r @APK@` over the build you already have (it is signed with the same key as
  earlier test builds; a different key needs an uninstall first, which clears the app's data).

## Notes
- The Windows DLL is **unsigned**: Defender or SmartScreen may warn about it.
- The Windows log is `%LOCALAPPDATA%\EchoVR\logs\nevr-*.jsonl`; the Quest log is
  `/sdcard/Android/data/com.readyatdawn.r15/files/nevr-sentinel.log`.
- Version string in both artifacts: `@VERSION@`.
