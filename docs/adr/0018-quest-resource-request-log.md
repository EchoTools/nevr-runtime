# ADR 0018: a log of every resource request the Quest game makes

Status: accepted, implemented (`src/quest/sentinel/resource_request_hook.{h,cpp}`, #871). Installed in development builds only (`nevr_build::kIsDev`); a release build skips both of its constructor steps and registers no counters. The hook only reads: the game's call, its arguments, its result and the request it fills are the game's, byte for byte.
Not yet observed on a headset: the "Prediction" section is what the first run must show.

## Context

A resource override (#871) replaces the bytes of a named game resource with a file the player supplies. Before any request is rewritten, the keys have to be known: which (type, name) pairs the Quest asks for, on which threads, in which order, and how a streamed mip is named. `CResourceManager::FillResourceFileRequest` (libr15 ReVault `0x26439f0`, pinned vaddr `0x25439f0`) is the one function every manifest resource passes through before `ReadFileAsync`: it binary-searches the manifest by (type symbol, name symbol) and fills a `SFileRequestData` (package file handle, chunk index, chunk offsets and sizes, offset in the chunk, size, alignment). It returns 0 on a hit and an error object on a miss. Six call sites in four functions reach it, all through one PLT stub (pinned `0xf30730`, `JUMP_SLOT` pinned `0x36efa80`): `ResourceLoadInternal` (the base loads, two sites), the archive request filler (two), `CGTextureCache::RequestNewMipmaps` (each streamed mip) and `CGTextureResource::BeginForceLoadAllMips`.

The PC game has the same function (echovr.exe `0x140fa1fe0`) with the same four callers; a PC twin of this log is tracked in #969.

## Decision

One GOT hook on the `FillResourceFileRequest` slot, a `CallbackThunk` with the caller's return address (`kCaller`), original first: the handler calls the game's function, then records, then returns the game's result.

Per request it writes one `resource_request` line the first time a key `(type, name, index)` is seen and again at the 2nd, 4th, 8th ... repeat (the `seen` field), so a request made thousands of times does not fill the log. The line carries the key, the owner (`x5`), whether an allocator was passed, the return value, the call site (the return address made relative to the libr15 load base, and its name when it is one of the six: `resource_load_a`, `resource_load_b`, `archive_load_a`, `archive_load_b`, `mip_request`, `force_load_all_mips`), the thread id, and the filled request: package handle, chunk index, the chunk's file offset, compressed and decompressed sizes, the offset in the chunk, the size and the alignment. A call that returned nonzero is one `resource_request_miss` line with the key, the return value and the call site; the request is not read.

Bounds, all of which leave the game's call untouched: the key table holds 8192 keys and probes at most 64 slots for one (a key that finds no slot is counted in the unlogged total and announced once by `resource_request_table_full`); at most 16384 request lines are written (the next is replaced by one `resource_request_cap` warning).

The handler logs after the original has returned, so no logging frame is live while game code runs; `TestHookFramesCarryNoPersonality` (`tests/quest`) checks the thunk's frames in the built library.

Two counters, `resource_request_calls` and `resource_request_thunk_faults` (the reporter's table holds `kMaxReportCounters`, 128; `integration_hooks_test` sums every hook's counters against it). Everything else is a per-event line, not a counter.

The constructor sequence asks `Steps::ResourceRequestLogWanted` (`nevr_build::kIsDev`). A release build skips `register_resource_request_counters` and `install_resource_request_log` with the reason `release_build`; a development build whose counters were refused skips the install with `counters_refused` and the hook's own rule logs that it installed nothing. The install runs right after the Vulkan extension window, before token auth.

## Prediction (written before a headset run)

1. `resource_request_install` with `result=installed`; `resource_request_calls` changes within the first second of the reporter's grace window or before the main menu. No change in 60 s means the slot is off the path.
2. No `resource_request_miss` line in a healthy launch.
3. Every `resource_request` line: handle is not -1, chunk is not -1, the alignment is a power of two (0x10 is the common value).
4. Streamed mips: the first line whose key is not a base load shows `index` 7 or higher, a `site` of `mip_request`, an owner that is not the `CResource` a base load of the same texture logs. Joined offline with the symbol cache (`<texture>_mip<N>`, N 8 to 13), each such name matches one `<texture>_mip<N>`; the prediction is N = index + 1.
5. Base loads log twice for a resource that has a second buffer (index 0 and 1).
6. At least two distinct thread ids; mip requests come from a thread that makes no base load.

What would prove it wrong: no `resource_request` line before the main menu (the slot is off the path); a hit with handle or chunk -1; a nonzero return for a name the PC manifest holds; a completion the later callback hook sees with no matching request line.

## Not claimed

That a request can be rewritten at this seam: this log only reads. What `x3` carries (the body does not use it). The thread identity of any caller (the log decides). The hook on other builds of libr15: the slot and the six call sites are pinned to build id `b243509c08ce677aeb95fa348016949b3fc45230` and checked against the real library by `just test-quest-hooks-pinned`.
