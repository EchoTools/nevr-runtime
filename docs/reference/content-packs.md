# Content packs (PC)

A content pack is a mod's game data, served while the game runs, with no game file changed: manifests that replace stock ones,
the packages those add, and the script DLLs that go with them. The runtime serves an installed pack itself, on a client and on
a dedicated server (`src/runtime/content/content_pack.h`); no plugin is needed.

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
would need one merged manifest, which nothing makes).

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
that changes anything it refuses.

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
- Quest. The Quest build places a bundled pack's files into its own data tree instead (ADR 0029); it serves no script.
- Two packs at once.
