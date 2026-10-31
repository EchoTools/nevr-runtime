# The engine's asset system

How echovr.exe names, finds, requests, reads and builds an asset, what triggers each step, and the order in
which things become ready. A level is an asset like any other; the level path is described as one consumer of
the general path.

Scope and evidence:

- **Binary.** echovr.exe, sha256 `b6d08277e5846900c81004b64b298df6acba834b69700a640b758bda94a52043` (the copy
  in the disassembly project `echovr` and the copy under `echovr/bin/win10/`). Every `0x14…` address below is a VA in
  that file.
- **Names.** Class and method names come from libr15.so, the Quest build of the same engine, whose Ghidra
  output carries demangled symbols (`NRadEngine::CResourceManager::AddPackage(NRadEngine::CSymbol64, char
  const*)` at libr15.so `0x2543238`). Each echovr.exe function is matched to its libr15.so twin by a shared
  string, a shared constant, or structure; the match basis is given where it is not a shared string. libr15.so
  thunk names are skewed by `0x100000` (`thunk_FUN_026439f0` is `FillResourceFileRequest` at `0x25439f0`), so
  its caller and callee lists are not used as evidence.
- **Coverage.** The disassembly project `echovr` holds 1042 binaries: 572 PE (echovr.exe, pnsrad.dll, pnsovr.dll,
  pnsdemo.dll, pnsradmatchmaking.dll and 567 script DLLs) and 470 ELF (libr15.so, libpnsrad.so,
  libpnsradmatchmaking.so, libpnsovr.so, test-foo.so and 465 script `.so` files). All 1042 were searched
  for the strings `resource`, `manifest`, `package`, `level`, `archive` and their own module name.
  The asset system lives in echovr.exe (PC) and libr15.so (Quest).
  - The four pns DLLs carry the literals `manifests` and `packages` (pnsrad.dll `0x180227868`, pnsovr.dll
    `0x180200188`, pnsdemo.dll `0x1801da938`, pnsradmatchmaking.dll `0x1801cb620`) with no reference to them.
  - The three Quest pns libraries link their own copy of the engine's streaming IO processor
    (libpnsrad.so `Initialize` `0x58d0a0`, `"Streaming IO"`, `"Failed reading package at offset %u with size
    %u"` in `0x58f590`) and `CFileTypes` symbols, reached only through data references. None of them
    contains the resource manager, package manifest or archive loader (no `"Cannot find resource"`,
    `cpackagemanifest`, `carchiveloader` strings in libpnsrad.so).
  - No script DLL or script `.so` contains `resource`, `manifest` or `package`. The disassembly records no imports
    for the PE script DLLs, and each script `.so` imports four or five C runtime functions. Script DLLs
    are themselves assets (see "Script DLLs").
- **Data.** Format claims were checked against the shipped files in
  `echovr/_data/5932408047/rad15/win10/{manifests,packages}`.

## Names

Every name in the engine is a `CSymbol64`: a 64-bit CRC-style hash of a lower-cased string, implemented by
`CSymbol64::Lookup` (`0x1400ce120`) and reproduced in `sdk/include/nevr/symbol_hash.h`. Hashing continues from a seed,
so `Make(seed, "b")` (`CSymbol64::Make`, `0x1400ce0c0`) equals the hash of the concatenated string.

An asset is identified by a pair **(type, name)**:

| Half | What it is | How it is computed |
| --- | --- | --- |
| type (logical) | the C++ class that builds the asset | `CSymbol64("<ClassName>")`, e.g. `CGameLevelResource` = `0xfd7c74d55c15302b`, `CArchiveResource` = `0x403b7b8867a17f09` |
| type (on disk) | the class plus a platform variant | `Make(CSymbol64("<ClassName>"), "Win10")` for variant 0 and `"Win10GPU"` for variant 1 (`CResourceID_BuildPath`, `0x140fa19a0`: platform string `PTR_s_Win10_142026270`, suffix table `PTR_DAT_142026220`) |
| name | the asset's author name | `CSymbol64("<name>")`, e.g. `mpl_arena_a` = `0x576ed3f8428ebc4b` |

