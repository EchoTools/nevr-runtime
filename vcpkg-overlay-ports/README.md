# Overlay ports

Each port here is a release its upstream has published that the pinned vcpkg (`.vcpkg-commit`) does not carry yet.
The root `vcpkg-configuration.json` names this directory, so the PC build (`just _vcpkg-mingw` and the CMake presets)
takes these ports in place of vcpkg's. The Quest build reads its own overlays (`src/quest/vcpkg-overlay-ports`).

| Port | Version | vcpkg's port at `.vcpkg-commit` | Source of these files |
|---|---|---|---|
| `ixwebsocket` | 12.1.0 | 12.0.1 | microsoft/vcpkg#54444 (`ports/ixwebsocket`), unchanged |
| `mbedtls` | 3.6.7 | 3.6.5 | vcpkg 2026.10.16's 3.6.5 port with the version and the `v3.6.7` archive's SHA512 |
| `miniupnpc` | 2.3.3 | 2.3.2 | vcpkg's 2.3.2 port with the version and the `miniupnpc_2_3_3` archive's SHA512 |
| `protobuf` | 7.36.2 (protoc 36.2) | 6.33.4 | microsoft/vcpkg#54414 (`ports/protobuf`), unchanged |
| `utf8-range` | 7.36.2 | 6.33.4 | microsoft/vcpkg#54414 (`ports/utf8-range`), unchanged; protobuf requires the same version |

When `.vcpkg-commit` moves to a vcpkg whose port is at this version or later, delete that port's directory here
(and this file and `vcpkg-configuration.json` with the last one).
