# ADR 0041: native code is built by CMake, not by scripts

Status: proposed. Applies to every native binary this repo builds, on PC and Quest. States the rule that the C++ Mingw
Addendum in AGENTS.md gives in one line ("Do NOT write shell scripts for cross-compilation. Use CMakePresets + just.").
Tracked by #2048.

## Context

The repo has one build system for native code: CMake. `CMakeLists.txt` and `CMakePresets.json` build the PC host, runtime,
launcher and PC tests; the `mingw-*` presets chain `cmake/toolchain-mingw64.cmake` through vcpkg
(`VCPKG_CHAINLOAD_TOOLCHAIN_FILE`). `src/quest/CMakePresets.json` builds the Quest sentinel and plugins and chains the NDK's own
toolchain file (`android.toolchain.cmake`) the same way. `justfile` recipes such as `build`, `build-android` and `test-auth-unit` call
`cmake --preset` and `cmake --build --preset ... --target ...` and run the result.

Native code is also built another way in places. The inventory is in "Where the rule is not kept" below; its shapes are:

- **A compiler command in a recipe.** 53 lines in `justfile` run `g++ -std=c++17` with `-Wall -Wextra -Werror` and list their
  sources and include paths by hand (`test-quest-*`, `quest-patcher`, `test-scripting`).
- **A compiler command in a Python script.** `tools/build_quest_combat_libs.py` runs the NDK `clang++` once for each Quest
  script library; `tools/gen_plugin_signature_fixtures.py` and `tools/winvm/systest.py` run `x86_64-w64-mingw32-gcc`.

What this costs, measured in the tree:

- **Flags and toolchain are repeated.** All 53 `g++ -std=` lines in `justfile` spell their own `-Wall -Wextra -Werror` and their own
  `-Isrc -Isdk/include`. A preset names its toolchain once (`VCPKG_CHAINLOAD_TOOLCHAIN_FILE` in
  `src/quest/CMakePresets.json` and `CMakePresets.json`); `build-quest-combat-libs` hands its script a compiler path
  (`--ndk-clang`).
- **The compiler cache and incremental builds are bypassed.** `justfile` sets `CMAKE_C_COMPILER_LAUNCHER` and
  `CMAKE_CXX_COMPILER_LAUNCHER` to ccache, which only a CMake build reads. A recipe's `g++` line recompiles every source
  each run; `tools/verify-parallel.sh` says "each test-quest-* compiles its own host binary and runs it, so the serial wall
  clock was ~8 compiles back to back".
- **No compile database.** The root `CMakeLists.txt` sets `CMAKE_EXPORT_COMPILE_COMMANDS ON`, and
  `tools/tests/test_werror_live.py` reads `build/mingw-release/compile_commands.json`. A `g++` line in a recipe is in no
  compile database, so an IDE, clang-tidy and that test do not see its flags.
- **A script can merge without its compiler ever running.** #1853 added `tools/build_quest_combat_libs.py`; its PR body says
  "Not run: a real compile (no NDK, no libsrc)", its tests use a stand-in compiler, check script and APK, and it states an
  assumption about the include directory that "the first real run fails in the compiler" if wrong. #2047 replaces the
  script with a CMake project and pins the NDK the game was built with (r21b), so the compiler comes from that NDK's CMake
  toolchain file rather than from a path argument.

## Decision

1. **Compiling and linking native code is done by CMake targets.** Native code is C, C++ and assembly sources, and what they
   make: PC DLLs and EXEs, the Quest sentinel, the Quest plugins, the Quest script libraries, host tools the build runs
   (`quest_patch_libr15`), and test programs, including a program whose only job is a compile check.
2. **Configuration is a preset and a toolchain file.** The compiler, its version pin, the sysroot, the API level, the flags and
   the include paths a binary needs are in `CMakePresets.json` (root, `src/quest`, or beside the CMake project it configures, as `src/quest/tests/q15t` does), a toolchain file, or the target's
   `CMakeLists.txt`. A toolchain the game's binaries require (the NDK that built them) is a toolchain file the preset names;
   it is never a compiler path passed to a script.
3. **Recipes dispatch.** A `justfile` recipe configures with `cmake --preset`, builds with `cmake --build --preset` (with
   `--target` to build part), and then runs, checks or packages what was built. A recipe, script or workflow step does not
   contain a compiler or linker command (`g++`, `gcc`, `clang`, `clang++`, `x86_64-w64-mingw32-*`, `aarch64-linux-android*-clang*`,
   `ld`, `ar`, `windres`, `ndk-build`), and does not compute a list of build steps and run them.
4. **What stays a script.** Python and shell remain for:
   - checks that read build output (`nm`, `objdump`, `readelf`, the ELF and PE verifiers, `compile_commands.json` readers);
   - tests that run a built program;
   - code generators that CMake runs through `add_custom_command` or `add_custom_target`, with their outputs declared;
   - packaging, repacking, zipping and signing of built files (the APK repack, `tools/package_release.py`,
     `tools/build_distribution.py`);
   - installing dependencies (`vcpkg install`).
