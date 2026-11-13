# Plugin resource sets (plugin API v9)

A plugin carries whole levels inside its own DLL: each level's archive and every resource the archive lists. It
registers them with the runtime from its init. The runtime serves them to the game's own resolver from the plugin's
file. Nothing is written to, or loaded into, the game's packages or manifests, and the plugin stays one file.

The declarations are in `sdk/include/nevr/plugin_interface.h` (`register_resources`, `NevrResourcesDesc`,
`NevrResourcesStatus`), `sdk/include/nevr/resource_set.h` (the format) and `sdk/include/nevr/pc/plugin_resources.h`
(embedding and registering).

## How the game finds a level, and where a set comes in

Every asset read goes through `CResourceManager::FillResourceFileRequest` (echovr.exe `0x140fa1fe0`): the archive
loader, `ResourceLoadInternal`, texture mip streaming and forced mip loads all call it
(`docs/engine/asset-system.md`, "Finding it"). It searches the manifest at `*(manager + 8)` for the (on-disk type,
name) pair and fills a read request: a file handle, the chunk's offset and sizes, the resource's place in the chunk.
The streaming reader then reads that file at that offset and decompresses the chunk. A level is its archive
(`CArchiveResourceWin10`, the level's name), which lists every resource the level needs, and its
`CGameLevelResourceWin10` root (`docs/engine/asset-system.md`, "The level").

The runtime's hook on that function (`src/runtime/patch/plugin_assets.cpp`, `Hook_Request`) asks the resource sets
first. When a mounted set holds the key and the manifest of the manager the request was made to does not, it calls the
game's own function with the sets' shadow manifest (`src/core/resource_shadow.h`): a manager and manifest in the engine's own layout, whose package
handle is the plugin's file. The game fills the request itself and its own reader reads the chunk from the plugin. Every
other key goes to the game's manifest unchanged.

## The format

A resource set is one block of bytes, `"NEVRRES1"` and a 48-byte header, then three tables (`resource_set.h`):

- **records**: the on-disk type, the name, the chunk, the offset in the decompressed chunk, the size, the alignment.
  These are the keys the game's manifests use: `Make(CSymbol64("<class>"), "Win10")` for the type
  (`CArchiveResourceWin10` is `0x2a41cf1c1d9e5d32`) and `CSymbol64("<name>")` for the name.
- **chunks**: zstd frames inside the set, each holding one or more records back to back.
- **overrides**: whole replacement bytes for resources the game already has (see "Overrides").

`nevr_resource_set::Parse` enforces the conditions the game's reader needs: every table and frame lies inside the set;
a chunk's reserved word is 0; a frame starts with the zstd magic; a record lies inside its chunk (a record of 0 bytes is
allowed, as in the game's own manifest); a record of at most `0x80000` bytes sits in a chunk of at most `0x80000`
decompressed bytes (the reader's block cache); a larger record is the only record in its chunk, at its whole size;
alignments are powers of two of at most 4096 (the game's manifests use 4, 16, 2048 and 4096); and no two records, and
no two overrides, share a key. Record order is free.

The registry adds what it needs to serve a set: an archive record is at most `0x80000` bytes (the game's largest is
66,256), so reading one never decompresses more than a block-cache chunk; and a set's chunks fit the shadow's chunk
indices beside the sets already held, and end below 4 GiB in the plugin's file.

## Embedding and registering

```cpp
#include "nevr/pc/plugin_resources.h"

NEVR_RESOURCE_SET_SECTION const unsigned char levels_data[] = {
#embed "levels.nevrres"
};

NEVR_PLUGIN_API int NevrPluginInitEx(const NevrGameContext* ctx, const char* args) {
    const NevrEmbeddedResourcesResult r = NevrRegisterEmbeddedResources(ctx, levels_data, sizeof(levels_data), "my levels");
    return r.status == NEVR_RESOURCES_OK ? 0 : 1;
}
```

The bytes must be file bytes of the plugin's own image: in one read-only section's raw data, with no base relocation in
them. The section `.nevrres` that `NEVR_RESOURCE_SET_SECTION` names is initialised read-only data. The runtime checks this
(`src/runtime/patch/image_file_range.h`) and compares the set's first and last MiB (the whole set when it is
2 MiB or less) of the file with the mapped bytes. Bytes between those two MiB are not compared: the file is held
deny-write from that read on, so it cannot change afterwards, but a file that differed in the middle before the open
is not caught. `#embed` is C23 and C++26; GCC
15 and Clang 19 accept it in C++ as an extension. The MinGW assembler has no `.pushsection` or `.previous` for PE, so an
`.incbin` in a top-level asm statement cannot return to the section the compiler was in. A 1 GiB set builds in about two
minutes.

The label is 1 to 64 bytes of printable ASCII with no double quote or backslash: it is written into log lines (quoted)
and into the login's JSON.

`register_resources` returns:

| Value | Name | Meaning |
| --- | --- | --- |
| 0 | `NEVR_RESOURCES_OK` | Registered; held until the init returns. |
| 1 | `NEVR_RESOURCES_BAD_ARGUMENT` | A null pointer, a short `struct_size`, nonzero `flags`, a label outside the rule above, or a call from outside this plugin's init on its thread. |
| 2 | `NEVR_RESOURCES_NOT_IN_FILE` | The bytes are not file bytes of a read-only section of the plugin's image, the file does not hold the set's first or last MiB as mapped, or a chunk ends past 4 GiB in the file. |
| 3 | `NEVR_RESOURCES_BAD_FORMAT` | Not a resource set, or a set the runtime cannot serve: a `CScriptResource` record (a script is a DLL found by name), a texture descriptor without its payload record, a level root without its archive, an archive that is over `0x80000` bytes or does not read, a chunk that decompresses to more than 128 MiB (ADR 0045's size rule; `size_limit`), a chunk whose zstd frame header states no content size or one other than the chunk's (`frame_size`), more chunks than the shadow has indices left. Also the answer when the host failed inside the call (out of memory). |
| 4 | `NEVR_RESOURCES_COLLIDES` | A key another set of this process holds, an override of a key another set's override replaces, or a resource a plugin asset (`NEVR_RegisterAsset`) is. An asset is keyed by its logical type and a record by an on-disk type, so an asset of type T and name N is the records (T, N), (`Make(T, "Win10")`, N) and (`Make(T, "Win10GPU")`, N). The first holder keeps it, and a plugin asset that is a resource a set holds is refused in turn. |
| 5 | `NEVR_RESOURCES_TOO_LATE` | The run's sets are already mounted. |

Every refusal is also a Warning: `[NEVR.RESOURCE] resource set refused plugin=<id> label="<label>" status=<n>
reason=<reason> detail="..."`.

## Order and lifetime

- Call it from `NevrPluginInit` / `NevrPluginInitEx`, either pass. An init that returns nonzero drops what it
  registered.
- The runtime mounts every set once. After the plugin pass it installs the resolver hook; the mount then runs from the
  per-frame tick on the game's own thread once the game has asked the resolver for its first resource. The game adds
  its two boot package sets one after the other (echovr.exe `0x141548da0`, `CResourceManager::AddPackage` at
  `0x14154973c` and `0x1415497a7`), and a tick can run between them, but it starts the streaming reader that serves
  requests only after both (`CStreamingIOProcessor::Initialize`, `0x141549902`). A mount still waiting a minute after
  the plugin pass writes one Warning that says why.
- Until the mount the game's manifests answer every request: a resource the game reads before it is not read from a
  set, and a set's overrides apply from the mount on (a resource read before it keeps the game's bytes). A client's
  login or a server's registration made before the mount carries none of the sets (no `nevr_resources`, and only the
  game's levels in `installed_levels`).
- At the mount it refuses a set that holds a key the game's manifests hold (a set adds resources, it does not replace
  them). It also refuses a set holding a level whose archive, or any archive that archive depends on through the sets,
  lists a resource or names an archive that neither the game, this set nor an earlier set serves; an archive the game
  has ends the walk there. The Warning names the archive and the key. After the mount only the mounted sets hold their
  keys: a refused set's keys are free for a plugin asset.
- A mount that throws (out of memory) is not tried again; its Error says whether the sets were published before the
  throw (`served=`). Without the resolver hook nothing is mounted, and a Warning names the plugins.
- The runtime holds the plugin's file open, read-only and deny-write, for the life of the process. The overrides it
  registers point into the plugin's image, which stays loaded: a plugin whose init fails registers none, and a plugin
  the host unloads takes the overrides in its image with it. A set is served for the rest of the process.
- A key a set holds that the game's manifest gains after the mount (a package set added later) is the game's: the
  resolver asks the manifest of the manager each request is made to, and the first such key is named in one Warning.

## Overrides

A set's overrides replace resources the game already has. They are an explicit exception to ADR 0045's decision 2
(cargo, then stock, then plugin packages): on the client that carries the plugin, an override replaces a stock name,
the same power `NEVR_RegisterResourceOverride` already gives a plugin, so it is declared per key and logged. Decision 2
still governs a set's records: a record never carries a stock name, the game's manifests come first, and so an
override cannot be a record.

Two sets cannot override one key: the second set is refused whole (`collides`, naming the key and the first set's
plugin). An earlier fixed override of the same resource wins. If one is registered before the mount, from the CDN cache or a
plugin's own `NEVR_RegisterResourceOverride`, the mount does not register the set's override. It counts the override in
`overrides_skipped=` and writes one Warning naming the key and the earlier entry's label (`resource set override
skipped`). An earlier entry competes when it is keyed by the same on-disk type, by the logical type whose buffer-0
on-disk type that is, or by type 0, which names buffer 0 of every type. Since the buffer of a type-0 entry cannot be
told from an on-disk type alone, a type-0 entry is always taken to compete (`FirstFixedOverrideLabelFor`,
`src/runtime/patch/resource_override.h`). An override registered after the mount follows the set's.

An override serves every load of its resource: a level loaded again in one process gets the set's bytes again. With
several fixed entries for one key, the first registered serves every load and the later ones are passed over
(`CachedOverrides.AFixedOverrideReplacesItsResourceOnEveryLoad`, `src/runtime/tests/test_resource_override.cpp`).

A served load leaves no buffer behind. A buffer-0 override gives the game a copy in its own heap, and the reader's own
buffer, which nothing else holds, is freed through the game's allocator once the copy is in place
(`src/runtime/patch/game_buffer.h`, `FreeReaderBuffer`); the `override applied` line says `orig_freed=yes`. A buffer-1
payload is written in place.

The runtime registers a mounted set's overrides itself, at the mount, with the
override registry (`src/runtime/patch/resource_override.h`): an override whose key the game's manifest does not hold
replaces nothing and is counted as unmatched. An override is keyed by on-disk type: buffer 0 for a `Win10` type, at any
size; buffer 1 for a `Win10GPU` type, served only when it is no larger than the game's. Overrides apply where the game
reads a resource through `AsyncResourceIOCallback` (`src/runtime/patch/resource_override.cpp`); an archive and a
streamed texture mip are read on other paths and are not replaced.

## What the game service sees

- A dedicated server lists every level of a mounted set in a registration made after the mount, in its
  `installed_levels` (`src/runtime/server/registered_levels.cpp`, read by `registration_metadata.cpp`), beside the
  levels of the game's own manifests, and counts them in `plugin_levels`. A set refused at the mount adds none.
- A game client reports the mounted sets at a login made after the mount, beside `nevr_plugins`. The key is absent when
  there are none:

```json
"nevr_resources": [ { "plugin": "levels", "label": "my levels", "levels": ["0xdb69545ff59c76d0"], "records": 90, "bytes": 1048576 } ]
```

## Logs (`[NEVR.RESOURCE]`)

`resource set registered` (Info), `resource set refused` (Warning; Error for `out_of_memory` or `exception`, a failure
inside the host), `resource sets dropped` (Info, an init that failed), `resource sets wait for the game's manifest`
(Info, once), `resource sets still not mounted ... first_request_seen=<bool> game_thread_seen=<bool>` (Warning, a
minute after the plugin pass), `resource sets not mounted ... plugins=<ids> reason=no_resolver_hook` (Warning),
`resource set refused at mount` (Warning), `resource set mounted ... levels=<n> level_names=<list> overrides=<n>
overrides_unmatched=<n> overrides_skipped=<n>`, `resource set override skipped ... key=<key> owner="<label>"` (Warning) and `resource sets mounted ... waited_ms=<n> duration_ms=<n>` (Info), `resource sets mount
failed reason=<reason> served=<bool>` (Error), `resource set reads ... served=<n> window_ms=<n>` (Info, at most once
per 10 s, for a window in which the game read from a set), `resource set read summary skipped` (Warning, once) and
`resource set keys answered by the game's manifest count=<n> first=<key> plugin=<id>` (Warning, once). No line is
written per request. A plugin id is written as printable ASCII, any other byte as `?`. The hooks' own line is
`[NEVR.ASSET] hooks installed submit=<yes|no> request=<yes|no>`, a Warning when either is missing.

## Not covered

- Quest. The resolver and its layout are the same on Quest (libr15 `0x25439f0`), but which file a Quest plugin's
  handle opens is not established.
- New script code. A `CScriptResource` is a DLL the game loads by name, which a single file cannot carry.
- MSVC: `NEVR_RESOURCE_SET_SECTION` and `#embed` need GCC or Clang.
