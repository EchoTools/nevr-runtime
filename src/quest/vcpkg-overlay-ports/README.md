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

`openssl/` is OpenSSL 4.0.3, the only TLS library in the Quest build (curl's `ssl` feature). vcpkg (release 2026.10.16
and master) stops at 3.6.5, and OpenSSL's 3.6 series reaches end of life on 2026-11-01; 4.0 is supported to 2027-05-14
(openssl-library.org/source). The files are vcpkg 2026.10.16's 3.6.5 port with: the version and the `openssl-4.0.3` archive's SHA512;
`windows/install-layout.patch`, `windows/install-pdbs.patch` and `unix/no-empty-dirs.patch` from microsoft/vcpkg#51742
(its 4.0.0 update, closed unmerged), which apply to 4.0.3; `script-prefix.patch` dropped (4.0 has no `c_rehash`, and
`unix/portfile.cmake` no longer moves it); `enable-static-engine` and `enable-capieng` dropped (4.0 has no engines).
Only the `unix/` path (arm64-android) is built here. When vcpkg's own port reaches 4.0.3 or later, delete `openssl/`.