The manifests on disk are keyed by the on-disk type, not the logical one. Labelling the 235 distinct type keys
in the two shipped manifests with the 2044 class names in echovr.exe's RTTI accounts for 233 of them
(`CGTextureResource` = `0x4a4c32c49300b8a0` / GPU `0xbeac1969cb7b8861`, `CScriptResource` = `0x991421b41de58370`,
`CArchiveResource` = `0x2a41cf1c1d9e5d32`, `CGameLevelResource` = `0xe8e38d7781a338a6`, …). Of the remaining two,
`0xae49fad43254367a` (17226 records) is `Make(CSymbol64("RawTexturePackfile"), "Win10")`: a texture's separately
streamed mips, named `<texture>_mip<N>` and read through `FillResourceFileRequest` by the texture streaming
(`0x1405bc7d0`, from `0x1405bcb32`, with index N) and from `0x14054d293` (index 0, no resource), as a client run
logged them (#1421). `0x06eace9f64204fd0` (567 records) is unlabelled.

A texture's two variants are its 256-byte descriptor (variant 0) and its pixel payload, a DDS (variant 1). nEVR's
`resource_override` names the variant an override replaces by its on-disk type (`src/runtime/patch/resource_buffer_key.h`):
it writes a variant-1 replacement over the buffer the read allocated, when it is no larger.

Package sets are named the same way. The game client's sets are `CSymbol64("r14netclient")` =
`0x48037dc70b0ecab2` and `CSymbol64("r14netclient_en_US")` = `0x2b47aab238f60515`, which are the two manifest
files that ship. The name is chosen in `0x1400ff4b0` (CR15Game vslot 5): `0x1400ffa4a` loads `r14netclient`,
`0x1400ffa51` loads `r14netserver` (`CSymbol64` = `0x48036dce101dc1b4`, which does not ship), and a `CMOVNZ`
on bit 2 of `*(pGame+0x8a70)` picks one. nEVR's `NETSERVER_LOGGING` patch (`src/runtime/patch/mode_patches.cpp`)
replaces that `CMOVNZ` at `0x1400ffa58` with `MOV RBX, RAX`, so an nEVR game server also loads
`r14netclient`.

The game holds no reverse table from hash to string at run time: `CResourceID_GetName` (`0x1400d0a40`) prints
`0x%016llX` unless a debug callback is installed, and nothing in echovr.exe installs one. nEVR's
`src/runtime/log/symcache_data.cpp` is what turns hashes in logs back into names.

## Where assets live

```
<dataroot>/_data/5932408047/rad15/win10/manifests/<%016llx package set>
<dataroot>/_data/5932408047/rad15/win10/packages/<%016llx package set>_<n>
```

The root is built by `CSettingsStore::Initialize` (`0x141521240`): `+0xc80` is `<dataroot>/_data/5932408047`,
`+0x1c88` is the project (`rad15`), and `+0x1080` is `<root>/_local`. `<root>` is the project root
`0x1400e2110` returns: the exe's directory with trailing `win10`, `win7`, `tools`, `win32` or `wvk` components
removed and one more parent taken (`0x1400df410`; on failure it logs `"Unable to find the project root dir from
exe dir '%s'"` and uses `.`). For `echovr/bin/win10/echovr.exe` that is `echovr/`, so `_local` is
`echovr/_local`, where the game also writes `r14logs`. The working directory plays no part. `CPackageManifest::AddPackage`
(`0x140fa09f0`) formats `"%s/%s/%s/manifests"` + `"%016llx"` and `"%s/%s/%s/packages"` + `"%016llx_%u"`.

### Manifest

A manifest file is a 24-byte header (`"ZSTD"`, u32 `0x10`, u64 decompressed length, u64 compressed length)
and one zstd frame. Decompressed, it is the in-memory `CPackageManifest` tables back to back:

| Offset | Field |
| --- | --- |
| `0x00` | u32 package count: files `<id>_0 … <id>_(n-1)` |
| `0x04` | u32 block size; above `0x80000` AddPackage refuses (`"Loading package data %s using larger block size (%u) than what we were initialized with %u"`, cpackagemanifest.cpp line `0x99`) |
| `0x38` | u64 resource count |
| `0x78` | u64 file count |
| `0xb8` | u64 chunk count |
| `0xc0` | resources (`0x20` each), then files (`0x28` each), then chunks (`0x10` each) |

- **Resource record** (`0x20`): u64 on-disk type, u64 name, u32 chunk, u32 offset in the decompressed chunk,
  u32 size, u32 alignment. Sorted by (type, name) compared as **signed** 64-bit integers (`FindEntry`
  `0x140f9eee0` uses `CMOVL`; the shipped manifest has 0 inversions as signed and 181 as unsigned); count at
  manifest `+0x30`.
- **Chunk record** (`0x10`): u32 package index, u32 file offset, u32 compressed size, u32 decompressed size;
  one zstd frame each, laid end to end in the package files. A chunk is either a block of small assets packed
  up to the block size, or one asset larger than the block size on its own (3734 such chunks, the largest
  78,294,084 bytes decompressed). Each package's chunk list ends with a zero-size record whose offset is the
  file size.
- **File record** (`0x28`): used by `LookupResourcePath` (`0x140fa2a20`) for loose Wwise files under
  `shared/%02hhx/%02hhx/%016llx%016llx`, only when the resource manager is not a package build
  (`*(mgr+4) == 0`, read by `0x1407287a0`). The shipped game has no `shared/` directory.

Measured on `48037dc70b0ecab2`: 3 package files, block size `0x80000`, 69651 resource records, 69651 file
records, 10304 chunk records including 3 end sentinels (`2b47aab238f60515` has 43 resources in one package and
21 chunk records); `mpl_arena_a`'s archive is
resource record `{chunk 6135, offset 388632, size 34816, align 16}`, and chunk 6135 is
`{package 1, offset 772180413, compressed 128285, decompressed 435016}`.

### Packages are files

AddPackage opens every `packages/<id>_<n>` with `CreateFileW` (`0x1400e15e0`) and keeps the handles in the
manifest's table at `+0x128`. The streaming IO thread reads chunks with `ReadFile` on that handle at an
`OVERLAPPED` offset (`0x1400e1ac0`). On the package path nothing maps a package into memory or accepts a
caller's buffer as the source (request `+0x458` is an optional destination, `+0x460` an allocator): a package
the engine reads is a file on disk. The same IO processor also serves reads that bypass the manifest (no chunk:
Wwise files through `0x140fa5420`, save files through `0x1406363e0`), which no asset load uses.

## Startup: what must exist before the first load

All of this runs synchronously inside `CEngine::Init` (`0x14154a950`), before the first level:

1. `InitGlobals` (`0x141548da0`, the disassembly name `NRadPhysics::InitGlobals`):
   - creates the factory manager `DAT_1420a2ad8` and the settings store `DAT_14209fe18`;
   - constructs the resource manager `DAT_1420a2ab8` (`0x140fa04b0`) and initializes it (`0x140fa2490`:
     16000-entry loaded table, a `0x168`-byte `CPackageManifest` at `+8`, platform `"win10"` at manifest `+0x160`;
     libr15.so `CResourceManager::Initialize` `0x2542fc0` stores `"android"` in the same field);
   - **adds the two package sets**: `CResourceManager::AddPackage` (`0x140fa1680`) at `0x14154973c` with
     `CSymbol64(name)`, then at `0x1415497a7` with `Make(Make(id, "_"), locale)`;
   - creates the streaming IO processor `DAT_1420a2ac0` and its `"Streaming IO"` thread.
2. Installs the resource types. Each `CResourceInstaller<T>` / `CFactory<T>` registers a factory under
   (`CSymbol64("CResource")` = `0xa71a14664bd4e047`, `CSymbol64("<T>")`) in the factory manager
   (`FindFactory` `0x1401f3a00`, `InsertFactory` `0x1401f3ee0`) and registers its two on-disk variants with
   `RegisterResource` (`0x140fa2db0`). Engine types first (`FUN_141563580`, `CFactory<CScriptResource>`
   `0x1410430d0`, `CFactory<CArchiveResource>` `0x1401fe460`, …), then the game's types through CEngine vslot 0
   (`CncaGame::vslot[8]` `0x1404f5bf0` → `CResourceInstaller<CGameLevelResource>` `0x14051a800`, …).
3. `"Loading global archives"` → `LoadArchive` (`0x1401fea70`) of the global archive (`engine_assets` in the
   observed run: `Loading archive engine_assets` precedes `Finished initializing engine`).
4. `"Loading game archives"` → `LoadArchiveAsync` (`0x1401feb90`) of the engine config's archive at `+0xc0`,
   only when it is not -1 and `+0xc8` is non-zero; that call logs nothing, and which archive it names is not
   established. The observed `Loading archive r14gamedata` comes after `Finished initializing engine`, from
   `0x1401fe8b0` (caller `0x140109520`).
5. Renderer, `CScriptSystem::InitializeForGame` (`0x1405f2650`), `"Finished initializing engine"`.

The boot manifests are merged before any type is installed, and the installers `CEngine::Init` calls run
before the first archive loads. About 250 `CResourceInstaller<…CR>` constructors exist; the callers of most
of them were not traced, so this does not establish that every type is installed by then.

## Requesting an asset

| Entry | VA | Behaviour |
| --- | --- | --- |
| `CResourceLoader::LoadResource(CResource*&, pair, init)` | `0x140fa2820` | Shared lock; if the pair is in the loaded table, add a reference and succeed only if its state is 4 (`"Requested resource %s:%s failed to load (state = %d)"`, cresourceloader.cpp `0x41`). Otherwise `SyncResourceLoad`. |
| `CResourceManager::SyncResourceLoad` | `0x140fa36f0` | As async, plus a sync flag (`res+0x6c |= 2`); pumps IO completions and waits for state 4, 1 or 3. |
| `CResourceManager::AsyncResourceLoad` | `0x140fa1890` | Exclusive lock on `mgr+0x50`; binary search of the loaded table (`mgr+0x10`, `0x18`-byte entries `{type, name, CResource*}`, count `mgr+0x40`); on a hit add a reference (`res+0x64`) and return without waiting; on a miss create the resource, start the read, insert it. |

The loaded table holds the logical type; the manifest holds the on-disk type. The two meet in
`ResourceLoadInternal` (`0x140fa3220`), which asks the resource (vtable `+0x48`) for its on-disk type for each
variant and starts one read per variant present (`res+0x68` is the pending mask; variant 1 only when
`DAT_1420a8a68 != 0`).

## Finding it

`CResourceManager::FillResourceFileRequest` (`0x140fa1fe0`, libr15.so `0x25439f0`) looks the
(on-disk type, name) pair up in the manifest (`FindEntry`), then the chunk, then the package file handle, and
fills a 0x4d0-byte streaming request whose debug name is `"0x%016llx.0x%016llx"`. If the pair is not in the
manifest it fails:

```
FUN_14151e280("…\\cresourcemanager.cpp",0x410,0xe8cc523d0fc9e5fe,
              "Cannot find resource %s:%s when EnableFileServerAccess_ is disabled",…)
```

There is no fallback in this function in either binary: no loose file, no file server, no download. **An asset
the engine can load is an asset some added manifest lists.** On Quest, the game's function is not the whole
story: our `FillResourceFileRequest` hook (`src/quest/sentinel/resource_request_hook.cpp`, `ServeLoose`) and the
reference build's cave (`q15t-upstream/tools/qcp_patches.py`, P9) serve a loose file from the platform data
directory when the manifest misses. The function itself still has no fallback.

## Reading it

`ReadFileAsync` (`0x1415261a0`, libr15.so `CStreamingIOProcessor::ReadFileAsync` `0x2a96564`) queues the
request for the `"Streaming IO"` thread (worker `0x141524cc0`). That thread:

- for an asset of at most `0x80000` bytes, uses an 8-slot cache of decompressed blocks keyed by chunk index
  (`0x1415255a0`): a cached chunk is copied from, otherwise the chunk is read and decompressed into a slot;
- for a larger asset, reads its chunk and decompresses it straight into the destination, bypassing the cache
  (`0x141525310`, mode 3);
- decompresses with zstd on a separate decompression thread (created by `0x141529e70`); a failed first read logs
  `"Failed reading package at offset %u with size %u"`, a failure later in the read sets status 7 silently;
- allocates the destination buffer with the allocator the request names (`0x141524910`; `ResourceLoadInternal`
  passes the global memory context `*(DAT_142098f38+0x10)` for variant 0 and `+0x18` for variant 1) unless the
  request supplied a destination;
- calls `AsyncResourceIOCallback` (`0x140fa16d0`) **on the IO thread**.

## Building it

1. **Creation, before the read.** `CreatePresenceInternal` (`0x140fa1c50`) asks the factory manager for
   (`0xa71a14664bd4e047`, logical type) (`CFactoryManager_CreateFromPair`, `0x1401f3900`). No factory →
   `"Presence factory not found for resource type: '%s'"` (cresourcemanager.cpp `0x1c6`). Otherwise the new
   `CResource` gets its name (vtable `+0x58`), refcount 1 and state 7.
2. **Bytes arrive.** `AsyncResourceIOCallback` stores the buffer at `res+0x40 + 8*variant` and its size at
   `res+0x50 + 8*variant`, clears the variant's pending bit, and when none remain sets state 2 (or 1 if any
   read failed) and schedules `InitializeResource` on the task scheduler (`"CResourceManager::AsyncResourceIOCallback"`),
   or runs it inline (`"Performing asset initialization on the IO thread."`) when there is no scheduler.
