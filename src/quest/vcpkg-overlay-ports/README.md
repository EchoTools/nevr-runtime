# Quest overlay ports

`yaml-cpp/` is vcpkg's own yaml-cpp 0.8.0 port (port-version 3) plus one patch,
`yaml-cpp-no-static-init.patch`. The yaml-cpp source file token.h defines `const std::string TokenNames[]` at namespace
scope, so six yaml-cpp translation units each carry a dynamic initializer and the sentinel's `.init_array` goes from
3 entries to 9. The sentinel allows exactly one initializer of its own (`tools/check_quest_static_init.sh`: a
namespace-scope object with a constructor runs after the ELF constructor and can reset what it produced). The patch
makes `TokenNames` a `constexpr const char* const` array; its only use is `operator<<(Token)`, which streams it.

The overlay is read through `src/quest/vcpkg-configuration.json`, so only the Quest build uses it. When the pinned
vcpkg moves yaml-cpp, re-copy the port files and re-run `just check-android-static-init`.

`astc-encoder/` is Arm's ASTC encoder 5.2.0 (Apache-2.0), the core library only: its own CMake installs the command-line
tools and the shared library, never the static core, so the portfile builds the static core (NEON on arm64) and installs
it and the astcenc header with a small CMake config (`unofficial-astcenc::astcenc`). The Quest friends panel (#1535) encodes
friends' pictures with it: the Quest stores its textures as ASTC 8x8, with no uncompressed format among its textures.
