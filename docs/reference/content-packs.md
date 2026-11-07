# Content packs

A content pack is a mod's game data, served while the game runs, with no game file changed: manifests that replace stock ones,
the packages those add, and the script DLLs that go with them. The runtime serves an installed pack itself, on a client and on
a dedicated server (`src/runtime/content/content_pack.h`); no plugin is needed. The Quest build serves a pack installed as a
folder the same way, with the same checks and the same log lines, and no scripts ("On Quest" below, #2312). Both builds compile
one decision: `src/runtime/content/content_core.h` reads `content.json`, checks every file, decides, and writes the lines; each
build supplies only its file access and its hook.

Use the smaller tools first when they are enough. `NEVR_RegisterResourceOverride` replaces a buffer of a resource the
game already lists: the first at any size, and the second (a texture's payload, keyed `Make(<logical>, "Win10GPU")`,
`sdk/include/nevr/pc/resource_registry.h`) when it is no larger than the game's own; `NEVR_RegisterAsset` adds a
single-buffer resource the game does not ship, from the plugin's own image (`plugin-assets.md`). A pack is for a package
set's worth of data, a resource the engine reads from a package file (texture streaming and archive loads read package
files, not memory: `../engine/asset-system.md`, "Packages are files"), or a script.

## Where a pack lives