3. **The type builds itself.** `InitializeResource` (`0x140fa2510`) takes a global spinlock (`DAT_1420dc500`),
   wraps each buffer in a stream and calls the type's own deserializer at vtable `+0x38`, then the hook at
   `+0x40`. Failure: `"Failed to serialize resource (type: '%s', name: '%s')"`, `"Unable to create resource
   for %s asset '%s'"`, state 3. Success: the variant-1 buffer is freed, the variant-0 buffer is kept, state 4.
   `CGameLevelResource`'s deserializer (`0x14051b290`) attaches its `0x578`-byte struct directly onto the
   buffer, so the built resource points into the bytes the IO thread allocated; it is not copied.
4. **Release.** `Unload` (`0x140fa3000`) drops a reference; at zero it removes the table entry, calls vtable
   `+0x30`, frees the buffers (`0x140fa2210`) and destroys the object.

`CResource` layout (ctor `0x1402a03d0`): `+0x28` logical type, `+0x38` name, `+0x40/+0x48` buffers,
`+0x50/+0x58` sizes, `+0x60` state, `+0x64` refcount, `+0x68` pending-variant mask, `+0x6c` flags.

| State (`res+0x60`) | Meaning | Set by |
| --- | --- | --- |
| 7 | created | `CreatePresenceInternal` |
| 0 | reads issued | `ResourceLoadInternal` |
| 5 | a read failed, others pending | `AsyncResourceIOCallback` |
| 1 | load failed | `AsyncResourceIOCallback` |
| 2 | bytes present, initialization scheduled | `AsyncResourceIOCallback` |
| 3 | initialization failed | `InitializeResource` |
| 4 | ready | `InitializeResource` |

