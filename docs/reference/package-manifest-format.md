# Package manifest format (echovr.exe 34.4.631547)

How the engine's resource manager learns which resources a package set holds. Decoded from the game's own
reader and checked against two shipped manifests. nevr-runtime reads the container and the resource array (the
records of type `CGameLevelResourceWin10`, for the installed levels a game server registers) in
`src/runtime/server/installed_levels.cpp`; nothing in it writes this format. The plugin asset path (`plugin-assets.md`)
does not use it.

## Where it lives

`<root>/<version>/<platform dir>/manifests/<%016llx package-set id>`, built by `FUN_140fa1b40` / `0x140fa09f0`
(`"%s/%s/%s/manifests"` and `"%016llx"`). Package data sits beside it as
`<root>/<version>/<platform dir>/packages/<%016llx id>_<n>` (`"%016llx_%u"`), opened with `CreateFileW` by
`FUN_1400e15e0` and read by handle and offset.

Shipped examples in `echovr-serverkit/dist/_data/5932408047/rad15/win10/manifests/`:
`2b47aab238f60515` (1509 bytes) and `48037dc70b0ecab2` (1941318 bytes).

## Container

| Offset | Size | Meaning |
| --- | --- | --- |
| 0x00 | 4 | ASCII `ZSTD` |
| 0x04 | 4 | `0x10` (u32) |
| 0x08 | 8 | decompressed length (u64) |
| 0x10 | 8 | compressed length (u64) |
| 0x18 | n | one zstd frame |

Read by `DecompressAndReadFile` `0x14152b090`. `2b47aab238f60515` is 24 + 1485 = 1509 bytes and decompresses
to 3624; `48037dc70b0ecab2` decompresses to 5179928.

## Body (little endian)

The body mirrors the engine's in-memory struct, so field offsets in the file are the struct's offsets.
Readers: `FUN_140f9f0c0` (header), `FUN_140f9f310` (array bodies), `FUN_140f9f550` / `FUN_140f9f6b0` (records).

| Offset | Size | Meaning |
| --- | --- | --- |
| 0x00 | u32 | package count: package files `<id>_0 .. <id>_n-1` |
| 0x04 | u32 | block size; the engine refuses a manifest above `0x80000` |
| 0x10 | u32 | capacity in bytes of the resource array (allocation hint) |
| 0x38 | u64 | resource count |
| 0x50 | u32 | capacity in bytes of the file array |
| 0x78 | u64 | file count |
| 0x90 | u32 | capacity in bytes of the chunk array |
| 0xb8 | u64 | chunk count |
| 0xc0 | | resource array, then file array, then chunk array, back to back |

Only the three counts decide how much is read.

| Array | Record size | Fields |
| --- | --- | --- |
| resources | 0x20 | `u64 type`, `u64 name`, `u32 chunk`, `u32 a`, `u32 size`, `u32 b` |
| files | 0x28 | five `u64`; key plus three words, used on the file-server path (`0x140fa2a20`) |
| chunks | 0x10 | `u32 package`, `u32 offset`, `u32 compressedSize`, `u32 uncompressedSize` |

A resource lives at byte `a` of the uncompressed chunk `chunk`, `size` bytes long. `b` was 16 in every record
seen. Chunks are zstd blocks of at most `block size` bytes (the request initializer calls the zstd setup
`0x1415270a0`).

Evidence, manifest `2b47aab238f60515`: counts 43 / 43 / 21, one package, block size `0x80000`; `0xc0 +
43*0x20 + 43*0x28 + 21*0x10 = 3624`. Chunk 0 is `{package 0, offset 0, compressed 0x1c79, uncompressed
0x40294}` and the three resources that reference it have `a`/`size` of `0/256`, `256/262292`, `262548/256`,
which sum to `0x40294`. Chunk offsets run consecutively (`0x0`, `0x1c79`, `0x1eecbf`, ...), each the previous
offset plus the previous compressed size.

## How the engine applies it

`0x140fa09f0` (wrapper `0x140fa1680`, called by `DownloadManifestAndPackages` `0x140ccde70` and
`NRadPhysics::InitGlobals` `0x141548da0`):

1. checks the manifest file exists, decompresses and parses it;
2. only when the package count is non-zero: merges the resource records into the sorted table at resource
   manager `+0x00` (rebasing `chunk` and package index by what is already there), appends the chunks at `+0x80`,
   opens each package file into the handle table at `+0x128`;
3. merges the file records into the table at `+0x40`;
4. refuses (the game's cpackagemanifest.cpp, source lines `0xb7` / `0xdb`) when any resource it contains is already loaded.

A manifest therefore cannot add a resource without a package file the engine can open, which is why plugin
assets are served at the completion instead (`plugin-assets.md`).
