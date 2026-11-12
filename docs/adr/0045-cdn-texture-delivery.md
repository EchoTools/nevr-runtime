# ADR 0045: Game textures are served from a CDN cache through the game's own resolver

Status: proposed. Applies to `src/core/resource_shadow.*`, `src/core/cargo_*`, `src/quest/cargo/`, the resolver hook in
`src/quest/sentinel/resource_request_hook.cpp` and, for the PC half, `src/runtime/patch/plugin_assets.cpp`. Supersedes
`docs/adr/0005-cosmetics-cdn-format.md` (the v1 `.evrp` tint packages, which write nothing to the game). Amends the cache path of
`docs/reference/content-packs.md` and the in-place write of `NEVR_RegisterResourceOverride` for textures. Read together with the
plugin-level ABI design, which uses the same shadow builder.

## Context

The game reads every resource through one function, `CResourceManager::FillResourceFileRequest` (echovr.exe `0x140fa1fe0`,
libr15.so `0x25439f0`): it binary-searches the package manifest for an (on-disk type, name) pair and fills a read request with a
file handle, a chunk and offsets. The base loads, the archive loader, the streamed mips and the forced full mip load all call it
(4 functions on PC, 6 call sites on Quest). The runtime hooks it on both platforms.

A texture is two entries under one name, each named by its on-disk type: buffer 0 is a descriptor, buffer 1 is the pixels
(`src/runtime/patch/resource_buffer_key.h`). On a Quest the poster `ui_mpl_poster_echo_pass` (name `0x3fa6d25bc24a7b01`) reads as:

| index | type | size | alignment | chunk | offset in chunk | chunk decompressed |
| --- | --- | --- | --- | --- | --- | --- |
| 0 | `0xe2efe7289d5985b8` | 248 | 16 | 231 | 493112 | 524120 |
| 1 | `0x489bb35d53ca50e9` | 8,388,608 | 16 | 239 | 0 | 8,388,608 |

Both reads come from one thread. The PC stock sizes differ (256 and 8,388,756). The sizes are properties of the stock game data.

The v1 CDN path (ADR 0005) fetches tint packages and writes nothing to the game. A resource-override callback exists on both
platforms, but it sees only finished primary buffers whose owner is a `CResource`, so it never reaches a streamed mip, and it
writes in place, so a payload must fit the buffer the stock read allocated and the registry holds a copy for the life of the
process.

## Decision

1. **The seam is the resolver.** A CDN buffer is served by calling the game's own function with a **shadow manager** in place of
   the live one. The shadow is a 16-byte manager and a manifest in the engine's own layout (`src/core/resource_shadow.h`), built
   from the cache's records. The engine fills the request itself and its own reader reads our file into a buffer its own allocator
   makes. No hook is added: the hook on the resolver exists on both platforms. The shadow builder is shared with the plugin-level
   package registry.
2. **Precedence: cargo, then stock, then plugin packages.** A cargo entry is a deliberate, published replacement of a stock name
   and is served before stock. A plugin package never replaces a stock name: a package that collides with stock is refused, so a
   server and its clients resolve a name to the same bytes. Each rule has its reason; neither changes the other.
3. **One process-wide chunk-index counter, first value `0x01000000`.** Shadow chunk words start above the stock manifest's 10,304
   chunks, because the engine's block cache is keyed by the chunk index alone: a cached block of one shadow would be served as
   another's. So every shadow in the process (cargo, plugin packages, a cargo republish) takes its indices from one counter that
   never gives a value out twice, and a build that finds fewer than it needs below `0x08000000` takes none and fails. Below that
   limit `index * 0x10` stays under 2^31, so the game's index multiply lands on the same entry in 32 or 64 bits, signed or unsigned.
4. **The manifest supersedes ADR 0005's.** One JSON document keyed `(platform, on-disk type, name)` (`src/core/cargo_manifest.h`), for
   any on-disk type, published as one zstd frame (`manifest.json.zst`) whose header states the content size. The client decodes it with
   the libzstd the runtime already links (not the game's `CSysCompression`, which needs the engine's allocator and function table
   before the engine runs) and holds the declared size to the device's memory before it decodes anything. A record of 0 bytes is
   listed in the manifest's `empty` list. A blob is one stock-format chunk: one zstd frame whose decompressed bytes are the buffer.
   The runtime never decompresses a blob; the SHA-256 of the stored frame proves it is the published one. The manifest is unsigned in
   this decision (see Not claimed).
5. **The host is `cargo.echo.taxi`, bucket `nevr-cargo`.** `blobs/sha256/<hex>` (immutable), `v1/manifests/<sha256>.json` (kept by
   hash), `v1/manifest.json` (the current copy, short cache). A test device names `v1/test/manifest.json` through `assets.cdn_url`;
   `v1/manifest.json` holds no label in this decision. The manifest-publish token is held by the project owner's delegate;
   whether an R2 token can be limited to a key prefix is open and not needed here.
6. **Objects are appended to a few store files and hashed once.** A blob is streamed into `store-<n>.pack` (at most 4 GiB: the engine's
   chunk offset is 32 bits) while it is hashed; its frame header is checked against the record (`cargo_validate.h`); when size, header
   and SHA-256 match, its bytes are synced and then one fixed record (SHA-256, file, offset, sizes, crc) is appended to `blobs.idx`
   and synced. A failure or a kill gives the bytes back at the next open. The manifest is saved as it was published. At boot the
   store reads the index and measures its files; nothing is read or hashed. At mount each store file is opened once, read-only,
   held for the life of the process, and serves every object in it. On Quest the directory is `<internalDataPath>/nevr/cache/`.