Nothing on this path loads a second asset; dependencies are the archive's job.

## Archives: how one asset brings the rest

A `CArchiveResource` lists other assets. Its body:

```
u32 D, D × u64 dependent archive names,
u32 N, N × {u64 on-disk type, u64 name},
u32 M, M × {u64 on-disk type, u32, u32}      (type/version table)
u32 0
```

All 298 shipped archives parse to exactly their size this way. `mpl_arena_a`'s archive has no dependent
archives and lists 2076 assets (758 textures, 197 materials,
139 physics, 68 script DLLs, …; 85 of them share the level's own name). `r14_glb_global_mp` depends on six
archives, including `r14gamedata` and `engine_assets`.

`CArchiveLoader` (source file carchiveloader.cpp, a name in the binary's strings):

1. `LoadArchiveAsync` (`0x1401feb90`, libr15.so `0x133fee8`) checks the cache (`0x1401fcb30`) and the loaded
   archives (`0x1401fe350`), else builds the archive's two on-disk ids and calls `StartAsyncLoad`
   (`0x1401fe750`), which uses `FillResourceFileRequest` and `ReadFileAsync` directly: the archive itself
   is read like any asset but is not entered in the loaded table.
2. `AsyncCallback` (`0x1401ff480`) creates the `CArchiveResource`; its deserializer (`0x1401fd850` →
   `ProcessLoadComplete` `0x1401fd430`) parses the lists above (`"unknown resource type %s in archive %s"`).
   `ProcessLoadComplete` also starts the load of each dependent archive.
3. The `"ArchiveLoader"` thread (`0x1401fcd80`, started by the loader's constructor `0x1401fc020` from
   `InitGlobals`) takes each parsed archive and walks its asset list. For each entry it maps the on-disk
   type back to the logical type through the type registry `RegisterResource` filled (`0x141521010` on
   `DAT_14209fe18`), then calls `AsyncResourceLoad({logical type, name})`, at most 400 in flight. An
   unregistered type logs `"No resource type registered for asset type %s"` (warning) and is skipped
   unless it is on the ignore list at `DAT_14201f0b0`.
   If `AsyncResourceLoad` returns an error (for example the asset is in no manifest), the thread passes it to
   `OutputError`, counts it on the archive (`+0x68`) and stops walking that archive.
4. The poll (`0x1401fd1b0`) recurses into dependent archives and waits until every listed asset reaches
   state 4 (`"Resource %s:%s failed load in archive %s"` on state 1 or 3, carchiveloader.cpp `0x188`), then
   runs the per-type completion hooks (`DAT_1420a2ac8`). An archive with a counted error fails:
   `"Error occured while streaming archive %s"` (carchiveloader.cpp `0x3d5`), and the poll returns the error.
5. `"Loading archive %s"` is logged by the blocking `LoadArchive` (`0x1401fea70`) and by `0x1401fe8b0`.

## The level

A level is the archive named after it, plus the `CGameLevelResource` of the same name inside it. All 32
shipped `CGameLevelResource` names have a same-named archive, and the same 32 names appear under
`CGameLevelInfoResource`, `CActorDataResource`, `CBVHResource`, `CComponentSpaceResource`,
`CMaterialTypesBVHResource` and `CScriptCR`.

Which level a session uses is not an asset: `CR15NetLobby` (ctor `0x140147ca0`) reads
`sourcedb/rad15/json/r14/config/matchsettings_config.json` (`gametypes[n]|levels`, `level_selection`) with
`CJson_LoadFromPath`, and `LobbySessionStartingCB` (`0x1401ad040`) takes the level from the lobby owner's
JSON `"level"` key or, failing that, from that table.

Load sequence on the game client:

1. `CR15NetGame::Update` (`0x1401bbdb0`) in state 6 sees the owner JSON `"ingame"` and calls
   `LoadMap` (`0x14017a050`): `"[NETGAME] Loading map '%s'"`, state 7 (`"loading level"`).
2. `CncaGame::CreateGameSpace` (`0x1404eac60`) starts a `CGameSpaceInitFiber` with archive loading enabled
   (option bit `0x400`).
3. `LoadLevel` (`0x1404fe050`, libr15.so `CncaGameSpace::LoadLevel` `0x17e5484`):
   `"[LEVELLOAD] Loading level '%s'"`; `LoadLevelName` (vslot 75; `<level>_lowspec` when `+0x7ae0` bit 31);
   `LoadArchiveAsync(<level>)` and yield until the archive and everything it lists is ready (an archive error
   returns from `LoadLevel`); `CResource_TryLoad` (`0x140fa27f0`) on a new `CGameLevelInstance`, whose
   `Load` (`0x14051bcf0`) calls `LoadResource({CGameLevelResource, <level>})` (a table hit by now), then the
   component space and the `CRadSourceResource` children; `CLevel::LoadHeartbeat` (`0x14062c940`) until the
   scene, materials and models are built; component systems, when the level asks for them
   (`*(param_1[0xe]+0x28) & 1`); `"[LEVELLOAD] Finished loading level '%s' in %llu ms"`.
4. The fiber's completion moves the net game to 8 (`"ready for game"`), or 9 on a headless server; a failure
   gives `"Failed to load level"` and state `0xffffffa5` (`0x1401833d0`).

Observed in a real run (`nevr-2026-10-10T12-34-11.852.jsonl`, nEVR's filtered log):
`Loading archive engine_assets`, `Finished initializing engine`, `Loading archive r14gamedata`,
`[LEVELLOAD] Loading level 'mnu_master'`,
`'r14_glb_global_root'` (97 ms), `'r14_glb_global_mp'` (4018 ms), `'mpl_lobby_b2'` (12135 ms).

## Adding a package set after boot

The engine has one path that adds a package set once the game is running: the `R15NetPackageDownload`
component (`CR15NetPackageDownloadCS`, ctor `0x140c95a70`, type `0x70e1ff2f17b22c9c`).

- **Who carries it.** Of the shipped levels, only `mpl_lobby_b2` and `mpl_tutorial_lobby` list a
  `CR15NetPackageDownloadCR`. `mpl_lobby_b2`'s has two records, `mpl_tutorial_lobby`'s one; every record's
  key (record `+0x20`) is `0x80603893591c60ff`. The string behind that hash is not known.
- **When.** `LoadLevel` → `CreateComponentSystems` (`0x1404ea030`) → `0x1404f05e0` → CR15Game vslot 64
  (`0x14011cb40`) → `0x140d2daf0` registers the per-system handlers, including `0x140d01c20` for this
  system at phase 5; the dispatcher (`0x1404f92e0`) calls them in phase order, and a non-zero return
  aborts the level load. So the handler runs inside the carrying level's own load, after that level's
  archive and assets are loaded, and blocks until the package set is added.
- **Trigger.** For each component instance, `0x140cf1c90` looks the key up in the JSON document at
  `NetGame+0x63260` (`0x1405fe1b0`: the key is `CSymbol64` of a node's path, object members joined as
  `"%s|%s"` and array elements as `"%s[%llu]"`, leading `|` stripped (`0x1405f7b20`); a string value is
  hashed to the package-set id, an integer value is the id). No value, or bit 2 of `**(NetGame+0x2da0)`
  set, means nothing happens (state 3). Instances are processed in order, so the first instance's lookup,
  fetch and add finish before the second instance logs `"More than one R15NetPackageDownload component
  found."` (warning); the same id is never fetched twice in one pass. The handler returns non-zero (and so
  aborts the level load) only if the HTTP connection object cannot be created; a failed fetch or add only
  sets the state.
- **The document.** `NetGame+0x63260` is the lobby-settings document, constructed in `ConfigureEngine`
  (`0x1401467cf`) and destroyed in `LogOut` (`0x14014db80`). The only code that assigns it, `0x1401c2a20`,
  logs `"[NETGAME] Lobby settings received: %s"` and move-assigns the parsed JSON. It is fed by the handler
  for `NSLobbySettingsResponse` (`0x14017de60`: parses and stores, nothing more) or, on the other branch of
  `0x140157fb0`, the TCP broadcaster handler for `SNSLobbySettingsResponse` (`0x5b71b22a4483bda5`,
  `0x14017dee0`). The TCP payload is a little-endian u64 decompressed length followed by a zlib stream
  (`Decompress` `0x14152a910`, zlib 1.2.11); the handler stores the document and re-broadcasts it through
  `0x1401a93e0`. `0x1401a93e0` serialises the document and sends it as `NSLobbySettingsResponse` (an 8-byte
  zero item, then the JSON text) to the peer it is given (`0xFFFFFFFFFFFFFFFD`, all peers, in the TCP handler).
  Its other caller, `0x1401aa750`, handles `NSLobbySendClientLobbySettings` and passes the requesting peer.
  `CNSLobby::AcceptPlayersSuccessCBHost` (`0x140603e20`) raises that request for each peer it accepts, so a
  client accepted after the document is stored receives it without anyone re-sending. `src/abi/symbols.h`
  names `0x5b71b22a4483bda5` `Tcp::LobbySettingsResponse`. The nEVR dedicated server takes the local handler
  set (`PatchEnableServer`), reads the game service's frames through its own ServerDB socket and relays this
  one itself (`src/runtime/server/lobby_settings_relay.h`). The game service's login settings
  (`SNSLoginSettings`) are a different document (`*(NetGame+0x64770)+0x3a0`), which this lookup does not
  consult.
- **Bit 2** of `*(pGame+0x8a70)`: no instruction in the shipped binary was found to set it. nEVR's
  `PatchEnableServer` (`src/runtime/patch/mode_patches.cpp`) writes `OR QWORD ptr [rax], 0x6` at
  `0x1401580c3`, inside `0x140157fb0`, so it sets the bit only when that function runs; whether it has run
  on an nEVR game server before a carrying level loads is not measured (`src/runtime/hook/addresses.h`
  places the site in `FUN_140116720`, which is wrong).
- **Fetch.** `DownloadManifestAndPackages` (`0x140ccde70`) uses
  `<root>\_local\rad15\win10\manifests\<id>` and `…\packages\<id>_<n>` (`<root>` as in "Where assets
  live": `echovr\_local` for this install). For each file `DownloadFile` (`0x140ccd890`) first checks
  whether it exists (`GetFileAttributesW`, `0x1400e0cd0`); **if it does, it is used as-is, with no request
  and no log line.** Otherwise it fetches `https://s3-us-west-2.amazonaws.com` + `/rad15-dlc/win10/{manifests|packages}/<id>[_<n>]`
  with up to five attempts. The log lines name the path only:
  `"[PACKAGE] Level '%s' downloading '/rad15-dlc/win10/manifests/<016llx>' (attempt %llu of %llu)"`, then
  `"writing"`, `"complete"` or `"failed"`; a non-OK status logs `"Received HTTP status code %u"` (warning).
- **Add.** It then reads the manifest for the package count (`ReadManifestFile`, `0x140fa2c30`) and calls
  `CResourceManager::AddPackage(DAT_1420a2ab8, id, <root>\_local)`. State `+0x140`: 1 added, 2 failed.
- **Merge rules** (`0x140fa09f0`). The chunk records are appended first (package index rebased by the
  package count). Then the resource records are merged one at a time: a new (type, name) is inserted, an
  existing one is overwritten in place unless that asset is already loaded (`0x140fa3410`: present in the
  loaded table with a live object). At the first loaded one the add stops with `"While adding manifest
  package '%s': '%s.%s' has already been loaded and cannot apply the package manifest…"`
  (cpackagemanifest.cpp `0xb7` / `0xdb`) and returns an error **without rolling back**: records merged
  before it stay merged, and the package files are opened and the package count raised only after the
  loop, so those records point at packages with no open handle. A failed `CreateFileW` on a package leaves
  the same state.
- **No lock.** AddPackage rewrites the manifest tables (resources, chunks, handles) without taking the
  manager's lock, while the ArchiveLoader thread and texture streaming (`0x1405bc7d0`) read them through
  `FillResourceFileRequest`.
- **Every load of the carrying level adds again.** The de-duplication lives for one handler pass, and
  AddPackage does not check for an id it already added, so each later load of `mpl_lobby_b2` re-runs the
  fetch (files present, no request) and AddPackage: chunks appended again, files opened again, and the
  partial-merge failure above if any asset the set lists is loaded at that moment.

The `-package <name>` command-line flag is parsed (`0x140503ab0`) and copied into the engine config
(`0x1404e9220`), but nothing reads it; boot adds only the two fixed sets.

## Script DLLs

`CScriptResource` is an asset type whose payload is a native DLL. Its deserializer (`0x1412ebb50`, vslot 7)
formats `"/scripts/%016llx.%s"` from its own name and `"dll"`, loads it with `LoadLibraryW` (`0x1400eb0f0`) and
calls its `setup_bindings` export (`"Failed to load script DLL %s."`, `"Failed to find script bindings
setup"`). Each of the 567 `CScriptResource` names in the manifest has a
`echovr/bin/win10/scripts/<name>.dll`; the directory also holds one DLL no manifest lists
(`d85ac74f375a81b8.dll`). An archive lists the scripts its level needs (68 for `mpl_arena_a`). The streaming
path reads the script's package bytes, but this deserializer never touches them (`res+0x40`/`+0x50` are not
read in `0x1412ebb50`); the script is its DLL on disk, found by name.

## Order of readiness

1. Settings store, factory manager, resource manager (`InitGlobals`).
2. The two boot package sets merged; package files opened.
3. Streaming IO thread and decompression thread.
4. Every resource type's factory and on-disk variants registered.
5. Global and game archives loaded (and with them every asset they list).
6. Per level: archive (and dependent archives) → every listed asset ready → `CGameLevelResource` →
   component space and sources → `CLevel` scene, models, materials → component systems → net game state 8/9.
7. During the load of a level that carries `R15NetPackageDownload` (after its own assets, before its
   component systems initialize): an extra package set may be merged, again on every load of that level.
   It stays merged for every level loaded afterwards in the same process; it cannot change any asset already
   loaded.

Within one asset: created (7) → reads issued (0) → bytes present (2) → ready (4), with the type's
deserializer running between 2 and 4 on a scheduler thread under a global lock.

## Logs

Engine lines go through `CLog::PrintfImpl` (`0x1400ebe70`; levels 1 debug, 2 info, 4 warning, 8 error;
every asset call site uses category 0). nEVR hooks it (`src/runtime/log/builtin_filter.cpp`) and writes
`%LOCALAPPDATA%\nevr\echovr\logs\nevr-<ts>.jsonl` with an `[EVR]` prefix and names resolved through the symcache;
its default suppressions drop `"Loading global archives"`, `"Loading game archives"`, `"Loading archive 0x"`
and `"[LEVELLOAD] Allocator used:"`, and it drops a `Loading archive 0x…` line even if the engine prints one,
so its absence from the JSONL is not evidence. Whether the engine's `Assert` (`0x14151e280`) writes its text
to the log itself, or only through an `OutputError` further up, is not established.

## Open

- The string behind key `0x80603893591c60ff`. About 10^6 candidates (every string in echovr.exe and the
  game service source, bare, as `a|b` pairs and with common suffixes) did not match.
- Which of the two lobby-settings handlers a game client registers at run time (`0x140157fb0` chooses by
  bits 1, 2 and 6 of the game flags), and when the document arrives at a client relative to the lobby
  level's load.
- What `Assert` (`0x14151e280`) and `OutputError` (`0x14151ee70`) write to the log, and so the exact text
  of a missing-asset or refused-add failure.
- Whether the game service sends `SNSLobbySettingsResponse` to a dedicated server at all: the server-side
  relay exists, and no sender is in this repository.
- Whether `0x140157fb0` (and so `PatchEnableServer`'s bit 2) runs on an nEVR game server before a carrying
  level loads.
- Whether keys of the form `name[i]` were among the brute-forced candidates for `0x80603893591c60ff`.
- Whether `DownloadFile` decompresses what it writes.
- The unlabelled on-disk type key `0x06eace9f64204fd0`.
- The name source for most per-asset names (textures, meshes): neither the binary nor the symcache has them.

## Prediction: an asset a plugin carries, on the engine's normal path

The model above says the engine loads only what an added manifest lists, reads it from package files on
disk, and adds a package set at two places: at boot (two fixed names) and in the `R15NetPackageDownload`
handler during a lobby level's load (any id, from `<root>\_local`). The prediction below is for the second,
since it is the only one that takes a set the game does not ship. It is a statement of what the model
implies, to be checked by a run; where a run disagrees, the model is what gets fixed.

**Preconditions.** A package set `P` (`echovr\_local\rad15\win10\manifests\<P>` and
`packages\<P>_0 … _<n-1>`) in the shipped formats: block size at most `0x80000`, small assets packed into
chunks of at most the block size, larger assets one per chunk, an end sentinel per package, records keyed by
on-disk type and sorted as signed 64-bit integers. No asset in `P` may already be loaded when it is added.
For a level `L`: a `CArchiveResource` named `L` in the shipped archive layout, listing every asset the level
needs, including `CGameLevelResource` `L` and `CComponentSpaceResource` `L`, each of them in `P` or a shipped
set, and no type outside the installed factories; every `CScriptResource` it lists has
`echovr\bin\win10\scripts\<name>.dll`. The game client's lobby-settings document has a node at the path
whose hash is `0x80603893591c60ff`, whose value is `P` (number) or a string hashing to `P`. nEVR's
`resource_override` hook on `AsyncResourceIOCallback` (`0x140fa16d0`) has no override registered for an asset
of `P`.

**Game client, in order** (lines as they appear in nEVR's JSONL, `[EVR]` prefix; names nEVR's symcache does
not know print as `0x%016llX`, as `'0xCB9977F7FC2B4526'` does in `nevr-2026-10-10T12-34-11.852.jsonl`):

1. Boot is unchanged: `Loading archive engine_assets`, `Loading archive r14gamedata`, `[LEVELLOAD] Loading
   level 'mnu_master'` …. Nothing reads `_local\rad15` at boot.
2. `[NETGAME] Lobby settings received: {…}` with the document, before step 4.
3. `[LEVELLOAD] Loading level 'mpl_lobby_b2'`. Its own assets load from the shipped set exactly as today,
   then `[NETID] … received ids` and `[BALANCE] Level 'mpl_lobby_b2' received initial settings`.
4. The handler's first instance finds `P` in the document, finds every file present and makes no request:
   **no `[PACKAGE]` line**. It reads the manifest and calls `AddPackage(echovr\_local)`; success writes no
   line. Then the second instance logs `More than one R15NetPackageDownload component found.` (warn), as in
   every run today, and finds the same id already done. Then `[NETVAR] Level 'mpl_lobby_b2' waiting for
   initial vars` and `[LEVELLOAD] Finished loading level 'mpl_lobby_b2' in … ms`.
5. The session moves to `L`: `[NETLOBBY] Starting session (gametype …, level …)` on the lobby owner,
   `[NETGAME] NetGame switching state (from …, to loading level)`, `[NETGAME] Loading map '…'`,
   `[LEVELLOAD] Loading level '0x<L>'`.
6. No `Loading archive` line for `L`: `LoadLevel` uses `LoadArchiveAsync`, which prints nothing (only the
   blocking loader and `0x1401fe8b0` print that text, and nEVR's filter would drop a `0x…` one anyway).
7. The `"ArchiveLoader"` thread requests every listed asset; each is read from `echovr\_local\…\packages\<P>_<i>`
   (or a shipped package) by the `"Streaming IO"` thread, deserialized, and reaches state 4. No line per
   asset.
8. `[LEVELLOAD] Finished loading level '0x<L>' in … ms`, then `[NETGAME] NetGame switching state (from
   loading level, to ready for game)`.
9. The package files stay open (AddPackage holds their handles), and the set stays merged for the rest of
   the process. Each later load of `mpl_lobby_b2` adds `P` again, silently while none of `P`'s assets is
   loaded; if one is (for example `L` is still loaded), that add stops partway as in the table below.

**Copies.** On this path the plugin's bytes exist once as files under `_local`. For a small asset the engine
reads its chunk into the block cache, decompresses it there and copies the asset into a buffer it allocates;
a large asset is decompressed straight into that buffer. No pointer into the plugin's image reaches the
engine.

**Deviations the model predicts, and their signatures:**

| Condition | What the log shows |
| --- | --- |
| Lobby settings arrive after `mpl_lobby_b2`'s component systems are created, or lack the key | no `[PACKAGE]` line, no add; at `L`, the failure rows below |
| `echovr\_local\…\manifests\<P>` absent | `[PACKAGE] Level 'mpl_lobby_b2' downloading '/rad15-dlc/win10/manifests/<p>' (attempt 1 of 5)` before the `More than one …` warning, then `Received HTTP status code …` (warning) or retry lines, then `Failed to download manifest …` (warning) |
| `P` lists an asset that is already loaded | the add stops at that record; records merged before it stay merged and point at packages that were never opened, so later loads of those assets fail their read; the refusal text appears only if `Assert`/`OutputError` log it |
| An asset `L`'s archive lists is in no added set | `FillResourceFileRequest` fails (`Cannot find resource %s:%s when EnableFileServerAccess_ is disabled`, if logged; on Quest our hook and the reference build first serve a loose file from the platform data directory, so this error follows only when that file is missing or cannot be opened); the archive thread stops walking; `Error occured while streaming archive %s`; `LoadLevel` returns the error; `Failed to load level` and state `load failed` |
| A `CScriptResource` `L` lists has no DLL in `scripts\` | `Failed to load script DLL %s.`; that asset reaches state 3 and the archive fails |
| An asset type has no factory | `No resource type registered for asset type %s` (warning) from the archive thread |
| A game server with bit 2 set (by `PatchEnableServer`, if `0x140157fb0` has run) loads `L` | the handler adds nothing and the warning is not logged; the game server fails `L` as in the "in no added set" row unless `P` was added there some other way |

**Already true today, measured.** In the 979 nEVR client logs (`nevr-*.jsonl`) under
`echovr/.wineprefix/drive_c/users/<user>/AppData/Local/EchoVR/logs/` (2026-06-17 to 2026-10-10), 233 runs
start loading `mpl_lobby_b2`; the 228 that finish it all log `More than one R15NetPackageDownload component
found.` between `[BALANCE] … received initial settings` and `[NETVAR] … waiting for initial vars` (e.g.
`nevr-2026-10-10T12-34-11.852.jsonl:397`). Five end during that load: `nevr-2026-08-04T20-16-01.304.jsonl`
and `nevr-2026-08-04T20-41-11.438.jsonl` before the warning, `nevr-2026-10-01T19-27-52.980.jsonl`,
`nevr-2026-10-01T19-31-09.718.jsonl` and `nevr-2026-10-01T19-33-04.824.jsonl` after it. None of the 979 contains `Lobby settings
received` or a `[PACKAGE]` line: today's game clients never receive a lobby-settings document, so the
handler always finds no value and adds nothing. The first precondition above is therefore unmet by the
current game service and game server.
