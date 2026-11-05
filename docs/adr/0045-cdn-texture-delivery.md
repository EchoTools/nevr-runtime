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
3. **Chunk base `0x01000000`.** Shadow chunk words start above the stock manifest's 10,304 chunks, because the engine's block
   cache is keyed by the chunk index alone, and `(base + n) * 0x10` stays below 2^31, so the game's index multiply lands on the same
   entry in 32 or 64 bits, signed or unsigned.
4. **The manifest supersedes ADR 0005's.** One JSON document keyed `(platform, on-disk type, name)` (`src/core/cargo_manifest.h`),
   a blob being one stock-format chunk: one zstd frame whose decompressed bytes are the buffer. The runtime never decompresses; the
   SHA-256 of the stored frame proves it is the published one. The manifest is unsigned in this decision (see Not claimed).
5. **The host is `cargo.echo.taxi`, bucket `nevr-cargo`.** `blobs/sha256/<hex>` (immutable), `v1/manifests/<sha256>.json` (kept by
   hash), `v1/manifest.json` (the current copy, short cache). A test device names `v1/test/manifest.json` through `assets.cdn_url`;
   `v1/manifest.json` holds no label in this decision. The manifest-publish token is held by the project owner's delegate;
   whether an R2 token can be limited to a key prefix is open and not needed here.
6. **The cache is verified, atomic and re-hashed on mount.** A blob is streamed to a temp file while hashing, compared by size and
   SHA-256, flushed and renamed to `<sha256>.blob`. The index (`{"version":2,"entries":[...]}`) is written whole, by temp and rename,
   after every blob it names. At mount each blob is opened read-only, held for the life of the process, and hashed again through the
   handle held, so the bytes the engine reads are the bytes hashed. A label's entries for one name mount together or not at all. The
   index is runtime-only. On Quest the cache is `<internalDataPath>/nevr/cache/`.
7. **Never block the boot.** The runtime mounts what the cache holds at boot with no network, fetches in the background on a
   client only, and mounts what arrives. A first launch may show the stock texture; a second launch shows the CDN texture from its
   first load with no request. Every request of a cargo key reads the cargo file; there is no served-once latch.
8. **Size rules, decided before any request.** `size` and `bytes` are at most 128 MiB. A buffer-1 entry whose `bytes` differs from its
   `stock_size` needs a buffer-0 entry for the same name in the same label, else the whole name is refused. The referenced blobs
   fit 512 MiB, else the label is skipped whole. The runtime holds no bytes in memory.
9. **Label states: defaults in code, config only changes them.** `lobby_posters` is on; any label not listed is off. Config
   (`assets.labels.<label>: on|off|required`, `assets.required_only`) changes states; the manifest never does. `required` never blocks
   a level load or a match, and a server cannot require labels of a client.
10. **Client only.** A dedicated server fetches nothing.

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
- **A cap on mounted handles on Quest.** Nothing is resident, but each blob holds a file descriptor.
- **Tint packages.** The v1 tint path writes nothing; removing it is a later change.
