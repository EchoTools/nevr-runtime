# Quest overlay ports

`yaml-cpp/` is vcpkg's own yaml-cpp 0.9.0 port (port-version 1, from vcpkg release 2026.10.16), unchanged. The
Quest build needs 0.9.0 or later: in 0.8.0, token.h defines `const std::string TokenNames[]` at namespace scope, so
six yaml-cpp translation units each carry a dynamic initializer and the sentinel's `.init_array` goes from 3 entries
to 9. The sentinel allows exactly one initializer of its own (`tools/check_quest_static_init.sh`: a namespace-scope
object with a constructor runs after the ELF constructor and can reset what it produced). 0.9.0 declares
`TokenNames` `constexpr const char*`, which needs no initializer.

The overlay is read through `src/quest/vcpkg-configuration.json`, so only the Quest build uses it, and it holds the
Quest build at 0.9.0 while the pinned vcpkg revision (`.vcpkg-commit`) ships an older yaml-cpp. When the pinned
vcpkg moves yaml-cpp, re-copy the port files and re-run `just check-android-static-init`.
