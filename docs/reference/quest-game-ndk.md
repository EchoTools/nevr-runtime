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

## What the toolchain file adds

The combat script libraries build with the NDK's own `build/cmake/android.toolchain.cmake`, so their compile and link
lines carry what that file adds to the driver alone: `-g -DANDROID -fdata-sections -ffunction-sections -funwind-tables
-fstack-protector-strong -no-canonical-prefixes -D_FORTIFY_SOURCE=2` when compiling, and `-Wl,--build-id
-Wl,--fatal-warnings -latomic -lm` when linking. `src/quest/combat_libs/CMakeLists.txt` turns the file's `--no-undefined` off
(`ANDROID_ALLOW_UNDEFINED_SYMBOLS`) and its STL link off (`-nostdlib++`), because the rows resolve symbols at load, from the
stock `libr15.so` or the system. Whether those defaults match the flags the game's libraries were compiled with is not
established here.
