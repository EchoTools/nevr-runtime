# nEVR runtime @VERSION@

Commit `@COMMIT@`.

@IF_APK@Two artifacts, built the same way from the same commit:
@IF_NOAPK@Two files. **No Quest APK in this release** (the Quest build is not part of the public set).

@IF_APK@| file | what |
@IF_NOAPK@| file | SHA-256 | what |
@IF_APK@|---|---|
@IF_NOAPK@|---|---|---|
@IF_APK@| `@ZIP@` | Windows: `BugSplat64.dll`, `nevr-runtime.dll`, `echovr_server.exe`, `install-nevr-runtime.exe`, `nevr-install.exe`, `install.ps1`, `uninstall.ps1`, `quest-diagnose.ps1`, `README.txt`, `SIGNING.txt`, `SHA256SUMS` |
@IF_NOAPK@| `@ZIP@` | `@SHA_ZIP@` | Windows, for players and server operators: `BugSplat64.dll`, `nevr-runtime.dll`, `echovr_server.exe`, `install-nevr-runtime.exe`, `nevr-install.exe` (all stripped, no debug symbols), `install.ps1`, `uninstall.ps1`, `quest-diagnose.ps1`, `README.txt`, `SIGNING.txt`, `SHA256SUMS` |
@IF_APK@| `@APK@` | Quest: the repacked APK |
@IF_APK@| `SHA256SUMS` | SHA-256 of the two files above |
@IF_NOAPK@| `@QUEST@` | `@SHA_QUEST@` | Quest library for packagers: a loose ELF, attested and not signed |

@IF_APK@Neither build needs a config file. The service endpoints and the public client keys are built in; the
@IF_APK@Windows DLL logs in with no `config.yaml`, and the Quest APK logs in and runs its social features
@IF_APK@with no `config.yaml` either.
@IF_NOAPK@The Windows DLL needs no config file: the service endpoints and the public client keys are built in, so
@IF_NOAPK@it logs in with no `config.yaml`.

## What to test
- Sign in (the device-code prompt on first run), reach the lobby, see your friends.
- Invite a friend and join a party; mute and ghost toggles.
- Leave and re-enter a match; voice chat (Windows: the runtime's microphone provider picks your headset's microphone first; the log's `[NEVR.MIC] capture device:` line names the one it used).
- Report anything odd with the newest log file (see below).

## Install and uninstall
- **Windows:** unzip, close the game, run `install.ps1` (PowerShell, `-ExecutionPolicy Bypass`). It runs `install-nevr-runtime.exe`
  (the core installer), which checks the signed release manifest, backs up
  the original `BugSplat64.dll` with a timestamp, renames a legacy `dbgcore.dll` aside and puts both new DLLs in; `uninstall.ps1`
  restores both. Nothing is deleted.
@IF_NOAPK@- **Dedicated server:** copy `echovr_server.exe` next to `echovr.exe` and write `%LOCALAPPDATA%\nevr\echovr\config.yaml`; the zip's
@IF_NOAPK@  `README.txt` has the command and the smallest working config.
@IF_APK@- **Quest:** uninstall the old version first; this build is signed with the new key. Then `adb install @APK@`.

## Signing
The runtime loads unsigned plugins by default, and logs a warning for each one. Start a run with `-require-signed-plugins` to load only plugins signed through the release-signer pipeline: an unsigned plugin (or one with a changed byte) is then refused and listed in the plugin manifest, and a plugin marked required that is refused stops a server.

A separate signing step may sign `BugSplat64.dll`, `nevr-runtime.dll`, `echovr_server.exe`, `install-nevr-runtime.exe`, `nevr-install.exe`, `install.ps1`, `uninstall.ps1` and `quest-diagnose.ps1` after the build and
replace the zip under the same name; these notes are the same either way. A file is signed when it carries a
signature, and `SIGNING.txt` inside the zip says how to check each one.
@IF_APK@Compare the zip's SHA-256 with `SHA256SUMS`; the zip's own `SHA256SUMS` lists the files inside it.
@IF_NOAPK@Compare the zip's SHA-256 with the table above; the zip's own `SHA256SUMS` lists the files inside it.
Windows Defender or SmartScreen may warn about a file that is not signed.
@IF_APK@The Quest APK is signed with the project's own key, which is a sideload signature, not a code-signing
@IF_APK@certificate.

## Notes
@IF_APK@- The Windows log is `%LOCALAPPDATA%\nevr\echovr\logs\nevr-*.jsonl`; the Quest log is
@IF_APK@  `/sdcard/Android/media/com.readyatdawn.r15/files/nevr-sentinel.log`, and the session before it beside it as `nevr-sentinel.session-<number>.log`.
@IF_APK@- The version is `@VERSION@`, in both artifacts.
@IF_NOAPK@- The Windows log is `%LOCALAPPDATA%\nevr\echovr\logs\nevr-*.jsonl`.
@IF_NOAPK@- The version is `@VERSION@`; `BugSplat64.dll` and `nevr-runtime.dll` each carry it with the commit in one identity string,
@IF_NOAPK@  `NEVR-BUILD @VERSION@ @COMMIT@`.
