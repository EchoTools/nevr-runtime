# Echo VR community beta: install, sign in, report bugs

This beta replaces one file in your Echo VR install and adds a second one beside it, so the game connects to the community servers. It is
for **Echo VR on PC (PCVR) on Windows**. It has only been smoke tested: finding what breaks is why you
have it, so please report anything odd (see "Report a bug").

## What is in the zip

| File | What to do with it |
|---|---|
| `BugSplat64.dll` | Install it: it replaces the game's file of the same name. |
| `nevr-runtime.dll` | Install it in `%LOCALAPPDATA%\nevr\echovr`, not beside `BugSplat64.dll`: the runtime that `BugSplat64.dll` loads. Without it the game stops at start. |
| `install.ps1`, `uninstall.ps1` | Optional: they do the install and the uninstall for you (see Install and Uninstall). |
| `README.txt`, `SIGNING.txt`, `SHA256SUMS` | Reference: the package's own notes, how to check each file's signature, and the file hashes. |
| `echovr_server.exe`, `quest-diagnose.ps1` | Ignore them unless you run a server or have a Quest problem (`README.txt` says how). |

## Install

If Echo VR Launcher manages your nEVR, update nEVR through the launcher. `install.ps1` stops and says so when the folder
is one the launcher lists, and changes nothing.

1. Close Echo VR.
2. Find your Echo VR folder: the one that contains `bin\win10\echovr.exe`.
3. Copy (do not rename) the existing `bin\win10\BugSplat64.dll` to `%LOCALAPPDATA%\nevr\echovr\installer\aside\BugSplat64.dll.original`
   (paste the folder into the File Explorer address bar; create the folders if they are not there). Keep it: it is how
   you uninstall. (`install.ps1` keeps the backup there as `BugSplat64.dll.original-<timestamp>` instead.) Nothing but
   `BugSplat64.dll` of ours belongs in the game folder.
4. Copy `BugSplat64.dll` from the zip into `bin\win10`. Copy `nevr-runtime.dll` into `%LOCALAPPDATA%\nevr\echovr`.
   If an older release left a `nevr-runtime.dll` in `bin\win10`, delete it: the game no longer loads it.
5. Start Echo VR the way you normally do.

## First start: sign in with Discord

The first time, the game needs you to sign in:

- Your browser opens the community sign-in page (`https://echovrce.com/device`). Sign in with
  Discord there.
- While it waits, the game window is titled **"Echo VR - sign in with Discord in your browser to
  continue"**. It waits up to 5 minutes.
- If the browser does not open, a box shows the address and a code: open the address yourself and enter
  the code.
- If the 5 minutes run out, close the game and start it again to get a new code.

After that the game logs in by itself on every start. Your sign-in is saved in a file named
`.credentials.json` in `%LOCALAPPDATA%\nevr\echovr` (older releases kept it in a `_local` folder of your Echo VR install; the game moves it on the first start). Do not share that file; delete it if you
want to sign in again (for example as a different Discord account).

## Uninstall

Close Echo VR, then either:

- if you installed with `install.ps1`: run `powershell -NoProfile -ExecutionPolicy Bypass -File uninstall.ps1 -Dir "<the bin\win10 folder>"`
  from the unzipped folder (add `-Root "<folder>"` if you gave `install.ps1` one). It restores your original `BugSplat64.dll`
  and sets `nevr-runtime.dll` aside in `%LOCALAPPDATA%\nevr\echovr\installer\aside`;
- if you installed by hand: delete `%LOCALAPPDATA%\nevr\echovr\nevr-runtime.dll`, and copy
  `%LOCALAPPDATA%\nevr\echovr\installer\aside\BugSplat64.dll.original` over `bin\win10\BugSplat64.dll`.

## Known issues

You do not need to report these:

- A friend's status (in a menu, a lobby or a match) is not live: it is what it was when your friends list
  last refreshed ([nakama#663](https://github.com/EchoTools/nakama/issues/663)).

## Report a bug

Open a report with the **Beta bug report** form:
<https://github.com/EchoTools/nevr-runtime/issues/new?template=beta-bug.yml>

It asks what happened, what you did just before, and roughly when (with your time zone), and for these
files. Attach them by dragging them into the form:

| File | Where it is |
|---|---|
| The game's log for that session (**always attach it**) | `%LOCALAPPDATA%\nevr\echovr\logs\`: the `nevr-<date>T<time>.jsonl` file from that session (newest first if it just happened). Paste `%LOCALAPPDATA%\nevr\echovr\logs` into the Explorer address bar to get there. |
| The start-up log | `bin\win10\logs\nevr-boot.jsonl` in your Echo VR folder. |
| If the game crashed | **`nevr-crash-<id>.txt`** in the same `%LOCALAPPDATA%\nevr\echovr\logs\` folder: the crash record (what failed, where, and what called it). |

**The line we read first.** The game's log holds a `render_config` line from the first rendered frame of the session, and another each time
the picture setup changes: your graphics card and driver, the headset and its refresh rate, the render resolution and scale, and the game's
graphics settings with what set them. A report about stutter, a low frame rate or a blurry picture is read against that line, so attach the log
even when the problem does not look like a crash. On a Quest headset the same line is in `nevr-sentinel.log`, in the game's folder on the
headset (`/sdcard/Android/media/com.readyatdawn.r15/files/`; the Android/data files folder on a headset where that one is not writable). The log of the previous session is kept beside it as `nevr-sentinel.session-<number>.log`.

**What these files contain:** your Discord account id, and file paths, which can include your Windows user
name. They do not contain your saved sign-in or its tokens. A
GitHub issue is public: if you would rather not post them, say so in the report and leave them out.