7. **Never block the boot.** The runtime mounts what the cache holds at boot with no network, fetches in the background on a
   client only, and mounts what arrives. A first launch may show the stock texture; a second launch shows the CDN texture from its
   first load with no request. Every request of a cargo key reads the cargo file; there is no served-once latch.
8. **Limits come from the device and the manifest, never from a constant** (`src/core/cargo_limits.h`). The manifest text, the entry
   count, one object's size, the storage a plan may add and the store files held open are each a share of the device's free memory,
   free space and free descriptors, read when the run starts; a missing fact stops the run with its name. What remains constant is
   what a format defines (`src/core/cargo_format.h`: SHA-256, hex, CRC-32, the zstd frame header, the blob index; and the engine's layout: a 4 GiB
   store file, 32-bit sizes, an alignment that is a power of two of at most 4096). Every memory-based bound on every path, the cargo
   limits and the first-run data step's manifest read, goes through one reader and one function (`src/core/device_memory.h`). A resolver
   pin is thread-local, as the game keeps its own per-thread state (FillResourceFileRequest echovr.exe `0x140fa1fe0`: slot lookup `0x140fa2155`, `TlsGetValue` through `CPool::GetOrCreate` `0x140fa2171`, allocation `0x140fa218d`), so
   no table size limits the threads that read resources. A manifest's decoded size is taken from its frame header, as the game takes a
   manifest's decompressed length from its file header (libr15 `0x2b9dd10`); the game bounds a manifest only by its allocator
   (`CSysCompression::DecompressAndReadFile` `0x2b9e0a0`), and here it is bounded by the share of the device's memory. A
   label is planned whole or skipped whole, with the reason named. A payload whose size differs from its stock buffer's still needs
   its descriptor (the per-binary pair table, `Platform::pairs`).
9. **Label states: defaults in code, config only changes them.** `lobby_posters` is on, `game` (the whole-game label) is required;
   any label not listed is off. Config (`assets.labels.<label>: on|off|required`, `assets.required_only`) changes states; the
   manifest never does. `required` never blocks a level load or a match, and a server cannot require labels of a client.
10. **Client only.** A dedicated server fetches nothing.
11. **Whole-game mode.** On Quest, `assets.whole_game: true` (needs `assets.manifest`) says the manifest holds every asset of the game, so
    the first-run data step (`docs/adr/0011-quest-game-data-first-run.md`) does not fetch the data zip and never ends the process for
    missing `_data`, in any build. The engine reads each boot package set's manifest file straight from disk, with no resolver hook in the
    path, and stops at init when one is absent; a manifest with package count 0 is accepted. So the step writes the two zero-package
    manifests (`src/quest/data/boot_stub.h`, sets `r14netclient` and `r14netclient_en_US`) where they are absent, keeps any manifest that is
    there, and then calls the launch's setup (`data_wiring::Options::all_assets_ready`: the download and the mount of the shadow manifest,
    which the first archive load needs) before the engine starts. The step runs that function when one is set and ends the launch with its
    message when it answers not-ok (status `worker_refused`). KNOWN-WRONG #2398:
    no production function is set in this tree. The sentinel (`src/quest/sentinel/entry.cpp`, `InstallPermissionWaitHook`) sets only the mode,
    so a launch with the mode on logs the Warning `data_all_assets_ready result=skipped reason=no_worker` and nothing downloads or mounts: the
    engine boots on the stubs and its first archive load finds no asset. Writing that function, from the first-run prefetch (not in this tree)
    and an adapter over the cargo store (`src/core/cargo_store.*`), and setting it in
    `entry.cpp` is the integration step (the APK package, P6). A stub that cannot be written ends the launch with a message, because the engine
    could not start either. A manifest that is already there is kept and logged (Warning when it is not the stub or cannot be read); the step
    does not check that the engine will accept it.

## Alternatives

- **Serve at the read-finished callback.** It reads the stock bytes first and writes ours over them, holds a resident copy, needs
  the payload to fit the stock buffer, and cannot replace a streamed mip. Rejected for textures; it stays for in-memory overrides.
- **Fill the request field by field.** It copies ten stores and the thread-local pool step the game's function ends with; any
  drift is ours. Calling the game's function with a shadow manager gets the engine's own result.
- **Extend ADR 0005's `.evrp` packages.** One symbol per package, a header with reserved bytes, and a payload the engine cannot
  read as it reads stock. Rejected.

## Measured

- `cargo.echo.taxi` presents a leaf from Google Trust Services (WE1) chained to GTS Root R4. `curl --cacert` with only the
  self-signed GTS Root R4 PEM from `src/quest/data/data_host_roots.h` (SHA-256 `349dfa40...3c7d`, the pin there) verified the
  connection (`ssl_verify_result` 0), so the three-root set already pinned for the data host chains to it.
- The poster's two Quest reads are the table above.

## Not claimed

- **Manifest authenticity.** The manifest is not signed. A holder of the publish token controls what a client mounts. Signing is
  security work with its own order.
- **That the engine builds a texture whose buffer 1 differs in size from stock.** The first test keeps buffer 1 at the stock size.
- **Raw disassembly of the Quest chunk-index arithmetic.** The shadow's base keeps both multiplies equal; the headset run decides.
- **The headset descriptor limit.** A mount holds one descriptor per store file, and the budget is read from the device (Decision 8); the value on a headset is not recorded here.
- **Tint packages.** The v1 tint path writes nothing; removing it is a later change.
