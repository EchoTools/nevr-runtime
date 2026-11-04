# The game's NDK (r21b)

The game's Quest binaries were built with Android NDK r21b. Two kinds of build in this repository are compared with them,
so those builds use the compiler that built them, and no other:

- the combat script libraries (`src/quest/combat_libs`, `just build-quest-combat-libs`), checked against the store APK's own
  script libraries;
- function reconstructions, which are proven equal to the original only when they compile to the same instructions.

Everything else under `src/quest` (the sentinel, the loader, the plugins) builds with the r26d NDK that `ANDROID_NDK_HOME`
names (`AGENTS.md`, "Quest APK"). r26d is not replaced.

## The pin

`tools/game_ndk.pin` is the one place the toolchain is named: the release (`r21b`), the build (`6352462`), the archive URL,
the archive's sha256 and the exact first line of `clang --version`. `just toolchain-r21b`
(`tools/install_game_ndk.py`) installs it:

1. downloads the archive once (3 attempts, 60 s per read) into `downloads/`;
2. refuses it unless its sha256 is the pin's;
3. extracts it into a temporary directory;
4. refuses it unless the first line of its `clang --version` equals the pin's `CLANG_VERSION`;
5. renames it into place as `ndk-r21b`.

A repeat run re-verifies the archive (when it is still there) and the clang line, and changes nothing. Each step is one JSON
line on standard output (standard error for an error), also appended to `ndk-install.log`. Where it installs: a directory
the repository's tools own, outside every checkout, so all worktrees share one copy:
`$XDG_CACHE_HOME/nevr-runtime/toolchain/` (else `~/.cache/nevr-runtime/toolchain/`). `--root DIR` overrides it and
`--print-dir` prints the NDK directory when it is installed.

`src/quest/combat_libs/CMakeLists.txt` reads the same pin: a configure whose compiler's first `--version` line is not the
pin's stops with both lines in the message. `tools/tests/test_install_game_ndk.py` runs the installer offline against a
fixture archive.

## Evidence for each pinned value

Every value was measured on the files named here. The reference binaries are the unmodified store APK's.

**Release r21b, build 6352462.** The store APK's `libr15.so` (sha256
`8dd9a961b9dca8566069a4f65b3ddee9c65682c4e9c91a6d41e3c5727b1d8b20`) and its script libraries carry the NDK's ident note:

```
$ readelf -p .note.android.ident lib/arm64-v8a/libr15.so
String dump of section '.note.android.ident':
  [     c]  Android
  [    18]  r21b
  [    58]  6352462
```

All 465 `lib<hash16>.so` script libraries in the same APK (`lib/arm64-v8a/`) read `r21b` and `6352462` in that note and have
the clang line below in `.comment`: 465 checked, 0 differ. The installed NDK's own `source.properties` reads
`Pkg.Revision = 21.1.6352462` (21.1 is r21b).

**The clang line.** `readelf -p .comment` on `libr15.so` and on a script library (`lib01a705a692929f30.so`):

```
Android (6317467 based on r365631c1) clang version 9.0.8 (https://android.googlesource.com/toolchain/llvm-project e0caee08e5f09b374a27a676d04978c81fcb1928) (based on LLVM 9.0.8svn)
```

`libr15.so` carries more compilers in `.comment` (clang 14.0.7, 7.0.2 and 8.0.0, a 5.0 clang, a Snapdragon LLVM, GCC 4.9.x):
the libraries linked into it came from several toolchains. The first entry is the one a script library carries alone, and the
one the pin names. The installed `clang --version` prints it as its first line, which is what the install gate compares.

**The URL and the sha256.** `curl -sSI` on the URL answers `HTTP/2 200` with `content-length: 1162377080`. The installer's
download verified to the pinned digest, `0c7af5dd23c5d2564915194e71b1053578438ac992958904703161c7672cbed7`
(`sha256_ok`, `bytes` `1162377080` in `ndk-install.log`), the same digest the pin was first taken from on an earlier download
of the same URL. The digest is trust-on-first-use: the pin does not rest on a checksum the NDK's publisher states for this
archive, and the clang gate is the second check.

## Flags

The combat script libraries build with the NDK's own `build/cmake/android.toolchain.cmake`. That file adds flags the driver
alone does not: `-g -DANDROID -fdata-sections -ffunction-sections -funwind-tables -fstack-protector-strong
-no-canonical-prefixes -D_FORTIFY_SOURCE=2 -Wformat -Werror=format-security` when compiling, and `-Wl,--build-id
-Wl,--fatal-warnings -Wl,--exclude-libs,... -latomic -lm` (and `-Wl,--no-undefined`, which `ANDROID_ALLOW_UNDEFINED_SYMBOLS`
turns off) when linking. A flag stays only when the store APK's libraries show it. A flag they show absent is removed or
overridden in `src/quest/combat_libs/CMakeLists.txt`. A flag they cannot settle is not established and keeps the toolchain
default. The libraries we build are script libraries, so the 465 script libraries of the store APK are the reference; the
store's `libr15.so` (a different binary, built with other flags in places) is listed where it differs.

Counts, measured with `readelf -S/-d/--dyn-syms/-n/-p` and the pinned NDK's `llvm-objdump -d` on the extracted libraries
(465 script libraries; `libr15.so`; our 116 libraries built with the toolchain file's defaults, as a control that each
measure sees the flag when it is on):