5. **A language with its own build tool is built by it.** The Rust installer (`installer/`) is built by Cargo and the Go
   test modules (`tests/`) by `go`. This rule is about C, C++ and assembly.
6. **A step CMake cannot express** is allowed only with a PR that names the CMake mechanism it tried
   (`add_custom_command`, an object library, a toolchain file, `try_compile`) and why it failed. That a script was quicker to
   write is not a reason.

## Where the rule is not kept

Each row is a violation of rule 3. A row goes when its issue closes; the grep
under "Finding a violation" is the list of record.

| Where | What it builds | Issue |
| --- | --- | --- |
| `tools/build_quest_combat_libs.py`, called by the `build-quest-combat-libs` recipe | the Quest combat script libraries | #2047 |
| the 18 `test-quest-*` recipes in `justfile` (`test-quest-data`, `-data-net`, `-invite`, `-shared`, `-router`, `-invite-transport`, `-tls`, `-hooks`, `-redirect`, `-hooks-pinned`, `-patcher`, `-patcher-pinned`, `-social`, `-social-pinned`, `-integration`, `-plugins`, `-autopilot`, `-integration-pinned`) | host test programs for the Quest code | #2049 |

Out of scope, by rule 4: `android-repack-libovr` and `android-repack-apk` in `justfile` (`patchelf`, `zip`, `zipalign`,
`apksigner`), `tools/quest_repack_libs.py`, `tools/quest_apk_rename_package.py`, `tools/quest_pack_from_game_data.py`,
`tools/package_release.py`, `tools/build_distribution.py` (CMake runs it for the `dist-sign` target), `tools/quest-diagnose.sh`;
the checks `tools/verify_pe_contract.py`, `tools/verify_host_no_core_state.py`, `tools/verify_symbols.py`,
`tools/check_quest_static_init.sh` and `tools/quest_link_objects.py`. Out of scope by rule 5: the `build-installer` recipe and
the Cargo step of `package-dev`.

## Finding a violation

Scope: `justfile`, `tools/` (not `tools/tests/`, which quote compilers as test data), every `*.sh`, `.github/workflows/`, `cmake/`.

- **V1.** A compiler or linker command in a recipe, a script or a workflow step. The search:

  ```
  git grep -nE '(^|[^[:alnum:]_.+-])(g\+\+|gcc|clang\+\+|clang|ndk-build|windres|x86_64-w64-mingw32-(gcc|g\+\+|ld|ar|windres)|aarch64-linux-android[0-9]*-clang(\+\+)?)($|[^[:alnum:]_+-])' -- justfile tools .github '*.sh' ':!tools/tests'
  ```

  A hit that only names the compiler (a `command -v g++` check, a comment, a package pin, a `--version` print) is read and not counted.
  A hit in the table above is a known violation with an issue; a hit not in the table is a new one.
- **V2.** `cmake -S ... -B ...` with `-DCMAKE_TOOLCHAIN_FILE=` or compiler options on the command line in a recipe, where a preset
  can hold them.
- **V3.** A script that runs `ninja`, `make` or `cmake --build` on a target list it computes.
- **V4.** A PR that adds a recipe or script with a V1 to V3 shape and does not name a rule 6 reason.

## Consequences

- A binary's compiler, flags and toolchain are in one place and a change to them is one diff.
- The compiler cache, incremental builds and `compile_commands.json` cover every native binary.
- A new Quest or PC binary is a target; the test for it is a target and a `ctest` or recipe run of it.
- The migration of the rows above is the work of their issues; until each closes, its recipe keeps its present form.

## Not claimed

- That the host test recipes are faster as CMake targets: no timing was taken.
- A `just verify` sensor for V1: the grep above is the check until the issues close, because a sensor would fail on the rows
  still open.
- That the Cargo and Go builds should change.

## References

- AGENTS.md, C++ Mingw Addendum: "Do NOT write shell scripts for cross-compilation. Use CMakePresets + just." and, among the
  things never to do, "Hardcode Windows SDK paths, mingw binaries, or vcpkg roots in CMake. Use presets and toolchain files."
- cmake-presets(7), https://cmake.org/cmake/help/latest/manual/cmake-presets.7.html: "One problem that CMake users often face is
  sharing settings with other people for common ways to configure a project. This may be done to support CI builds, or for
  users who frequently use the same build."
- cmake-toolchains(7), https://cmake.org/cmake/help/latest/manual/cmake-toolchains.7.html: "In cross-compiling scenarios, a toolchain
  file may be specified with information about compiler and utility paths." and "One may use cmake-presets(7) to specify toolchain
  files."
- Android NDK, CMake guide, https://developer.android.com/ndk/guides/cmake: "The toolchain file used for the NDK is located in the NDK at
  <NDK>/build/cmake/android.toolchain.cmake."
