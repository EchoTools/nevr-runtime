# Echo VR community beta: install, sign in, report bugs

This beta replaces one file in your Echo VR install and adds a second, so the game connects to the
community servers. It is for **Echo VR on PC (PCVR) on Windows**. It has only been smoke tested: finding
what breaks is why you have it, so please report anything odd (see "Report a bug").

The two files are a **matched pair** and must always be installed together: `BugSplat64.dll` (the stable
loader the game reads) and `nevr.dll` (the runtime it loads). Install only one of them and the game either
won't start or will start without the community features.

## What is in the zip

| File | What to do with it |
|---|---|
| `BugSplat64.dll` | Install — it replaces the crash reporter the game loads at startup. |
| `nevr.dll` | Install — it goes next to `BugSplat64.dll`. |
| `INSTALL.md` | This page. |
| `echovr_server.exe`, `README.md` | Ignore them. They are for people running game servers or developing the runtime. |

## Install

1. Close Echo VR.
2. Find your Echo VR folder: the one that contains `bin\win10\echovr.exe`.
3. In `bin\win10`, rename the existing `BugSplat64.dll` to `BugSplat64.dll.original`. Keep it: it is how
   you uninstall.
4. Copy **both** `BugSplat64.dll` and `nevr.dll` from the zip into `bin\win10`. The two files must come
   from the same zip — do not mix one from an old download with one from a new one.
5. Start Echo VR the way you normally do.

## First start: sign in with Discord

The first time, the game needs you to sign in:

- Your browser opens the community sign-in page (`https://echovrce.com/login/device`). Sign in with
  Discord there.
- While it waits, the game window is titled **"Echo VR - sign in with Discord in your browser to
  continue"**. It waits up to 5 minutes.
- If the browser does not open, a box shows the address and a code: open the address yourself and enter
  the code.
- If the 5 minutes run out, close the game and start it again to get a new code.

After that the game logs in by itself on every start. Your sign-in is saved in a file named
`.credentials.json` in a `_local` folder of your Echo VR install. Do not share that file; delete it if you
want to sign in again (for example as a different Discord account).

## Uninstall

Close Echo VR, delete **both** `bin\win10\BugSplat64.dll` and `bin\win10\nevr.dll`, and rename
`BugSplat64.dll.original` back to `BugSplat64.dll`.

## If the two files are mismatched

`BugSplat64.dll` and `nevr.dll` must be the same version. If they are not — for example `nevr.dll` is
from an older download and `BugSplat64.dll` from a newer one — the game starts but stays on the stock
servers instead of the community ones. This is deliberate: rather than run with a mix that was never
tested together, the loader runs the original game.

The loader records exactly what happened in a start-up record at
`%LOCALAPPDATA%\EchoVR\logs\nevr-bootstrap.jsonl` (paste `%LOCALAPPDATA%\EchoVR\logs` into the Explorer
address bar to get there). The line begins `{"event":"runtime_load"` and carries:

- `outcome` — what happened. `loaded` means the pair matched. `abi_rejected` means the two files are
  different versions; `runtime_absent` means `nevr.dll` is missing; `missing_entry` / `load_failed`
  mean `nevr.dll` is damaged or from a different program.
- `expected_abi` vs `actual_abi` — the version the loader wants vs the version the `nevr.dll` beside it
  actually provides (for example `expected_abi: "1.0"` but `actual_abi: "2.0"`). When they differ, the
  two files are a mismatched pair.
- `win32_error` — the Windows error code when a file could not be loaded (for example `2` means "file
  not found", i.e. `nevr.dll` was not copied in).

If the game is not connecting to the community servers, that `runtime_load` line is the first thing to
check: if `outcome` is anything other than `loaded`, uninstall both files and install both again from the
same zip.

## Known issues

You do not need to report these:

- No in-game voice chat yet ([#15](https://github.com/EchoTools/nevr-runtime/issues/15)).
- A friend's status (in a menu, a lobby or a match) is not live: it is what it was when your friends list
  last refreshed ([nakama#663](https://github.com/EchoTools/nakama/issues/663)).
- A friend added on the website only appears after you restart the game
  ([#57](https://github.com/EchoTools/nevr-runtime/issues/57)).
- If the game loses its connection to the server (your internet drops, or the server restarts), it may
  not recover on its own: restart the game ([#70](https://github.com/EchoTools/nevr-runtime/issues/70)).

## Report a bug

Open a report with the **Beta bug report** form:
<https://github.com/EchoTools/nevr-runtime/issues/new?template=beta-bug.yml>

It asks what happened, what you did just before, and roughly when (with your time zone), and for these
files. Attach them by dragging them into the form:

| File | Where it is |
|---|---|
| The game's log for that session | `%LOCALAPPDATA%\EchoVR\logs\`: the `nevr-<date>T<time>.jsonl` file from that session (newest first if it just happened). Paste `%LOCALAPPDATA%\EchoVR\logs` into the Explorer address bar to get there. |
| The start-up log | `bin\win10\logs\nevr-boot.jsonl` in your Echo VR folder. |
| The loader start-up record | `%LOCALAPPDATA%\EchoVR\logs\nevr-bootstrap.jsonl` — the `runtime_load` line (see "If the two files are mismatched"). |
| If the game crashed | **`nevr-crash-<id>.txt`** in the same `%LOCALAPPDATA%\EchoVR\logs\` folder: the crash record (what failed, where, and what called it). Also the newest files in `_temp\crashes\` and in `bin\win10\_temp\crashes\` in your Echo VR folder (names like `RAD_CRASHDUMP_<you>_echovr_<day>_<time>.log` and `.json`). |

**What these files contain:** your Discord account id; the crash files also contain your Windows user
name and computer name. They do not contain your saved sign-in or its tokens. A
GitHub issue is public: if you would rather not post them, say so in the report and leave them out.
