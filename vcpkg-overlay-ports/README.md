# Overlay ports

Each port here is a release its upstream has published that the pinned vcpkg (`.vcpkg-commit`) does not carry yet.
The root `vcpkg-configuration.json` names this directory, so the PC build (`just _vcpkg-mingw` and the CMake presets)
takes these ports in place of vcpkg's. The Quest build reads its own overlays (`src/quest/vcpkg-overlay-ports`).

| Port | Version | vcpkg's port at `.vcpkg-commit` | Source of these files |
|---|---|---|---|
| `mbedtls` | 3.6.7 | 3.6.4 (vcpkg master: 3.6.5) | vcpkg 2026.10.16's 3.6.5 port with the version and the `v3.6.7` archive's SHA512 |

When `.vcpkg-commit` moves to a vcpkg whose port is at this version or later, delete that port's directory here
(and this file and `vcpkg-configuration.json` with the last one).