`<root>\content\<id>\`, where `<root>` is the runtime's root, `%LOCALAPPDATA%\nevr\echovr` (`nevr_root::ContentDir`,
ADR 0012), never the game install: nothing of ours is written there (the standing rule in `AGENTS.md`, "The game
folder"). The folder holds `content.json` and the files it lists. A pack that is installed is served: no `config.yaml`
key turns it on or off. To run the game stock, remove the pack's folder. One pack at a time: with two or more folders
under `content\`, none is served (a pack's manifest replaces a stock manifest whole, so two packs that change one set
would need one merged manifest, which nothing makes). On Quest the folder is `<media root>/nevr/content/<id>/` ("On Quest").

## content.json

Format 1 (`src/runtime/content/content_json.h`):

```json
{
  "format": 1,
  "id": "examplepack",
  "version": "1.0.0",
  "data_dir": "_data/5932408047/rad15/win10",
  "manifests": [
    {
      "package": "48037dc70b0ecab2",
      "stock_sha256": "<sha-256 of the stock manifest the pack was built from>",
      "stock_packages": 3,
      "file": "manifests/48037dc70b0ecab2",
      "packages": ["packages/48037dc70b0ecab2_3"]
    }
  ],
  "scripts": ["scripts/190ed14fe6c74dd3.dll"],
  "files": {
    "manifests/48037dc70b0ecab2": "<sha-256>",
    "packages/48037dc70b0ecab2_3": "<sha-256>",
    "scripts/190ed14fe6c74dd3.dll": "<sha-256>"
  }
}
```

- `id` is the folder's name: `[a-z0-9][a-z0-9._-]{0,63}` (`src/runtime/content/pack_rules.h`, the rule the Quest
  bundled pack uses too, ADR 0029).
- Paths are relative to the pack folder, `/`-separated, each part a plain name (letters, digits, `.`, `_`, `-`, not
  starting with a dot). `data_dir` is relative to the game folder. `files` lists every file the other keys name, each
  with its sha-256.
- A manifest the pack serves in place of a stock one is the whole set: the stock records and the pack's, with the
  package count raised to cover the pack's own packages, numbered on from `stock_packages`. Its layout is the stock
  layout (`../engine/asset-system.md`, "Manifest"). Such a manifest fits only the stock manifest it was built from,
  which `stock_sha256` names. An added package's number is never below `stock_packages`, so a pack never stands in for
  a stock package.
- A script is `scripts/<16 lowercase hex>.dll`: the `CScriptResource` of that name, replaced or added.

## Building a pack

`tools/cmd/content-pack-build` builds a pack from an install's own stock files: its steps read stock resources and
scripts and say what the pack replaces or adds, and it writes the folder above (`content.json` format 1, one manifest
per changed set over the stock one, one new package per changed set, and `scripts/<16 hex>.dll`). The manifest and
package code is `tools/internal/radpack`. A pack is built on the machine that has the stock files, never from game bytes
in this repository (ADR 0028). The build's step list holds the EchoCombat pack's steps as they land (#600); with no step
that changes anything it refuses. The steps today give the DRONE the assault bullet's effects and particle rows and its
actor the shared script row, in the bullet, particle and script tables of the five combat levels
(`tools/cmd/content-pack-build/steps_drone.go`), and add four weapons and three ordnances to mp_equipment's
lists: six the game defines in mp_weapon_settings but does not list, and the translocator
(`tools/cmd/content-pack-build/steps_equipment.go`). It writes the pack into a new folder beside `-out` and renames it
to `-out` when every file is in it, so a failed write leaves no `-out`, and it refuses an `-out` inside `-game-dir`, links
resolved: nothing of ours is written into a game folder.

## When a pack is refused

The runtime checks the pack once, at the game's first command-line pass, after the early plugin pass and before the
original call, inside which the game reads its manifests and opens its packages (`PreprocessCommandLineHook`,
`src/runtime/lifecycle/boot.cpp`). The pack is refused whole, with one Warning that names the reason, and the game
runs on its stock data, when:

| `reason` | |
| --- | --- |
| `several_packs` | more than one folder under `content\` |
| `loaded_too_late` | the game's resource manager already exists, so it has read its manifests |
| `no_content_json` | the folder has no readable `content.json` |
| a `content.json` token | the descriptor breaks a rule above (`bad_id`, `id_mismatch`, `bad_package`, `bad_script`, `unlisted_file`, ...; the list is in `content_json.h`) |
| `file_missing`, `file_empty`, `file_changed` | a listed file does not open, is empty, or is not its sha-256 |
| `stock_mismatch` | a stock manifest is not the one the pack was built from |
| `signed_only` | the pack carries scripts and the run was started with `-require-signed-plugins` (a script is native code the game runs; a pack without scripts is served) |
| `unsupported_game` | the pack carries scripts and the game is not echovr.exe 631547 |
| `hook_target_changed`, `hook_failed` | the game's file open is not the bytes the hook was written against, or the hook did not install |
| `scripts_unsupported` | Quest only: the pack carries scripts, and the Quest loads no script from a pack folder |
| `bundled_pack_placed` | Quest only: a manifest the pack replaces was replaced by the APK's bundled pack (ADR 0029), which kept the stock one as `<set>.stock` |

Each file that passes is opened once, hashed through that handle, and held open with read sharing only for the rest of
the process, so nothing can change, rename or delete it between the check and the game's read.

## How it is served

**Manifests and packages.** The game opens both through one function, `CFile::Open` (echovr.exe `0x1400e15e0`,
`(const char* utf8_path, uint32_t flags, HANDLE* out)`): AddPackage (`0x140fa09f0`) opens each package at `0x140fa1264`,
and `DecompressAndReadFile` (`0x14152b090`) reads each manifest through `CFileStream` (vtable `0x141d3ed28`, slot
`+0x78`, `0x1415220a0`, which calls `CFile::Open` at `0x1415222ac`). Both open with flags 1, a read. The runtime hooks
`CFile::Open` once: a read of a stock path the pack replaces opens the pack's file instead, and every other open (a
write, another file) goes to the game's function unchanged. The path is UTF-8 (the function widens it with code page
65001), so a root under any user name works.

**Scripts.** `CScriptResource`'s loader (`0x1412ebb50`) loads `<exe dir>/scripts/<16 hex>.dll` through the game's
module loader, `VA_LOAD_SCRIPT_DLL`, `0x1400EB0F0` in `sdk/include/nevr/pc/address_registry.h`, as `(path, nullptr, 0, 0)`:

`HMODULE __fastcall (const char* path, void* arg, int call_main, int reuse)`

which ends in `LoadLibraryW`. The runtime does not hook the module loader: a load rewriter on the runtime's own
LoadLibrary hooks (`src/runtime/hook/dll_load_hook.h`, `AddLoadRewriter`, registered as `content-pack`) maps the pack's
held file for each script the pack carries. In the shipped game no existence check precedes that load, so a script the
pack adds loads the same way as one it replaces.

Never hook `LoadLibraryA`, `LoadLibraryW`, `LoadLibraryExA` or `LoadLibraryExW`. The runtime owns those four
(`AddLoadFilter`), records each with HookGuard, and after each plugin's init checks the addresses it recorded. A plugin
that re-hooked one is reported through `ServerFatal`, whose line says the runtime's patches at those addresses "are no
longer applied" (`src/runtime/ext/plugin_loader.cpp`): a dedicated server stops, a client logs it as a warning.

## On Quest

The Quest build (`src/quest/sentinel/content_pack_hook.h`, `src/quest/data/content_folder.h`) runs the same decision over its
own data. What differs:

| | PC | Quest |
| --- | --- | --- |
| Where a pack lives | `<root>\content\<id>\` | `<media root>/nevr/content/<id>/`; the media root is `/sdcard/Android/media/com.readyatdawn.r15/files` (`data_root::kMediaRoot`), where the sentinel's logs are too |
| `data_dir` | `_data/5932408047/rad15/win10` | `_data/5932408047/rad15/android`, relative to the root the engine reads |
| When it is checked | the first command-line pass, before the original call | in the data step, after the bundled pack step (ADR 0029) and before the game's first manifest read (`src/quest/sentinel/data_wiring.h`) |
| The function hooked | `CFile::Open`, echovr.exe `0x1400e15e0`, detoured | `NRadEngine::CSysFile::Open(const char* path, unsigned flags, long* handle)`, libr15 `0xf85538`, through libr15's GOT slot `0x36f6450` (no code page is written, N151) |
| Who opens through it | AddPackage and `CFileStream` | AddPackage (`bl` at `0x2540f38` and `0x2540fe0`) and `CFile::Open`'s tail call (`b` at `0x11e8978`), which every manifest read takes; all three branch to the PLT stub `0xf3dad0` (`kCSysFileOpenContentSites`, `src/quest/sentinel/pinned_targets.h`) |
| A read | flags 1: bit 0 set and none of `0x2`, `0x4`, `0x8`, `0x10` (`nevr_content::IsServedRead`) | the same rule |
| The file handed to the game | the pack's file by path, held open with read sharing only | the held descriptor reopened through `/proc/self/fd/<n>`: the file that was hashed, even after a rename over its path, with an offset of its own |
| A path's spelling | any case, `/` or `\`, the `\\?\` prefix | the `com.readyatdawn.afk` media root counts as the running package's (`src/quest/sentinel/path_rewrite.h`) |
| Scripts | served | the pack is refused whole (`scripts_unsupported`) |
| Too late | the game's resource manager exists | the same, read at libr15 `0x376ed60` (written at `0x2aa89bc`) |
| Refusals of its own | `hook_target_changed` | `bundled_pack_placed`; `hook_failed` names the slot and the GOT status |

The game's function returns 0 for an open that succeeded (AddPackage tests it with `cbnz w0` at `0x2540f3c`), as on PC, so a
served open it fails is the same Warning. A Quest pack is made like a PC one; `tools/cmd/content-test-pack -platform android`
makes the test pack from a copy of the Quest's data folder. To install a pack, copy its folder to
`<media root>/nevr/content/<id>/`; to run the game stock, remove that folder.

## What the log shows

`[NEVR.CONTENT]` (`docs/standards/logging.md`):

```
[NEVR.CONTENT] no content pack installed dir=<root>\content (the game runs on its stock data)
[NEVR.CONTENT] content pack served id=<id> version=<v> manifests=<n> packages=<n> scripts=<n> files_checked=<n> dir=<root>\content (...)
[NEVR.CONTENT] content pack refused id=<id> reason=<token> detail="<what>" dir=<root>\content (nothing of the pack is served; ...)
[NEVR.CONTENT] manifest served id=<id> name=<set> game_path=<stock path> pack_file=<pack file> count=1
[NEVR.CONTENT] package served id=<id> name=<set>_<n> game_path=<stock path> pack_file=<pack file> count=1
[NEVR.CONTENT] script served id=<id> name=<hash>.dll game_path=<stock path> pack_file=<pack file> count=1
```

The first serve of each file is Info; a repeat is Debug. A served open the game's function fails is a Warning with its
result.

On Quest each line is one sentinel record, `{"event":"content","msg":"<the line>"}`, in `<media root>/nevr/logs` and logcat;
the text is the PC's, with the Quest's paths.

## Trust

A pack's script DLLs are native code the game runs. In a run started with `-require-signed-plugins` (N147) a pack that
carries scripts is refused whole; nothing signs a pack's scripts yet. Outside such a run they are checked against
`content.json`'s sha-256 and nothing else.

## What the game service sees

Nothing reports what a pack contains: `nevr_assets` in the login lists only what plugins registered with
`NEVR_RegisterAsset`.

## Not covered

- What a pack may contain. This page says how a pack is served, not where its bytes come from. ADR 0028 keeps
  game-derived packs off release pages.
- Scripts on Quest. The Quest loads no script from a pack folder; a pack that carries one is refused.
- Two packs at once.
