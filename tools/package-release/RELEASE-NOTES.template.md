# nEVR runtime @VERSION@

Commit `@COMMIT@`.

@IF_APK@Two artifacts, built the same way from the same commit:
@IF_NOAPK@One artifact: the Windows zip. **No Quest APK in this release** (the Quest build is not part of
@IF_NOAPK@the public set).

| file | what |
|---|---|
| `@ZIP@` | Windows: `BugSplat64.dll`, `install.ps1`, `uninstall.ps1`, `README.txt`, `SIGNING.txt`, `SHA256SUMS` |
@IF_APK@| `@APK@` | Quest: the repacked, debug-signed APK |
@IF_APK@| `SHA256SUMS` | SHA-256 of the two files above |
@IF_NOAPK@| `SHA256SUMS` | SHA-256 of the file above |

@IF_APK@Neither build needs a config file. The service endpoints and the public client keys are built in; the
@IF_APK@Windows DLL logs in with no `config.yaml`, and the Quest APK turns its login and social features on
@IF_APK@with no `nevr-quest.json`.
@IF_NOAPK@The Windows DLL needs no config file: the service endpoints and the public client keys are built in, so
@IF_NOAPK@it logs in with no `config.yaml`.

## What to test
- Sign in (the device-code prompt on first run), reach the lobby, see your friends.
- Invite a friend and join a party; mute and ghost toggles.
- Leave and re-enter a match; voice chat (Windows: the game's own microphone path).
- Report anything odd with the newest log file (see below).

## Install and uninstall
- **Windows:** unzip, close the game, run `install.ps1` (PowerShell, `-ExecutionPolicy Bypass`). It backs up
  the original `BugSplat64.dll` with a timestamp and renames a legacy `dbgcore.dll` aside; `uninstall.ps1`
  restores both. Nothing is deleted.
@IF_APK@- **Quest:** `adb install -r @APK@` over the build you already have (it is signed with the same key as
@IF_APK@  earlier test builds; a different key needs an uninstall first, which clears the app's data).

## Signing
A separate signing step may sign `BugSplat64.dll`, `install.ps1` and `uninstall.ps1` after the build and
replace the zip under the same name; these notes are the same either way. A file is signed when it carries a
signature, and `SIGNING.txt` inside the zip says how to check each one. Compare the zip's SHA-256 with
`SHA256SUMS`; the zip's own `SHA256SUMS` lists the files inside it. Windows Defender or SmartScreen may warn
about a file that is not signed.
@IF_APK@The Quest APK is debug-signed with the same key as earlier test builds, which is a sideload signature,
@IF_APK@not a code-signing certificate.

## Notes
@IF_APK@- The Windows log is `%LOCALAPPDATA%\EchoVR\logs\nevr-*.jsonl`; the Quest log is
@IF_APK@  `/sdcard/Android/data/com.readyatdawn.r15/files/nevr-sentinel.log`.
@IF_APK@- The version is `@VERSION@`, in both artifacts.
@IF_NOAPK@- The Windows log is `%LOCALAPPDATA%\EchoVR\logs\nevr-*.jsonl`.
@IF_NOAPK@- The version is `@VERSION@`; `BugSplat64.dll` carries it with the commit in one identity string,
@IF_NOAPK@  `NEVR-BUILD @VERSION@ @COMMIT@`.