| Measure | Store script libraries (465) | Store `libr15.so` | Ours with the toolchain defaults (116) | Ours as built now (116) |
| --- | --- | --- | --- | --- |
| a `.debug_*` section | 0 (`.symtab` in 465) | 0 (no `.symtab`) | 116 | 0 |
| `NT_GNU_BUILD_ID` note | 0 | 1 | 116 | 0 |
| imports `__stack_chk_fail` | 465 | 1 | 113 | 113 |
| TLS stack-guard loads (`mrs ..., TPIDR_EL0`) | 23582 | 53 | 3860 | 3832 |
| imports a `__*_chk` function other than `__stack_chk_fail` | 0 | 1 (`__FD_CLR_chk`, `__FD_ISSET_chk`, `__FD_SET_chk` only) | 2 (`__memcpy_chk`) | 0 |
| frame-pointer setups (`add`/`mov x29, sp`) of function prologues (`stp x.., x30, [sp`) | 53 of 22588 | 6421 of 62287 | 10109 of 10638 | 3878 of 10396 |
| `NEEDED` | `libc.so`, `libdl.so` in all 465; `libm.so` in 0 | `libm.so`, `libc++_shared.so` and others | `libc.so`, `libdl.so`, `libm.so` in all 116 | `libc.so`, `libdl.so` in all 116 (`libr15.so` in 88) |
| `.eh_frame` / `.gcc_except_table` | 465 / 0 | 1 / 1 | 116 / 0 | 116 / 0 |
| imports a libm-only symbol / a `libdl.so` symbol | 0 / 0 | 1 / 1 | 0 / 88 | 0 / 88 |

Ablations on our 116 libraries, one flag changed alone from the build as it is now ("differs" is the number of libraries whose
sequence of instruction mnemonics differs; the total is 6182355 instructions), and the decision for each flag:

| Flag | What the store libraries show | Effect on our 116 of putting the toolchain default back | Decision |
| --- | --- | --- | --- |
| `-g` | no `.debug_*` section in 465 or in `libr15.so` | `-g`: 40 differ (123 more instructions) | Overridden: `-g0`, and `-Wl,--strip-debug` (the static libgcc unwinder that 88 of our libraries link carries its own `.debug_*`). That the store's libraries were compiled with `-g` and stripped afterwards is not excluded. |
| `-D_FORTIFY_SOURCE=2` | 0 of 465 import a `__*_chk` function; the same flag on our rows gives 2 of 116 | `_FORTIFY_SOURCE=2`: 116 differ, 1641402 fewer instructions | Overridden: `-U_FORTIFY_SOURCE`. The two libraries that import `__memcpy_chk` when fortified have no namesake in the store, so the absence is shown by the count, not by a pair. |
| frame pointer | set up in 0.2% of prologues (`libr15.so`: 10%); the toolchain default 95% | the default: 116 differ, 13499 more instructions | Added: `-fomit-frame-pointer`. 3878 setups remain, in code the flag does not reach: the donor objects and the unwinder of `libgcc.a`. |
| `-Wl,--build-id` | no build-id note in 465 (`libr15.so` has one) | build-id on: 46 differ (layout; 2 more instructions) | Overridden: `-Wl,--build-id=none`. |
| `-lm` | `NEEDED libm.so` in 0 of 465; none imports a libm-only symbol | the C++ driver: `NEEDED libm.so` in 116; 17 differ | Dropped: the C++ driver adds `-lm` to every link, so the rows are linked with the C driver (`LINKER_LANGUAGE C`) and CMake's implicit C++ libraries are cleared. |
| `-fstack-protector-strong` | all 465 import `__stack_chk_fail`, 23582 guard loads; `libr15.so` imports it once with 53 loads | removing it: 113 differ, 37761 fewer instructions | Kept. `libr15.so` is built without a stack protector; the script libraries are not. Strong against basic or all is not established. |
| `-funwind-tables` | `.eh_frame` in 465, 0.091 of `.text` bytes; ours 0.028 with it and 0.022 without, so the ratio does not settle it (the donor code carries no CFI) | removing it: 81 differ, same instruction count | Not established; kept. |
| `-fdata-sections`, `-ffunction-sections` | not visible in a linked library | removing both: 14 differ (6 fewer instructions) | Not established; kept. |
| `-DANDROID` | not visible | removing it: 0 differ | Not established; kept. |
| `-latomic` | static; no `libatomic.so` in any `NEEDED` | removing it: 0 differ | Not established; kept. |
| `-Wl,--fatal-warnings` | not visible | removing it: 0 differ | Not established; kept. |
| `-O2`, `-fPIC`, exceptions on | `-O2` and `-fPIC` are the issue's flags | `-fno-exceptions -fno-rtti`: 0 differ | Not established beyond the issue; kept. |

The control shows each measure sees its flag: with the toolchain defaults our libraries have debug sections (116), a build-id
(116), `NEEDED libm.so` (116), frame-pointer setups in 95% of prologues, and two `__memcpy_chk` imports.

Not settled by this table: whether the store's libraries were compiled with `-g` and stripped, how strong the stack protector
is, and whether any flag in the last six rows was on. 88 of our libraries import `dl_iterate_phdr` (the unwinder of
`libgcc.a` is linked in) where none of the 465 does; no flag removes that, it follows from the rows' code.
