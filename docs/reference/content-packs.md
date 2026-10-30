# Content packs (PC)

A content pack is a mod's game data, served while the game runs, with no game file changed: resources the game does
not ship, resources that grow, a second buffer that grows, and the script DLLs that go with them. The runtime
serves no pack itself. A plugin serves it, on the seams this page names; the runtime's part is to load that plugin in
time and to leave those seams to it.

Use the smaller tools first when they are enough. `NEVR_RegisterResourceOverride` replaces a buffer of a resource the
game already lists: the first at any size, and the second (a texture's payload, keyed `Make(<logical>, "Win10GPU")`,
`sdk/include/nevr/pc/resource_registry.h`) when it is no larger than the game's own; `NEVR_RegisterAsset` adds a
single-buffer resource the game does not ship, from the plugin's own image (`plugin-assets.md`). A pack needs this
page when it adds a package set's worth of data, a resource the engine reads from a package file (texture streaming
and archive loads read package files, not memory: `../engine/asset-system.md`, "Packages are files"), or a script.

## Load early

List the plugin with `early: true` in `config.yaml` (`example-config.yaml`, the `plugins:` keys). The game reads its
manifests and opens its package files during its first command-line pass, and the early pass loads before that call
(`src/runtime/lifecycle/boot.cpp`, `LoadPlugins(PluginPhase::Early)` before `EchoVR::PreprocessCommandLine`). A plugin
in the normal pass arrives after the game holds its stock packages open, and nothing makes the game read them again.

An early plugin runs before a client's graphics device exists: it stays off the graphics path (N146).

## Where the pack lives

Under the runtime's root, `%LOCALAPPDATA%\nevr\echovr` (`<root>` below), never in the game install: nothing of ours is
written there (the standing rule in `AGENTS.md`, "The game folder"; ADR 0012). Plugins load from `<root>\plugins\`
(`nevr_root::PluginsDir`), so a plugin finds the root as the parent of its own directory. The pack's directory under
the root (for example `<root>\content\<id>\`) is the plugin's own: the runtime neither reads nor writes it.

## Manifests and packages

AddPackage (`0x140fa09f0`) opens each package of a set, `<data>/packages/<%016llx set>_<n>`, with `CreateFileW`: once
per package it builds the path from `"%s/%s/%s/packages"` and `"%016llx_%u"` and calls `0x1400e15e0` (`0x140fa1264`;
`package-manifest-format.md`, "Where it lives"). How it reads the manifest, `<data>/manifests/<%016llx set>`, is not
traced: it checks that the file exists with `GetFileAttributesW` (through `0x1400e0cd0`), and among its direct calls,
five deep, the only path to `CreateFileW` or `CreateFileA` is that package open; it also calls through pointers, which
that search does not follow. A plugin that serves a manifest finds that read first, and answers the existence check
too.

Nothing this tree loads into the game process hooks `CreateFileW` or `CreateFileA`, so a pack plugin may answer the
package opens for reading with its own files and pass every other open through.

The engine builds a set from that one manifest (`package-manifest-format.md`, "How the engine applies it"), so a
manifest the pack serves in place of a stock one is the whole set: the stock records and the pack's, with the package
count raised to cover the pack's own packages, numbered on from the stock count. Its layout is the stock layout
(`../engine/asset-system.md`, "Manifest": the block size limit, and records sorted by type and name compared as signed
64-bit integers). Such a manifest fits only the stock manifest it was built from.

## Script DLLs

A `CScriptResource` is a DLL found by name: its deserializer (`0x1412EBB50`) builds the path from
`"/scripts/%016llx.%s"` and `"dll"` and never reads the script's package bytes (`../engine/asset-system.md`, "Script
DLLs"). It loads the DLL through the game's module loader, `VA_LOAD_SCRIPT_DLL`, `0x1400EB0F0` in
`sdk/include/nevr/pc/address_registry.h`, as `(path, nullptr, 0, 0)`:

`HMODULE __fastcall (const char* path, void* arg, int call_main, int reuse)`

With `reuse` set, it first returns the module `GetModuleHandleA(path)` finds; otherwise it widens the path and calls
`LoadLibraryW`. With `call_main` set, it then calls the module's `RadPluginMain(arg)`. It returns the module, or null.
The runtime does not hook this function, so a pack plugin that carries scripts hooks it and loads its own DLL, by its
full path, for each name the pack carries.

The engine loads other modules through the same function, with `reuse` set: `pnsrad.dll` and `pnsradmatchmaking.dll`
from its module loader (`0x14105aa70` -> `0x14105ab40`, the call at `0x14105ab7c`; `pnsrad-load-boundary.md`), and
XInput (`XInput9_1_0`) from `0x141055fa0`. The runtime patches `pnsrad.dll` and `pnsradmatchmaking.dll` when that
`LoadLibraryW` returns. So the hook serves only the script paths the pack carries, and passes every other call to the
original with all four arguments unchanged.

Never hook `LoadLibraryA`, `LoadLibraryW`, `LoadLibraryExA` or `LoadLibraryExW`. The runtime owns those four
(`src/runtime/hook/dll_load_hook.h`, `AddLoadFilter`), records each with HookGuard, and after each plugin's init
checks the addresses it recorded. A plugin that re-hooked one is reported through `ServerFatal`, whose line says the
runtime's patches at those addresses "are no longer applied" (`src/runtime/ext/plugin_loader.cpp`): a dedicated server
stops, a client logs it as a warning.

## Trust

In a run started with `-require-signed-plugins` the pack plugin is a plugin like any other and loads only when it is
signed (N147). The host checks plugin files, their imports, and every later load of a file in `<root>\plugins\`; it
does not check a file a plugin opens or loads from elsewhere. A script DLL a pack plugin loads from its pack directory
is therefore checked by that plugin or by nothing.

## What the game service sees

The login's `nevr_plugins` lists the pack plugin like any other plugin. Nothing reports what a pack contains:
`nevr_assets` lists only what plugins registered with `NEVR_RegisterAsset`.

## Not covered

- What a pack may contain. This page says how a pack is served, not where its bytes come from. For Quest, ADR 0019
  (Proposed) recommends (R8) that a plugin carry no game-derived bytes.
- Quest. Nothing here is traced on the Quest build, whose engine and scripts are separate binaries.
- A dedicated server's need for the same pack.
- Two packs at once: a pack's manifest replaces a stock manifest whole, so two packs that change one set need one
  merged manifest, which nothing makes.
