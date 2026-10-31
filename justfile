# NEVR Runtime Build System

# Default preset: mingw-release on Linux, release on Windows
default_preset := if os() == "linux" { "mingw-release" } else { "release" }
preset := env("PRESET", default_preset)

# The BSR commit gen/ is generated from (tools/proto_stamp.py reads this line). Bump it only with the schema:
# `buf registry commit list buf.build/echotools/nevr-api --page-size 1`. It matches nevr-stream's Go module revision (N132).
proto_pin := "259a506a3e1b490ba3482e24e92ba4db"

# Shared compiler cache: used when ccache is installed, ignored when it is not (CI and release builds are unchanged).
# NEVR_CCACHE= (set, empty) configures a fresh tree without it.
# CMake reads the launcher variables on a build tree's first configure, so an existing tree keeps the choice it was configured with.
# base_dir is the main checkout, so every worktree under it hashes the same relative include paths.
export CMAKE_C_COMPILER_LAUNCHER := env("NEVR_CCACHE", `command -v ccache 2>/dev/null || true`)
export CMAKE_CXX_COMPILER_LAUNCHER := env("NEVR_CCACHE", `command -v ccache 2>/dev/null || true`)
# One config for every build, kept in the cache directory (tools/ccache_config.sh copies tools/ccache.conf there): a path inside a
# worktree dies with the worktree, and a process that still carries it falls back to ccache's 5 GiB default (#1736).
export CCACHE_BASEDIR := `dirname "$(git rev-parse --path-format=absolute --git-common-dir)"`
export CCACHE_DIR := if path_exists("/mnt/games/cache/ccache") == "true" { "/mnt/games/cache/ccache" } else { env("HOME") / ".cache/ccache" }
export CCACHE_CONFIGPATH := `tools/ccache_config.sh "$(if [ -d /mnt/games/cache/ccache ]; then echo /mnt/games/cache/ccache; else echo "$HOME/.cache/ccache"; fi)" 2>/dev/null || echo "$PWD/tools/ccache.conf"`

# Show available recipes (default)
default:
    @just --list

# Regenerate the EVR symbol cache C++ source from the pinned Dualgame/echo-symbols
# echo_vr-pc subset (tools/symbols.pin). Runs automatically as a dependency of
# configure; an unchanged pin skips the fetch, so an offline build uses the
# committed symcache_data.cpp. The input is fetched at exactly the pinned sha.
generate-symcache:
    #!/usr/bin/env bash
    set -euo pipefail
    repo_root="{{ justfile_directory() }}"
    cd "$repo_root/tools/symcache"
    go run . regenerate \
        --pin-file "$repo_root/tools/symbols.pin" \
        --out "$repo_root/src/runtime/log/symcache_data.cpp" \
        --previous-from "$repo_root/src/runtime/log/symcache_data.cpp" \
        --if-pin-changed

# Configure CMake (fails early with the fix when a fresh worktree lacks its build inputs)
configure: _build-guard generate-symcache _vcpkg-mingw _proto-current _build-inputs
    @unset VCPKG_ROOT && cmake --preset {{ preset }} > /dev/null 2>&1 || (unset VCPKG_ROOT && cmake --preset {{ preset }})

# Regenerate gen/ when it is not the output for the pinned BSR commit (after vcpkg, whose protoc does the work)
_proto-current:
    @python3 -I tools/proto_stamp.py --root "{{ justfile_directory() }}" ensure --protoc-dir "{{ justfile_directory() }}/build/{{ preset }}/vcpkg_installed/x64-linux/tools/protobuf"

# Stop with the fix when the submodule or gen/ a root-preset build needs is missing (after vcpkg, whose protoc `just proto` uses)
_build-inputs:
    @tools/worktree-setup.sh --check

# extern/{minhook,breakpad,lss} and gen/ are copied from the main checkout; the worktree gets its own vcpkg root
# (build/vcpkg-root), so its builds never wait on another worktree's vcpkg lock; build/<preset> is seeded from the
# main checkout's (tools/seed_build_tree.py; a tree built at another commit has its configure re-run)
# Make a fresh git worktree buildable: its inputs, its vcpkg root and a seeded build tree
worktree-setup:
    tools/worktree-setup.sh
    tools/vcpkg_root.sh

# Link the tracked post-checkout hook into this clone's hooks directory, once per clone: a new linked worktree
# then gets the gitignored files .worktreeinclude lists (.mcp.json and the local Claude settings). Never overwrites a different hook.
install-hooks:
    tools/install-git-hooks.sh

# Remove the worktrees under .claude/worktrees whose work has landed. Dry run unless `--apply` (tools/reap_merged.py)
reap-merged *args:
    python3 -I tools/reap_merged.py {{ args }}

# Make the Quest release timer's own checkout (a detached worktree at origin/main under the work root), link its units from there into the user systemd and arm the timer (RELEASING.md, "The Quest half of the train"; #1210). The unit moves that checkout to the fetched origin/main before every run
install-quest-release-timer:
    #!/usr/bin/env bash
    set -euo pipefail
    CHECKOUT="$HOME/src/nevr-runtime-work/release-timer/checkout"
    test -f "$HOME/.config/metis/r2-evr-echo-taxi.env" || { echo "install-quest-release-timer: refused: ~/.config/metis/r2-evr-echo-taxi.env is missing" >&2; exit 1; }
    git -C "{{ justfile_directory() }}" fetch --quiet origin main
    if [ ! -e "$CHECKOUT/.git" ]; then
        mkdir -p "$(dirname "$CHECKOUT")"
        git -C "{{ justfile_directory() }}" worktree add --detach "$CHECKOUT" origin/main
    fi
    for unit in nevr-quest-release.service nevr-quest-release-failed.service nevr-quest-release.timer; do
        test -f "$CHECKOUT/tools/systemd/$unit" || { echo "install-quest-release-timer: refused: $CHECKOUT has no tools/systemd/$unit (origin/main does not carry the units yet)" >&2; exit 1; }
        systemctl --user link "$CHECKOUT/tools/systemd/$unit"
    done
    systemctl --user daemon-reload
    systemctl --user enable --now nevr-quest-release.timer
    systemctl --user list-timers nevr-quest-release.timer --no-pager

# Arm the release train guard as a user timer (hourly at :07) from the same dedicated checkout as the Quest timer; the units are linked from origin/main, and the guard runs there (RELEASING.md, "When to release"; #1409)
install-train-guard-timer:
    #!/usr/bin/env bash
    set -euo pipefail
    CHECKOUT="$HOME/src/nevr-runtime-work/release-timer/checkout"
    git -C "{{ justfile_directory() }}" fetch --quiet origin main
    if [ ! -e "$CHECKOUT/.git" ]; then
        mkdir -p "$(dirname "$CHECKOUT")"
        git -C "{{ justfile_directory() }}" worktree add --detach "$CHECKOUT" origin/main
    fi
    for unit in nevr-train-guard.service nevr-train-guard-failed.service nevr-train-guard.timer; do
        test -f "$CHECKOUT/tools/systemd/$unit" || { echo "install-train-guard-timer: refused: $CHECKOUT has no tools/systemd/$unit (origin/main does not carry the units yet)" >&2; exit 1; }
        systemctl --user link "$CHECKOUT/tools/systemd/$unit"
    done
    systemctl --user daemon-reload
    systemctl --user enable --now nevr-train-guard.timer
    systemctl --user list-timers nevr-train-guard.timer --no-pager

# Remove one linked worktree: every proof first, then `git worktree remove` without --force and without `git submodule deinit` (tools/remove_worktree.py; --dry-run proves and changes nothing)
remove-worktree *args:
    python3 -I tools/remove_worktree.py {{ args }}

# The fixed build checkout (tools/build_checkout.py, #1736) is built only while `git status --porcelain` is empty; every other tree is
# not checked. Builds that are cached come from that one clean directory (AGENTS.md, "Builds").
_build-guard:
    @python3 -I tools/build_checkout.py guard

# Check REV out in the fixed build checkout (creating it the first time) and print where it is; refuses when it is not clean.
# Build there with `cd $(just build-checkout-path) && just build-android`.
build-checkout rev="origin/main":
    python3 -I tools/build_checkout.py sync {{ rev }}

# The path of the fixed build checkout ($NEVR_BUILD_CHECKOUT, else <work root>/build-checkout/nevr-runtime).
build-checkout-path:
    @python3 -I tools/build_checkout.py path

# The compiler cache's statistics as the builds see it (the repo's config, not ccache's 5 GiB default that a bare `ccache -s` reads).
ccache-stats:
    ccache -s

# Build all components
build: configure
    @cmake --build --preset {{ preset }}

# Build only the echovr_server.exe launcher
launcher: configure
    cmake --build --preset {{ preset }} --target echovr_server

# Build with full compiler output
verbose-build: configure
    cmake --build --preset {{ preset }}

# Create the stripped server distribution package (nevr-runtime-v<ver>-windows-server) and its symbol files in dist/symbols/
dist: build
    @cmake --build --preset {{ preset }} --target dist

# Create distribution with full output
verbose-dist: build
    cmake --build --preset {{ preset }} --target dist

# Regenerate C++ protobuf from BSR at proto_pin (buf.build/echotools/nevr-api) and stamp gen/ (tools/proto_stamp.py).
# Uses vcpkg's host protoc to match the runtime version; the protoc must be installed (`just configure` does that).
# Generate before CMake configure: `configure` regenerates on its own when gen/ is stale, so this recipe is for forcing a refresh.
proto:
    python3 -I tools/proto_stamp.py --root "{{ justfile_directory() }}" generate --protoc-dir "{{ justfile_directory() }}/build/{{ preset }}/vcpkg_installed/x64-linux/tools/protobuf"

# Remove build and dist directories
clean:
    rm -rf build/ dist/

# --- Android / Quest (arm64-v8a) ---
# Self-contained NDK sub-project at src/quest (root CMake is MinGW-only).
# NDK: r26d by default; override with ANDROID_NDK_HOME.
# The Quest configure lines keep the ccache launcher only in the fixed, clean build checkout (tools/build_checkout.py, #1736): there
# the directory is the same on every build, so a cached object is the one a fresh compile would write. Anywhere else the launcher is
# cleared, so no worktree's directory is put into another's .so.

ndk := env("ANDROID_NDK_HOME", "/home/andrew/src/android-ndk-r26d")

# Configure the Android arm64-v8a build (NDK r26d, API 26, static libc++)
configure-android: _build-guard
    #!/usr/bin/env bash
    set -euo pipefail
    launcher=$(python3 -I tools/build_checkout.py quest-launcher "${CMAKE_CXX_COMPILER_LAUNCHER:-}")
    cd src/quest && CMAKE_C_COMPILER_LAUNCHER="$launcher" CMAKE_CXX_COMPILER_LAUNCHER="$launcher" ANDROID_NDK_HOME="{{ ndk }}" cmake --preset android-arm64

# Build the Android arm64-v8a crash-reporter .so
# Job count: CMAKE_BUILD_PARALLEL_LEVEL, else 4 (a bare -j would use every core and override the variable).
build-android: configure-android
    ANDROID_NDK_HOME="{{ ndk }}" cmake --build build/android-arm64 -j "${CMAKE_BUILD_PARALLEL_LEVEL:-4}"

# Build with full compiler output
verbose-build-android: configure-android
    ANDROID_NDK_HOME="{{ ndk }}" cmake --build build/android-arm64 -v

# Fail when the Quest sentinel gains a load-time initializer beyond its one ELF constructor
check-android-static-init: build-android
    #!/usr/bin/env bash
    set -euo pipefail
    # The object list is what the linker was given (build.ninja's link statement and the in-tree
    # archives it links), not what `find` sees: tools/quest_link_objects.py. A failure there stops
    # the recipe (the assignment is the command), so an empty list can never pass the check.
    objects=$(python3 tools/quest_link_objects.py build/android-arm64 sentinel/libovrplatformloader.so)
    tools/check_quest_static_init.sh "{{ ndk }}/toolchains/llvm/prebuilt/linux-x86_64/bin" build/android-arm64/sentinel/libovrplatformloader.so $objects

# Run the Quest .so ground-truth (ELF-shape) tests
test-android: check-android-static-init
    # The sentinel embeds exactly the public defaults file (the build reads no other source).
    python3 tools/check_embedded_defaults.py --header build/android-arm64/generated/nevr_builtin_defaults.h --binary build/android-arm64/sentinel/libovrplatformloader.so
    # CSymbol64Hash sign-extends a byte >= 0x80 under the NDK's AArch64 clang, where plain char is unsigned (#657).
    # The vectors are static_asserts, so compiling the file is the check; the NDK's static bionic binary does not
    # start under qemu-user (TLS segment alignment), so nothing is run here.
    "{{ ndk }}/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android26-clang++" -std=c++17 -Wall -Wextra -Werror -Isrc -Isdk/include -fsyntax-only src/quest/tests/symbol_hash_test.cpp
    cd tests/quest && go test -count=1 -v ./...

# Black-box crash-ingest contract gate. Requires a non-production staging sink;
# see docs/adr/0002-crash-report-ingest.md.
test-crash-ingest-contract:
    cd tests/crash-ingest && go test -v ./...

# Repack: rename the real libovrplatformloader.so -> _orig.so (fixes soname).
# Operates on a COPY under build/; never mutates the source-of-truth extract.
# On-device install (repack APK + sideload) is out of scope here — no prod deploy.
android-repack-libovr src="/mnt/games/evr/-src-evr-reconstruction/cache/quest_triage/apk_contents/lib/arm64-v8a/libovrplatformloader.so":
    #!/usr/bin/env bash
    set -euo pipefail
    out="build/android-arm64/repacked"
    mkdir -p "$out"
    cp "{{ src }}" "$out/libovrplatformloader.so"
    patchelf --set-soname libovrplatformloader_orig.so \
        --output "$out/libovrplatformloader_orig.so" "$out/libovrplatformloader.so"
    rm -f "$out/libovrplatformloader.so"
    echo "Renamed original -> $out/libovrplatformloader_orig.so (soname patched)"
    readelf -d "$out/libovrplatformloader_orig.so" | grep -i soname

# Full sideload repack: hijack an APK's libovrplatformloader.so with our shim,
# forwarding to the renamed original. Produces a zipaligned APK, signed with the Quest key in pass, and
# verifies both libs' ELF shape from the SIGNED output (no headset needed).
# It also sets the manifest's versionCode (tools/quest_apk_version.py): the store APK's 4987566 is below the
# 4987570 build the community installer puts on a headset, which Android refuses to update (a downgrade). The
# code comes from the build's own version (kVersion in <shim dir>/../generated/nevr_build_info.h), and the signed
# APK's code is read back with aapt2 before the recipe says it is ready.
# All work under build/android-arm64/repack — never mutates the source APK or shim.
# `version` (X.Y.Z) names the build's version when the shim was built elsewhere and there is no build info beside it: a release
# packages CI's loader (release-quest-apk). The versionCode comes from it, and the shim must carry the same string.
# `pack` (docs/adr/0029-quest-bundled-content-pack.md) is a game-data pack zip to carry as APK assets under assets/nevr-pack/ (STORED), which
# the sentinel places over the stock data at the first run after its data step; `pack_meta` is the file that describes it (its id, version, the stock
# manifest digests it replaces and the zip's prefix; tools/quest_repack_libs.py stage-pack --help). Both are empty for every release and package-dev APK,
# and then the recipe does nothing of it: the APK is the one the recipe made before the parameters existed.
# `apk` is required (the store APK path). The signing key is the one persistent Quest key stored in
# pass (tools/quest_keystore.sh): it is read into a private tmpfs file for the signing command only
# and is never generated here. Store it once with: just import-quest-keystore <keystore>.
android-repack-apk apk shim="build/android-arm64/sentinel/libovrplatformloader.so" workdir="build/android-arm64/repack" plugin="" version="" extra_rows="" patcher="" json_include="" plugin_early="" plugin_off="" pack="" pack_meta="":
    #!/usr/bin/env bash
    set -euo pipefail
    root="$(pwd)"
    apk="{{ apk }}"; shim="{{ shim }}"; plugin="{{ plugin }}"; version="{{ version }}"
    extra_rows="{{ extra_rows }}"; patcher="{{ patcher }}"; json_include="{{ json_include }}"
    plugin_early="{{ plugin_early }}"; plugin_off="{{ plugin_off }}"
    pack="{{ pack }}"; pack_meta="{{ pack_meta }}"
    work="$root/{{ workdir }}"
    # An APK from an earlier run never outlives a run that stops: package-dev takes whatever is at this path.
    rm -f "$work/r15-repacked.apk" "$work/r15-repacked-aligned.apk" "$work/r15_nevr-sentinel_signed.apk"
    for t in unzip patchelf zip zipalign apksigner aapt2 readelf nm keytool pass python3; do
        command -v "$t" >/dev/null || { echo "MISSING required tool: $t" >&2; exit 1; }
    done
    [ -f "$apk" ]  || { echo "APK not found: $apk" >&2; exit 1; }
    [ -f "$shim" ] || { echo "shim not found (build it: just build-android): $shim" >&2; exit 1; }
    if [ -n "$pack" ]; then
        [ -f "$pack" ] || { echo "pack not found: $pack" >&2; exit 1; }
        [ -n "$pack_meta" ] && [ -f "$pack_meta" ] || { echo "android-repack-apk: pack names a game-data pack, and pack_meta (its description file) is not a file: '$pack_meta'" >&2; exit 1; }
    elif [ -n "$pack_meta" ]; then
        echo "android-repack-apk: pack_meta describes a pack, and no pack is given" >&2; exit 1
    fi
    # libr15.so is patched on the host (docs/adr/0021 section 9): the store's file goes through quest_patch_libr15 and the
    # result takes its place in the APK. `patcher` is an already-built tool (the repack tests give a stand-in); empty builds
    # build/quest-patcher/quest_patch_libr15 with `just quest-patcher`, which needs nlohmann/json.hpp: `json_include` (the
    # directory holding nlohmann/json.hpp), else the first of the trees below that a build has installed it into. A clean
    # clone has none of them: `just build` (mingw-*), `just preset=mingw-dev _vcpkg-mingw` or the Quest build installs one.
    # `extra_rows` (docs/adr/0021 section 11) is a file of word-patch rows for a variant build: the patcher and verify-signed
    # both get it. The public recipes (package-dev, release-quest-apk) pass none.
    [ -z "$extra_rows" ] || [ -f "$extra_rows" ] || { echo "extra rows file not found: $extra_rows" >&2; exit 1; }
    # The shim must be the build for those rows: the patched libr15's ID depends on them, and a shim built for others refuses
    # the library while its stubs still branch through slots only an attached shim fills (tools/quest_repack_libs.py).
    python3 -I tools/quest_repack_libs.py check-shim-rows --shim "$shim" ${extra_rows:+--extra-rows "$extra_rows"}
    if [ -z "$patcher" ]; then
        inc=""
        json_trees=("build/{{ preset }}/vcpkg_installed/x64-mingw-static/include" build/mingw-dev/vcpkg_installed/x64-mingw-static/include build/android-dev/vcpkg_installed/arm64-android/include build/android-arm64/vcpkg_installed/arm64-android/include build/quest-host-vcpkg/x64-linux/include)
        if [ -n "$json_include" ]; then
            [ -f "$json_include/nlohmann/json.hpp" ] || { echo "android-repack-apk: FAIL - $json_include/nlohmann/json.hpp not found (json_include)" >&2; exit 1; }
            inc="$json_include"
        else
            for tree in "${json_trees[@]}"; do
                if [ -f "$tree/nlohmann/json.hpp" ]; then inc="$tree"; break; fi
            done
        fi
        [ -n "$inc" ] || { echo "android-repack-apk: FAIL - nlohmann/json.hpp, which the libr15 patcher needs, is in none of: ${json_trees[*]}; run 'just build' with a mingw-* preset (or 'just preset=mingw-dev _vcpkg-mingw'), or pass json_include" >&2; exit 1; }
        just quest-patcher "$inc"
        patcher="$root/build/quest-patcher/quest_patch_libr15"
    fi
    [ -x "$patcher" ] || { echo "android-repack-apk: the libr15 patcher is not an executable file: $patcher" >&2; exit 1; }
    extra=()
    if [ -n "$extra_rows" ]; then extra=(--extra-rows "$extra_rows"); fi
    # `plugin` (docs/adr/0019-quest-plugin-loader.md): the plugin libraries to carry in the APK, for a headset run of the loader:
    # one path, or several separated by colons (a path may hold spaces). Empty for every release and package-dev APK. Each file is lib<name>.so.
    # A packaged plugin loads by default: the repack writes a manifest of them (tools/quest_repack_libs.py write-plugin-manifest) and
    # config.yaml's `plugins:` list only overrides it. `plugin_early` and `plugin_off` (colon-separated file names of packaged plugins)
    # record `early` and `enabled: false` in that manifest. Every file gets every check, and no two may share a name.
    plugins=()
    if [ -n "$plugin" ]; then
        IFS=: read -r -a plugins <<<"$plugin"
        llvm_bin="{{ ndk }}/toolchains/llvm/prebuilt/linux-x86_64/bin"
        [ -x "$llvm_bin/llvm-nm" ] || llvm_bin="$(dirname "$(command -v llvm-nm || true)")"
        seen_names=" "
        for p in "${plugins[@]}"; do
            [ -f "$p" ] || { echo "plugin not found: $p" >&2; exit 1; }
            case "$(basename "$p")" in
                lib?*.so) ;;
                *) echo "plugin must be named lib<name>.so: $p" >&2; exit 1 ;;
            esac
            case "$seen_names" in
                *" $(basename "$p") "*) echo "two plugins are named $(basename "$p")" >&2; exit 1 ;;
            esac
            seen_names="$seen_names$(basename "$p") "
            # `zip` replaces an entry of the same path: a plugin named like any lib/ entry of the store APK, or like a library
            # staged below, would take that library's place. Refuse before anything is staged (tools/quest_repack_libs.py).
            python3 -I tools/quest_repack_libs.py check-plugin-name --apk "$apk" --plugin "$p"
            # The loader refuses a plugin whose NEVR note does not list the running libr15's build ID (plugin_image.h): an APK
            # whose plugin is refused on the headset is refused here, against the store APK's own libr15.so.
            python3 -I tools/quest_repack_libs.py check-plugin-build-ids --apk "$apk" --plugin "$p"
            # ADR 0019 section 1: a plugin runs the same static-init check as the shim, minus the shim's own constructor.
            tools/check_quest_static_init.sh --plugin "$llvm_bin" "$p"
        done
    fi
    # plugin_early / plugin_off: packaged file names, recorded in the manifest (tools/quest_repack_libs.py refuses a name that is
    # not packaged). Without a plugin there is nothing to record them on, so they are refused.
    early_args=(); off_args=()
    if [ -n "$plugin_early" ]; then
        IFS=: read -r -a early_names <<<"$plugin_early"
        for n in "${early_names[@]}"; do early_args+=(--early "$n"); done
    fi
    if [ -n "$plugin_off" ]; then
        IFS=: read -r -a off_names <<<"$plugin_off"
        for n in "${off_names[@]}"; do off_args+=(--off "$n"); done
    fi
    if [ "${#plugins[@]}" -eq 0 ] && [ "${#early_args[@]}" -gt 0 ]; then
        echo "android-repack-apk: plugin_early names a packaged plugin, and no plugin is given" >&2; exit 1
    fi
    if [ "${#plugins[@]}" -eq 0 ] && [ "${#off_args[@]}" -gt 0 ]; then
        echo "android-repack-apk: plugin_off names a packaged plugin, and no plugin is given" >&2; exit 1
    fi
    # The version the versionCode comes from: `version` when the shim was built elsewhere (a release's CI-built loader has no
    # build tree here), else the build's version header, which sits beside the shim's build tree (build/<tree>/sentinel/ ->
    # build/<tree>/generated/).
    if [ -n "$version" ]; then
        version_source=(--version "$version")
    else
        build_info="$(dirname "$(dirname "$shim")")/generated/nevr_build_info.h"
        [ -f "$build_info" ] || { echo "build info not found (build it: just build-android): $build_info" >&2; exit 1; }
        version_source=(--build-info "$build_info")
    fi
    rm -rf "$work/stage" "$work/orig" "$work/manifest" "$work/libr15-patch-report.json"
    mkdir -p "$work/stage/lib/arm64-v8a" "$work/orig" "$work/manifest"
    # 1. extract the real original loader from the store APK
    unzip -o -q "$apk" lib/arm64-v8a/libovrplatformloader.so -d "$work/orig"
    # 2. rename original + patch soname so the NEEDED string matches the file on disk
    patchelf --set-soname libovrplatformloader_orig.so \
        --output "$work/stage/lib/arm64-v8a/libovrplatformloader_orig.so" \
        "$work/orig/lib/arm64-v8a/libovrplatformloader.so"
    # 3. drop our shim in under the hijacked name
    cp "$shim" "$work/stage/lib/arm64-v8a/libovrplatformloader.so"
    stage_libs=(lib/arm64-v8a/libovrplatformloader.so lib/arm64-v8a/libovrplatformloader_orig.so)
    # 3b. libr15.so: the store's file through the patcher (it refuses unless the file is the pinned one, and says why on
    # stderr), staged as the file the APK will carry. It is not in stage_libs: the store stores libr15.so DEFLATED and the zip
    # step below does the same for it, where the two libraries above go in STORED.
    unzip -o -q "$apk" lib/arm64-v8a/libr15.so -d "$work/orig"
    "$patcher" --stock "$work/orig/lib/arm64-v8a/libr15.so" --out "$work/stage/lib/arm64-v8a/libr15.so" \
        --report "$work/libr15-patch-report.json" ${extra[@]+"${extra[@]}"}
    [ -s "$work/stage/lib/arm64-v8a/libr15.so" ] || { echo "the libr15 patcher wrote no libr15.so: $patcher" >&2; exit 1; }
    for p in ${plugins[@]+"${plugins[@]}"}; do
        cp "$p" "$work/stage/lib/arm64-v8a/$(basename "$p")"
        stage_libs+=("lib/arm64-v8a/$(basename "$p")")
    done
    # 3d. the bundled content pack (docs/adr/0029-quest-bundled-content-pack.md): staged as APK assets, every file through every check
    # (tools/quest_repack_libs.py stage-pack), and the staged tree checked as the runtime reads it. Nothing happens without a pack.
    if [ -n "$pack" ]; then
        python3 -I tools/quest_repack_libs.py stage-pack --pack-zip "$pack" --meta "$pack_meta" --stage "$work/stage" --store-apk "$apk"
        python3 -I tools/quest_repack_libs.py pack-problems --stage "$work/stage"
    fi
    # 3c. the manifest of the packaged plugins: their names, staged hashes, early and enabled. Written only when a plugin is packaged,
    # so a repack without one is the same APK as before (docs/adr/0019-quest-plugin-loader.md, section 2).
    if [ "${#plugins[@]}" -gt 0 ]; then
        python3 -I tools/quest_repack_libs.py write-plugin-manifest --stage "$work/stage" --plugin "${plugins[@]}" \
            ${early_args[@]+"${early_args[@]}"} ${off_args[@]+"${off_args[@]}"}
        stage_libs+=("lib/arm64-v8a/libnevr_plugin_manifest.so")
    fi
    # 4. fresh APK copy, strip the store's v1 signature, inject both libs STORED (-0)
    unsigned="$work/r15-repacked.apk"
    aligned="$work/r15-repacked-aligned.apk"
    signed="$work/r15_nevr-sentinel_signed.apk"
    # Any stop after this point, a failed check included (set -e quits at a failing command), removes the signed APK:
    # quest-install and the pinned tests take whatever sits at this path.
    done_ok=0
    trap '[ "$done_ok" = 1 ] || rm -f "$signed"' EXIT
    cp -f "$apk" "$unsigned"
    zip -q -d "$unsigned" 'META-INF/*.RSA' 'META-INF/*.SF' 'META-INF/*.MF' || true
    ( cd "$work/stage" && zip -q -0 -X "$unsigned" "${stage_libs[@]}" )
    ( cd "$work/stage" && zip -q -X "$unsigned" lib/arm64-v8a/libr15.so )
    # The pack's entries go in STORED (-0), like the libraries: the file names were checked plain by stage-pack.
    if [ -n "$pack" ]; then
        ( cd "$work/stage" && find assets -type f -print | LC_ALL=C sort | zip -q -0 -X "$unsigned" -@ )
    fi
    # 4b. versionCode: extract the compiled manifest, set the code the build's version gives (the tool refuses
    # unless the field is exactly where Android reads it, and unless the shim carries the header's version),
    # and put the manifest back. A 4-byte write: the entry keeps every other byte.
    unzip -o -q "$unsigned" AndroidManifest.xml -d "$work/manifest"
    version_code=$(python3 -I tools/quest_apk_version.py patch-manifest "$work/manifest/AndroidManifest.xml" "${version_source[@]}" --shim "$shim")
    ( cd "$work/manifest" && zip -q -X "$unsigned" AndroidManifest.xml )
    # 5. page-align uncompressed .so, then sign (v2/v3) — align BEFORE sign.
    # The key is a private tmpfs copy for this one command, shredded when it returns; the passphrase
    # reaches apksigner through the environment (`env:`), never through its command line.
    zipalign -p -f 4 "$unsigned" "$aligned"
    tools/quest_keystore.sh with bash -c 'exec apksigner sign --ks "$NEVR_QUEST_KS" --ks-key-alias "$NEVR_QUEST_KS_ALIAS" --ks-pass env:NEVR_QUEST_KS_PASS --key-pass env:NEVR_QUEST_KS_PASS --out "$1" "$2"' _ "$signed" "$aligned"
    verified=$(apksigner verify --print-certs "$signed")
    printf '%s\n' "$verified" | sed -n 1p
    # The code Android will compare is read back from the SIGNED APK by aapt2, not taken from what was written.
    signed_code=$(aapt2 dump badging "$signed" | sed -n "s/^package: .* versionCode='\([0-9]*\)'.*/\1/p")
    [ "$signed_code" = "$version_code" ] || { echo "signed APK versionCode is '$signed_code', expected $version_code" >&2; exit 1; }
    echo "versionCode $signed_code (the community installer's build is 4987570)"
    # 6. VERIFY from the SIGNED apk — the headless packaging proof
    rm -rf "$work/verify"; mkdir -p "$work/verify"
    unzip -o -q "$signed" 'lib/arm64-v8a/libovrplatformloader*.so' -d "$work/verify"
    echo "== shim  lib/arm64-v8a/libovrplatformloader.so =="
    readelf -d "$work/verify/lib/arm64-v8a/libovrplatformloader.so" | grep -iE 'soname|needed'
    nm -D "$work/verify/lib/arm64-v8a/libovrplatformloader.so" | grep nevr_sentinel_marker
    echo "== renamed original  lib/arm64-v8a/libovrplatformloader_orig.so =="
    readelf -d "$work/verify/lib/arm64-v8a/libovrplatformloader_orig.so" | grep -i soname
    echo ""
    # Every library of the store APK (the loader aside, which the shim replaces) is in the SIGNED apk with its bytes, libr15.so
    # being the patcher's output for the store's (the tool runs again here, on its own copy; its three digests are printed),
    # and every staged library is there as staged.
    python3 -I tools/quest_repack_libs.py verify-signed --source "$apk" --signed "$signed" --stage "$work/stage" \
        --patcher "$patcher" --scratch "$work" ${extra[@]+"${extra[@]}"}
    done_ok=1
    echo "Signed sideload APK ready -> $signed"
    echo "Install with: just quest-install"

# Store the Quest APK signing key (once) and its passphrase in pass, so every repack signs with the
# same key and the headset accepts it as an update. Reads the passphrase from NEVR_QUEST_KS_IMPORT_PASS
# (never an argument) and refuses to replace a key that is already stored.
import-quest-keystore file:
    #!/usr/bin/env bash
    set -euo pipefail
    tools/quest_keystore.sh import "{{ file }}"

# Print the Quest APK signer's SHA-256 (lowercase hex, no colons): compare it with the app's signer
# on the headset before installing, a different one forces an uninstall.
quest-keystore-fingerprint:
    #!/usr/bin/env bash
    set -euo pipefail
    tools/quest_keystore.sh fingerprint

# Refuse (exit 1, one line per problem) unless this clone is exactly origin's release branch: every local
# tag is on origin and at the same commit, no uncommitted or untracked change, HEAD is origin/<base>'s tip.
# It also refuses a version part outside the Quest versionCode rule and warns near its limits (development distance N above 900,
# minor or patch above 90; tools/quest_apk_version.py).
# Read-only (git ls-remote is the only network call; there is no fetch). Run it before `just package-dev`,
# before pushing a v<x.y.z> release tag and before `gh release create` (tools/release_preflight.py). To start from a
# tag's commit instead of origin's tip (release-quest-apk does), run tools/release_preflight.py --tag vX.Y.Z.
release-preflight base="main":
    tools/release_preflight.py --base {{ quote(base) }}

# A LOCAL package: the Windows zip and the Quest APK from a CLEAN tree at HEAD, in build/package-dev/<sha>/.
# Every non-tag build stamps a DEVELOPMENT version, X.Y.(Z+1)-dev.<N>+<sha> (cmake/set_project_version_from_git.cmake),
# and the files are named nevr-runtime-v<X.Y.Z-dev.N>-<sha7>-*: this recipe cannot produce a release. A release
# is a vX.Y.Z tag built by CI (.github/workflows/build.yml); its version is exactly the tag.
# Both builds embed config/public-defaults.env (git-ignored: copy it from the main checkout or from the
# .example) and need no config file to log in; the gate (tools/package_release.py) refuses the package
# otherwise, and refuses a binary whose identity literal is not this commit's development version.
# The Quest APK is signed with the one persistent key stored in pass (tools/quest_keystore.sh),
# imported once with `just import-quest-keystore <keystore>`; a new key is never generated, because
# a different key forces an uninstall on the headset. `store_apk` is the unmodified store APK.
# Neither build needs a config file: the Quest redirects, bridges, logs in and runs the social facade from the
# embedded endpoints and keys alone (config.yaml only overrides).
# The Quest APK is a repack of the store game: it is for testers and is published outside GitHub, never attached to a release.
# This never tags, uploads or publishes.
package-dev store_apk="/mnt/games/cache/r15_goldmaster_store.apk":
    #!/usr/bin/env bash
    set -euo pipefail
    # First: a package is built only from a pristine clone that is origin's main (tools/release_preflight.py).
    tools/release_preflight.py
    tools/quest_keystore.sh check
    [ -f "{{ store_apk }}" ] || { echo "package-dev: store APK not found: {{ store_apk }}" >&2; exit 1; }
    commit=$(git rev-parse HEAD)
    echo "package-dev: LOCAL build of ${commit:0:7}: it stamps a DEVELOPMENT version, not a release (a release is a vX.Y.Z tag built by CI)" >&2
    out="build/package-dev/${commit:0:7}"
    # Windows DLL: its own build tree (build/mingw-dev).
    # (The MinGW build uses this checkout's own vcpkg root; the Quest preset below needs VCPKG_ROOT.)
    (
        unset VCPKG_ROOT
        just preset=mingw-dev _vcpkg-mingw
        cmake --preset mingw-dev
        cmake --build --preset mingw-dev
    )
    # The core installer install.ps1 runs (installer/, ADR 0023), for Windows, locked and stripped.
    CARGO_TARGET_X86_64_PC_WINDOWS_GNU_LINKER=x86_64-w64-mingw32-gcc cargo build --locked --release --target x86_64-pc-windows-gnu \
        --manifest-path installer/Cargo.toml --bin install-nevr-runtime --target-dir build/installer
    installer=build/installer/x86_64-pc-windows-gnu/release/install-nevr-runtime.exe
    x86_64-w64-mingw32-strip "$installer"
    # The same file under the name programs outside this repository still call (#1492): one build, one copy.
    cp "$installer" "$(dirname "$installer")/nevr-install.exe"
    just _quest-apk "{{ store_apk }}"
    python3 tools/package_release.py --commit "$commit" --out "$out" \
        --dll build/mingw-dev/bin/BugSplat64.dll \
        --runtime-dll build/mingw-dev/bin/nevr-runtime.dll \
        --server-exe build/mingw-dev/bin/echovr_server.exe \
        --installer-exe "$installer" \
        --installer-compat-exe "$(dirname "$installer")/nevr-install.exe" \
        --apk build/android-dev/repack/r15_nevr-sentinel_signed.apk \
        --pc-header build/mingw-dev/generated/nevr_builtin_defaults.h \
        --quest-header build/android-dev/generated/nevr_builtin_defaults.h \
        --quest-build-info build/android-dev/generated/nevr_build_info.h

# The Quest half of a development package: the sentinel built in build/android-dev, checked, and repacked from the store
# APK. Private: package-dev only (a release packages CI's loader, release-quest-apk).
_quest-apk store_apk: _build-guard
    #!/usr/bin/env bash
    set -euo pipefail
    launcher=$(python3 -I tools/build_checkout.py quest-launcher "${CMAKE_CXX_COMPILER_LAUNCHER:-}")
    (cd src/quest && CMAKE_C_COMPILER_LAUNCHER="$launcher" CMAKE_CXX_COMPILER_LAUNCHER="$launcher" ANDROID_NDK_HOME="{{ ndk }}" cmake --preset android-arm64-dev)
    ANDROID_NDK_HOME="{{ ndk }}" cmake --build build/android-dev -j "${CMAKE_BUILD_PARALLEL_LEVEL:-4}"
    objects=$(python3 tools/quest_link_objects.py build/android-dev sentinel/libovrplatformloader.so)
    tools/check_quest_static_init.sh "{{ ndk }}/toolchains/llvm/prebuilt/linux-x86_64/bin" build/android-dev/sentinel/libovrplatformloader.so $objects
    just android-repack-apk "{{ store_apk }}" build/android-dev/sentinel/libovrplatformloader.so build/android-dev/repack

# The development APK for an autopilot run on a Quest (#1202, docs/adr/0025-quest-autopilot-route.md): the local
# android-arm64 sentinel (a development build: the input seam refuses a release) with libnevr_autopilot.so beside it,
# repacked from the store APK. The plugin is packaged and off by default (plugin_off); a config.yaml entry that names
# nevr_autopilot turns it on. Writes build/android-arm64/repack/r15_nevr-sentinel_signed.apk, the path test-quest-hooks-pinned
# reads. No release or package-dev APK carries the plugin.
quest-autopilot-apk store_apk="/mnt/games/cache/r15_goldmaster_store.apk": build-android
    just android-repack-apk "{{ store_apk }}" build/android-arm64/sentinel/libovrplatformloader.so build/android-arm64/repack build/android-arm64/integration/libnevr_autopilot.so "" "" "" "" "" libnevr_autopilot.so

# The q15t plugin libraries (#1546, src/quest/plugins/q15t/README.md): libq15t.so and libq15t_early.so, built with the android-arm64
# preset, each through the plugin static-init check (tools/check_quest_static_init.sh --plugin). Copies them to build/quest-q15t/,
# checks that each names the pinned libr15 build id and that libq15t.so carries the shipped defaults (the config marker in plugin.cpp),
# and prints the sha256 of each. No release or package-dev APK carries them; the variant recipe (release-quest-variant-apk) packages them.
build-quest-q15t: configure-android
    #!/usr/bin/env bash
    set -euo pipefail
    ANDROID_NDK_HOME="{{ ndk }}" cmake --build build/android-arm64 --target nevr_q15t nevr_q15t_early -j "${CMAKE_BUILD_PARALLEL_LEVEL:-4}"
    built=build/android-arm64/integration/q15t
    out=build/quest-q15t
    mkdir -p "$out"
    id=$(sed -n 's/.*kLibR15StockBuildId = "\([0-9a-f]*\)".*/\1/p' src/quest/sentinel/pinned_targets.h)
    [ -n "$id" ] || { echo "build-quest-q15t: no kLibR15StockBuildId in src/quest/sentinel/pinned_targets.h" >&2; exit 1; }
    for lib in libq15t.so libq15t_early.so; do
        [ -f "$built/$lib" ] || { echo "build-quest-q15t: $built/$lib was not built" >&2; exit 1; }
        cp "$built/$lib" "$out/$lib"
        tools/check_quest_static_init.sh --plugin "{{ ndk }}/toolchains/llvm/prebuilt/linux-x86_64/bin" "$out/$lib"
        grep -qaF "$id" "$out/$lib" || { echo "build-quest-q15t: $lib does not name libr15 build id $id" >&2; exit 1; }
    done
    grep -qaF "q15t-config ec_words=[types=VPTO equip hide] ecfx_default_on=1 gate_link=1 gate_perm=1 gate_bots=1 levelname=1 hz_level=1 build_id=$id" "$out/libq15t.so" \
        || { echo "build-quest-q15t: libq15t.so does not carry the shipped defaults (q15t-config marker)" >&2; exit 1; }
    (cd "$out" && sha256sum libq15t.so libq15t_early.so)

# One unattended autopilot run on the attached Quest (tools/quest_autopilot_run.py --help): pushes the route, starts the
# game with the proximity sensor overridden, waits for route_end, and records the run under the work root.
quest-autopilot-run route *args:
    python3 tools/quest_autopilot_run.py --route "{{ route }}" {{ args }}

# The Quest APK of a RELEASE: the repack of the store game with the loader CI built from the release tag. That loader is
# the release's own asset (nevr-runtime-vX.Y.Z-quest-libovrplatformloader.so): it is downloaded (tools/quest_release_loader.py
# fetch: the sha256 GitHub recorded for it, then `gh attestation verify`), checked to carry the tag's version and commit
# (tools/package_release.py quest-library), repacked, and the signed APK's loader is compared with the asset's sha256 before
# the APK is copied out. The loader is not compiled, so the asset and the APK's loader are one file (the repack also builds the
# host libr15 patcher, `just quest-patcher`, and patches the store's libr15.so with it, docs/adr/0021 section 9). `gh` is $GH_BIN (default gh).
# Run it from a clone whose HEAD is the tag's commit and that passes `tools/release_preflight.py --tag <tag>` (HEAD is the
# tag's commit, which need not be origin's tip: an older tag builds after main has moved on; the tree is clean and origin has
# the tag at that commit); the release (a draft is enough) must carry the asset. Writes
# build/quest-release/<tag>/nevr-runtime-<tag>-quest.apk and its .sha256; it uploads nothing and never tags. The APK is
# published outside GitHub (RELEASING.md, "Quest APKs never go on GitHub").
release-quest-apk tag store_apk="/mnt/games/cache/r15_goldmaster_store.apk": (_release-quest-apk "release-quest-apk" tag store_apk "" "" "" "")

# The unlisted variant APK of a release (tools/quest_release_timer.py builds it in the same run as the release APK): the same
# repack of the store game with the same release loader asset, plus the plugin libraries `plugins` names (one path, or several
# separated by colons; the timer passes copies it has checked against their pins). Everything `release-quest-apk` checks is
# checked here too, with the same tag and clone rules and the same refusals under this recipe's name. Writes
# build/quest-release/<tag>/variant/nevr-runtime-<tag>-q15t-quest.apk and its .sha256 beside, in sha256sum format. The APK is
# never listed in the manifest: tools/publish_quest_apk.py --variant puts it on the release host. It uploads nothing and never tags.
# `pack` and `pack_meta` (both empty, or both a file) carry a game-data pack as APK assets (android-repack-apk, docs/adr/0029-quest-bundled-content-pack.md);
# only this variant takes them, never the release APK.
release-quest-variant-apk tag plugins store_apk="/mnt/games/cache/r15_goldmaster_store.apk" pack="" pack_meta="": (_release-quest-apk "release-quest-variant-apk" tag store_apk plugins "q15t" pack pack_meta)

# The body of release-quest-apk and release-quest-variant-apk. `label` is the public recipe's name, which every message of the
# run carries. `plugin` and `variant` are both empty (the release APK, which never carries a plugin) or both set (the variant
# APK, written under build/quest-release/<tag>/variant). `pack` and `pack_meta` are empty (every release APK) or, for the variant, a game-data pack
# zip and its description file.
_release-quest-apk label tag store_apk plugin variant pack pack_meta:
    #!/usr/bin/env bash
    set -euo pipefail
    label="{{ label }}"; tag="{{ tag }}"; plugin="{{ plugin }}"; variant="{{ variant }}"; pack="{{ pack }}"; pack_meta="{{ pack_meta }}"
    [[ "$tag" =~ ^v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$ ]] || { echo "$label: not a release tag (vX.Y.Z): $tag" >&2; exit 1; }
    [ -z "$variant" ] || [ -n "$plugin" ] || { echo "$label: the variant APK carries plugin libraries, and none is named" >&2; exit 1; }
    [ -n "$variant" ] || [ -z "$plugin" ] || { echo "$label: a release APK carries no plugin library" >&2; exit 1; }
    [ -n "$variant" ] || [ -z "$pack" ] || { echo "$label: a release APK carries no game-data pack" >&2; exit 1; }
    [ -n "$pack" ] || [ -z "$pack_meta" ] || { echo "$label: pack_meta describes a pack, and no pack is given" >&2; exit 1; }
    tools/release_preflight.py --tag "$tag"
    tag_commit=$(git rev-parse --verify "refs/tags/$tag^{commit}") || { echo "$label: tag $tag is not in this clone" >&2; exit 1; }
    head=$(git rev-parse HEAD)
    [ "$tag_commit" = "$head" ] || { echo "$label: HEAD ${head:0:7} is not the commit of $tag (${tag_commit:0:7}); check out the tag's commit" >&2; exit 1; }
    [ -f "{{ store_apk }}" ] || { echo "$label: store APK not found: {{ store_apk }}" >&2; exit 1; }
    tools/quest_keystore.sh check
    ver="${tag#v}"
    out="build/quest-release/$tag"
    name="nevr-runtime-$tag-quest.apk"
    if [ -n "$variant" ]; then
        out="$out/variant"
        name="nevr-runtime-$tag-$variant-quest.apk"
    fi
    rm -rf "$out"
    mkdir -p "$out"
    echo "$label: packaging the $tag loader asset at ${head:0:7}" >&2
    loader_sha=$(python3 -I tools/quest_release_loader.py fetch --tag "$tag" --out "$out/loader")
    python3 tools/package_release.py quest-library --lib "$out/loader/libovrplatformloader.so" --version "$ver" --commit "$tag_commit"
    if [ -n "$pack" ]; then
        just android-repack-apk "{{ store_apk }}" "$out/loader/libovrplatformloader.so" "$out/repack" "$plugin" "$ver" "" "" "" "" "" "$pack" "$pack_meta"
    else
        just android-repack-apk "{{ store_apk }}" "$out/loader/libovrplatformloader.so" "$out/repack" "$plugin" "$ver"
    fi
    python3 -I tools/quest_release_loader.py verify-apk --apk "$out/repack/r15_nevr-sentinel_signed.apk" --sha256 "$loader_sha"
    cp -f "$out/repack/r15_nevr-sentinel_signed.apk" "$out/$name"
    (cd "$out" && sha256sum "$name" > "$name.sha256")
    cat "$out/$name.sha256"
    if [ -n "$variant" ]; then
        echo "$label: $out/$name is $tag at ${head:0:7}, with the release's loader (sha256 $loader_sha). It is NOT a release asset and not in the manifest: tools/publish_quest_apk.py --variant $variant puts it on the release host." >&2
    else
        echo "$label: $out/$name is $tag at ${head:0:7}, with the release's loader (sha256 $loader_sha). It is NOT a release asset: publish it outside GitHub (RELEASING.md)." >&2
    fi

# Put the Quest APK that `release-quest-apk <tag>` wrote, and its .sha256, on R2 under nevr-runtime/<tag>/, then fetch both
# back from the public host and compare size and sha256 (tools/publish_quest_apk.py; #1098). Run it from the clone that
# built the APK. The bucket, the public host, the release-signer executable and a GitHub read token come from the
# environment only (R2_ENDPOINT, R2_BUCKET, R2_ACCESS_KEY_ID, R2_SECRET_ACCESS_KEY, RELEASE_PUBLIC_BASE, RELEASE_SIGNER_DIR,
# RELEASE_SIGNER_BIN, GH_TOKEN); a missing one is a refusal that names it. Nothing is overwritten.
publish-quest-apk tag:
    python3 tools/publish_quest_apk.py "{{ tag }}"

# Build the core installer CLI (installer/, ADR 0023) for this machine: installer/target/release/install-nevr-runtime.
# Locked to the committed Cargo.lock; set CARGO_TARGET_DIR to keep the regenerable build tree out of the checkout.
build-installer:
    cargo build --locked --release --manifest-path installer/Cargo.toml --bin install-nevr-runtime

# Install the NEVR Quest APK + game data onto the connected headset (sideload) through the core installer
# (tools/quest-install.sh calls `install-nevr-runtime quest install`). The core fetches the signed release manifest, takes the
# APK and the game data the channel's release names, checks both, plans from what the headset holds and installs;
# every run writes a [NEVR.INSTALL] log (the path is in its output). The recipe holds no hash, URL or version.
# Pass `--yes-uninstall` (`just quest-install --yes-uninstall`) to let the core remove a differently signed install
# when the headset refuses an in-place update; that uninstall deletes the app's data on the headset. Without it the run
# ends `needs-user` and says what it would delete. `--version V` installs release V instead of the channel's.
# Needs adb, aapt2 and apksigner on PATH. A locally built tester APK is installed by giving the recipe its path
# (`just quest-install build/android-dev/repack/r15_nevr-sentinel_signed.apk`), or a directory that holds exactly one
# .apk and its <name>.apk.sha256: the core checks the file itself, logs every step as an unverified dev artifact, and
# still takes the game data from the signed manifest.
[positional-arguments]
quest-install *args: build-installer
    NEVR_INSTALL="${CARGO_TARGET_DIR:-installer/target}/release/install-nevr-runtime" tools/quest-install.sh "$@"

# --- Tests ---

# The Python tool suite, kept: full output, JUnit XML, the pytest-randomly seed, and a start and an end
# record in runs.jsonl per run, under $NEVR_TEST_LOG_DIR (default test-runs/tools-tests in the work root, tools/work_root.py).
# The end record names the failing tests. Exit code is pytest's, except 2 for a record that cannot be kept
# and 128 + the signal for an interrupt, 130 for CTRL+C (tools/run_tool_tests.py). Args replace the
# default `-q tools/tests` (#913).
[positional-arguments]
test-tools *ARGS:
    python3 tools/run_tool_tests.py "$@"

# The core installer (installer/, ADR 0023): library, CLI and C ABI tests. The build is locked to the committed
# Cargo.lock; set CARGO_TARGET_DIR to keep the regenerable build tree out of the checkout.
test-installer *ARGS:
    cargo test --locked --manifest-path installer/Cargo.toml {{ ARGS }}

# Run all system tests
test-system:
    cd tests/system && go test -v ./...

# Run quick system tests only
test-system-short:
    cd tests/system && go test -v -short ./...

# Run DLL loading tests only
test-system-dll:
    cd tests/system && go test -v -short -run ".*DLL.*" ./...

# Run system tests with verbose output, no cache
test-system-verbose:
    cd tests/system && go test -v -count=1 ./...

# Run the built runtime on a native Windows VM (libvirt) and judge the boot.
# Needs WINVM_USER/WINVM_PASS and `just build` first; see
# tools/winvm/README.md. Exit 1 = runtime failed a check,
# exit 2 = the VM/environment is unusable.
test-winvm *ARGS:
    tools/winvm/systest.py {{ARGS}}

# One social scenario, end to end, unattended (docs/design/2026-10-01-social-scenario-harness.md):
# builds the mingw-scenario DLL (test-only control endpoint), launches the client the
# launch-client.sh way, runs tools/scenario/scenarios/NAME.yaml and prints a PASS/FAIL table.
scenario NAME *ARGS:
    #!/usr/bin/env bash
    set -euo pipefail
    just preset=mingw-scenario build
    python3 tools/scenario/run_scenario.py "tools/scenario/scenarios/{{NAME}}.yaml" {{ARGS}}

# The scheduled smoke: one client boot proving the three things every scenario assumes (in game,
# control endpoint listening, a fire drives the game), with a non-zero record and an alarm on
# failure. The user timer runs tools/scenario/smoke.sh from its own main-tracking worktree.
smoke *ARGS:
    tools/scenario/smoke.sh {{ARGS}}

# ARGS pass to run_all.py: --config <dir>/config.json runs the suite with that directory's config.yaml.
scenario-all *ARGS:
    #!/usr/bin/env bash
    set -euo pipefail
    just preset=mingw-scenario build
    python3 tools/scenario/run_all.py {{ARGS}}

# Run plugin ground truth tests (no game binary needed)
test-plugins-groundtruth:
    cd tests/plugins && go test -v -run "TestGroundTruth" ./...

# Run all plugin tests (needs game binary + MCP harness)
test-plugins:
    cd tests/plugins && go test -v -timeout 10m ./...

# Run plugin tests in short mode (skips integration, runs ground truth only)
test-plugins-short:
    cd tests/plugins && go test -v -short ./...

# Run plugin tests with verbose output, no cache
test-plugins-verbose:
    cd tests/plugins && go test -v -count=1 -timeout 10m ./...

# Run auth ground truth tests (no game binary, no network)
test-auth-groundtruth:
    cd tests/plugins && go test -v -run "TestGroundTruth_No|TestGroundTruth_Auth" ./...

# Run the C++ GTest suite under Wine (cross-compiled). Fail-close: exits nonzero if
# the GTest binary cannot be built or found, or if any test fails. Builds the test
# target with -DBUILD_TESTING=ON (the default presets do not enable it). Add new
# GTest executables here as they land.
test-auth-unit:
    #!/usr/bin/env bash
    set -euo pipefail
    unset VCPKG_ROOT
    # A test binary that hangs or runs away must fail the gate, not block it or exhaust the machine:
    # 124 is timeout's "timed out" status and 137 is a process killed by the memory cap. The cap
    # (systemd user scope, MemoryMax=4G, no swap) applies when a probe scope with the same properties
    # starts; when systemd-run is missing or cannot start a user scope (no user manager or session bus,
    # as in a container), the run keeps only the time limit and says so. The probe does not detect a
    # host whose cgroups do not enforce MemoryMax. systemd-run expands "$" in its arguments, which the
    # test path below never contains. Each scope is its own unit, so the wrapper that runs this recipe
    # does not bound the test processes: each test has its own 4G cap.
    # A failing binary is recorded and the loop goes on, so one failure does not hide the binaries after it
    # (#1228); the summary after the last binary names every failure and exits non-zero.
    test_failures=()
    run_test() {
        local rc=0
        if command -v systemd-run >/dev/null && systemd-run --user --scope --quiet -p MemoryMax=4G -p MemorySwapMax=0 -- true 2>/dev/null; then
            systemd-run --user --scope --quiet -p MemoryMax=4G -p MemorySwapMax=0 -- timeout -k 10 900 wine "$1" || rc=$?
        else
            echo "test-auth-unit: systemd-run unavailable or cannot start a user scope: running $1 with the time limit only (no memory cap)" >&2
            timeout -k 10 900 wine "$1" || rc=$?
        fi
        if [[ "$rc" -eq 124 ]]; then
            echo "test-auth-unit: FAIL — $1 timed out after 900s" >&2
            test_failures+=("$1 (timed out)")
        elif [[ "$rc" -eq 137 ]]; then
            echo "test-auth-unit: FAIL — $1 killed: memory cap exceeded (MemoryMax=4G)" >&2
            test_failures+=("$1 (memory cap)")
        elif [[ "$rc" -ne 0 ]]; then
            echo "test-auth-unit: FAIL — $1 exited $rc" >&2
            test_failures+=("$1 (exit $rc)")
        fi
    }
    cmake --preset {{ preset }} -DBUILD_TESTING=ON > /dev/null 2>&1 \
        || cmake --preset {{ preset }} -DBUILD_TESTING=ON
    cmake --build --preset {{ preset }} --target test_xpid_patch --target test_parse_endpoint --target test_behavioral --target test_token_auth --target test_messages --target test_crash_recovery --target test_nevr_config --target test_service_map --target test_service_config --target test_social_facade --target test_scenario_early_quit --target test_early_quit_lockout --target test_lobby_full_backoff --target test_party_follow_dedup --target test_server_team_size --target test_menu_version_label --target test_splash_skip --target test_ui_texture_mips --target test_cmdline_strip --target test_schannel_cred_guard --target test_hooking --target test_dll_load_hook --target test_boot_log_routing --target test_plugin_load_plan --target test_system_module_loader --target test_login_redirect_override --target test_websocket_frame --target test_protobuf_transport --target test_websocket_client_auth --target test_url_diagnostics --target test_serverdb_uri --target test_self_check --target test_crash_send --target test_party_share_check --target test_party_leave_check --target test_quit_check --target test_friend_request_check --target test_credential_cache_check --target test_status_redirect_check --target test_callback_unregistration --target test_server_context --target test_session_unregister --target test_mic_lifecycle --target test_telemetry_snapshot_store --target test_telemetry_game_properties --target test_coop_ai_trace --target test_frame_stats --target test_evr_codec --target test_legacy_codec --target test_legacy_session --target test_symbol_hash --target test_socket_role --target test_login_refusal --target test_invite_wizard --target test_invite_worker --target test_invite_service --target test_ui_text --target test_invite_choices --target test_invite_join --target test_asset_cache --target test_session_router --target test_plugin_signature --target test_plugin_tree --target test_plugin_trust --target test_asset_registry --target test_installed_levels --target test_telemetry_game_type --target test_arena_round_time --target test_nevr_root --target test_arena_public_clock --target test_arena_public_clock_hook --target test_render_config --target test_render_config_hook --target test_server_bots --target test_deadlock_monitor
    cmake --build --preset {{ preset }} --target test_mic_dsp
    cmake --build --preset {{ preset }} --target test_game_image_guard
    cmake --build --preset {{ preset }} --target test_runtime_abi
    cmake --build --preset {{ preset }} --target test_runtime_export_guard
    cmake --build --preset {{ preset }} --target test_bootstrap_attach
    cmake --build --preset {{ preset }} --target test_host_json --target test_host_stop_line --target test_bootstrap_diagnostics --target test_host_crash_capture --target test_host_crash_send --target test_host_crash_report --target test_host_runtime_load --target test_host_minidump --target test_host_rendezvous --target test_host_exports --target test_host_crash --target test_rendezvous --target test_host_runtime_verify
    # The host units carry no test seam when NEVR_HOST_TESTING is not defined: each is compiled again
    # without it with the build's own flags and its object is read with nm (docs/adr/0008). Fail-close.
    python3 tools/verify_host_seams.py "build/{{ preset }}"
    cmake --build --preset {{ preset }} --target test_export_trace
    # Signed fixtures for the plugin signature gate (ADR 0010, #741), made once for test_plugin_signature and the
    # gate tests in test_behavioral: a throwaway PKI and osslsigncode, offline, over the real test plugin. A
    # missing tool or a failed generation fails the recipe; the tests fail, never skip, without the directory.
    source tools/lib/work_root.sh
    work_dir "$NEVR_WORK_ROOT/test-scratch" "Temporary directories of the tests; each test removes its own when it ends." \
        "a directory in here left behind by an interrupted test run can be removed at any time; this directory when no test run is live."
    sigfix="$(mktemp -d "$NEVR_WORK_ROOT/test-scratch/test-plugin-signature.XXXXXX")"
    trap 'rm -rf "$sigfix"' EXIT
    # test_nevr_config writes its fixture files here (a Linux path; Wine reaches it as Z:); removed with sigfix.
    export NEVR_CONFIG_TEST_DIR="$sigfix/nevr-config-tests"
    python3 tools/gen_plugin_signature_fixtures.py "$sigfix" --plugin-dll "build/{{ preset }}/bin/plugins/test_plugin_onframe.dll" --plugin-dll "build/{{ preset }}/bin/rendezvous/fixtures/good/nevr-runtime.dll"
    export NEVR_SIGFIX_DIR="$sigfix"
    # The real signed v5.4.3 BugSplat64.dll (#745), fetched by digest once and kept under build/: test_plugin_trust
    # verifies the production pins against it. A failed fetch or a digest that does not match fails the recipe, and
    # a RealSigned test without the file fails. A release build (build.yml sets NEVR_RELEASE_BUILD=1 on the Verify
    # step of a release or a draft) never fetches from a host outside GitHub: it excludes the RealSigned tests and
    # says so, and the PR/local run is where they run.
    if [[ "${NEVR_RELEASE_BUILD:-}" == "1" ]]; then
        echo "test-auth-unit: release build: the RealSigned tests are excluded (no fixture fetch outside GitHub); PR and local runs run them" >&2
        export GTEST_FILTER='-RealSigned.*'
    else
        NEVR_REAL_SIGNED_DLL="$(python3 tools/fetch_real_signed_dll.py build/real-signed-cache)"
        export NEVR_REAL_SIGNED_DLL
    fi
    # #1414: the invite service's host tests (patch/invite_service.cpp against a loopback server). Built and run here
    # because no other line of this recipe names them.
    cmake --build --preset {{ preset }} --target test_invite_service
    bin="build/{{ preset }}/bin/test_invite_service.exe"
    if [[ ! -f "$bin" ]]; then
        echo "ERROR: GTest binary not found: $bin" >&2
        exit 1
    fi
    run_test "$bin"
    bin="build/{{ preset }}/bin/test_xpid_patch.exe"
    if [[ ! -f "$bin" ]]; then
        echo "ERROR: GTest binary not found: $bin" >&2
        echo "       (is 'gtest' available in vcpkg for triplet x64-mingw-static?)" >&2
        exit 1
    fi
    run_test "$bin"
    bin="build/{{ preset }}/bin/test_mic_dsp.exe"
    if [[ ! -f "$bin" ]]; then
        echo "ERROR: GTest binary not found: $bin" >&2
        exit 1
    fi
    run_test "$bin"
    bin="build/{{ preset }}/bin/test_game_image_guard.exe"
    if [[ ! -f "$bin" ]]; then
        echo "ERROR: GTest binary not found: $bin" >&2
        exit 1
    fi
    run_test "$bin"
    bin="build/{{ preset }}/bin/test_runtime_abi.exe"
    if [[ ! -f "$bin" ]]; then
        echo "ERROR: GTest binary not found: $bin" >&2
        exit 1
    fi
    run_test "$bin"
    bin="build/{{ preset }}/bin/test_runtime_export_guard.exe"
    if [[ ! -f "$bin" ]]; then
        echo "ERROR: GTest binary not found: $bin" >&2
        exit 1
    fi
    run_test "$bin"
    # The attach-time MinHook proof gate (not a GTest: its own PASS/FAIL lines are the evidence). The
    # suite runs each scenario in a child process and fails on a hang, a loader-lock inversion or a
    # partially installed detour.
    bin="build/{{ preset }}/bin/bootstrap-attach/test_bootstrap_attach.exe"
    if [[ ! -f "$bin" ]]; then
        echo "ERROR: bootstrap attach harness not found: $bin" >&2
        exit 1
    fi
    run_test "$bin"
    # The stable BugSplat64.dll host (src/runtime/host/): the fixed-buffer JSON writer, the
    # diagnostic recorder, the minidump capture, the runtime load state machine, the rendezvous's prologue
    # validation, the export thunks and the minidump parser the crash harness trusts.
    for name in test_host_json test_host_stop_line test_bootstrap_diagnostics test_host_crash_capture test_host_crash_send test_host_crash_report test_host_runtime_load test_host_minidump test_host_rendezvous test_host_runtime_verify test_host_exports; do
        bin="build/{{ preset }}/bin/$name.exe"
        if [[ ! -f "$bin" ]]; then
            echo "ERROR: GTest binary not found: $bin" >&2
            exit 1
        fi
        run_test "$bin"
    done
    # The two child-process harnesses (their own PASS/FAIL lines are the evidence): a deliberate
    # fault through the real createReport export for ten destination and runtime scenarios, and the
    # GameMainWrapper rendezvous with the NEVR_SetGameModule launcher contract.
    for bin in "build/{{ preset }}/bin/host-crash/test_host_crash.exe" "build/{{ preset }}/bin/rendezvous/test_rendezvous.exe"; do
        if [[ ! -f "$bin" ]]; then
            echo "ERROR: host harness not found: $bin" >&2
            exit 1
        fi
        run_test "$bin"
    done
    bin="build/{{ preset }}/bin/test_export_trace.exe"
    if [[ ! -f "$bin" ]]; then
        echo "ERROR: GTest binary not found: $bin" >&2
        exit 1
    fi
    run_test "$bin"
    bin="build/{{ preset }}/bin/test_token_auth.exe"
    if [[ ! -f "$bin" ]]; then
        echo "ERROR: GTest binary not found: $bin" >&2
        exit 1
    fi
    run_test "$bin"
    bin="build/{{ preset }}/bin/test_messages.exe"
    if [[ ! -f "$bin" ]]; then
        echo "ERROR: GTest binary not found: $bin" >&2
        exit 1
    fi
    run_test "$bin"
    bin="build/{{ preset }}/bin/test_crash_recovery.exe"
    if [[ ! -f "$bin" ]]; then
        echo "ERROR: GTest binary not found: $bin" >&2
        exit 1
    fi
    run_test "$bin"
    bin="build/{{ preset }}/bin/test_parse_endpoint.exe"
    if [[ ! -f "$bin" ]]; then
        echo "ERROR: GTest binary not found: $bin" >&2
        echo "       (is 'gtest' available in vcpkg for triplet x64-mingw-static?)" >&2
        exit 1
    fi
    run_test "$bin"
    bin="build/{{ preset }}/bin/test_behavioral.exe"
    if [[ ! -f "$bin" ]]; then
        echo "ERROR: GTest binary not found: $bin" >&2
        echo "       (is 'gtest' available in vcpkg for triplet x64-mingw-static?)" >&2
        exit 1
    fi
    run_test "$bin"
    bin="build/{{ preset }}/bin/test_nevr_config.exe"
    if [[ ! -f "$bin" ]]; then
        echo "ERROR: GTest binary not found: $bin" >&2
        echo "       (is 'gtest'/'yaml-cpp' available in vcpkg for triplet x64-mingw-static?)" >&2
        exit 1
    fi
    run_test "$bin"
    bin="build/{{ preset }}/bin/test_service_map.exe"
    if [[ ! -f "$bin" ]]; then
        echo "ERROR: GTest binary not found: $bin" >&2
        echo "       (is 'gtest'/'yaml-cpp' available in vcpkg for triplet x64-mingw-static?)" >&2
        exit 1
    fi
    run_test "$bin"
    bin="build/{{ preset }}/bin/test_service_config.exe"
    if [[ ! -f "$bin" ]]; then
        echo "ERROR: GTest binary not found: $bin" >&2
        exit 1
    fi
    run_test "$bin"
    bin="build/{{ preset }}/bin/test_social_facade.exe"
    if [[ ! -f "$bin" ]]; then
        echo "ERROR: GTest binary not found: $bin" >&2
        exit 1
    fi
    run_test "$bin"
    bin="build/{{ preset }}/bin/test_early_quit_lockout.exe"
    if [[ ! -f "$bin" ]]; then
        echo "ERROR: GTest binary not found: $bin" >&2
        exit 1
    fi
    run_test "$bin"
    bin="build/{{ preset }}/bin/test_lobby_full_backoff.exe"
    if [[ ! -f "$bin" ]]; then
        echo "ERROR: GTest binary not found: $bin" >&2
        exit 1
    fi
    run_test "$bin"
    bin="build/{{ preset }}/bin/test_menu_version_label.exe"
    if [[ ! -f "$bin" ]]; then
        echo "ERROR: GTest binary not found: $bin" >&2
        exit 1
    fi
    run_test "$bin"
    bin="build/{{ preset }}/bin/test_splash_skip.exe"
    if [[ ! -f "$bin" ]]; then
        echo "ERROR: GTest binary not found: $bin" >&2
        exit 1
    fi
    run_test "$bin"
    bin="build/{{ preset }}/bin/test_ui_texture_mips.exe"
    if [[ ! -f "$bin" ]]; then
        echo "ERROR: GTest binary not found: $bin" >&2
        exit 1
    fi
    run_test "$bin"
    bin="build/{{ preset }}/bin/test_cmdline_strip.exe"
    if [[ ! -f "$bin" ]]; then
        echo "ERROR: GTest binary not found: $bin" >&2
        exit 1
    fi
    run_test "$bin"
    bin="build/{{ preset }}/bin/test_scenario_early_quit.exe"
    if [[ ! -f "$bin" ]]; then
        echo "ERROR: GTest binary not found: $bin" >&2
        exit 1
    fi
    run_test "$bin"
    bin="build/{{ preset }}/bin/test_schannel_cred_guard.exe"
    if [[ ! -f "$bin" ]]; then
        echo "ERROR: GTest binary not found: $bin" >&2
        exit 1
    fi
    run_test "$bin"
    bin="build/{{ preset }}/bin/test_hooking.exe"
    if [[ ! -f "$bin" ]]; then
        echo "ERROR: GTest binary not found: $bin" >&2
        exit 1
    fi
    run_test "$bin"
    bin="build/{{ preset }}/bin/test_dll_load_hook.exe"
    if [[ ! -f "$bin" ]]; then
        echo "ERROR: GTest binary not found: $bin" >&2
        exit 1
    fi
    run_test "$bin"
    bin="build/{{ preset }}/bin/test_plugin_load_plan.exe"
    if [[ ! -f "$bin" ]]; then
        echo "ERROR: GTest binary not found: $bin" >&2
        echo "       (is 'gtest'/'yaml-cpp' available in vcpkg for triplet x64-mingw-static?)" >&2
        exit 1
    fi
    run_test "$bin"
    for test_name in test_boot_log_routing test_system_module_loader test_login_redirect_override test_websocket_frame test_protobuf_transport test_websocket_client_auth test_url_diagnostics test_serverdb_uri test_self_check test_crash_send test_party_share_check test_party_leave_check test_quit_check test_friend_request_check test_credential_cache_check test_status_redirect_check test_callback_unregistration test_server_context test_session_unregister test_mic_lifecycle test_telemetry_snapshot_store test_telemetry_game_properties test_coop_ai_trace test_frame_stats test_evr_codec test_legacy_codec test_legacy_session test_symbol_hash test_socket_role test_login_refusal test_invite_wizard test_invite_worker test_invite_service test_ui_text test_invite_choices test_invite_join test_asset_cache test_session_router test_asset_registry test_installed_levels test_telemetry_game_type test_arena_round_time test_nevr_root test_arena_public_clock test_arena_public_clock_hook test_render_config test_render_config_hook test_server_team_size test_server_bots test_deadlock_monitor; do
        bin="build/{{ preset }}/bin/${test_name}.exe"
        if [[ ! -f "$bin" ]]; then
            echo "ERROR: GTest binary not found: $bin" >&2
            exit 1
        fi
        run_test "$bin"
    done
    bin="build/{{ preset }}/bin/test_plugin_signature.exe"
    if [[ ! -f "$bin" ]]; then
        echo "ERROR: GTest binary not found: $bin" >&2
        exit 1
    fi
    run_test "$bin"
    bin="build/{{ preset }}/bin/test_plugin_tree.exe"
    if [[ ! -f "$bin" ]]; then
        echo "ERROR: GTest binary not found: $bin" >&2
        exit 1
    fi
    run_test "$bin"
    bin="build/{{ preset }}/bin/test_plugin_trust.exe"
    if [[ ! -f "$bin" ]]; then
        echo "ERROR: GTest binary not found: $bin" >&2
        exit 1
    fi
    run_test "$bin"
    # #1415 (docs/adr/0017): the invite worker and the per-frame applier, built and run here. No recipe named them before, so
    # they never ran in test-auth-unit; the applier's test drives the real invite service on a loopback server.
    cmake --build --preset {{ preset }} --target test_invite_worker --target test_invite_answer_apply
    for bin in "build/{{ preset }}/bin/test_invite_worker.exe" "build/{{ preset }}/bin/test_invite_answer_apply.exe"; do
        if [[ ! -f "$bin" ]]; then
            echo "ERROR: GTest binary not found: $bin" >&2
            exit 1
        fi
        run_test "$bin"
    done
    # test_broadcaster_bridge / test_broadcaster_guards live in
    # ~/src/nevr-runtime-plugins with the broadcaster-bridge plugin, which
    # holds and tests the N72/N73 guards.
    # #1228: every binary above has run; name each failure and fail the recipe once, at the end.
    if [[ "${#test_failures[@]}" -gt 0 ]]; then
        echo "test-auth-unit: FAIL — ${#test_failures[@]} test binary(ies) failed: ${test_failures[*]}" >&2
        exit 1
    fi

# Run auth integration tests (needs game binary + MCP harness)
test-auth-integration:
    cd tests/system && go test -v -run "TestAuth" ./...

# Run all auth tests
test-auth: test-auth-groundtruth test-auth-unit

# yaml-cpp, zlib and nlohmann-json for the Quest host tests and the libr15 patcher: installed from the tools/quest-host-deps manifest for the x64-linux triplet into
# build/quest-host-vcpkg (never a system package). Its vcpkg root is build/vcpkg-root-quest-host, not _vcpkg-mingw's
# build/vcpkg-root: vcpkg holds flock(2) on <root>/.vcpkg-root for a whole install, and a child it started can keep
# that lock after it exits, which can make this install fail with EBUSY on the mingw install's root (#1408). The sentinel parses
# config.yaml with the PC's nevr_config, so the host tests that compile quest_config.cpp link it.
_vcpkg-quest-host:
    #!/usr/bin/env bash
    set -euo pipefail
    jd="{{ justfile_directory() }}"
    manifest="$jd/tools/quest-host-deps/vcpkg.json"
    lib="$jd/build/quest-host-vcpkg/x64-linux/lib/libyaml-cpp.a"
    json="$jd/build/quest-host-vcpkg/x64-linux/include/nlohmann/json.hpp"
    # Idempotent and race-safe. Every consumer (test-quest-shared/redirect/integration, quest-patcher)
    # checks one of the two artifacts below: when both exist and the library is no older than the
    # manifest, there is nothing to install. The header is part of the test so an install made from
    # a manifest without nlohmann-json is completed. `just verify` builds
    # it once, serially, before the parallel block, so those workers hit this fast path and
    # never run vcpkg concurrently. When the install is needed it is serialized on a lock
    # file so two concurrent test-quest-* recipes cannot build the shared host install at
    # once (vcpkg would otherwise contend on its own root lock or race in its buildtrees).
    if [[ -f "$lib" && -f "$json" && ! "$lib" -ot "$manifest" ]]; then
        exit 0
    fi
    mkdir -p "$jd/build"
    lock="$jd/build/quest-host-vcpkg.install.lock"
    exec 9>"$lock"
    flock 9
    if [[ -f "$lib" && -f "$json" && ! "$lib" -ot "$manifest" ]]; then
        exit 0
    fi
    root="$("$jd/tools/vcpkg_root.sh" vcpkg-root-quest-host)"
    cd "$root"
    unset VCPKG_ROOT
    ./vcpkg install --triplet=x64-linux --host-triplet=x64-linux \
        --x-manifest-root="$jd/tools/quest-host-deps" \
        --x-install-root="$jd/build/quest-host-vcpkg"

# Host tests for the Quest first-run data step's core (src/quest/data: sha256, the zip extractor, the pins and decision, the
# staging swap; docs/adr/0011-quest-game-data-first-run.md). zlib comes from the tools/quest-host-deps manifest for the
# x64-linux triplet, never a system package. Fail-close: a compile error, a missing library or any failed check exits nonzero.
test-quest-data: _vcpkg-quest-host
    #!/usr/bin/env bash
    set -euo pipefail
    out="build/quest-data-host"
    mkdir -p "$out"
    host="{{ justfile_directory() }}/build/quest-host-vcpkg/x64-linux"
    [[ -f "$host/lib/libz.a" ]] || { echo "test-quest-data: FAIL — $host/lib/libz.a not found (just _vcpkg-quest-host installs it)" >&2; exit 1; }
    for f in sha256 zip staging fetch data_step pack; do
        g++ -std=c++17 -Wall -Wextra -Werror -fno-exceptions -Isrc -Isdk/include -isystem "$host/include" -c \
            "src/quest/data/$f.cpp" -o "$out/$f.o"
    done
    g++ -std=c++17 -Wall -Wextra -Werror -Isrc -Isdk/include -isystem "$host/include" \
        "$out/sha256.o" "$out/zip.o" "$out/staging.o" src/quest/tests/data_test.cpp "$host/lib/libz.a" \
        -o "$out/data_test"
    timeout -k 5 300 "$out/data_test"
    g++ -std=c++17 -Wall -Wextra -Werror -Isrc -Isdk/include -isystem "$host/include" \
        "$out/sha256.o" "$out/zip.o" "$out/staging.o" "$out/fetch.o" src/quest/tests/fetch_test.cpp "$host/lib/libz.a" \
        -o "$out/fetch_test"
    timeout -k 5 300 "$out/fetch_test"
    g++ -std=c++17 -Wall -Wextra -Werror -Isrc -Isdk/include -isystem "$host/include" \
        "$out/sha256.o" "$out/zip.o" "$out/staging.o" "$out/fetch.o" "$out/data_step.o" src/quest/tests/data_step_test.cpp "$host/lib/libz.a" \
        -o "$out/data_step_test"
    timeout -k 5 300 "$out/data_step_test"
    g++ -std=c++17 -Wall -Wextra -Werror -Isrc -Isdk/include -isystem "$host/include" \
        "$out/sha256.o" "$out/zip.o" "$out/staging.o" "$out/fetch.o" "$out/pack.o" src/quest/tests/pack_test.cpp "$host/lib/libz.a" \
        -o "$out/pack_test"
    timeout -k 5 300 "$out/pack_test"

# The data fetch over real HTTP (src/quest/data/curl_transport.cpp and fetch.cpp against src/quest/tests/range_server.py):
# explicit Range, a server that ignores it, a dropped connection, an error page, a refusal with no trust anchors or over
# plain http, and a whole fetch that resumes. Needs system libcurl, openssl, python3 and zlib development files, as
# test-quest-tls does for libcurl; zlib comes from the host vcpkg install.
test-quest-data-net: _vcpkg-quest-host
    #!/usr/bin/env bash
    set -euo pipefail
    for tool in openssl python3 pkg-config g++; do
        command -v "$tool" >/dev/null || { echo "test-quest-data-net: missing tool: $tool" >&2; exit 1; }
    done
    pkg-config --exists libcurl || { echo "test-quest-data-net: libcurl development files not found (pkg-config libcurl)" >&2; exit 1; }
    out="build/quest-data-net-host"
    rm -rf "$out"
    mkdir -p "$out"
    host="{{ justfile_directory() }}/build/quest-host-vcpkg/x64-linux"
    [[ -f "$host/lib/libz.a" ]] || { echo "test-quest-data-net: FAIL — $host/lib/libz.a not found (just _vcpkg-quest-host installs it)" >&2; exit 1; }
    g++ -std=c++17 -Wall -Wextra -Werror -pthread -Isrc -Isdk/include -isystem "$host/include" $(pkg-config --cflags libcurl) \
        src/quest/data/sha256.cpp src/quest/data/zip.cpp src/quest/data/staging.cpp src/quest/data/fetch.cpp \
        src/quest/data/curl_transport.cpp src/quest/data/data_host_roots.cpp src/quest/tests/curl_transport_test.cpp \
        "$host/lib/libz.a" -o "$out/curl_transport_test" $(pkg-config --libs libcurl) -lssl -lcrypto
    cd "$out"
    export NEVR_TEST_TMP="$PWD"
    # A throwaway CA and the leaves the HTTPS tests need: one valid, one expired, one for another host, one self-signed.
    openssl req -x509 -newkey rsa:2048 -nodes -keyout trust.key -out trust.pem -subj "/CN=nevr-data-test-ca" -days 2 \
        -addext "basicConstraints=critical,CA:TRUE" -addext "keyUsage=critical,keyCertSign,cRLSign" 2>/dev/null
    printf 'subjectAltName=DNS:localhost,IP:127.0.0.1\nbasicConstraints=CA:FALSE\n' > ext-good.cnf
    printf 'subjectAltName=DNS:other.invalid\nbasicConstraints=CA:FALSE\n' > ext-other.cnf
    openssl req -newkey rsa:2048 -nodes -keyout leaf.key -out leaf.csr -subj "/CN=localhost" 2>/dev/null
    openssl x509 -req -in leaf.csr -CA trust.pem -CAkey trust.key -CAcreateserial -days 2 -extfile ext-good.cnf -out good.pem 2>/dev/null
    openssl x509 -req -in leaf.csr -CA trust.pem -CAkey trust.key -CAcreateserial -not_before 20200101000000Z \
        -not_after 20200102000000Z -extfile ext-good.cnf -out expired.pem 2>/dev/null
    openssl x509 -req -in leaf.csr -CA trust.pem -CAkey trust.key -CAcreateserial -days 2 -extfile ext-other.cnf -out wronghost.pem 2>/dev/null
    openssl req -x509 -newkey rsa:2048 -nodes -keyout selfsigned.key -out selfsigned.pem -subj "/CN=localhost" -days 2 \
        -addext "subjectAltName=DNS:localhost,IP:127.0.0.1" 2>/dev/null
    ./curl_transport_test write-zip payload.zip
    : > requests.log
    servers=()
    start() {  # name, extra args: a server writes <name>.port and logs to <name>.log
        local name="$1"; shift
        : > "$name.log"
        python3 -I -W ignore ../../src/quest/tests/range_server.py "$name.port" "$name.log" payload.zip "$@" &
        servers+=("$!")
    }
    trap 'kill "${servers[@]}" 2>/dev/null || true' EXIT
    start plain
    start good good.pem leaf.key
    start expired expired.pem leaf.key
    start wronghost wronghost.pem leaf.key
    start selfsigned selfsigned.pem selfsigned.key
    start tls11 good.pem leaf.key tls11
    for name in plain good expired wronghost selfsigned tls11; do
        for _ in $(seq 1 100); do [ -s "$name.port" ] && break; sleep 0.1; done
        [ -s "$name.port" ] || { echo "test-quest-data-net: the $name server did not start" >&2; exit 1; }
    done
    # The host's OpenSSL may already refuse TLS 1.1 on its own; a permissive config leaves only the transport's own
    # floor (CURLOPT_SSLVERSION) between the client and the TLS 1.1 server.
    printf 'openssl_conf = nevr_lax\n[nevr_lax]\nssl_conf = nevr_ssl\n[nevr_ssl]\nsystem_default = nevr_sys\n[nevr_sys]\nMinProtocol = TLSv1\nCipherString = ALL:@SECLEVEL=0\n' > lax-openssl.cnf
    OPENSSL_CONF="$PWD/lax-openssl.cnf" timeout -k 5 300 ./curl_transport_test "$(cat plain.port)" plain.log trust.pem "$PWD"
    # The step as the game reaches it: through the permission-wait handler's thunk entry (src/quest/sentinel/data_wiring.cpp).
    # The hooks it links call the pinned factories, which take the patched libr15's identity (sentinel::pinned::LibR15PatchedBuildId,
    # docs/adr/0021 section 7): libr15_identity.cpp and libr15_patch_identity.cpp below. sha256.cpp is already in this link.
    cd "{{ justfile_directory() }}"
    g++ -std=c++17 -Wall -Wextra -Werror -fno-exceptions -pthread -Isrc -Isdk/include -Isrc/quest/sentinel -isystem "$host/include" \
        $(pkg-config --cflags libcurl) src/quest/tests/data_wiring_test.cpp src/quest/sentinel/data_wiring.cpp \
        src/quest/sentinel/obb_skip_hook.cpp src/quest/sentinel/permission_wait_hook.cpp src/quest/sentinel/process_end_hook.cpp \
        src/quest/sentinel/got_hook.cpp src/quest/sentinel/hook_log.cpp src/quest/sentinel/hook_report.cpp \
        src/quest/sentinel/libr15_identity.cpp src/quest/patch/libr15_patch_identity.cpp \
        src/quest/data/sha256.cpp src/quest/data/zip.cpp src/quest/data/staging.cpp src/quest/data/fetch.cpp \
        src/quest/data/data_step.cpp src/quest/data/pack.cpp src/quest/data/curl_transport.cpp src/quest/data/data_host_roots.cpp \
        "$host/lib/libz.a" -o "$out/data_wiring_test" $(pkg-config --libs libcurl) -lssl -lcrypto -ldl
    NEVR_TEST_TMP="$out" timeout -k 5 300 "$out/data_wiring_test"
    echo "test-quest-data-net: the fetch passes over real HTTP on the host"

# The shared invite service for the Quest, on the host (docs/adr/0017, #966, S1): src/runtime/patch/invite_service.cpp and its libcurl
# transport, compiled with the host g++ against the system libcurl (pkg-config, as test-quest-tls does) and nlohmann-json from the
# quest-host install, then run: the link and the off path (src/quest/tests/invite_service_link_test.cpp). Fail-close.
test-quest-invite: _vcpkg-quest-host
    #!/usr/bin/env bash
    set -euo pipefail
    command -v g++ >/dev/null || { echo "test-quest-invite: missing tool: g++" >&2; exit 1; }
    pkg-config --exists libcurl || { echo "test-quest-invite: libcurl development files not found (pkg-config libcurl)" >&2; exit 1; }
    out="build/quest-invite-host"
    rm -rf "$out"
    mkdir -p "$out"
    json_inc="{{ justfile_directory() }}/build/quest-host-vcpkg/x64-linux/include"
    [[ -f "$json_inc/nlohmann/json.hpp" ]] || { echo "test-quest-invite: FAIL — $json_inc/nlohmann/json.hpp not found (just _vcpkg-quest-host installs it)" >&2; exit 1; }
    g++ -std=c++17 -Wall -Wextra -Werror -pthread -Isrc -Isdk/include -isystem "$json_inc" $(pkg-config --cflags libcurl) \
        src/runtime/patch/invite_service.cpp src/runtime/patch/invite_transport_curl.cpp \
        src/quest/tests/invite_service_link_test.cpp -o "$out/invite_service_link_test" $(pkg-config --libs libcurl)
    timeout -k 5 120 "$out/invite_service_link_test"
    echo "test-quest-invite: invite service link and off path pass on the host"

# Run the Quest redirect test on the host. src/quest/tests/service_redirect_test.cpp is
# compiled for Android by src/quest/CMakeLists.txt but cannot execute there; this
# compiles the same test, the same vectors (service_redirect_vectors.h) and the same
# shared source (src/runtime/lifecycle/service_redirect.cpp) with the host g++ and runs
# it. No NDK. Fail-close: a compile error or any vector mismatch exits nonzero.
test-quest-shared: _vcpkg-quest-host
    #!/usr/bin/env bash
    set -euo pipefail
    out="build/quest-shared-host"
    mkdir -p "$out"
    # config.yaml is parsed by src/core/nevr_config.cpp with yaml-cpp (the PC's parser, shared). yaml-cpp comes
    # from the tools/quest-host-deps manifest for the x64-linux triplet, never a system package.
    yaml_host="{{ justfile_directory() }}/build/quest-host-vcpkg/x64-linux"
    yaml_lib="$yaml_host/lib/libyaml-cpp.a"
    [[ -f "$yaml_lib" ]] || { echo "test-quest-shared: FAIL — $yaml_lib not found (just _vcpkg-quest-host installs it)" >&2; exit 1; }
    yaml_inc=(-isystem "$yaml_host/include")
    g++ -std=c++17 -Wall -Wextra -Werror -Isrc -Isdk/include \
        src/runtime/lifecycle/service_redirect.cpp \
        src/quest/tests/service_redirect_test.cpp \
        -o "$out/service_redirect_test"
    timeout -k 5 120 "$out/service_redirect_test"
    # The shared crash report (#1605): what it prints, what it writes and in what order, with the platform seams faked.
    # The same source builds into the PC runtime (nevr_core) and the Quest sentinel. The ELF reader is the Quest side's
    # build id, read from a synthetic image.
    g++ -std=c++17 -Wall -Wextra -Werror -Isrc -Isdk/include \
        src/core/crash_report.cpp src/quest/tests/crash_report_test.cpp -o "$out/crash_report_test"
    timeout -k 5 120 "$out/crash_report_test"
    g++ -std=c++17 -Wall -Wextra -Werror -Isrc -Isdk/include \
        src/quest/sentinel/elf_build_id.cpp src/quest/tests/elf_build_id_test.cpp -o "$out/elf_build_id_test"
    timeout -k 5 120 "$out/elf_build_id_test"
    # The codec, config and sentinel tests parse JSON with nlohmann::json. Use the header the build installed
    # from vcpkg.json for the mingw triplet the mingw-* presets build with, never a system package and
    # never whichever triplet sorts first. Only the nlohmann directory is exposed to the host compiler.
    # Fail loudly when the build has not installed it.
    json_src="build/{{ preset }}/vcpkg_installed/x64-mingw-static/include/nlohmann"
    if [[ ! -f "$json_src/json.hpp" ]]; then
        echo "test-quest-shared: FAIL — $json_src/json.hpp not found; run 'just build' with a mingw-* preset first (the header comes from vcpkg.json for the x64-mingw-static triplet, not a system package)" >&2
        exit 1
    fi
    json_inc="$out/json_inc"
    mkdir -p "$json_inc"
    ln -sfn "$PWD/$json_src" "$json_inc/nlohmann"
    g++ -std=c++17 -Wall -Wextra -Werror -Isrc -Isdk/include -isystem "$json_inc" \
        src/runtime/compat/evr_codec.cpp \
        src/runtime/compat/login_profile.cpp \
        src/quest/tests/evr_codec_test.cpp \
        -o "$out/evr_codec_test"
    timeout -k 5 120 "$out/evr_codec_test"
    g++ -std=c++17 -Wall -Wextra -Werror -Isrc -Isdk/include -isystem "$json_inc" "${yaml_inc[@]}" \
        src/runtime/lifecycle/service_redirect.cpp \
        src/quest/sentinel/quest_config.cpp src/core/nevr_config.cpp \
        src/quest/tests/quest_config_test.cpp "$yaml_lib" \
        -o "$out/quest_config_test"
    "$out/quest_config_test"
    # The hardware dump (#335): field schema, procfs/sysfs readers against a fake root, the os section's
    # never-omitted fields, and the record table (also under ThreadSanitizer: the libr15 hooks write it from
    # game threads while the dump thread reads it).
    hwdump_core=(src/quest/diag/hwdump_field.cpp src/quest/diag/hwdump_fs.cpp src/quest/diag/hwdump_os.cpp \
        src/quest/diag/hwdump_records.cpp src/quest/diag/hwdump_report.cpp src/quest/tests/hwdump_core_test.cpp)
    g++ -std=c++17 -Wall -Wextra -Werror -Isrc -Isdk/include -isystem "$json_inc" "${hwdump_core[@]}" -pthread -o "$out/hwdump_core_test"
    timeout -k 5 120 "$out/hwdump_core_test"
    g++ -std=c++17 -Wall -Wextra -Werror -Isrc -Isdk/include -isystem "$json_inc" -fsanitize=thread -g -O1 "${hwdump_core[@]}" \
        -pthread -o "$out/hwdump_core_test_tsan"
    timeout -k 5 300 "$out/hwdump_core_test_tsan"
    # The libr15 hook handlers, built like the sentinel's copy (-fno-exceptions): arguments pass through bit
    # for bit, results come back unchanged, nothing is read past the game's buffers.
    g++ -std=c++17 -Wall -Wextra -Werror -fno-exceptions -Isrc -Isdk/include src/quest/diag/hwdump_records.cpp \
        src/quest/diag/hwdump_handlers.cpp src/quest/tests/hwdump_handlers_test.cpp -o "$out/hwdump_handlers_test"
    timeout -k 5 120 "$out/hwdump_handlers_test"
    # Sentinel activation + logging against a stand-in liblog. The test's constructor has priority
    # 102, so it runs before activation.cpp's static initializers whatever the link order.
    files="$out/sentinel-files"
    rm -rf "$files"
    g++ -std=c++17 -Wall -Wextra -Werror -Isrc -Isdk/include -isystem "$json_inc" "${yaml_inc[@]}" -Isrc/quest/tests/stub -Isrc/quest/sentinel \
        -DNEVR_QUEST_FILES_DIR="\"$files\"" \
        src/quest/tests/sentinel_host_test.cpp \
        src/quest/sentinel/activation.cpp \
        src/quest/sentinel/sentinel_log.cpp src/core/crash_report.cpp \
        src/quest/sentinel/quest_config.cpp src/core/nevr_config.cpp \
        src/runtime/lifecycle/service_redirect.cpp "$yaml_lib" \
        -o "$out/sentinel_host_test"
    "$out/sentinel_host_test"
    # The platform-neutral token-auth core and the Quest session, under a fake HTTP
    # server and a fake clock. Same sources the NDK build compiles (src/quest/CMakeLists.txt).
    g++ -std=c++17 -Wall -Wextra -Werror -pthread -Isrc -Isdk/include -isystem "$json_inc" \
        src/core/auth_refresh.cpp src/core/device_auth_flow.cpp src/core/device_poll_response.cpp \
        src/quest/auth/session.cpp src/quest/auth/atomic_write.cpp src/quest/auth/file_store.cpp \
        src/quest/auth/prompt_presenters.cpp src/quest/auth/prompt_board.cpp \
        src/quest/tests/auth_core_test.cpp \
        -o "$out/auth_core_test"
    "$out/auth_core_test"
    echo "test-quest-shared: token-auth core and Quest session tests pass on the host"
    # CurlHttpClient's trust handling against real TLS peers on loopback (test CA, Android-style
    # old-hash CA directory). Host libcurl with an OpenSSL backend, libssl and libcrypto.
    g++ -std=c++17 -Wall -Wextra -Werror -pthread -Isrc -Isdk/include -isystem "$json_inc" \
        src/core/auth_refresh.cpp src/core/device_auth_flow.cpp src/core/device_poll_response.cpp \
        src/quest/auth/session.cpp src/quest/auth/atomic_write.cpp src/quest/auth/file_store.cpp src/quest/auth/quest_token_auth.cpp \
        src/quest/auth/prompt_presenters.cpp src/quest/auth/prompt_board.cpp \
        src/quest/auth/curl_http.cpp src/quest/auth/ca_bundle.cpp src/quest/tests/tls_ca_test.cpp \
        -lcurl -lssl -lcrypto -o "$out/tls_ca_test"
    "$out/tls_ca_test"
    echo "test-quest-shared: CurlHttpClient TLS trust tests pass on the host"
    # Quest login rewrite: the compose half (login_rewrite.cpp, exceptions enabled) and the apply
    # half (login_apply.cpp, built -fno-exceptions exactly as on the device, because it runs while
    # game code is live) plus the shared PCVR login builder, against a fake CJson. The Android
    # adapter (login_hook.cpp) cannot run on the host; build-android compiles it. The login tests
    # also link the prerequisite handlers and the stand-ins (both -fno-exceptions as on the device),
    # because the send gate and the end-to-end flow test drive them together with the rewrite.
    for f in login/login_apply login/login_prerequisites login/login_standin; do
        g++ -std=c++17 -Wall -Wextra -Werror -fno-exceptions -DNEVR_QUEST_TESTING -Isrc -Isdk/include -c \
            "src/quest/$f.cpp" -o "$out/$(basename "$f").o"
    done
    g++ -std=c++17 -Wall -Wextra -Werror -fno-exceptions -Isrc -Isdk/include -c \
        src/quest/sentinel/hook_log.cpp -o "$out/hook_log.o"
    g++ -std=c++17 -Wall -Wextra -Werror -DNEVR_QUEST_TESTING -Isrc -Isdk/include -isystem "$json_inc" \
        "$out/login_apply.o" "$out/login_prerequisites.o" "$out/login_standin.o" "$out/hook_log.o" \
        src/quest/login/login_rewrite.cpp \
        src/runtime/compat/login_profile.cpp \
        src/quest/tests/login_rewrite_test.cpp \
        -o "$out/login_rewrite_test"
    "$out/login_rewrite_test"
    # Quest login prerequisites: the handler bodies driven through a fake Platform SDK and the game's
    # four callbacks. The install (login_prerequisites_install.cpp) needs libpnsovr.so for real slots;
    # build-android compiles it and test-quest-hooks-pinned resolves its slots in the real library.
    g++ -std=c++17 -Wall -Wextra -Werror -DNEVR_QUEST_TESTING -Isrc -Isdk/include -isystem "$json_inc" \
        "$out/login_prerequisites.o" "$out/login_standin.o" "$out/hook_log.o" \
        src/quest/tests/login_prerequisites_test.cpp \
        -o "$out/login_prerequisites_test"
    "$out/login_prerequisites_test"
    # The install against a host with no libpnsovr.so loaded: fully partial, fail-safe, idempotent.
    # The thunk-defining TU and the backend are -fno-exceptions (callback_thunk.h); the test too.
    # NEVR_QUEST_TESTING: the install's slot seam (login_prerequisites.h) is compiled for the test only.
    g++ -std=c++17 -Wall -Wextra -Werror -fno-exceptions -DNEVR_QUEST_TESTING -Isrc -Isdk/include -Isrc/quest/sentinel -c \
        src/quest/login/login_prerequisites_install.cpp -o "$out/login_prerequisites_install.o"
    for f in got_hook hook_report; do
        g++ -std=c++17 -Wall -Wextra -Werror -fno-exceptions -Isrc -Isdk/include -Isrc/quest/sentinel -c \
            "src/quest/sentinel/$f.cpp" -o "$out/$f.o"
    done
    g++ -std=c++17 -Wall -Wextra -Werror -fno-exceptions -DNEVR_QUEST_TESTING -Isrc -Isdk/include -Isrc/quest/sentinel \
        "$out/login_prerequisites.o" "$out/login_prerequisites_install.o" "$out/login_standin.o" \
        "$out/got_hook.o" "$out/hook_log.o" "$out/hook_report.o" \
        src/quest/tests/login_prerequisites_install_test.cpp \
        -o "$out/login_prerequisites_install_test" -ldl -pthread
    "$out/login_prerequisites_install_test"
    # The per-process stand-ins and their predicates. NEVR_QUEST_TESTING exposes SetForTest/ResetForTest,
    # which exist only in the test build.
    g++ -std=c++17 -Wall -Wextra -Werror -fno-exceptions -DNEVR_QUEST_TESTING -Isrc -Isdk/include \
        "$out/login_standin.o" src/quest/tests/login_standin_test.cpp -o "$out/login_standin_test"
    "$out/login_standin_test"
    # CSymbol64Hash on bytes >= 0x80 (#657): the vectors are static_asserts and a run-time loop. The NDK compile
    # of the same file is in test-android (plain char is unsigned there).
    g++ -std=c++17 -Wall -Wextra -Werror -Isrc -Isdk/include src/quest/tests/symbol_hash_test.cpp -o "$out/symbol_hash_test"
    "$out/symbol_hash_test"
    # The same file with plain char unsigned, as under the NDK: it runs the loop with an unsigned char, which the
    # NDK compile in test-android only parses. A bare static_cast<uint64_t>(c) fails to compile here.
    g++ -std=c++17 -funsigned-char -Wall -Wextra -Werror -Isrc -Isdk/include src/quest/tests/symbol_hash_test.cpp -o "$out/symbol_hash_test_uchar"
    "$out/symbol_hash_test_uchar"
    echo "test-quest-shared: all redirect and EVR codec vectors pass on the host"

# Shared EVR session router and the Quest loopback transport on the host. Plain g++, no NDK,
# fail-close:
#   session_router_test        the router state machine through fake transports (login ordering,
#                              remote close, limits, backpressure)
#   ws_wire_test               RFC 6455 handshake and frame decoder under partial reads and bad input
#   loopback_game_server_test  the real loopback server + router + a raw TCP "game" client
#   remote_ws_test             the remote transport policy (wss only, one attempt, no downgrade) and
#                              worker through a fake connector and the real router
test-quest-router:
    #!/usr/bin/env bash
    set -euo pipefail
    out="build/quest-router-host"
    mkdir -p "$out"
    # The loopback server and its test write and parse their JSON records with nlohmann::json: the header
    # the build installed from vcpkg.json for the x64-mingw-static triplet, never a system package.
    json_src="build/{{ preset }}/vcpkg_installed/x64-mingw-static/include/nlohmann"
    if [[ ! -f "$json_src/json.hpp" ]]; then
        echo "test-quest-router: FAIL - $json_src/json.hpp not found; run 'just build' with a mingw-* preset first" >&2
        exit 1
    fi
    json_inc="$out/json_inc"
    mkdir -p "$json_inc"
    ln -sfn "$PWD/$json_src" "$json_inc/nlohmann"
    cxx=(g++ -std=c++17 -Wall -Wextra -Werror -pthread -Isrc -Isdk/include -isystem "$json_inc")
    "${cxx[@]}" src/runtime/compat/session_router.cpp src/runtime/compat/evr_codec.cpp \
        src/quest/tests/session_router_test.cpp -o "$out/session_router_test"
    "$out/session_router_test"
    # The router test again with the standard library's checked containers: an erase through an iterator the
    # container invalidated aborts there, where an unchecked build only gives wrong answers.
    "${cxx[@]}" -D_GLIBCXX_DEBUG src/runtime/compat/session_router.cpp src/runtime/compat/evr_codec.cpp \
        src/quest/tests/session_router_test.cpp -o "$out/session_router_test_checked"
    "$out/session_router_test_checked"
    "${cxx[@]}" src/quest/net/ws_wire.cpp src/quest/tests/ws_wire_test.cpp -o "$out/ws_wire_test"
    "$out/ws_wire_test"
    "${cxx[@]}" src/quest/net/ws_wire.cpp src/quest/net/loopback_game_server.cpp \
        src/runtime/compat/session_router.cpp src/runtime/compat/evr_codec.cpp \
        src/quest/tests/loopback_game_server_test.cpp -o "$out/loopback_game_server_test"
    "$out/loopback_game_server_test"
    "${cxx[@]}" src/quest/net/remote_ws.cpp src/runtime/compat/session_router.cpp \
        src/runtime/compat/evr_codec.cpp src/quest/tests/remote_ws_test.cpp -o "$out/remote_ws_test"
    "$out/remote_ws_test"
    # The loopback server under ThreadSanitizer: it has the accept thread, per-connection threads and
    # the concurrent-Start test, so a data race on listenFd_/port_/token_ or an unsynchronised
    # Start/Stop would be reported. -O1 also exercises warnings -O0 hides. halt_on_error makes any
    # race fail the recipe.
    tsan=(g++ -std=c++17 -Wall -Wextra -Werror -pthread -fsanitize=thread -O1 -g -Isrc -Isdk/include -isystem "$json_inc")
    "${tsan[@]}" src/quest/net/ws_wire.cpp src/quest/net/loopback_game_server.cpp \
        src/runtime/compat/session_router.cpp src/runtime/compat/evr_codec.cpp \
        src/quest/tests/loopback_game_server_test.cpp -o "$out/loopback_game_server_test_tsan"
    TSAN_OPTIONS="halt_on_error=1 exitcode=66" "$out/loopback_game_server_test_tsan"
    echo "test-quest-router: router, WebSocket wire, loopback server (incl. ThreadSanitizer) and remote transport tests pass on the host"

# The Quest remote WebSocket connector (libcurl over TLS) against real TLS servers on the host:
# src/quest/tests/curl_ws_tls_test.cpp, with certificates made here by openssl and the throwaway
# server src/quest/tests/tls_ws_server.py. A chain that verifies connects; a wrong CA, a wrong host
# name, a self-signed leaf, an empty trust store and a non-TLS server each fail; ws:// is refused; the
# plaintext server never sees an upgrade request. Needs openssl, python3 and libcurl development files
# (pkg-config libcurl) and libssl/libcrypto on the host. Fail-close: a missing tool exits nonzero.
#
# The Quest invite transport and the service seam it plugs into (#966, docs/adr/0017): the transport over a loopback HTTP server
# on the host (plain http only where the test asks for it), the production client's fail-closed case, the shared service with the
# Quest transport, and the run's switch (the network.invite_codes feature). The link test of the shared service is test-quest-invite
# above. Needs libcurl development files (pkg-config libcurl) and the nlohmann header from `just build` with a mingw-* preset.
# Fail-close: a missing tool or header exits nonzero.
test-quest-invite-transport:
    #!/usr/bin/env bash
    set -euo pipefail
    pkg-config --exists libcurl || { echo "test-quest-invite-transport: libcurl development files not found (pkg-config libcurl)" >&2; exit 1; }
    out="build/quest-invite-transport-host"
    rm -rf "$out"
    mkdir -p "$out"
    json_src="build/{{ preset }}/vcpkg_installed/x64-mingw-static/include/nlohmann"
    if [[ ! -f "$json_src/json.hpp" ]]; then
        echo "test-quest-invite-transport: FAIL — $json_src/json.hpp not found; run 'just build' with a mingw-* preset first" >&2
        exit 1
    fi
    json_inc="$out/json_inc"
    mkdir -p "$json_inc"
    ln -sfn "$PWD/$json_src" "$json_inc/nlohmann"
    g++ -std=c++17 -Wall -Wextra -Werror -pthread -Isrc -Isdk/include -isystem "$json_inc" $(pkg-config --cflags libcurl) \
        src/quest/tests/invite_transport_quest_test.cpp \
        src/quest/invite/invite_transport_quest.cpp src/quest/invite/invite_run.cpp \
        src/quest/auth/curl_http.cpp src/quest/auth/ca_bundle.cpp \
        src/runtime/patch/invite_service.cpp src/runtime/patch/invite_transport_curl.cpp \
        -o "$out/invite_transport_quest_test" $(pkg-config --libs libcurl) -lssl -lcrypto
    timeout -k 5 300 "$out/invite_transport_quest_test"
    echo "test-quest-invite-transport: Quest invite transport and service seam tests pass on the host"

test-quest-tls:
    #!/usr/bin/env bash
    set -euo pipefail
    for tool in openssl python3 pkg-config g++; do
        command -v "$tool" >/dev/null || { echo "test-quest-tls: missing tool: $tool" >&2; exit 1; }
    done
    pkg-config --exists libcurl || { echo "test-quest-tls: libcurl development files not found (pkg-config libcurl)" >&2; exit 1; }
    out="build/quest-tls-host"
    rm -rf "$out"
    mkdir -p "$out"
    g++ -std=c++17 -Wall -Wextra -Werror -pthread -Isrc -Isdk/include $(pkg-config --cflags libcurl) \
        src/quest/net/curl_ws_connector.cpp src/quest/net/remote_ws.cpp src/quest/auth/ca_bundle.cpp \
        src/runtime/compat/session_router.cpp src/runtime/compat/evr_codec.cpp src/quest/tests/curl_ws_tls_test.cpp \
        -o "$out/curl_ws_tls_test" $(pkg-config --libs libcurl) -lssl -lcrypto
    cd "$out"
    mk_ca() { # name
        openssl req -x509 -newkey rsa:2048 -nodes -keyout "$1.key" -out "$1.pem" -subj "/CN=$1" -days 2 \
            -addext "basicConstraints=critical,CA:TRUE" -addext "keyUsage=critical,keyCertSign,cRLSign" 2>/dev/null
    }
    mk_ca nevr-test-ca
    mk_ca nevr-other-ca
    openssl req -newkey rsa:2048 -nodes -keyout server.key -out server.csr -subj "/CN=nevr-test" 2>/dev/null
    printf 'subjectAltName=IP:127.0.0.1,DNS:nevr-test.invalid\nbasicConstraints=CA:FALSE\nkeyUsage=digitalSignature,keyEncipherment\nextendedKeyUsage=serverAuth\n' > server.ext
    openssl x509 -req -in server.csr -CA nevr-test-ca.pem -CAkey nevr-test-ca.key -CAcreateserial -days 2 \
        -extfile server.ext -out server.pem 2>/dev/null
    openssl req -x509 -newkey rsa:2048 -nodes -keyout selfsigned.key -out selfsigned.pem -subj "/CN=selfsigned" -days 2 \
        -addext "subjectAltName=IP:127.0.0.1" 2>/dev/null
    # Trust directories for the CA loader (one certificate each), not a CApath: see ca_bundle.h.
    mkdir trust-good trust-other
    cp nevr-test-ca.pem trust-good/nevr-test-ca.pem
    cp nevr-other-ca.pem trust-other/nevr-other-ca.pem
    : > plain.stats
    pids=()
    cleanup() { for p in "${pids[@]}"; do kill "$p" 2>/dev/null || true; done; }
    trap cleanup EXIT
    server=../../src/quest/tests/tls_ws_server.py
    python3 -I "$server" tls server.pem server.key good.port & pids+=($!)
    python3 -I "$server" tls-delay server.pem server.key delay.port & pids+=($!)
    python3 -I "$server" tls-stall server.pem server.key stall.port & pids+=($!)
    python3 -I "$server" tls-trickle server.pem server.key trickle.port & pids+=($!)
    python3 -I "$server" tls selfsigned.pem selfsigned.key selfsigned.port & pids+=($!)
    python3 -I "$server" plain plain.stats plain.port & pids+=($!)
    for f in good.port delay.port stall.port trickle.port selfsigned.port plain.port; do
        for _ in $(seq 1 100); do [ -s "$f" ] && break; sleep 0.1; done
        [ -s "$f" ] || { echo "test-quest-tls: server for $f did not start" >&2; exit 1; }
    done
    ./curl_ws_tls_test trust-good trust-other "$(cat good.port)" "$(cat selfsigned.port)" \
        "$(cat plain.port)" plain.stats "$(cat stall.port)" "$(cat delay.port)" "$(cat trickle.port)"
    echo "test-quest-tls: verified-TLS connector tests pass on the host"

# Quest hook backend on the host. Builds three fixture shared objects (BIND_NOW with
# RELRO, BIND_NOW without RELRO, lazy) and runs src/quest/tests/got_hook_test.cpp,
# which drives the production GotHook, CallbackThunk and core/hook_lifecycle.h
# against them and against images built in memory. No NDK, no Android. Fail-close.
test-quest-hooks json_include="": _vcpkg-quest-host
    #!/usr/bin/env bash
    set -euo pipefail
    out="build/quest-hooks-host"
    mkdir -p "$out"
    cxx=(g++ -std=c++17 -Wall -Wextra -Werror -Isrc -Isdk/include -Isrc/quest/sentinel)
    # The patched libr15's identity (sentinel::pinned::LibR15PatchedBuildId, docs/adr/0021 section 7): a link of any
    # translation unit that calls a pinned_targets.h factory takes these three objects.
    for src in sentinel/libr15_identity.cpp patch/libr15_patch_identity.cpp data/sha256.cpp; do
        "${cxx[@]}" -fno-exceptions -c "src/quest/$src" -o "$out/id_$(basename "$src" .cpp).o"
    done
    ident=("$out/id_libr15_identity.o" "$out/id_libr15_patch_identity.o" "$out/id_sha256.o")
    # Type-level gates: the control compiles; each snippet that breaks one rule must fail to
    # compile, with the message that names the rule (not for some unrelated reason).
    snip=src/quest/tests/compile_fail
    "${cxx[@]}" -fno-exceptions -fsyntax-only "$snip/control.cpp"
    "${cxx[@]}" -fno-exceptions -fsyntax-only "$snip/row_control.cpp"
    # The patterns below are GCC's wording, which is why the recipe pins g++ (clang words the same errors differently).
    # Each snippet must fail with errors that are ALL the rule's own: every `error:` line has to
    # match the rule's pattern (a second, unrelated error fails the check), and there must be one.
    for pair in fake_thunk:"error: static assertion failed: InstallThunk requires a CallbackThunk" row_fake_thunk:"error: static assertion failed: InstallRowThunk requires a CallbackThunk" direct_record:"HookRecord.*is private within this context" plain_handler:"error: invalid conversion from .*::Handler"; do
        name="${pair%%:*}"; want="${pair#*:}"
        if "${cxx[@]}" -fno-exceptions -fsyntax-only "$snip/$name.cpp" > "$out/$name.err" 2>&1; then
            echo "test-quest-hooks: $snip/$name.cpp compiled, but must not" >&2; exit 1
        fi
        # Fail closed: a diagnostics file that is missing or empty (the redirect failed, so the compiler never
        # ran) or counts that are not numbers must not read as "no unrelated errors".
        [ -s "$out/$name.err" ] || { echo "test-quest-hooks: $snip/$name.cpp left no diagnostics in $out/$name.err; the gate did not run" >&2; exit 1; }
        total=$(grep -c 'error:' "$out/$name.err" || true)
        matched=$(grep 'error:' "$out/$name.err" | grep -c "$want" || true)
        case "$total$matched" in
            ''|*[!0-9]*) echo "test-quest-hooks: $snip/$name.cpp: could not count diagnostics (total='$total' matched='$matched')" >&2; exit 1;;
        esac
        if [ "$total" -lt 1 ] || [ "$total" -ne "$matched" ]; then
            echo "test-quest-hooks: $snip/$name.cpp: $total error line(s), $matched match '$want'; every error must be the rule's own:" >&2
            cat "$out/$name.err" >&2; exit 1
        fi
    done
    "${cxx[@]}" -shared -fPIC -Wl,--build-id=sha1 src/quest/tests/got_fixture_provider.cpp \
        -o "$out/libgotfx_provider.so"
    link=(-fPIC -shared -Wl,--build-id=sha1 -L"$out" -lgotfx_provider -Wl,-rpath,'$ORIGIN')
    "${cxx[@]}" "${link[@]}" -Wl,-z,now,-z,relro src/quest/tests/got_fixture_consumer.cpp \
        -o "$out/libgotfx_consumer_now.so"
    "${cxx[@]}" "${link[@]}" -Wl,-z,now,-z,norelro src/quest/tests/got_fixture_consumer.cpp \
        -o "$out/libgotfx_consumer_norelro.so"
    "${cxx[@]}" "${link[@]}" -Wl,-z,lazy,-z,norelro src/quest/tests/got_fixture_consumer.cpp \
        -o "$out/libgotfx_consumer_lazy.so"
    # The thunk fixture is built WITH exceptions; everything that includes
    # callback_thunk.h is built without (the header refuses otherwise).
    "${cxx[@]}" -c src/quest/tests/thunk_exception_fixture.cpp -o "$out/thunk_exception_fixture.o"
    "${cxx[@]}" -fno-exceptions src/quest/tests/got_hook_test.cpp src/quest/sentinel/got_hook.cpp \
        src/quest/sentinel/hook_log.cpp src/quest/sentinel/hook_report.cpp "$out/thunk_exception_fixture.o" \
        -o "$out/got_hook_test" -ldl -pthread
    timeout 300 "$out/got_hook_test" "$out"  # a hang is a failure, not a stuck gate
    # The sentinel's run-time half of the libr15 build-time patch (docs/adr/0021 test 4): Attach on the shipped patcher's
    # output for a synthetic stock libr15 at the table's real addresses, laid out in host memory (text pages r-x); every
    # refusal of section 7, each row check, the trampolines, the activation store, and a protection seam that must see no
    # call on the image. The patcher and its identity are built with exceptions (nlohmann::json), the sentinel half and the
    # test without. nlohmann::json comes from the tools/quest-host-deps manifest (as in quest-patcher); pass
    # json_include to use another header tree.
    attach_json="{{ json_include }}"
    [ -n "$attach_json" ] || attach_json="build/quest-host-vcpkg/x64-linux/include"
    [ -f "$attach_json/nlohmann/json.hpp" ] || { echo "test-quest-hooks: FAIL - $attach_json/nlohmann/json.hpp not found; 'just _vcpkg-quest-host' installs it, or pass json_include" >&2; exit 1; }
    mkdir -p "$out/attach_json_inc"
    ln -sfn "$(cd "$attach_json" && pwd)/nlohmann" "$out/attach_json_inc/nlohmann"
    attach=(g++ -std=c++17 -Wall -Wextra -Werror -D_GLIBCXX_ASSERTIONS -Isrc -Isdk/include -Isrc/quest/sentinel -isystem "$out/attach_json_inc" -pthread)
    for src in src/quest/patch/libr15_patcher.cpp src/quest/patch/libr15_extra_rows.cpp src/quest/patch/libr15_patch_identity.cpp src/quest/data/sha256.cpp; do
        "${attach[@]}" -c "$src" -o "$out/attach_$(basename "$src" .cpp).o"
    done
    "${attach[@]}" -fno-exceptions src/quest/tests/libr15_attach_test.cpp \
        "$out/attach_libr15_patcher.o" "$out/attach_libr15_extra_rows.o" "$out/attach_libr15_patch_identity.o" "$out/attach_sha256.o" \
        "$out/id_libr15_identity.o" src/quest/sentinel/libr15_attach.cpp src/quest/sentinel/process_end_hook.cpp src/quest/sentinel/arm64_reloc.cpp \
        src/quest/sentinel/got_hook.cpp src/quest/sentinel/hook_log.cpp src/quest/sentinel/hook_report.cpp \
        -o "$out/libr15_attach_test" -ldl
    timeout 300 "$out/libr15_attach_test" src/quest/tests/fixtures
    # The three feature files that take their hooks through rows of the patched libr15 (docs/adr/0021 sections 5 and 7, test 6),
    # and the credits install (section 8), each run against the same Attach-verified synthetic image (libr15_live_image.h):
    # the slots the installs wrote, the log lines they emitted, the refusals that leave a row pass-through, and no protection
    # change on the image. The feature files and the sentinel's attach half are built without exceptions, as on the device.
    attach_objs=("$out/attach_libr15_patcher.o" "$out/attach_libr15_extra_rows.o" "$out/attach_libr15_patch_identity.o" "$out/attach_sha256.o" "$out/id_libr15_identity.o")
    attach_sentinel=(src/quest/sentinel/libr15_attach.cpp src/quest/sentinel/process_end_hook.cpp src/quest/sentinel/arm64_reloc.cpp \
        src/quest/sentinel/got_hook.cpp src/quest/sentinel/hook_log.cpp src/quest/sentinel/hook_report.cpp)
    "${attach[@]}" -fno-exceptions -Isrc/quest/tests src/quest/tests/row_hooks_install_test.cpp \
        src/quest/sentinel/splash_skip_hook.cpp src/quest/sentinel/ui_texture_mips.cpp src/quest/social/lobby_full_hooks.cpp \
        "${attach_objs[@]}" "${attach_sentinel[@]}" -o "$out/row_hooks_install_test" -ldl
    timeout 300 "$out/row_hooks_install_test"
    "${attach[@]}" -fno-exceptions -Isrc/quest/tests src/quest/tests/credits_text_test.cpp src/quest/sentinel/credits_text.cpp \
        "${attach_objs[@]}" "${attach_sentinel[@]}" -o "$out/credits_text_test" -ldl
    timeout 300 "$out/credits_text_test"
    # The variant build (docs/adr/0021 section 11): the sentinel compiled with NEVR_LIBR15_EXTRA_ROWS and the source configure
    # generates for the example fixture. The rows it reads back are the parser's, the patched ID it expects is the patcher's for
    # them and not the public one, and its digest is the file's. Built without exceptions, as on the device. The generator is the
    # one the Quest build runs (cmake/libr15_extra_rows.cmake).
    extra="$out/extra_rows"; rm -rf "$extra"; mkdir -p "$extra"
    cmake -DNEVR_EXTRA_ROWS_FILE="$PWD/src/quest/tests/fixtures/libr15_extra_rows_example.json" \
        -DNEVR_EXTRA_ROWS_OUT="$extra/libr15_extra_rows_data.cpp" -P src/quest/cmake/generate_libr15_extra_rows.cmake
    "${attach[@]}" -fno-exceptions -DNEVR_LIBR15_EXTRA_ROWS=1 -c src/quest/sentinel/libr15_identity.cpp -o "$extra/identity.o"
    "${attach[@]}" -fno-exceptions -c src/quest/patch/libr15_extra_rows.cpp -o "$extra/extra_rows.o"
    "${attach[@]}" -fno-exceptions -c "$extra/libr15_extra_rows_data.cpp" -o "$extra/data.o"
    "${attach[@]}" -fno-exceptions -c src/quest/sentinel/hook_log.cpp -o "$extra/hook_log.o"
    "${attach[@]}" -Isrc/quest/tests src/quest/tests/libr15_extra_rows_identity_test.cpp \
        "$extra/identity.o" "$extra/extra_rows.o" "$extra/data.o" "$extra/hook_log.o" \
        "$out/attach_libr15_patch_identity.o" "$out/attach_sha256.o" -o "$extra/libr15_extra_rows_identity_test"
    timeout 120 "$extra/libr15_extra_rows_identity_test" src/quest/tests/fixtures
    # An embedded text that does not parse stops the process with the log line and never falls back to the public rows
    # (SIGILL from __builtin_trap, exit status 132). Configure parses the file first, so this is a corrupted build; the generator
    # alone embeds whatever it is given, which is how the case is made here.
    printf '{' > "$extra/unparsable.json"
    cmake -DNEVR_EXTRA_ROWS_FILE="$PWD/$extra/unparsable.json" -DNEVR_EXTRA_ROWS_OUT="$extra/unparsable_data.cpp" -P src/quest/cmake/generate_libr15_extra_rows.cmake
    "${attach[@]}" -fno-exceptions -c "$extra/unparsable_data.cpp" -o "$extra/unparsable_data.o"
    "${attach[@]}" -Isrc/quest/tests src/quest/tests/libr15_extra_rows_identity_test.cpp \
        "$extra/identity.o" "$extra/extra_rows.o" "$extra/unparsable_data.o" "$extra/hook_log.o" \
        "$out/attach_libr15_patch_identity.o" "$out/attach_sha256.o" -o "$extra/unparsable_identity_test"
    unparsable_rc=0
    timeout 60 "$extra/unparsable_identity_test" src/quest/tests/fixtures > "$extra/unparsable.out" 2>&1 || unparsable_rc=$?
    [ "$unparsable_rc" -eq 132 ] || { echo "test-quest-hooks: FAIL - an unparsable embedded rows text exited $unparsable_rc, not 132 (SIGILL from __builtin_trap)" >&2; cat "$extra/unparsable.out" >&2; exit 1; }
    grep -qF '"event":"libr15_extra_rows"' "$extra/unparsable.out" && grep -qF '"status":"unparsable"' "$extra/unparsable.out" || { echo "test-quest-hooks: FAIL - the unparsable embedded text left no libr15_extra_rows log line" >&2; cat "$extra/unparsable.out" >&2; exit 1; }
    # Every byte value survives the generated source: a file of bytes 0 to 255 is embedded and written back byte for byte.
    python3 -c 'import sys; sys.stdout.buffer.write(bytes(range(256)))' > "$extra/all_bytes.bin"
    cmake -DNEVR_EXTRA_ROWS_FILE="$PWD/$extra/all_bytes.bin" -DNEVR_EXTRA_ROWS_OUT="$extra/all_bytes_data.cpp" -P src/quest/cmake/generate_libr15_extra_rows.cmake
    "${cxx[@]}" -I"$extra" src/quest/tests/extra_rows_embedding_check.cpp "$extra/all_bytes_data.cpp" -o "$extra/embedding_check"
    "$extra/embedding_check" > "$extra/all_bytes.out"
    cmp "$extra/all_bytes.bin" "$extra/all_bytes.out"
    # A file the patcher would refuse stops the Quest build at configure. The configure step is cmake/libr15_extra_rows.cmake's
    # nevr_libr15_extra_rows, run here in a project of its own (the Android project's own configure takes minutes and needs the
    # NDK; this is the same function with the same arguments).
    json_dir="$(cd "$attach_json" && pwd)"
    configure_extra() { # file expected-substring (empty: must succeed)
        local dir="$extra/configure"; rm -rf "$dir"
        if cmake -S src/quest/tests/extra_rows_configure -B "$dir" -DNEVR_LIBR15_EXTRA_ROWS="$1" -DNEVR_JSON_INCLUDE="$json_dir" \
                -DNEVR_SRC_ROOT="$PWD/src" > "$extra/configure.log" 2>&1; then
            [ -z "$2" ] || { echo "test-quest-hooks: configure accepted $1, but must refuse it with '$2'" >&2; cat "$extra/configure.log" >&2; exit 1; }
        else
            [ -n "$2" ] || { echo "test-quest-hooks: configure refused $1" >&2; cat "$extra/configure.log" >&2; exit 1; }
            grep -qF "$2" "$extra/configure.log" || { echo "test-quest-hooks: configure refused $1, but not with '$2'" >&2; cat "$extra/configure.log" >&2; exit 1; }
        fi
    }
    configure_extra "$PWD/src/quest/tests/fixtures/libr15_extra_rows_example.json" ""
    [ -f "$extra/configure/generated.cpp" ] || { echo "test-quest-hooks: configure of the fixture generated no source" >&2; exit 1; }
    printf '{"format": ' > "$extra/malformed.json"
    configure_extra "$PWD/$extra/malformed.json" "extra_rows_json"
    sed 's/"stock_build_id": "b2/"stock_build_id": "b3/' src/quest/tests/fixtures/libr15_extra_rows_example.json > "$extra/other_libr15.json"
    configure_extra "$PWD/$extra/other_libr15.json" "extra_rows_stock_build_id"
    sed 's/"kind": "word_patch"/"kind": "word_patch", "extra": 1/' src/quest/tests/fixtures/libr15_extra_rows_example.json > "$extra/unknown_field.json"
    configure_extra "$PWD/$extra/unknown_field.json" "extra_rows_unknown_field"
    configure_extra "$PWD/$extra/does_not_exist.json" "the file does not exist"
    : > "$extra/empty.json"
    configure_extra "$PWD/$extra/empty.json" "extra_rows_json"
    # The generator alone refuses an empty file too (a build must not embed nothing and call it a variant).
    empty_rc=0
    cmake -DNEVR_EXTRA_ROWS_FILE="$PWD/$extra/empty.json" -DNEVR_EXTRA_ROWS_OUT="$extra/empty_data.cpp" -P src/quest/cmake/generate_libr15_extra_rows.cmake > "$extra/empty.log" 2>&1 || empty_rc=$?
    [ "$empty_rc" -ne 0 ] && grep -qF "the file is empty" "$extra/empty.log" || { echo "test-quest-hooks: FAIL - the generator did not refuse an empty extra-rows file" >&2; cat "$extra/empty.log" >&2; exit 1; }
    # The PC-relative relocator (arm64_reloc.h): each kind decoded and then run, original window against the
    # relocated trampoline far away, on a small A64 interpreter. Pure; no device.
    "${cxx[@]}" -fno-exceptions -pthread src/quest/tests/arm64_reloc_test.cpp \
        src/quest/sentinel/arm64_reloc.cpp -o "$out/arm64_reloc_test" -pthread
    timeout 120 "$out/arm64_reloc_test"
    # The lobby-full policy (#1001): the #889 cases on the 16-byte lobby key. Pure; no libr15 here.
    "${cxx[@]}" -pthread src/quest/tests/lobby_full_test.cpp -o "$out/lobby_full_test"
    timeout 60 "$out/lobby_full_test"
    # The frame dispatcher (#966 S7): posted work runs only on Tick, once, in order; posting from another thread;
    # try-lock contention skips without blocking. Built as the sentinel builds it (no exceptions).
    "${cxx[@]}" -fno-exceptions -pthread src/quest/tests/frame_dispatch_test.cpp \
        src/quest/invite/frame_dispatch.cpp src/quest/sentinel/hook_log.cpp \
        -o "$out/frame_dispatch_test"
    timeout 60 "$out/frame_dispatch_test"
    # The login-prompt hook (#239): the real handler and prompt board, built as the sentinel builds
    # them (no exceptions), driven through the thunk entry.
    "${cxx[@]}" -fno-exceptions -pthread src/quest/tests/login_prompt_hook_test.cpp \
        src/quest/sentinel/login_prompt_hook.cpp src/quest/invite/frame_dispatch.cpp src/quest/auth/prompt_board.cpp \
        src/quest/sentinel/got_hook.cpp src/quest/sentinel/hook_log.cpp src/quest/sentinel/hook_report.cpp \
        "${ident[@]}" -o "$out/login_prompt_hook_test" -ldl
    timeout 120 "$out/login_prompt_hook_test"
    # The login-failed page's title (#1137): the real handler and prompt board driven through the thunk entry, against a
    # stand-in for the script library's string_t constructor.
    "${cxx[@]}" -fno-exceptions -pthread src/quest/tests/login_title_hook_test.cpp \
        src/quest/sentinel/login_title_hook.cpp src/quest/auth/prompt_board.cpp \
        src/quest/sentinel/got_hook.cpp src/quest/sentinel/hook_log.cpp src/quest/sentinel/hook_report.cpp \
        "${ident[@]}" -o "$out/login_title_hook_test" -ldl
    timeout 120 "$out/login_title_hook_test"
    # The whole no-credentials chain (#581): the real prerequisite handlers and the real prompt hook together,
    # token auth awaiting the player -> the answers delivered -> the game's local failure -> the code written.
    g++ -std=c++17 -Wall -Wextra -Werror -fno-exceptions -DNEVR_QUEST_TESTING -Isrc -Isdk/include -c \
        src/quest/login/login_prerequisites.cpp -o "$out/chain_login_prerequisites.o"
    g++ -std=c++17 -Wall -Wextra -Werror -fno-exceptions -DNEVR_QUEST_TESTING -Isrc -Isdk/include -c \
        src/quest/login/login_standin.cpp -o "$out/chain_login_standin.o"
    "${cxx[@]}" -fno-exceptions -DNEVR_QUEST_TESTING -pthread src/quest/tests/login_signin_code_chain_test.cpp \
        src/quest/sentinel/login_prompt_hook.cpp src/quest/invite/frame_dispatch.cpp src/quest/auth/prompt_board.cpp \
        "$out/chain_login_prerequisites.o" "$out/chain_login_standin.o" \
        src/quest/sentinel/got_hook.cpp src/quest/sentinel/hook_log.cpp src/quest/sentinel/hook_report.cpp \
        "${ident[@]}" -o "$out/login_signin_code_chain_test" -ldl
    timeout 120 "$out/login_signin_code_chain_test"
    # The OBB-mount skip (#319): the real handlers driven through the thunk entries, against a model of the
    # game's callback and wait loop.
    "${cxx[@]}" -fno-exceptions -pthread src/quest/tests/obb_skip_hook_test.cpp \
        src/quest/sentinel/obb_skip_hook.cpp \
        src/quest/sentinel/got_hook.cpp src/quest/sentinel/hook_log.cpp src/quest/sentinel/hook_report.cpp \
        "${ident[@]}" -o "$out/obb_skip_hook_test" -ldl
    timeout 120 "$out/obb_skip_hook_test"
    # The data-path rewrite of the pinned service-only libr15 (#1737): afk prefix to the running package, exact segment only.
    "${cxx[@]}" -fno-exceptions src/quest/tests/path_rewrite_test.cpp -o "$out/path_rewrite_test"
    timeout 60 "$out/path_rewrite_test"
    # The process ends when the game's android_main returns (#621): the real handler through the thunk entry,
    # and a real process ended under a writer that saves the way the Quest code saves.
    "${cxx[@]}" -shared -fPIC -Wl,--build-id=sha1 src/quest/tests/process_end_fixture_provider.cpp \
        -o "$out/libpefx_provider.so"
    "${cxx[@]}" -fPIC -shared -Wl,--build-id=sha1 -L"$out" -lpefx_provider -Wl,-rpath,'$ORIGIN' -Wl,-z,now \
        src/quest/tests/process_end_fixture_consumer.cpp -o "$out/libpefx_consumer.so"
    "${cxx[@]}" -fno-exceptions -pthread src/quest/tests/process_end_hook_test.cpp \
        src/quest/sentinel/process_end_hook.cpp src/quest/auth/atomic_write.cpp \
        src/quest/sentinel/got_hook.cpp src/quest/sentinel/hook_log.cpp src/quest/sentinel/hook_report.cpp \
        "${ident[@]}" -o "$out/process_end_hook_test" -ldl
    timeout 300 "$out/process_end_hook_test" "$out"
    # Our crash handler stays first (#1605): the real handler and the sigaction/signal hooks, driven by real signals and a
    # fixture library that calls libc's sigaction and signal through PLT slots as libr15 does.
    "${cxx[@]}" -fPIC -shared -Wl,--build-id=sha1 -Wl,-z,now src/quest/tests/crash_first_fixture_consumer.cpp \
        -o "$out/libcffx_consumer.so"
    # The same fixture under the names of the provider libraries the late install looks for (loaded after the constructor).
    for provider in libpnsrad.so libpnsradmatchmaking.so libpnsovr.so; do
        "${cxx[@]}" -fPIC -shared -Wl,--build-id=sha1 -Wl,-z,now src/quest/tests/crash_first_fixture_consumer.cpp -o "$out/$provider"
    done
    "${cxx[@]}" -fno-exceptions -pthread src/quest/tests/crash_first_hook_test.cpp \
        src/quest/sentinel/crash_first_hook.cpp \
        src/quest/sentinel/got_hook.cpp src/quest/sentinel/hook_log.cpp src/quest/sentinel/hook_report.cpp \
        "${ident[@]}" -o "$out/crash_first_hook_test" -ldl
    timeout 120 "$out/crash_first_hook_test" "$out"
    # The game waits for the storage permission it just asked for (#640): the real handler through the thunk entry,
    # against a stand-in for RequestPermissions and a model of CheckPermission.
    "${cxx[@]}" -fno-exceptions -pthread src/quest/tests/permission_wait_hook_test.cpp \
        src/quest/sentinel/permission_wait_hook.cpp \
        src/quest/sentinel/got_hook.cpp src/quest/sentinel/hook_log.cpp src/quest/sentinel/hook_report.cpp \
        "${ident[@]}" -o "$out/permission_wait_hook_test" -ldl
    timeout 120 "$out/permission_wait_hook_test"
    # The Vulkan extension window (#868): the real wrapper chain through the thunk entries against a fake driver, with the
    # Quest 3 driver's 137-entry list as the central case.
    "${cxx[@]}" -fno-exceptions -pthread src/quest/tests/vk_ext_window_test.cpp \
        src/quest/sentinel/vk_ext_window.cpp \
        src/quest/sentinel/got_hook.cpp src/quest/sentinel/hook_log.cpp src/quest/sentinel/hook_report.cpp \
        "${ident[@]}" -o "$out/vk_ext_window_test" -ldl
    timeout 120 "$out/vk_ext_window_test"
    # The OS build floor (#945): the real handler through the thunk entry against a stand-in for CSysInfo::OSBuildVersion,
    # a number below the floor, above it, none, and an unknown floor.
    "${cxx[@]}" -fno-exceptions -pthread src/quest/tests/os_build_gate_hook_test.cpp \
        src/quest/sentinel/os_build_gate_hook.cpp \
        src/quest/sentinel/got_hook.cpp src/quest/sentinel/hook_log.cpp src/quest/sentinel/hook_report.cpp \
        "${ident[@]}" -o "$out/os_build_gate_hook_test" -ldl
    timeout 120 "$out/os_build_gate_hook_test"
    # The platform provider is libpnsrad (#1220): the four real handlers through their thunk entries against stand-ins for
    # CNSProvider::Create, Destroy, ProviderID and UserProviderID, CSysModule::Unload and CJson::TString: the name rule,
    # the appid kept, the OVR answer after a rename, and ShutdownEngine's three Destroys shutting libpnsrad down once.
    "${cxx[@]}" -fno-exceptions -pthread src/quest/tests/pns_provider_hook_test.cpp \
        src/quest/sentinel/pns_provider_hook.cpp \
        src/quest/sentinel/got_hook.cpp src/quest/sentinel/hook_log.cpp src/quest/sentinel/hook_report.cpp \
        "${ident[@]}" -o "$out/pns_provider_hook_test" -ldl
    timeout 120 "$out/pns_provider_hook_test"
    # The main-menu QUIT (#1276): the real handler through the thunk entry against stand-ins for CVR::ShowSystemUI and
    # ANativeActivity_finish (the activity finished; no app, no activity or no finisher leaves the game's call).
    "${cxx[@]}" -fno-exceptions -pthread src/quest/tests/quit_hook_test.cpp \
        src/quest/sentinel/quit_hook.cpp src/quest/sentinel/process_end_hook.cpp \
        src/quest/sentinel/got_hook.cpp src/quest/sentinel/hook_log.cpp src/quest/sentinel/hook_report.cpp \
        "${ident[@]}" -o "$out/quit_hook_test" -ldl
    timeout 120 "$out/quit_hook_test"
    # The Quest main menu shows the runtime version (#747): the real handler through the thunk entry against a stand-in for
    # CCanvasUICS::SetText (the lobby_version item replaced, every other item, an empty text and a null text left alone).
    # The handler includes the build-tree header generated/nevr_build_info.h; a stub stands in for it here.
    mkdir -p "$out/menu_version_gen/generated"
    printf 'namespace nevr_build { inline constexpr const char* kVersion = "5.17.0"; }\n' > "$out/menu_version_gen/generated/nevr_build_info.h"
    "${cxx[@]}" -I"$out/menu_version_gen" -fno-exceptions -pthread src/quest/tests/menu_version_label_test.cpp \
        src/quest/social/menu_version_label.cpp \
        src/quest/sentinel/got_hook.cpp src/quest/sentinel/hook_log.cpp src/quest/sentinel/hook_report.cpp \
        "${ident[@]}" -o "$out/menu_version_label_test" -ldl
    timeout 120 "$out/menu_version_label_test"
    # The texture-quality hook (#943): the real handler through the thunk entry against a stand-in for vrapi_GetSystemPropertyInt,
    # every level's eye size, the pass-through of every other id, the latch, `stock`, and the shared slot's observer.
    "${cxx[@]}" -fno-exceptions -pthread src/quest/tests/texture_quality_hook_test.cpp \
        src/quest/sentinel/texture_quality_hook.cpp \
        src/quest/sentinel/got_hook.cpp src/quest/sentinel/hook_log.cpp src/quest/sentinel/hook_report.cpp \
        "${ident[@]}" -o "$out/texture_quality_hook_test" -ldl
    timeout 120 "$out/texture_quality_hook_test"
    # The Texture Resolution control (#1012), pure half: the two settings documents rewritten in memory, the saved value read back,
    # the level <-> menu index mapping. Built with exceptions, like quest_config.cpp; no game header.
    "${cxx[@]}" -isystem "$out/attach_json_inc" src/quest/tests/texture_menu_test.cpp src/quest/sentinel/texture_menu.cpp \
        -o "$out/texture_menu_test"
    timeout 120 "$out/texture_menu_test"
    # The control's five handlers and the live apply (#1012) through their thunk entries, against a stand-in engine, resize, VR
    # flags, settings, validator and JSON reader: the engine's order, what it waits for, what it refuses, a failed rebuild, the
    # first delivery, the saved level, the two documents, the clamp and the log. The hook is built without exceptions as on the
    # device; the rewrite it calls keeps them (it runs before the game is called: NEVR_OUTSIDE_GAME_CALL).
    "${cxx[@]}" -fno-exceptions -pthread -isystem "$out/attach_json_inc" -c src/quest/sentinel/texture_menu_hook.cpp \
        -o "$out/texture_menu_hook.o"
    "${cxx[@]}" -isystem "$out/attach_json_inc" -c src/quest/sentinel/texture_menu.cpp -o "$out/texture_menu.o"
    "${cxx[@]}" -fno-exceptions -pthread src/quest/tests/texture_menu_hook_test.cpp "$out/texture_menu_hook.o" "$out/texture_menu.o" \
        src/quest/sentinel/texture_quality_hook.cpp \
        src/quest/sentinel/got_hook.cpp src/quest/sentinel/hook_log.cpp src/quest/sentinel/hook_report.cpp \
        "${ident[@]}" -o "$out/texture_menu_hook_test" -ldl
    timeout 120 "$out/texture_menu_hook_test"
    # The hardware dump on the shared vrapi_GetSystemPropertyInt slot (#943): owned by the texture-quality hook, the dump
    # installs nothing there and records the runtime's own answers through the observer.
    "${cxx[@]}" -fno-exceptions -pthread src/quest/tests/hwdump_share_test.cpp \
        src/quest/sentinel/texture_quality_hook.cpp src/quest/diag/hwdump_install.cpp \
        src/quest/diag/hwdump_handlers.cpp src/quest/diag/hwdump_records.cpp \
        src/quest/sentinel/got_hook.cpp src/quest/sentinel/hook_log.cpp src/quest/sentinel/hook_report.cpp \
        "${ident[@]}" -o "$out/hwdump_share_test" -ldl
    timeout 120 "$out/hwdump_share_test"
    # The render-configuration log (#1139): the five real handlers through the thunk entries against stand-ins for the VrApi
    # functions, the line at the first frame and on each forced change, the globals read only from the proved image, the poller.
    "${cxx[@]}" -fno-exceptions -pthread src/quest/tests/render_config_hook_test.cpp \
        src/quest/sentinel/render_config_hook.cpp src/quest/sentinel/texture_quality_hook.cpp \
        src/quest/sentinel/clock_levels_hook.cpp \
        src/quest/sentinel/got_hook.cpp src/quest/sentinel/hook_log.cpp src/quest/sentinel/hook_report.cpp \
        "${ident[@]}" -o "$out/render_config_hook_test" -ldl
    timeout 120 "$out/render_config_hook_test"
    # The log-only resource request hook (#871): the real handler through the thunk entry against a stand-in for
    # FillResourceFileRequest that fills a request the way the pinned function does.
    "${cxx[@]}" -fno-exceptions -pthread src/quest/tests/resource_request_hook_test.cpp \
        src/quest/sentinel/resource_request_hook.cpp \
        src/quest/sentinel/got_hook.cpp src/quest/sentinel/hook_log.cpp src/quest/sentinel/hook_report.cpp \
        "${ident[@]}" -o "$out/resource_request_hook_test" -ldl
    timeout 120 "$out/resource_request_hook_test"
    # The clock-level override (#928): the decision, its switch and its counters. The handler that calls it is the
    # render-configuration hook's, tested below.
    "${cxx[@]}" -fno-exceptions -pthread src/quest/tests/clock_levels_hook_test.cpp \
        src/quest/sentinel/clock_levels_hook.cpp \
        src/quest/sentinel/hook_log.cpp src/quest/sentinel/hook_report.cpp \
        -o "$out/clock_levels_hook_test" -ldl
    timeout 120 "$out/clock_levels_hook_test"
    # The Quest main-menu cards (#966): the real handler through the thunk entry against a stand-in callback block, a
    # CAllocator with the slots FreeResource and the streaming worker use, and the game's callback.
    "${cxx[@]}" -fno-exceptions -pthread src/quest/tests/menu_cards_hook_test.cpp \
        src/quest/sentinel/menu_cards_hook.cpp src/quest/data/sha256.cpp \
        src/quest/sentinel/got_hook.cpp src/quest/sentinel/hook_log.cpp src/quest/sentinel/hook_report.cpp \
        "${ident[@]:0:2}" -o "$out/menu_cards_hook_test" -ldl
    timeout 120 "$out/menu_cards_hook_test"
    # The CREATE GAME / JOIN VIA CODE observer on libr15's MetricLog (#966, ADR 0017 slice S2): the real observer through the
    # thunk entry, built without exceptions, against a stand-in for the game's MetricLog. The shared parse needs nlohmann::json,
    # so the header this recipe installed (attach_json_inc, above) is on the include path.
    "${cxx[@]}" -fno-exceptions -pthread -isystem "$out/attach_json_inc" src/quest/tests/wizard_metric_hook_test.cpp \
        src/quest/invite/wizard_metric_hook.cpp src/quest/data/sha256.cpp \
        src/quest/sentinel/got_hook.cpp src/quest/sentinel/hook_log.cpp src/quest/sentinel/hook_report.cpp \
        "${ident[@]:0:2}" -o "$out/wizard_metric_hook_test" -ldl
    timeout 120 "$out/wizard_metric_hook_test"
    # The Quest join (#966 S4, ADR 0017): the staging, the route and repeat gates, and the begin order, against a fake game block at
    # the Quest offsets (kQuestLayout). The service's two symbols are stubbed inside the test. Built without exceptions.
    "${cxx[@]}" -fno-exceptions -pthread -isystem "$out/attach_json_inc" src/quest/tests/invite_join_quest_test.cpp \
        src/quest/invite/join_game_quest.cpp src/quest/invite/ui_text_quest.cpp src/quest/invite/frame_dispatch.cpp src/quest/data/sha256.cpp \
        src/quest/sentinel/got_hook.cpp src/quest/sentinel/hook_log.cpp src/quest/sentinel/hook_report.cpp \
        "${ident[@]:0:2}" -o "$out/invite_join_quest_test" -ldl
    timeout 120 "$out/invite_join_quest_test"
    # The controller-input seam (#1202): the VrApi layout rules, the registration's every refusal and its one owner, and
    # the real handler through the thunk entry against stand-ins for vrapi_GetCurrentInputState and
    # vrapi_GetInputDeviceCapabilities. got_pinned_test resolves both slots in the real libr15.
    "${cxx[@]}" -fno-exceptions -pthread src/quest/tests/input_seam_test.cpp src/quest/plugin/input_seam_hook.cpp \
        src/quest/sentinel/got_hook.cpp src/quest/sentinel/hook_log.cpp src/quest/sentinel/hook_report.cpp "${ident[@]}" \
        -o "$out/input_seam_test" -ldl
    timeout 120 "$out/input_seam_test"
    # The toast presenter against a fake JNI (#807): a Java exception thrown at each JNI call in turn, null or not, is
    # never left pending. jni.h is the NDK's own; the recipe stops when it is absent.
    jni="{{ ndk }}/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/include/jni.h"
    [ -f "$jni" ] || { echo "test-quest-hooks: FAIL: $jni not found (the toast test needs the NDK's jni.h)" >&2; exit 1; }
    mkdir -p "$out/jni"
    cp "$jni" "$out/jni/jni.h"
    "${cxx[@]}" -isystem "$out/jni" -fno-exceptions -pthread src/quest/tests/toast_presenter_test.cpp \
        src/quest/sentinel/toast_presenter.cpp src/quest/sentinel/hook_log.cpp -o "$out/toast_presenter_test" -ldl
    timeout 60 "$out/toast_presenter_test"
    # The permission request with a defined request code (#947) against the same fake-JNI shape: the array of names,
    # the method and the int the variadic call carries, and a thrown exception at each JNI call never left pending.
    "${cxx[@]}" -isystem "$out/jni" -fno-exceptions -pthread src/quest/tests/permission_request_test.cpp \
        src/quest/sentinel/permission_request.cpp -o "$out/permission_request_test"
    timeout 60 "$out/permission_request_test"

# Quest config-string redirect on the host. Builds two fixture shared objects that import
# CJson::TString by its mangled name through a PLT slot (src/quest/redirect/tests), then runs
# redirect_test, which drives the production ServiceRedirector, the typed thunks and GotHook
# against them with a fake game config. No NDK, no Android. Fail-close.
test-quest-redirect: _vcpkg-quest-host
    #!/usr/bin/env bash
    set -euo pipefail
    out="build/quest-redirect-host"
    mkdir -p "$out"
    yaml_host="{{ justfile_directory() }}/build/quest-host-vcpkg/x64-linux"
    yaml_lib="$yaml_host/lib/libyaml-cpp.a"
    [[ -f "$yaml_lib" ]] || { echo "test-quest-redirect: FAIL - $yaml_lib not found (just _vcpkg-quest-host installs it)" >&2; exit 1; }
    # nlohmann::json comes from the header the build installed from vcpkg.json (as in test-quest-integration),
    # never a system package: the CI builder image has none.
    json_src="build/{{ preset }}/vcpkg_installed/x64-mingw-static/include/nlohmann"
    if [[ ! -f "$json_src/json.hpp" ]]; then
        echo "test-quest-redirect: FAIL - $json_src/json.hpp not found; run 'just build' with a mingw-* preset first" >&2
        exit 1
    fi
    json_inc="$out/json_inc"
    mkdir -p "$json_inc"
    ln -sfn "$PWD/$json_src" "$json_inc/nlohmann"
    cxx=(g++ -std=c++17 -Wall -Wextra -Werror -Isrc -Isdk/include -Isrc/quest/sentinel -isystem "$json_inc" -isystem "$yaml_host/include")
    fx=src/quest/redirect/tests
    "${cxx[@]}" -shared -fPIC -Wl,--build-id=sha1 "$fx/tstring_fixture_provider.cpp" \
        -o "$out/libredirfx_provider.so"
    link=(-fPIC -shared -Wl,--build-id=sha1 -L"$out" -lredirfx_provider -Wl,-rpath,'$ORIGIN' -Wl,-z,now,-z,relro)
    "${cxx[@]}" "${link[@]}" "$fx/tstring_fixture_consumer.cpp" -o "$out/libredirfx_consumer_a.so"
    "${cxx[@]}" "${link[@]}" "$fx/tstring_fixture_consumer.cpp" -o "$out/libredirfx_consumer_b.so"
    # The thunk translation unit and the hook backend are built without exceptions (as on Android);
    # everything else, including the test, with them.
    "${cxx[@]}" -fno-exceptions -c src/quest/redirect/tstring_thunks.cpp -o "$out/tstring_thunks.o"
    for f in got_hook hook_log hook_report; do
        "${cxx[@]}" -fno-exceptions -c "src/quest/sentinel/$f.cpp" -o "$out/$f.o"
    done
    # The patched libr15's identity (sentinel::pinned::LibR15PatchedBuildId, docs/adr/0021 section 7): tstring_thunks.cpp
    # calls the pinned factories, so the link takes the three objects test-quest-hooks takes, built as on the device.
    for src in sentinel/libr15_identity.cpp patch/libr15_patch_identity.cpp data/sha256.cpp; do
        "${cxx[@]}" -fno-exceptions -c "src/quest/$src" -o "$out/id_$(basename "$src" .cpp).o"
    done
    # The thunk's frames must sit under the personality-free CIE: no "zPLR" augmentation, at least one "zR".
    frames="$out/tstring_thunks.frames.txt"
    readelf --debug-dump=frames "$out/tstring_thunks.o" > "$frames"
    if grep -q '"zPLR"' "$frames"; then
        echo "test-quest-redirect: FAIL - tstring_thunks.o has frames under a personality CIE (zPLR)" >&2
        exit 1
    fi
    grep -q '"zR"' "$frames" || { echo "test-quest-redirect: FAIL - tstring_thunks.o has no zR frames to check" >&2; exit 1; }
    srcs=("$fx/redirect_test.cpp" src/quest/redirect/service_redirector.cpp src/quest/redirect/hook_adapter.cpp
        src/quest/sentinel/quest_config.cpp src/core/nevr_config.cpp src/runtime/lifecycle/service_redirect.cpp
        src/runtime/lifecycle/stable_string_pool.cpp)
    objs=("$out/tstring_thunks.o" "$out/got_hook.o" "$out/hook_log.o" "$out/hook_report.o"
        "$out/id_libr15_identity.o" "$out/id_libr15_patch_identity.o" "$out/id_sha256.o")
    "${cxx[@]}" "${srcs[@]}" "${objs[@]}" "$yaml_lib" -o "$out/redirect_test" -ldl -pthread
    "$out/redirect_test" "$out"
    # The same test under ThreadSanitizer at -O1: the cache lock is only observable as a data race, and
    # -O1 also exercises the warnings that -O0 hides. The thunk TU and backend stay uninstrumented
    # objects (they hold no shared state beyond atomics).
    tsan=(-fsanitize=thread -O1 -g)
    "${cxx[@]}" "${tsan[@]}" "${srcs[@]}" "${objs[@]}" "$yaml_lib" -o "$out/redirect_test_tsan" -ldl -pthread
    TSAN_OPTIONS="halt_on_error=1 exitcode=66" "$out/redirect_test_tsan" "$out"

# Resolve the pinned Quest targets in the real libr15.so / libpnsradmatchmaking.so /
# libpnsovr.so (docs/adr/0003). Extracts the three from the pinned APK, checks their SHA-256, and runs
# src/quest/tests/got_pinned_test.cpp. Fail-close, including when the APK is absent:
# it is a 58 MB artifact that is not in the repository, so this is not part of
# `just verify`.
test-quest-hooks-pinned apk="build/android-arm64/repack/r15_nevr-sentinel_signed.apk" store_apk="/mnt/games/cache/r15_goldmaster_store.apk":
    #!/usr/bin/env bash
    set -euo pipefail
    apk="{{ apk }}"; store_apk="{{ store_apk }}"
    [ -f "$apk" ] || { echo "test-quest-hooks-pinned: pinned APK not found: $apk" >&2; exit 1; }
    [ -f "$store_apk" ] || { echo "test-quest-hooks-pinned: store APK not found: $store_apk" >&2; exit 1; }
    out="build/quest-hooks-pinned"
    rm -rf "$out"; mkdir -p "$out/lib"
    unzip -o -q "$apk" lib/arm64-v8a/libpnsradmatchmaking.so lib/arm64-v8a/libpnsovr.so lib/arm64-v8a/lib7f2363b8976491aa.so lib/arm64-v8a/libpnsrad.so -d "$out/lib"
    # libr15.so is the STORE's: the signed APK carries the patched file (docs/adr/0021 section 9), which these pins are not about.
    unzip -o -q "$store_apk" lib/arm64-v8a/libr15.so -d "$out/lib"
    echo "8dd9a961b9dca8566069a4f65b3ddee9c65682c4e9c91a6d41e3c5727b1d8b20  $out/lib/lib/arm64-v8a/libr15.so" | sha256sum -c -
    echo "36236ab1df5783da57c064b0fbccc3a61c0e1d150c208022fbfc9cd6e5ed60ee  $out/lib/lib/arm64-v8a/libpnsradmatchmaking.so" | sha256sum -c -
    echo "26e9a216a710d42a303346a4ca5b84037ff38250ea725dc7112b055fcacada79  $out/lib/lib/arm64-v8a/libpnsovr.so" | sha256sum -c -
    echo "80c36268dbefd9f20ccae0aa35d3efe6f6480a8179f9f46709e71e33e0f110e4  $out/lib/lib/arm64-v8a/lib7f2363b8976491aa.so" | sha256sum -c -
    echo "d9995c877a6623d8e48879f80c5749f313a0eba8a00b35a936c0df60e6d23aa8  $out/lib/lib/arm64-v8a/libpnsrad.so" | sha256sum -c -
    g++ -std=c++17 -Wall -Wextra -Werror -fno-exceptions -Isrc -Isdk/include -Isrc/quest/sentinel \
        src/quest/tests/got_pinned_test.cpp src/quest/sentinel/got_hook.cpp \
        src/quest/sentinel/hook_log.cpp src/quest/sentinel/libr15_identity.cpp src/quest/patch/libr15_patch_identity.cpp \
        src/quest/data/sha256.cpp -o "$out/got_pinned_test" -ldl
    "$out/got_pinned_test" "$out/lib/lib/arm64-v8a/libr15.so" "$out/lib/lib/arm64-v8a/libpnsradmatchmaking.so" \
        "$out/lib/lib/arm64-v8a/libpnsovr.so" "$out/lib/lib/arm64-v8a/lib7f2363b8976491aa.so" "$out/lib/lib/arm64-v8a/libpnsrad.so"
    # #411: every ovr_* import the message pump and the login callbacks can reach is hooked or guarded, so a
    # synthetic message handle never reaches the SDK (tools/pinned_ovr_imports.txt says how each is treated).
    python3 tools/pinned_ovr_import_walk.py "$out/lib/lib/arm64-v8a/libpnsovr.so" --expect tools/pinned_ovr_imports.txt
    # #431: ovr_User_GetOrgScopedID has nine Social callers besides the login's three; only the login's are
    # answered locally, and the list the sentinel gates on equals what the library has.
    python3 tools/pinned_ovr_import_walk.py "$out/lib/lib/arm64-v8a/libpnsovr.so" --sites tools/pinned_ovr_sites.txt \
        --header src/quest/login/login_prerequisite_targets.h
    # #944: every site the community patch repo edits in libpnsovr.so still holds the stock word and sits in the
    # function tools/quest_patcher_sites.tsv names, so the table's mapping to our hooks is about this library.
    python3 tools/quest_patcher_sites.py --lib "$out/lib/lib/arm64-v8a/libpnsovr.so"

# #1187: every change the community patch repo applies to the Quest APK has a row of
# tools/quest_apk_patch_map.tsv (what our build does instead, the issue that will, or why none is needed), checked
# against a read-only checkout of that repo at the commit the table pins. `just test-tools` checks the table alone.
test-quest-apk-patch-map patch_repo:
    python3 tools/quest_apk_patch_map.py --repo "{{ patch_repo }}"

# The libr15 build-time patcher (docs/adr/0021): builds the host tool quest_patch_libr15 into
# build/quest-patcher/ with plain host g++. nlohmann::json comes from the tools/quest-host-deps manifest, which
# _vcpkg-quest-host installs for the x64-linux triplet (never a system package), so a clean clone builds it with no
# other build first; pass the include directory that holds nlohmann/json.hpp as json_include to use another tree.
quest-patcher json_include="": _vcpkg-quest-host
    #!/usr/bin/env bash
    set -euo pipefail
    out="build/quest-patcher"
    inc="{{ json_include }}"
    [ -n "$inc" ] || inc="build/quest-host-vcpkg/x64-linux/include"
    if [[ ! -f "$inc/nlohmann/json.hpp" ]]; then
        echo "quest-patcher: FAIL - $inc/nlohmann/json.hpp not found; 'just _vcpkg-quest-host' installs it, or pass json_include" >&2
        exit 1
    fi
    mkdir -p "$out/json_inc"
    ln -sfn "$(cd "$inc" && pwd)/nlohmann" "$out/json_inc/nlohmann"
    g++ -std=c++17 -O2 -Wall -Wextra -Werror -Isrc -Isdk/include -isystem "$out/json_inc" \
        src/quest/patch/quest_patch_libr15.cpp src/quest/patch/libr15_patcher.cpp src/quest/patch/libr15_extra_rows.cpp src/quest/patch/libr15_patcher_cli.cpp \
        src/quest/patch/libr15_patch_identity.cpp src/quest/data/sha256.cpp src/quest/sentinel/arm64_reloc.cpp \
        -o "$out/quest_patch_libr15"
    [ -x "$out/quest_patch_libr15" ] || { echo "quest-patcher: FAIL - $out/quest_patch_libr15 was not produced" >&2; exit 1; }
    echo "quest-patcher: built $out/quest_patch_libr15"

# The patcher and its tables on the host (docs/adr/0021 tests 1 to 3 and 9): the stub codec against
# binutils-checked vectors, the patch table and the build ID derived from it, and the tool on a synthetic
# ELF built in memory (the byte diff, every refusal with its row id, the extra-rows file, the command line).
# The relocator window cases run in test-quest-hooks (arm64_reloc_test). No NDK, no device,
# no real libr15. Fail-close.
test-quest-patcher json_include="": (quest-patcher json_include)
    #!/usr/bin/env bash
    set -euo pipefail
    out="build/quest-patcher"
    # _GLIBCXX_ASSERTIONS: an out-of-bounds operator[] or an empty optional dereference in the patcher aborts the
    # test that reached it, instead of reading past the buffer and passing.
    cxx=(g++ -std=c++17 -Wall -Wextra -Werror -D_GLIBCXX_ASSERTIONS -Isrc -Isdk/include -isystem "$out/json_inc" -pthread)
    scratch="$out/test-scratch"
    rm -rf "$scratch"; mkdir -p "$scratch"
    trap 'rm -rf "$scratch"' EXIT
    "${cxx[@]}" src/quest/tests/stub_codec_test.cpp -o "$out/stub_codec_test"
    timeout 60 "$out/stub_codec_test"
    "${cxx[@]}" src/quest/tests/libr15_patch_table_test.cpp src/quest/patch/libr15_patch_identity.cpp src/quest/data/sha256.cpp \
        -o "$out/libr15_patch_table_test"
    timeout 60 "$out/libr15_patch_table_test"
    "${cxx[@]}" src/quest/tests/libr15_patcher_test.cpp src/quest/patch/libr15_patcher.cpp src/quest/patch/libr15_extra_rows.cpp src/quest/patch/libr15_patcher_cli.cpp \
        src/quest/patch/libr15_patch_identity.cpp src/quest/data/sha256.cpp src/quest/sentinel/arm64_reloc.cpp \
        -o "$out/libr15_patcher_test"
    timeout 300 "$out/libr15_patcher_test" "$scratch" src/quest/tests/fixtures "$out/quest_patch_libr15"
    echo "test-quest-patcher: stub codec, patch table and patcher tests pass on the host"

# The shipped patcher on the real store libr15.so (docs/adr/0021 test 2, pinned): checks the file's SHA-256,
# runs the tool twice and checks the output against the file and readelf (build ID, exactly which byte ranges
# differ, stubs and credits words decoded back to their slots, segments, sections and end symbols). The
# libr15 is the store APK's 58 MB file: pass its path, it is not in the repository and nothing here copies it
# in. The two patched copies are written under build/quest-patcher/pinned-scratch and removed at the end.
# `signed_libr15` (docs/adr/0021 test 5): the lib/arm64-v8a/libr15.so extracted from the SIGNED APK of a repack; with it
# SignedApkLibr15IsThePatchersOutput requires that file to equal the tool's output for the stock file byte for byte and to
# carry the patched build ID. Without it that test is not run and the run says so by name.
# Fail-close, including when the file is absent.
test-quest-patcher-pinned libr15 json_include="" signed_libr15="": (quest-patcher json_include)
    #!/usr/bin/env bash
    set -euo pipefail
    libr15="{{ libr15 }}"; signed_libr15="{{ signed_libr15 }}"
    [ -f "$libr15" ] || { echo "test-quest-patcher-pinned: libr15.so not found: $libr15" >&2; exit 1; }
    [ -z "$signed_libr15" ] || [ -f "$signed_libr15" ] || { echo "test-quest-patcher-pinned: signed libr15.so not found: $signed_libr15" >&2; exit 1; }
    signed_arg=()
    if [ -n "$signed_libr15" ]; then signed_arg=("--signed-libr15=$signed_libr15"); fi
    out="build/quest-patcher"
    echo "8dd9a961b9dca8566069a4f65b3ddee9c65682c4e9c91a6d41e3c5727b1d8b20  $libr15" | sha256sum -c -
    scratch="$out/pinned-scratch"
    rm -rf "$scratch"; mkdir -p "$scratch"
    trap 'rm -rf "$scratch"' EXIT
    g++ -std=c++17 -O1 -Wall -Wextra -Werror -Isrc -Isdk/include -pthread \
        src/quest/tests/libr15_patcher_pinned_test.cpp src/quest/patch/libr15_patch_identity.cpp src/quest/data/sha256.cpp \
        -o "$out/libr15_patcher_pinned_test"
    timeout 600 "$out/libr15_patcher_pinned_test" "$out/quest_patch_libr15" "$libr15" "$scratch" ${signed_arg[@]+"${signed_arg[@]}"} | tee "$scratch/run.log"
    # With a signed file the test must have run and passed: a run that dropped it (SKIP) is not a pass of test 5.
    if [ -n "$signed_libr15" ]; then
        grep -qx 'PASS SignedApkLibr15IsThePatchersOutput' "$scratch/run.log" || { echo "test-quest-patcher-pinned: SignedApkLibr15IsThePatchersOutput did not run and pass" >&2; exit 1; }
    fi
    echo "test-quest-patcher-pinned: the tool's output on the pinned libr15 is as the table says"

# Quest social provider on the host (docs/adr/0003, "Social provider"): the ABI pins against the
# recorded vtable, the facade driven through its vtable, the hook decision through the real callback
# thunk, and the exception contract. The frames that sit under a call into the game
# (social_game_calls.cpp, social_install.cpp) and everything that includes callback_thunk.h are built
# with -fno-exceptions, and tools/check_quest_social_frames.sh pins that their objects carry no
# personality and no LSDA (with a negative control on the object that does). No NDK, no Android, no
# APK. Fail-close.
test-quest-social:
    #!/usr/bin/env bash
    set -euo pipefail
    out="build/quest-social-host"
    mkdir -p "$out"
    # nlohmann::json comes from the header the build installed from vcpkg.json (as in test-quest-integration),
    # never a system package: the CI builder image has none.
    json_src="build/{{ preset }}/vcpkg_installed/x64-mingw-static/include/nlohmann"
    if [[ ! -f "$json_src/json.hpp" ]]; then
        echo "test-quest-social: FAIL - $json_src/json.hpp not found; run 'just build' with a mingw-* preset first" >&2
        exit 1
    fi
    json_inc="$out/json_inc"
    mkdir -p "$json_inc"
    ln -sfn "$PWD/$json_src" "$json_inc/nlohmann"
    on=(g++ -std=c++17 -Wall -Wextra -Werror -Isrc -Isdk/include -Isrc/quest/sentinel -isystem "$json_inc")
    off=("${on[@]}" -fno-exceptions)
    "${on[@]}" src/quest/tests/social_abi_test.cpp -o "$out/social_abi_test"
    "$out/social_abi_test" src/quest/tests/fixtures/cnsovrsocial_vtable.txt
    "${on[@]}" -c src/quest/social/social_facade.cpp -o "$out/social_facade.o"
    "${on[@]}" -c src/quest/social/social_frames.cpp -o "$out/social_frames.o"
    "${on[@]}" -c src/quest/sentinel/hook_log.cpp -o "$out/hook_log.o"
    "${off[@]}" -c src/quest/sentinel/got_hook.cpp -o "$out/got_hook.o"
    "${off[@]}" -c src/quest/sentinel/hook_report.cpp -o "$out/hook_report.o"
    "${off[@]}" -c src/quest/social/social_game_calls.cpp -o "$out/social_game_calls.o"
    "${off[@]}" -c src/quest/social/social_install.cpp -o "$out/social_install.o"
    "${off[@]}" -c src/quest/social/social_invite_gate.cpp -o "$out/social_invite_gate.o"
    # The patched libr15's identity (sentinel::pinned::LibR15PatchedBuildId, docs/adr/0021 section 7), which the hook
    # translation units above call through the pinned_targets.h factories.
    for src in sentinel/libr15_identity.cpp patch/libr15_patch_identity.cpp data/sha256.cpp; do
        "${off[@]}" -c "src/quest/$src" -o "$out/id_$(basename "$src" .cpp).o"
    done
    ident=("$out/id_libr15_identity.o" "$out/id_libr15_patch_identity.o" "$out/id_sha256.o")
    # The friend-name decoder, as the Quest build compiles it: no static registration (the sentinel carries no
    # dynamic initializer; InstallSocialHook registers it).
    "${on[@]}" -DNEVR_SOCIAL_NAMES_NO_STATIC_REGISTRATION -c src/runtime/compat/social_names.cpp -o "$out/social_names.o"
    # The frames live across a call into the game carry no exception machinery.
    tools/check_quest_social_frames.sh nm readelf \
        "$out/social_game_calls.o=SlotUpdateEntry" "$out/social_game_calls.o=SlotResetEntry" \
        "$out/social_install.o=OnSocial" "$out/social_install.o=OnPresence" "$out/social_invite_gate.o=OnBoolean"
    if err="$(tools/check_quest_social_frames.sh nm readelf "$out/social_facade.o=Facade" 2>&1)"; then
        echo "test-quest-social: the frame checker accepted an object with landing pads (it is blind)" >&2
        exit 1
    fi
    grep -q 'personality' <<<"$err" || { echo "test-quest-social: the negative control failed for another reason: $err" >&2; exit 1; }
    "${on[@]}" src/quest/tests/social_facade_test.cpp "$out/social_facade.o" "$out/social_frames.o" \
        "$out/social_game_calls.o" "$out/hook_log.o" -o "$out/social_facade_test" -ldl -pthread
    "$out/social_facade_test"
    "${on[@]}" src/quest/tests/social_names_test.cpp "$out/social_facade.o" "$out/social_frames.o" \
        "$out/social_game_calls.o" "$out/hook_log.o" "$out/social_names.o" -o "$out/social_names_test" -ldl -pthread -lzstd
    "$out/social_names_test"
    "${off[@]}" src/quest/tests/social_install_test.cpp "$out/social_install.o" "$out/social_facade.o" \
        "$out/social_game_calls.o" "$out/got_hook.o" "$out/hook_log.o" "$out/hook_report.o" "$out/social_names.o" \
        "$out/social_invite_gate.o" "${ident[@]}" -o "$out/social_install_test" -ldl -pthread -lzstd
    timeout 300 "$out/social_install_test"  # a hang is a failure, not a stuck gate

# The social pins against the real libr15.so and libpnsovr.so (docs/adr/0003). Extracts both from
# the store APK, checks their SHA-256, runs src/quest/tests/social_pinned_test.cpp, and checks that
# the recorded vtable (src/quest/tests/fixtures/cnsovrsocial_vtable.txt) is what the library holds.
# Fail-close, including when the APK is absent (58 MB, not in the repository), so it is not part of
# `just verify`.
test-quest-social-pinned apk="build/android-arm64/repack/r15_nevr-sentinel_signed.apk" store_apk="/mnt/games/cache/r15_goldmaster_store.apk":
    #!/usr/bin/env bash
    set -euo pipefail
    apk="{{ apk }}"; store_apk="{{ store_apk }}"
    [ -f "$apk" ] || { echo "test-quest-social-pinned: pinned APK not found: $apk" >&2; exit 1; }
    [ -f "$store_apk" ] || { echo "test-quest-social-pinned: store APK not found: $store_apk" >&2; exit 1; }
    out="build/quest-social-pinned"
    rm -rf "$out"; mkdir -p "$out/lib"
    unzip -o -q "$apk" lib/arm64-v8a/libpnsovr.so -d "$out/lib"
    # libr15.so is the STORE's: the signed APK carries the patched file (docs/adr/0021 section 9), which these pins are not about.
    unzip -o -q "$store_apk" lib/arm64-v8a/libr15.so -d "$out/lib"
    lib="$out/lib/lib/arm64-v8a"
    echo "8dd9a961b9dca8566069a4f65b3ddee9c65682c4e9c91a6d41e3c5727b1d8b20  $lib/libr15.so" | sha256sum -c -
    echo "26e9a216a710d42a303346a4ca5b84037ff38250ea725dc7112b055fcacada79  $lib/libpnsovr.so" | sha256sum -c -
    g++ -std=c++17 -Wall -Wextra -Werror -Isrc -Isdk/include -Isrc/quest/sentinel -c src/quest/social/social_facade.cpp \
        -o "$out/social_facade.o"
    g++ -std=c++17 -Wall -Wextra -Werror -Isrc -Isdk/include -DNEVR_SOCIAL_NAMES_NO_STATIC_REGISTRATION -c \
        src/runtime/compat/social_names.cpp -o "$out/social_names.o"
    g++ -std=c++17 -fno-exceptions -Wall -Wextra -Werror -Isrc -Isdk/include -Isrc/quest/sentinel \
        src/quest/tests/social_pinned_test.cpp src/quest/social/social_install.cpp src/quest/social/social_invite_gate.cpp \
        src/quest/social/social_game_calls.cpp "$out/social_facade.o" "$out/social_names.o" \
        src/quest/sentinel/got_hook.cpp src/quest/sentinel/hook_log.cpp src/quest/sentinel/hook_report.cpp \
        src/quest/sentinel/libr15_identity.cpp src/quest/patch/libr15_patch_identity.cpp src/quest/data/sha256.cpp \
        -o "$out/social_pinned_test" -ldl -pthread -lzstd
    "$out/social_pinned_test" "$lib/libr15.so" "$lib/libpnsovr.so"
    "$out/social_pinned_test" --dump "$lib/libpnsovr.so" > "$out/vtable.txt"
    diff -u src/quest/tests/fixtures/cnsovrsocial_vtable.txt "$out/vtable.txt"

# Quest sentinel integration on the host (docs/adr/0003, "Integration"): the constructor sequence with
# fakes (order, gating by feature, counters before the single reporter start, one failing piece leaves the
# others running), the post-load policy, the dlopen handler and the pinned dlopen target, the identity
# source, the social switch, the frame tap, the counter budget over the real hook libraries, and the
# bridge end to end through the real loopback server and router with a fake connector. The hook
# translation units are built -fno-exceptions as on the device. No NDK, no Android. Fail-close.
test-quest-integration: _vcpkg-quest-host
    #!/usr/bin/env bash
    set -euo pipefail
    out="build/quest-integration-host"
    mkdir -p "$out"
    yaml_host="{{ justfile_directory() }}/build/quest-host-vcpkg/x64-linux"
    yaml_lib="$yaml_host/lib/libyaml-cpp.a"
    [[ -f "$yaml_lib" ]] || { echo "test-quest-integration: FAIL - $yaml_lib not found (just _vcpkg-quest-host installs it)" >&2; exit 1; }
    json_src="build/{{ preset }}/vcpkg_installed/x64-mingw-static/include/nlohmann"
    if [[ ! -f "$json_src/json.hpp" ]]; then
        echo "test-quest-integration: FAIL - $json_src/json.hpp not found; run 'just build' with a mingw-* preset first" >&2
        exit 1
    fi
    json_inc="$out/json_inc"
    mkdir -p "$json_inc"
    ln -sfn "$PWD/$json_src" "$json_inc/nlohmann"
    on=(g++ -std=c++17 -Wall -Wextra -Werror -Isrc -Isdk/include -Isrc/quest/sentinel -isystem "$json_inc" -isystem "$yaml_host/include")
    off=("${on[@]}" -fno-exceptions)
    # 1. sequence, post-load policy, identity, social switch, frame tap
    "${on[@]}" src/quest/tests/integration_sequence_test.cpp \
        src/quest/integration/ctor_sequence.cpp src/quest/integration/post_load.cpp \
        src/quest/integration/identity_source.cpp src/quest/integration/stage_log.cpp src/quest/integration/bridge_uri.cpp \
        src/quest/net/frame_tap.cpp src/quest/sentinel/hook_log.cpp src/runtime/compat/evr_codec.cpp \
        src/quest/integration/self_check_wiring.cpp src/runtime/compat/self_check.cpp \
        -o "$out/integration_sequence_test" -pthread
    timeout 300 "$out/integration_sequence_test"
    # 2. the hook translation units and the counter budget
    "${off[@]}" -c src/quest/sentinel/got_hook.cpp -o "$out/got_hook.o"
    "${off[@]}" -c src/quest/sentinel/hook_report.cpp -o "$out/hook_report.o"
    "${off[@]}" -c src/quest/redirect/tstring_thunks.cpp -o "$out/tstring_thunks.o"
    "${off[@]}" -c src/quest/integration/dlopen_hook.cpp -o "$out/dlopen_hook.o"
    "${off[@]}" -c src/quest/integration/social_shim.cpp -o "$out/social_shim.o"
    "${off[@]}" -c src/quest/sentinel/login_prompt_hook.cpp -o "$out/login_prompt_hook.o"
    "${off[@]}" -c src/quest/invite/frame_dispatch.cpp -o "$out/frame_dispatch.o"
    "${off[@]}" -c src/quest/sentinel/login_title_hook.cpp -o "$out/login_title_hook.o"
    "${off[@]}" -c src/quest/sentinel/obb_skip_hook.cpp -o "$out/obb_skip_hook.o"
    "${off[@]}" -c src/quest/sentinel/texture_quality_hook.cpp -o "$out/texture_quality_hook.o"
    "${off[@]}" -c src/quest/sentinel/texture_menu_hook.cpp -o "$out/texture_menu_hook.o"
    "${on[@]}" -c src/quest/sentinel/texture_menu.cpp -o "$out/texture_menu.o"
    "${off[@]}" -c src/quest/sentinel/quit_hook.cpp -o "$out/quit_hook.o"
    "${off[@]}" -c src/quest/sentinel/resource_request_hook.cpp -o "$out/resource_request_hook.o"
    "${off[@]}" -c src/quest/sentinel/clock_levels_hook.cpp -o "$out/clock_levels_hook.o"
    "${off[@]}" -c src/quest/login/login_counters.cpp -o "$out/login_counters.o"
    # login_counters.cpp reads the local-answer counters of login_prerequisites.cpp (#411).
    "${off[@]}" -c src/quest/login/login_prerequisites.cpp -o "$out/login_prerequisites.o"
    "${off[@]}" -c src/quest/login/login_standin.cpp -o "$out/login_standin.o"
    "${off[@]}" -c src/quest/auth/prompt_board.cpp -o "$out/prompt_board.o"
    "${off[@]}" -c src/quest/social/social_game_calls.cpp -o "$out/social_game_calls.o"
    "${off[@]}" -c src/quest/social/social_install.cpp -o "$out/social_install.o"
    "${off[@]}" -c src/quest/social/social_invite_gate.cpp -o "$out/social_invite_gate.o"
    # The patched libr15's identity (sentinel::pinned::LibR15PatchedBuildId, docs/adr/0021 section 7), which the hook
    # translation units above call through the pinned_targets.h factories.
    for src in sentinel/libr15_identity.cpp patch/libr15_patch_identity.cpp data/sha256.cpp; do
        "${off[@]}" -c "src/quest/$src" -o "$out/id_$(basename "$src" .cpp).o"
    done
    ident=("$out/id_libr15_identity.o" "$out/id_libr15_patch_identity.o" "$out/id_sha256.o")
    "${on[@]}" -c src/quest/social/social_facade.cpp -o "$out/social_facade.o"
    "${on[@]}" -c src/quest/sentinel/hook_log.cpp -o "$out/hook_log.o"
    "${on[@]}" -DNEVR_SOCIAL_NAMES_NO_STATIC_REGISTRATION -c src/runtime/compat/social_names.cpp -o "$out/social_names.o"
    # The dlopen hook's frames carry no personality (the one helper it calls is annotated and lives in
    # another translation unit).
    readelf --debug-dump=frames "$out/dlopen_hook.o" > "$out/dlopen_hook.frames.txt"
    if grep -q '"zPLR"' "$out/dlopen_hook.frames.txt"; then
        echo "test-quest-integration: FAIL - dlopen_hook.o has frames under a personality CIE (zPLR)" >&2
        exit 1
    fi
    grep -q '"zR"' "$out/dlopen_hook.frames.txt" || { echo "test-quest-integration: FAIL - dlopen_hook.o has no zR frames to check" >&2; exit 1; }
    "${on[@]}" src/quest/tests/integration_hooks_test.cpp src/quest/integration/post_load.cpp src/quest/integration/ctor_sequence.cpp \
        src/quest/redirect/service_redirector.cpp src/quest/redirect/hook_adapter.cpp \
        src/quest/sentinel/quest_config.cpp src/core/nevr_config.cpp src/runtime/lifecycle/service_redirect.cpp \
        src/runtime/lifecycle/stable_string_pool.cpp \
        "$out/got_hook.o" "$out/hook_report.o" "$out/tstring_thunks.o" "$out/dlopen_hook.o" "$out/social_shim.o" \
        "$out/social_game_calls.o" "$out/social_install.o" "$out/social_invite_gate.o" "$out/social_facade.o" "$out/hook_log.o" "$out/social_names.o" \
        "$out/login_prompt_hook.o" "$out/frame_dispatch.o" "$out/login_title_hook.o" "$out/obb_skip_hook.o" "$out/resource_request_hook.o" "$out/texture_quality_hook.o" "$out/texture_menu_hook.o" "$out/texture_menu.o" "$out/clock_levels_hook.o" "$out/prompt_board.o" "$out/login_counters.o" \
        "$out/login_prerequisites.o" "$out/login_standin.o" \
        "${ident[@]}" "$yaml_lib" -o "$out/integration_hooks_test" -ldl -pthread -lzstd
    timeout 300 "$out/integration_hooks_test"
    # 3. the bridge end to end, with a fake connector (SessionBridge still links the libcurl connector it
    # builds when none is injected)
    "${on[@]}" -pthread $(pkg-config --cflags libcurl) \
        src/quest/tests/session_bridge_test.cpp src/quest/net/session_bridge.cpp src/quest/net/frame_tap.cpp \
        src/quest/net/curl_ws_connector.cpp src/quest/auth/ca_bundle.cpp \
        src/quest/net/ws_wire.cpp src/quest/net/loopback_game_server.cpp src/quest/net/remote_ws.cpp \
        src/runtime/compat/session_router.cpp src/runtime/compat/evr_codec.cpp src/runtime/server/serverdb_uri.cpp \
        -o "$out/session_bridge_test" $(pkg-config --libs libcurl) -lssl -lcrypto
    timeout 300 "$out/session_bridge_test"
    echo "test-quest-integration: all integration tests pass on the host"

# The Quest plugin loader on the host (docs/adr/0019-quest-plugin-loader.md, slice 1): the plan, the ELF and build-ID
# checks, the loader against real shared objects built from the fixture plugin (one lib<variant>.so per way a plugin can be
# right or wrong, each with a GNU build ID), the record log, the platform, and the sentinel's binding of it (the counts the
# constructor sequence acts on, once-only planning, the finish that never waits on a running pass). x86-64 fixtures, so the
# loader's machine check is told AArch64 or x86-64 explicitly. No NDK, no Android. Fail-close.
test-quest-plugins:
    #!/usr/bin/env bash
    set -euo pipefail
    out="build/quest-plugins-host"
    mkdir -p "$out"
    json_src="build/{{ preset }}/vcpkg_installed/x64-mingw-static/include/nlohmann"
    if [[ ! -f "$json_src/json.hpp" ]]; then
        echo "test-quest-plugins: FAIL - $json_src/json.hpp not found; run 'just build' with a mingw-* preset first" >&2
        exit 1
    fi
    json_inc="$out/json_inc"
    mkdir -p "$json_inc"
    ln -sfn "$PWD/$json_src" "$json_inc/nlohmann"
    cxx=(g++ -std=c++17 -Wall -Wextra -Werror -Isrc -Isdk/include -Isrc/quest/sentinel -Isrc/quest/tests/stub -isystem "$json_inc")
    fixture() { # library-file-stem plugin-name [macros...]
        local stem="$1" name="$2"; shift 2
        "${cxx[@]}" -shared -fPIC -Wl,--build-id=sha1 -DFX_NAME="\"$name\"" "$@" \
            src/quest/tests/plugin_fixture.cpp -o "$out/lib$stem.so"
    }
    fixture fx_ok ok
    fixture fx_second second
    fixture fx_noids noids -DFX_NO_IDS
    fixture fx_otherids otherids -DFX_IDS='"3333333333333333333333333333333333333333"'
    fixture fx_initfail initfail -DFX_INIT_RC=3
    fixture fx_noinfo noinfo -DFX_NO_INFO
    fixture fx_legacy legacy -DFX_LEGACY_INIT
    # The files that include callback_thunk.h (through pinned_targets.h) are built without exceptions, as on the device.
    "${cxx[@]}" -fno-exceptions -c src/quest/sentinel/got_hook.cpp -o "$out/got_hook.o"
    "${cxx[@]}" -fno-exceptions -c src/quest/sentinel/hook_report.cpp -o "$out/hook_report.o"
    "${cxx[@]}" -fno-exceptions -c src/quest/plugin/plugin_game_image.cpp -o "$out/plugin_game_image.o"
    "${cxx[@]}" -fno-exceptions -c src/quest/plugin/input_seam_hook.cpp -o "$out/input_seam_hook.o"
    "${cxx[@]}" -fno-exceptions -c src/quest/sentinel/libr15_identity.cpp -o "$out/libr15_identity.o"
    loader=(src/quest/plugin/packaged_plugins.cpp src/quest/plugin/plugin_plan.cpp src/quest/plugin/plugin_image.cpp src/quest/plugin/plugin_loader.cpp
            src/quest/plugin/plugin_record_log.cpp src/quest/plugin/plugin_platform.cpp src/quest/data/sha256.cpp
            src/quest/sentinel/hook_log.cpp src/quest/sentinel/sentinel_log.cpp src/core/crash_report.cpp
            "$out/libr15_identity.o" src/quest/patch/libr15_patch_identity.cpp
            "$out/got_hook.o" "$out/hook_report.o" "$out/plugin_game_image.o" "$out/input_seam_hook.o")
    "${cxx[@]}" -pthread src/quest/tests/plugin_loader_test.cpp "${loader[@]}" \
        -o "$out/plugin_loader_test" -ldl
    timeout -k 5 120 "$out/plugin_loader_test" "$out"
    "${cxx[@]}" -pthread src/quest/tests/packaged_plugins_test.cpp src/quest/plugin/packaged_plugins.cpp \
        src/quest/plugin/plugin_plan.cpp src/runtime/ext/plugin_load_plan.cpp -o "$out/packaged_plugins_test"
    timeout -k 5 120 "$out/packaged_plugins_test" src/quest/tests/fixtures
    "${cxx[@]}" -pthread src/quest/tests/plugin_runtime_test.cpp src/quest/plugin/plugin_runtime.cpp \
        src/runtime/ext/plugin_load_plan.cpp "${loader[@]}" -o "$out/plugin_runtime_test" -ldl
    timeout -k 5 120 "$out/plugin_runtime_test" "$out"
    # The constructor's attach step (docs/adr/0021 sections 6, 7, 10, 12): the real Attach on the shipped patcher's output laid
    # out in host memory, and the plugin identity and build-ID alias it registers. The patcher and its identity are built with
    # exceptions (nlohmann::json); the sentinel half is built without, as on the device. The identity objects are the loader's.
    for src in src/quest/patch/libr15_patcher.cpp src/quest/patch/libr15_extra_rows.cpp; do
        "${cxx[@]}" -D_GLIBCXX_ASSERTIONS -c "$src" -o "$out/step_$(basename "$src" .cpp).o"
    done
    "${cxx[@]}" -fno-exceptions -c src/quest/integration/libr15_attach_step.cpp -o "$out/step_libr15_attach_step.o"
    for src in libr15_attach process_end_hook arm64_reloc; do
        "${cxx[@]}" -fno-exceptions -D_GLIBCXX_ASSERTIONS -c "src/quest/sentinel/$src.cpp" -o "$out/step_$src.o"
    done
    "${cxx[@]}" -pthread -Isrc/quest/tests -D_GLIBCXX_ASSERTIONS src/quest/tests/libr15_attach_step_test.cpp \
        "$out/step_libr15_attach_step.o" "$out/step_libr15_patcher.o" "$out/step_libr15_extra_rows.o" \
        "$out/step_libr15_attach.o" "$out/step_process_end_hook.o" "$out/step_arm64_reloc.o" \
        "${loader[@]}" -o "$out/libr15_attach_step_test" -ldl
    timeout -k 5 120 "$out/libr15_attach_step_test" src/quest/tests/fixtures
    echo "test-quest-plugins: all plugin loader tests pass on the host"

# The q15t plugin's host tests (src/quest/tests/q15t/): the plugin built for the host against fake game memory and a
# fake loader, one program per module, in the capped build slice. No NDK for most; the modules whose tests cross-compile find the
# NDK at ANDROID_NDK_HOME. The mutation run (tests/mutants.sh, about an hour) is not part of this gate. Fail-close.
test-quest-q15t:
    bash src/quest/tests/q15t/run_host_tests.sh

# The Quest autopilot plugin on the host (#1202, docs/adr/0025-quest-autopilot-route.md): the route grammar, every
# derived bit and limit, the clock, the filter against a fake clock, the event ring (two producers), and the plugin's
# exports against a fake host with a real route file and its real reporter thread; and every shipped route file
# (tools/quest_autopilot_routes/) parses. The start-file case and the two stopped-run cases each run in their own process
# (the plugin keeps one run per process). Built as the device builds it, without exceptions. Fail-close.
test-quest-autopilot:
    #!/usr/bin/env bash
    set -euo pipefail
    out="build/quest-autopilot-host"
    mkdir -p "$out/scratch"
    # nlohmann::json (plugin.cpp) comes from the header the build installed from vcpkg.json, never a system package:
    # the CI builder image has none.
    json_src="build/{{ preset }}/vcpkg_installed/x64-mingw-static/include/nlohmann"
    if [[ ! -f "$json_src/json.hpp" ]]; then
        echo "test-quest-autopilot: FAIL - $json_src/json.hpp not found; run 'just build' with a mingw-* preset first" >&2
        exit 1
    fi
    json_inc="$out/json_inc"
    mkdir -p "$json_inc"
    ln -sfn "$PWD/$json_src" "$json_inc/nlohmann"
    cxx=(g++ -std=c++17 -Wall -Wextra -Werror -fno-exceptions -pthread -Isrc -Isdk/include -isystem "$json_inc")
    "${cxx[@]}" src/quest/tests/autopilot_route_test.cpp src/quest/plugins/autopilot/route.cpp \
        -o "$out/autopilot_route_test"
    routes=(tools/quest_autopilot_routes/*.txt)
    [ -f "${routes[0]}" ] || { echo "test-quest-autopilot: no shipped route file" >&2; exit 1; }
    timeout 60 "$out/autopilot_route_test" "${routes[@]}"
    "${cxx[@]}" -DNEVR_AUTOPILOT_BUILD_IDS='"0000000000000000000000000000000000000000"' \
        src/quest/tests/autopilot_run_test.cpp src/quest/plugins/autopilot/route.cpp \
        src/quest/plugins/autopilot/autopilot.cpp src/quest/plugins/autopilot/plugin.cpp src/quest/data/sha256.cpp \
        -o "$out/autopilot_run_test"
    timeout 120 "$out/autopilot_run_test" "$out/scratch"
    timeout 60 "$out/autopilot_run_test" "$out/scratch" start_file
    timeout 60 "$out/autopilot_run_test" "$out/scratch" stopped
    timeout 60 "$out/autopilot_run_test" "$out/scratch" stopped_waiting
    timeout 60 "$out/autopilot_run_test" "$out/scratch" fd_zero
    echo "test-quest-autopilot: all autopilot tests pass on the host"

# The integration's pinned dlopen target against the real libr15.so (docs/adr/0003). Extracts it from the
# store APK, checks its SHA-256 and resolves LibR15Dlopen() the way the loader would lay the image out.
# Fail-close, including when the APK is absent (58 MB, not in the repository), so it is not part of
# `just verify`.
test-quest-integration-pinned apk="build/android-arm64/repack/r15_nevr-sentinel_signed.apk" store_apk="/mnt/games/cache/r15_goldmaster_store.apk":
    #!/usr/bin/env bash
    set -euo pipefail
    apk="{{ apk }}"; store_apk="{{ store_apk }}"
    [ -f "$apk" ] || { echo "test-quest-integration-pinned: pinned APK not found: $apk" >&2; exit 1; }
    [ -f "$store_apk" ] || { echo "test-quest-integration-pinned: store APK not found: $store_apk" >&2; exit 1; }
    out="build/quest-integration-pinned"
    mkdir -p "$out/lib"
    # libr15.so is the STORE's: the signed APK carries the patched file (docs/adr/0021 section 9), which these pins are not about.
    unzip -o -q "$store_apk" lib/arm64-v8a/libr15.so -d "$out/lib"
    lib="$out/lib/lib/arm64-v8a"
    echo "8dd9a961b9dca8566069a4f65b3ddee9c65682c4e9c91a6d41e3c5727b1d8b20  $lib/libr15.so" | sha256sum -c -
    off=(g++ -std=c++17 -Wall -Wextra -Werror -fno-exceptions -Isrc -Isdk/include -Isrc/quest/sentinel)
    "${off[@]}" -c src/quest/integration/dlopen_hook.cpp -o "$out/dlopen_hook.o"
    g++ -std=c++17 -Wall -Wextra -Werror -Isrc -Isdk/include -Isrc/quest/sentinel -c src/quest/integration/post_load.cpp -o "$out/post_load.o"
    "${off[@]}" src/quest/tests/integration_pinned_test.cpp "$out/dlopen_hook.o" "$out/post_load.o" \
        src/quest/sentinel/got_hook.cpp src/quest/sentinel/hook_log.cpp src/quest/sentinel/hook_report.cpp \
        src/quest/sentinel/libr15_identity.cpp src/quest/patch/libr15_patch_identity.cpp src/quest/data/sha256.cpp \
        -o "$out/integration_pinned_test" -ldl -pthread
    "$out/integration_pinned_test" "$lib/libr15.so"

# --- Verify (closed-loop gate) ---

# docs/lua-scripting.md. Fail-close: a missing submodule or header, a build error, a failed check or
# fewer tests than the floor exits nonzero. Part of `just verify`.
# Build src/scripting for mingw and run its tests under wine, then the script type checker on the host.
test-scripting:
    #!/usr/bin/env bash
    set -euo pipefail
    unset VCPKG_ROOT
    json="$PWD/build/{{ preset }}/vcpkg_installed/x64-mingw-static/include"
    if [[ ! -f "$json/nlohmann/json.hpp" ]]; then
        echo "test-scripting: FAIL — $json/nlohmann/json.hpp not found; run 'just build' with a mingw-* preset first" >&2
        exit 1
    fi
    if [[ ! -f extern/luau/VM/src/lapi.cpp ]]; then
        echo "test-scripting: FAIL — extern/luau is empty; run: git submodule update --init extern/luau" >&2
        exit 1
    fi
    export NEVR_TEST_TMPDIR="$PWD/build/scripting-mingw-shared/tmp"
    mkdir -p "$NEVR_TEST_TMPDIR"
    # One test executable under wine: it must exit 0, print "<n> tests, 0 failed checks" and run at least
    # the floor, so a test that silently stops being registered fails the gate.
    run_suite() { # $1 executable, $2 minimum number of tests
        local log rc=0 summary n failed
        log=$(WINEDEBUG=-all timeout -k 10 600 wine "$1" 2>&1) || rc=$?
        log=${log//$'\r'/}  # wine's console writes CRLF
        if ! summary=$(grep -E '^[0-9]+ tests, [0-9]+ failed checks$' <<<"$log"); then
            printf '%s\n' "$log" >&2
            echo "test-scripting: FAIL — $1 printed no test summary (exit $rc)" >&2
            exit 1
        fi
        read -r n _ failed _ <<<"$(tail -n 1 <<<"$summary" | tr -d ',')"
        if [[ "$rc" -ne 0 || "$failed" -ne 0 || "$n" -lt "$2" ]]; then
            printf '%s\n' "$log" >&2
            echo "test-scripting: FAIL — $1: exit $rc, $n tests (floor $2), $failed failed checks" >&2
            exit 1
        fi
        echo "test-scripting: [$mode] $(basename "$1"): $n tests, 0 failed checks"
    }
    # Both modes of the Luau binding: the shared state (the default) and one state per script (for
    # debugging, NEVR_LUAU_SHARED_STATE=OFF). Neither may rot.
    for mode in shared pervm; do
        out="$PWD/build/scripting-mingw-$mode"
        flag=ON
        [[ "$mode" == pervm ]] && flag=OFF
        cmake -S src/scripting -B "$out" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/toolchain-mingw64.cmake" \
            -DCMAKE_BUILD_TYPE=Release -DNEVR_SCRIPT_VM=luau -DNEVR_JSON_INCLUDE_DIR="$json" \
            -DNEVR_LUAU_SHARED_STATE="$flag" > /dev/null
        cmake --build "$out"
        run_suite "$out/nevr_script_registry_test.exe" 20
        run_suite "$out/nevr_script_host_test.exe" 12
        run_suite "$out/nevr_script_stubs_test.exe" 3
        run_suite "$out/nevr_script_conformance.exe" 28
        run_suite "$out/vm/luau/nevr_luau_memory_test.exe" 5
        if [[ "$mode" == shared ]]; then
            run_suite "$out/nevr_luau_shared_test.exe" 10
        fi
        # The two runaway probes report instead of asserting; each must end with the script stopped.
        dos=$(WINEDEBUG=-all timeout -k 10 120 wine "$out/nevr_script_conformance.exe" --pattern-dos 2>&1)
        dos=${dos//$'\r'/}
        if ! grep -qE '^T5d pattern_dos: returned after .* owner disabled$' <<<"$dos"; then
            printf '%s\n' "$dos" >&2
            echo "test-scripting: FAIL — [$mode] a runaway inside string.find was not stopped" >&2
            exit 1
        fi
        dos=$(WINEDEBUG=-all timeout -k 10 120 wine "$out/nevr_script_conformance.exe" --gc-dos 2>&1)
        dos=${dos//$'\r'/}
        if ! grep -qE '^T5e gc_dos: returned after ' <<<"$dos"; then
            printf '%s\n' "$dos" >&2
            echo "test-scripting: FAIL — [$mode] a looping finalizer was not stopped" >&2
            exit 1
        fi
        echo "test-scripting: [$mode] --pattern-dos and --gc-dos stopped"
    done
    # The author-side checker: the definitions come from a host build of the stubs generator.
    host="$PWD/build/scripting-host"
    mkdir -p "$host"
    g++ -std=c++17 -O1 -Wall -Wextra -Werror -pthread -Isrc -Isdk/include -isystem "$json" \
        src/scripting/host_registry.cpp src/scripting/script_stubs.cpp src/scripting/tests/script_stubs_test.cpp \
        -o "$host/script_stubs_test"
    cmake -S src/scripting/check -B "$PWD/build/script-check" -G Ninja -DCMAKE_BUILD_TYPE=Release > /dev/null
    cmake --build "$PWD/build/script-check"
    src/scripting/check/check_test.sh "$host/script_stubs_test" "$PWD/build/script-check/nevr_script_check" "$host/check"

# Aggregate verify gate for the all-the-way-down canon: build everything, then run
# the hardened C++ GTest suite under Wine. Fail-close: exits nonzero on any failure.
# Success derives from the real compiler/linker + test artifacts, not a proxy
# [day-one-kit §5]. EXCLUDES the Go integration suites — the evr-test-harness
# dependency is excised [RULINGS.md 2026-07-20 "Test harness excised"].
verify:
    #!/usr/bin/env bash
    set -euo pipefail
    unset VCPKG_ROOT
    # The log names what it tested, so a pass is never re-run to prove it.
    echo "verify: HEAD $(git rev-parse HEAD)"
    dirty="$(git status --porcelain)"
    echo "verify: tree $( [ -z "$dirty" ] && echo clean || echo DIRTY)"
    # A dirty tree names its files, so the log says what differs from the commit it names (#844).
    if [ -n "$dirty" ]; then printf '%s\n' "$dirty" | sed 's/^/verify: dirty /'; fi
    just build
    # The DLL embeds exactly the public defaults file (the build reads no other source).
    python3 tools/check_embedded_defaults.py --header build/{{ preset }}/generated/nevr_builtin_defaults.h --binary build/{{ preset }}/bin/nevr-runtime.dll
    # --- Parallel test block --------------------------------------------------
    # Ten independent suites run concurrently (capped at nproc) instead of one after
    # another: each test-quest-* compiles its own host binary in its own out dir and runs
    # it, so the serial wall clock was ~10 compiles back to back. The shared vcpkg host
    # prereq (yaml-cpp) is built once, serially, here, so the workers that depend on it hit
    # the idempotent guard in _vcpkg-quest-host and never run vcpkg concurrently.
    # tools/verify-parallel.sh fails closed and names the failing suite: any suite failing
    # fails the whole gate. The suite list below is the gate's full coverage.
    just _vcpkg-quest-host
    bash tools/verify-parallel.sh \
        test-auth-unit \
        test-quest-shared \
        test-quest-hooks \
        test-quest-router \
        test-quest-tls \
        test-quest-redirect \
        test-quest-social \
        test-quest-data \
        test-quest-data-net \
        test-quest-integration \
        test-quest-invite \
        test-quest-plugins \
        test-quest-autopilot \
        test-quest-q15t \
        test-scripting
    # --- Sensor plumbing (N93) -----------------------------------------------
    # Under `set -o pipefail` a pipeline returns the RIGHTMOST nonzero status.
    # In `if grep A … | grep -v B; then FAIL; fi` a stage-1 hard error (rc 2 —
    # unreadable subject, bad pattern) hands stage 2 an empty stream, stage 2
    # answers "no match" (rc 1), and the positive-form `if` reads the pipeline as
    # clean: the sensor PASSES while observing nothing. Measured 2026-07-28:
    # `grep -rn P /nonexistent` alone rc=2; piped through `grep -v legacy` rc=1.
    # Rule: stage 1 runs ALONE, output and exit code captured; sensor_stage1
    # asserts the code before any filtering (rc 0 = hits, rc 1 = genuinely no
    # match, rc >= 2 = abort naming the subject). It is called OUTSIDE any
    # pipeline on purpose — a pipeline element runs in a subshell, so an `exit 1`
    # inside one kills only the subshell and the recipe carries on, reproducing
    # the exact blindness this helper removes. Captures are per-sensor variables
    # so `set -u` catches a deleted capture that still has a consumer.
    # KNOWN-OPEN (measured, ledger N93): the single-stage positive sensors
    # (`if grep -q P subject; then FAIL`) are blind the same way — `if` cannot
    # tell rc 1 from rc 2. Left for a follow-up unit; do not add new ones.
    sensor_stage1() { # $1 sensor name, $2 subject, $3 stage-1 exit code
        if [ "$3" -ge 2 ]; then
            echo "verify: FAIL — sensor '$1': stage-1 exited $3 (an error, not 'no match'). The sensor could not read its subject: $2" >&2
            exit 1
        fi
    }
    # Same class, different shape: an awk body-extraction that finds nothing
    # (renamed function, restructured file) exits 0 with EMPTY output, and every
    # downstream check of the empty body silently passes.
    sensor_nonempty() { # $1 sensor name, $2 what was being extracted, $3 captured text
        if [ -z "$3" ]; then
            echo "verify: FAIL — sensor '$1': subject extraction produced nothing ($2 not found). The sensor is watching a range that no longer exists." >&2
            exit 1
        fi
    }
    # SIGPIPE + pipefail = a FALSE FAIL (N101). `cmd | grep -q P` under
    # `set -o pipefail` returns 141 WHEN THE PATTERN IS FOUND: grep -q exits at
    # the first match, the upstream write end is still writing, the kernel raises
    # SIGPIPE on it, and pipefail promotes 141 to the pipeline status. `if !`
    # then takes the FAIL branch on a correct tree. It is a RACE — it only fires
    # when the match sits early in a capture large enough that the writer has not
    # finished. Measured: 55,887-byte capture, match at line 185 of 1350 -> rc=141.
    # Rule: feed a captured variable to grep with a HERESTRING, never a pipeline.
    #   RIGHT:  grep -qF "$pat" <<<"$CAPTURE"
    #   WRONG:  printf '%s\n' "$CAPTURE" | grep -qF "$pat"
    # (Pipelines whose last stage reads ALL input -- grep -c, grep -v, plain
    # grep -- are safe; only early-exit readers like grep -q race.)
    # THE COMMENT STRIPPER (N99). Sensors strip comment lines before matching so
    # that the prose explaining a rule cannot satisfy the rule's own check. The
    # obvious spelling for that is `^[[:space:]]*(//|\*|/\*)` — and it is WRONG
    # in C: `  *engineFlags &= MASK;` is a pointer dereference at statement
    # start, and `^[[:space:]]*\*` eats it. The N99 sensor was written that way,
    # deleted the exact line it was watching, and failed on a CORRECT tree
    # (measured: seven consecutive `just verify` runs red, including the clean
    # control). A `*` begins a comment continuation only when followed by
    # whitespace, `/`, or end of line. Use this spelling, not the obvious one.
    #   ^-anchored (file content):  ^[[:space:]]*(//|/\*|\*[[:space:]/]|\*$)
    #   :-anchored (grep -n output): :[[:space:]]*(//|/\*|\*[[:space:]/]|\*$)
    # N34/N103: src/gameserver/ does not exist. This guard keeps the tree from
    # being recreated and wired: a second gameserver copy is how N48 shipped
    # half-implemented for five weeks. CMake would hard-fail on a missing
    # directory, but this names the reason instead of the symptom.
    if grep -Pn '^\s*add_subdirectory\s*\(\s*src/gameserver\s*\)' CMakeLists.txt; then
        echo "verify: FAIL — src/gameserver/ must not come back (N103). The compiled path is src/runtime/server/." >&2
        echo "Re-adding that tree recreates the two-copy split that let N48 ship half-implemented. Route the change to src/runtime/server/." >&2
        exit 1
    fi
    # Quest remote transport (ADR 0003): TLS verification and routing are part of the source, not a setting.
    # This sensor is a tripwire for an edit that relaxes them, NOT the guarantee: test-quest-tls drives the
    # real connector against real servers and is what proves the behaviour. It is deliberately strict and
    # literal-minded: comment-stripped (N99 spelling), herestring-fed (N101), case-insensitive on option
    # names (libcurl's protocol strings are case-insensitive too), and it requires
    #   (a) every curl_easy_setopt call to name its option with a CURLOPT_ literal, so no variable can
    #       smuggle an option id past it;
    #   (b) every CURLOPT_ name in the file to be on the allowlist below (anything else fails);
    #   (c) each security-critical option to be set EXACTLY ONCE, with its required value.
    QTLS_RC=0; QTLS_CODE=$(grep -vE '^[[:space:]]*(//|/\*|\*[[:space:]/]|\*$)' src/quest/net/curl_ws_connector.cpp) || QTLS_RC=$?
    sensor_stage1 "Quest TLS verification" "src/quest/net/curl_ws_connector.cpp" "$QTLS_RC"
    sensor_nonempty "Quest TLS verification" "non-comment lines of src/quest/net/curl_ws_connector.cpp" "$QTLS_CODE"
    qtls_fail() { echo "verify: FAIL — Quest TLS verification: $1 (ADR 0003: the remote WebSocket stays verified, wss-only, proxy-free and redirect-free)." >&2; exit 1; }
    QTLS_CALLS=$(grep -ciE 'curl_easy_setopt' <<<"$QTLS_CODE" || true)
    QTLS_LITERAL=$(grep -ciE 'curl_easy_setopt\(curl,[[:space:]]*CURLOPT_[A-Z0-9_]+,' <<<"$QTLS_CODE" || true)
    [ "$QTLS_CALLS" = "$QTLS_LITERAL" ] || qtls_fail "$QTLS_CALLS curl_easy_setopt line(s) but only $QTLS_LITERAL name their option with a direct CURLOPT_ literal"
    if grep -qiE 'curl_easy_option|curl_easy_setopt[^(]' <<<"$QTLS_CODE"; then qtls_fail "curl_easy_option_* or an indirect curl_easy_setopt reference"; fi
    QTLS_ALLOWED=" CURLOPT_URL CURLOPT_CONNECT_ONLY CURLOPT_HTTPHEADER CURLOPT_NOSIGNAL CURLOPT_CONNECTTIMEOUT CURLOPT_TIMEOUT CURLOPT_XFERINFODATA CURLOPT_XFERINFOFUNCTION CURLOPT_NOPROGRESS CURLOPT_FOLLOWLOCATION CURLOPT_NOPROXY CURLOPT_PROTOCOLS_STR CURLOPT_SSL_VERIFYPEER CURLOPT_SSL_VERIFYHOST CURLOPT_SSLVERSION CURLOPT_CAINFO_BLOB "
    QTLS_NAMES=$(grep -oiE 'CURLOPT_[A-Z0-9_]+' <<<"$QTLS_CODE" || true)
    while IFS= read -r qtls_name; do
        [ -n "$qtls_name" ] || continue
        qtls_upper=$(tr '[:lower:]' '[:upper:]' <<<"$qtls_name")
        case "$QTLS_ALLOWED" in *" $qtls_upper "*) ;; *) qtls_fail "option $qtls_name is not on the connector's allowlist" ;; esac
        [ "$qtls_name" = "$qtls_upper" ] || qtls_fail "option name $qtls_name is not spelled in upper case"
    done <<<"$QTLS_NAMES"
    for pair in 'CURLOPT_SSL_VERIFYPEER|1L' 'CURLOPT_SSL_VERIFYHOST|2L' 'CURLOPT_PROTOCOLS_STR|"wss"' 'CURLOPT_FOLLOWLOCATION|0L' 'CURLOPT_NOPROXY|"\*"' 'CURLOPT_CAINFO_BLOB|&blob' 'CURLOPT_SSLVERSION|static_cast<long>\(CURL_SSLVERSION_TLSv1_2\)'; do
        qtls_opt="${pair%%|*}"; qtls_val="${pair#*|}"
        qtls_sets=$(grep -ciE "curl_easy_setopt\(curl,[[:space:]]*${qtls_opt}," <<<"$QTLS_CODE" || true)
        qtls_good=$(grep -cE "curl_easy_setopt\(curl, ${qtls_opt}, ${qtls_val}\)" <<<"$QTLS_CODE" || true)
        [ "$qtls_sets" = "1" ] || qtls_fail "$qtls_opt is set $qtls_sets time(s); it must be set exactly once"
        [ "$qtls_good" = "1" ] || qtls_fail "$qtls_opt is not set to its required value"
    done
    QTLS_PROTO=$(grep -ciE 'CURLOPT_[A-Z_]*PROTO' <<<"$QTLS_CODE" || true)
    [ "$QTLS_PROTO" = "1" ] || qtls_fail "$QTLS_PROTO protocol-related option(s); exactly one (CURLOPT_PROTOCOLS_STR) is allowed"
    # N111: the per-frame dispatcher, COMMENT-STRIPPED once and reused below.
    # `grep -q 'EnsureStackReserve()' tick.cpp` matches `// EnsureStackReserve();`
    # just as happily as the real call, so every one of these "the call site is
    # present" sensors passed on a commented-out call site. Measured 2026-07-29:
    # commenting the call out left `just verify` GREEN.
    # Herestring, never a pipeline — `grep -vE ... | grep -q P` returns 141 on a
    # match because grep -q exits early and SIGPIPEs stage 1 (N101).
    TICK_RC=0; TICK_CODE=$(grep -vE '^[[:space:]]*(//|/\*|\*[[:space:]/]|\*$)' src/runtime/frame/tick.cpp) || TICK_RC=$?
    sensor_stage1 "N111 tick dispatcher" "src/runtime/frame/tick.cpp" "$TICK_RC"
    sensor_nonempty "N111 tick dispatcher" "non-comment lines of src/runtime/frame/tick.cpp" "$TICK_CODE"

    # Wave I — source-level verifiers: each fix must have its call site present.
    # These are automated red→green tests: they fail when the call site is missing
    # (the bug state) and pass when the fix is in place.
    # N59: PatchDscProvider call site in initialize.cpp
    if ! grep -q 'PatchDscProvider()' src/runtime/lifecycle/initialize.cpp; then
        echo "verify: FAIL — N59 PatchDscProvider call site missing from initialize.cpp" >&2
        exit 1
    fi
    # N68: TickPlugins/TickModules wired into per-frame hook
    if ! grep -q 'TickPlugins' <<<"$TICK_CODE"; then
        echo "verify: FAIL — N68 TickPlugins call site missing from per-frame hook" >&2
        exit 1
    fi
    if ! grep -q 'TickModules' <<<"$TICK_CODE"; then
        echo "verify: FAIL — N68 TickModules call site missing from per-frame hook" >&2
        exit 1
    fi
    if ! grep -q 'NotifyModulesStateChange' src/runtime/lifecycle/state_machine.cpp; then
        echo "verify: FAIL — N68 NotifyModulesStateChange call site missing from state path" >&2
        exit 1
    fi
    # N60: stop() must be called OUTSIDE g_pairsMutex lock (deadlock prevention)
    if ! grep -q 'remoteToStop->stop()' src/runtime/compat/ws_bridge.cpp; then
        echo "verify: FAIL — N60 stop()-outside-lock pattern missing from ws_bridge" >&2
        exit 1
    fi
    # N61: matchmaker must register independent callback on shared remote
    if ! grep -q 'loginRemote->setOnMessageCallback' src/runtime/compat/ws_bridge.cpp; then
        echo "verify: FAIL — N61 matchmaker callback registration missing from ws_bridge" >&2
        exit 1
    fi
    # N63: re-entry gate must call ForceFatalExit, not return
    N63_RC=0; grep -q 'Sleep(100);\s*$' src/runtime/lifecycle/crash_recovery.cpp || N63_RC=$?
    sensor_stage1 "N63 re-entry gate" "src/runtime/lifecycle/crash_recovery.cpp" "$N63_RC"
    if [ "$N63_RC" -eq 0 ]; then
        echo "verify: FAIL — N63 re-entry gate still returns (Sleep+return), should be ForceFatalExit" >&2
        exit 1
    fi
    # N106: the OAuth2 device-flow credential must remain REACHABLE on a server.
    # 571a41b stopped persisting the access token (refresh token only, on disk),
    # which made `HasValidToken()` permanently false in a fresh process — so the
    # cached-token branch became dead code and every server silently fell back to
    # password auth. Nothing else refreshes in server mode: nevr_token_auth::Init
    # returns early on is_server, before the background refresh thread starts.
    # This exchange is the ONLY place a dedicated server can mint an access token.
    N106_RC=0; N106_GS=$(grep -hvE '^[[:space:]]*(//|/\*|\*[[:space:]/]|\*$)' src/runtime/server/gameserver.cpp src/runtime/server/gameserver_serverdb.cpp) || N106_RC=$?
    sensor_stage1 "N106 OAuth2 refresh reachable" "src/runtime/server/gameserver.cpp src/runtime/server/gameserver_serverdb.cpp" "$N106_RC"
    sensor_nonempty "N106 OAuth2 refresh reachable" "non-comment lines of gameserver.cpp and gameserver_serverdb.cpp" "$N106_GS"
    if ! grep -q 'HasValidRefreshToken()' <<<"$N106_GS"; then
        echo "verify: FAIL — N106 the gameserver no longer exchanges a refresh token for an" >&2
        echo "access token. The OAuth2 device-flow path becomes unreachable on a server and" >&2
        echo "every registration silently degrades to password auth (N106)." >&2
        exit 1
    fi
    if ! grep -q 'RefreshAuthToken(auth' <<<"$N106_GS"; then
        echo "verify: FAIL — N106 HasValidRefreshToken() is checked but RefreshAuthToken is" >&2
        echo "never called — the branch tests the credential and then discards it." >&2
        exit 1
    fi
    # N64/N105: BeginGracefulShutdown must release the listener before ForceFatalExit.
    # Assert the DIRECT call: a symbol the linker must resolve, not a name looked up at runtime.
    # (Matching the STRING 'WsBridge_Shutdown' would be satisfied on every run by a GetProcAddress
    # that returns null.)
    if ! grep -q 'StopWebSocketBridgeListener()' src/runtime/server/gameserver.cpp; then
        echo "verify: FAIL — N64/N105 BeginGracefulShutdown does not call StopWebSocketBridgeListener();" >&2
        echo "the ws bridge listening socket leaks as a wineserver zombie (N37: no SO_REUSEADDR)." >&2
        exit 1
    fi
    # The graceful console-control path must release it too, without a
    # runtime symbol lookup during shutdown (N62).
    if ! grep -q 'StopWebSocketBridgeListener()' src/runtime/lifecycle/crash_recovery.cpp; then
        echo "verify: FAIL — N105 PerformGracefulShutdown does not call StopWebSocketBridgeListener()." >&2
        exit 1
    fi
    # Scoped to ResolveShutdownDependencies, NOT the whole file: crash_recovery.cpp
    # legitimately resolves the four kernel32 hooks (CreateProcessA/W, ExitProcess,
    # TerminateProcess) at INIT, where the loader lock is fine. The constraint is
    # that the shutdown-dependency resolver performs no lookup at all —
    # the bridge is in-process. PerformGracefulShutdown's own body is covered
    # separately by the N62 sensor above.
    N105_RC=0; N105_RSD=$(awk '/^void ResolveShutdownDependencies/,/^}/' src/runtime/lifecycle/crash_recovery.cpp) || N105_RC=$?
    sensor_stage1 "N105 shutdown resolver" "src/runtime/lifecycle/crash_recovery.cpp" "$N105_RC"
    sensor_nonempty "N105 shutdown resolver" "ResolveShutdownDependencies() body" "$N105_RSD"
    if grep -qE 'GetProcAddress|GetModuleHandleA' <<<"$N105_RSD"; then
        echo "verify: FAIL — N62/N105 ResolveShutdownDependencies resolves a symbol at runtime again." >&2
        echo "The ws bridge is compiled into this DLL; call StopWebSocketBridgeListener() directly." >&2
        exit 1
    fi
    # N102: the two fail-fast sites N48 introduced must live in the copy that
    # SHIPS. 647cca7 wrote them into src/gameserver/ — a tree that stopped being
    # built on 2026-06-26 — so the feature was recorded as done while production
    # had no fatal path at all for either condition. Assert on the compiled file
    # (src/runtime/server/), never the dead one.
    N102_RC=0; N102_GS=$(grep -hvE '^[[:space:]]*(//|/\*|\*[[:space:]/]|\*$)' src/runtime/server/gameserver.cpp src/runtime/server/gameserver_callbacks.cpp src/runtime/server/gameserver_serverdb.cpp) || N102_RC=$?
    sensor_stage1 "N102 gameserver fail-fast" "src/runtime/server/gameserver.cpp src/runtime/server/gameserver_callbacks.cpp src/runtime/server/gameserver_serverdb.cpp" "$N102_RC"
    sensor_nonempty "N102 gameserver fail-fast" "non-comment lines of gameserver.cpp, gameserver_callbacks.cpp and gameserver_serverdb.cpp" "$N102_GS"
    for site in 'registration rejected by ServerDB' 'no valid token for ServerDB connection'; do
        if ! grep -qF "$site" <<<"$N102_GS"; then
            echo "verify: FAIL — N102 the fail-fast for '${site}' is missing from the SHIPPING gameserver." >&2
            echo "Without it the server runs degraded for hours instead of exiting with a cause (N102)." >&2
            exit 1
        fi
    done
    # Both sites shall use ServerFatal, not FatalError: ServerFatal is mode-gated
    # (Warning in client mode) and calls ForceFatalExit directly. The stranded
    # original used FatalError, which depends on a handler being installed first.
    if grep -qE 'FatalError\("(GameServer registration|Server authentication)' <<<"$N102_GS"; then
        echo "verify: FAIL — N102 a gameserver fail-fast uses FatalError; use ServerFatal (mode-gated)." >&2
        exit 1
    fi
    # N72/N73: the broadcaster ingress guards live with the plugin in
    # ~/src/nevr-runtime-plugins (this repo is PUBLIC and the
    # plugin is a broadcaster injection tool), and their sensors live there too —
    # asserting on a path that does not exist here would fail-closed forever,
    # and asserting nothing would silently drop the guarantee.
    # --- Crash-path invariants (N67/N69/N70) ---------------------------------
    # These are call-graph assertions, not behavioural tests. A crash handler that
    # violates them fails only during a crash, where no test is watching — so the
    # sensor has to be the source itself.
    #
    # N70: no Log() anywhere in the VEH call graph. Log() reaches
    # std::lock_guard(g_file_mutex) in builtin_log_filter, so a fault raised while
    # that mutex was held would deadlock the handler on its own log lock. The
    # crash path must use VehPrintf (raw WriteFile) exclusively.
    # Matches the call form `Log(EchoVR::LogLevel` — 348 of 355 `Log(` occurrences in
    # src/runtime, and every real call site (the remainder are prose in comments).
    # Deliberately NOT a word-boundary regex: `\bLog\(` also matches the phrase "Log()"
    # in the explanatory comments inside these very functions. Deliberately NOT the
    # `(^|[^a-zA-Z_])Log\(` idiom either — GNU grep's ERE mishandles `^` inside an
    # alternation group and that pattern silently matches nothing (verified 2026-07-26).
    N70_VEH_RC=0; N70_VEH_BODY=$(awk '/^LONG WINAPI BreakpointVEH/,/^}/' src/runtime/lifecycle/crash_recovery.cpp) || N70_VEH_RC=$?
    sensor_stage1 "N70 BreakpointVEH" "src/runtime/lifecycle/crash_recovery.cpp" "$N70_VEH_RC"
    sensor_nonempty "N70 BreakpointVEH" "BreakpointVEH() body in src/runtime/lifecycle/crash_recovery.cpp" "$N70_VEH_BODY"
    if grep -qF 'Log(EchoVR::LogLevel' <<<"$N70_VEH_BODY"; then
        echo "verify: FAIL — N70 BreakpointVEH calls Log(); crash path must use VehPrintf (raw WriteFile)." >&2
        echo "Log() reaches std::lock_guard(g_file_mutex) — a crash during log emission would self-deadlock." >&2
        exit 1
    fi
    N70_WCD_RC=0; N70_WCD_BODY=$(awk '/^static void WriteCrashDump/,/^}/' src/runtime/lifecycle/crash_recovery.cpp) || N70_WCD_RC=$?
    sensor_stage1 "N70 WriteCrashDump" "src/runtime/lifecycle/crash_recovery.cpp" "$N70_WCD_RC"
    sensor_nonempty "N70 WriteCrashDump" "WriteCrashDump() body in src/runtime/lifecycle/crash_recovery.cpp" "$N70_WCD_BODY"
    if grep -qF 'Log(EchoVR::LogLevel' <<<"$N70_WCD_BODY"; then
        echo "verify: FAIL — N70 WriteCrashDump calls Log(); crash path must use VehPrintf (raw WriteFile)." >&2
        exit 1
    fi
    # N70: the handler must never enumerate modules (loader lock). The snapshot is
    # taken at init by CacheModuleTable and read from the cache during a crash.
    if grep -q 'EnumProcessModules' <<<"$N70_WCD_BODY"; then
        echo "verify: FAIL — N70 WriteCrashDump enumerates modules; takes the loader lock. Read g_moduleCache instead." >&2
        exit 1
    fi
    if ! grep -q 'CacheModuleTable()' src/runtime/lifecycle/crash_recovery.cpp; then
        echo "verify: FAIL — N70 CacheModuleTable() call site missing; handler would have no module snapshot." >&2
        exit 1
    fi
    # N69: stack reserve must be claimed, or the overflow handler has no room to run.
    if ! grep -q 'SetThreadStackGuarantee' src/runtime/lifecycle/crash_recovery.cpp; then
        echo "verify: FAIL — N69 SetThreadStackGuarantee missing; stack-overflow handler cannot run." >&2
        exit 1
    fi
    if ! grep -q 'EnsureStackReserve()' <<<"$TICK_CODE"; then
        echo "verify: FAIL — N69 EnsureStackReserve() missing from per-frame hook; game threads uncovered." >&2
        exit 1
    fi
    # N67: the crash flags are written from any thread and read from the faulting
    # thread. VERIFIED-BY-TYPE only binds the artifact that ships — this grep asserts
    # the type is on THIS file, the one the linker consumes (an earlier fix landed
    # in a copy CMake did not build).
    if ! grep -q 'std::atomic<bool> g_crashReporterSuppressed' src/runtime/lifecycle/crash_recovery.cpp; then
        echo "verify: FAIL — N67 g_crashReporterSuppressed is not std::atomic<bool> in the SHIPPING path." >&2
        exit 1
    fi
    if ! grep -q 'std::atomic<bool> g_justSuppressedCrash' src/runtime/lifecycle/crash_recovery.cpp; then
        echo "verify: FAIL — N67 g_justSuppressedCrash is not std::atomic<bool> in the SHIPPING path." >&2
        exit 1
    fi
    # N36: InitializeAfterGameImageGuard() runs from DllMain under the loader lock, where the game's CLog is
    # not usable. Log() reports through the boot tee while the tee's sink is set (nevr_boot_log_tee::Init() to
    # Close(), core/logging.h), so the boot phase must start before InitializeFunctionPointers() makes
    # EchoVR::WriteLog non-null. The routing itself is driven by test_boot_log_routing; the sink and its
    # callers are checked by tools/tests/test_patch_detour_logging.py.
    N36_RC=0; N36_BODY=$(awk '/^static VOID InitializeAfterGameImageGuard\(\)/,/^}/' src/runtime/lifecycle/initialize.cpp) || N36_RC=$?
    sensor_stage1 "N36 boot phase before function pointers" "src/runtime/lifecycle/initialize.cpp" "$N36_RC"
    sensor_nonempty "N36 boot phase before function pointers" "InitializeAfterGameImageGuard() body in src/runtime/lifecycle/initialize.cpp" "$N36_BODY"
    N36_CODE=$(sed 's#//.*##' <<<"$N36_BODY")
    N36_INIT_LINE=$(awk '/nevr_boot_log_tee::Init\(\);/ { print NR; exit }' <<<"$N36_CODE")
    N36_FP_LINE=$(awk '/EchoVR::InitializeFunctionPointers\(\);/ { print NR; exit }' <<<"$N36_CODE")
    if [ -z "$N36_INIT_LINE" ] || [ -z "$N36_FP_LINE" ] || [ "$N36_INIT_LINE" -ge "$N36_FP_LINE" ]; then
        echo "verify: FAIL — N36 InitializeAfterGameImageGuard() in src/runtime/lifecycle/initialize.cpp does not call" >&2
        echo "nevr_boot_log_tee::Init() before EchoVR::InitializeFunctionPointers(): Log() would enter the game's CLog under the loader lock." >&2
        exit 1
    fi
    # Plugin shutdown can join threads and release module references. DllMain
    # runs under the loader lock and must never enter plugin teardown; process
    # exit also does not promise the optional plugin shutdown callback.
    DLLMAIN_RC=0; DLLMAIN_BODY=$(awk '/^BOOL APIENTRY DllMain\(/,/^}/' src/runtime/lifecycle/dllmain.cpp) || DLLMAIN_RC=$?
    sensor_stage1 "Plugin detach teardown boundary" "src/runtime/lifecycle/dllmain.cpp" "$DLLMAIN_RC"
    sensor_nonempty "Plugin detach teardown boundary" "DllMain() body in src/runtime/lifecycle/dllmain.cpp" "$DLLMAIN_BODY"
    if grep -Eq 'UnloadPlugins[[:space:]]*\(|NvrPluginShutdown' <<<"$DLLMAIN_BODY"; then
        echo "verify: FAIL — DllMain calls plugin shutdown/unload under the loader lock." >&2
        exit 1
    fi
    # --- Observability invariants (N77/N78/N79/N80/N81) ----------------------
    # N77: NEVR's own lines must be exempt from game-noise suppression patterns.
    # Without the guard, a substring rule aimed at echovr noise silently deletes
    # NEVR structured output — e.g. it would erase "Finished initializing engine"
    # (the witness cited in the N7/N8/N10 closes) and mask real ExitProcess reports.
    if ! grep -q 'if (IsNevrLine(message)) return false;' src/runtime/log/builtin_filter.cpp; then
        echo "verify: FAIL — N77 NEVR-line exemption missing from ShouldSuppress; game-noise patterns can delete NEVR output." >&2
        exit 1
    fi
    # N77: these two patterns must never return to the blanket suppress list.
    N77A_RC=0; grep -qF '"Finished initializing engine",' src/runtime/log/builtin_filter.cpp || N77A_RC=$?
    sensor_stage1 "N77 boot-witness suppression" "src/runtime/log/builtin_filter.cpp" "$N77A_RC"
    if [ "$N77A_RC" -eq 0 ]; then
        echo "verify: FAIL — N77 'Finished initializing engine' is suppressed again; that string is the headless-boot witness (N7/N8/N10)." >&2
        exit 1
    fi
    N77B_RC=0; grep -qF '"ExitProcess(",' src/runtime/log/builtin_filter.cpp || N77B_RC=$?
    sensor_stage1 "N77 ExitProcess suppression" "src/runtime/log/builtin_filter.cpp" "$N77B_RC"
    if [ "$N77B_RC" -eq 0 ]; then
        echo "verify: FAIL — N77 'ExitProcess(' is suppressed again; it masks real process-exit reports. Use rate limiting (N78)." >&2
        exit 1
    fi
    # N78: frequent lines are collapsed into a counted summary, never deleted.
    if ! grep -q 'RateVerdict::Collapse' src/runtime/log/builtin_filter.cpp; then
        echo "verify: FAIL — N78 rate-limited summarisation missing; filter can only delete, not summarise (docs/standards/logging.md Rule 4)." >&2
        exit 1
    fi
    # N79: filter health must be emitted while the process is alive. Shutdown() is
    # unreachable in production — every server exit is TerminateProcess, which runs
    # no DLL detach.
    if ! grep -q 'MaybeEmitHealth();' src/runtime/log/builtin_filter.cpp; then
        echo "verify: FAIL — N79 periodic health emission missing; suppression ratio only reachable at a shutdown that never runs." >&2
        exit 1
    fi
    # N80: both log writers must stamp the shared run ID, or the two files cannot be joined.
    if ! grep -q 'GetRunId()' src/runtime/log/boot_log_tee.cpp; then
        echo "verify: FAIL — N80 run ID missing from BootLogTee; boot log cannot be correlated or split by run." >&2
        exit 1
    fi
    if ! grep -q 'GetRunId()' src/runtime/log/builtin_filter.cpp; then
        echo "verify: FAIL — N80 run ID missing from the runtime log writer." >&2
        exit 1
    fi
    # N81/N45: no bare [NEVR] tag on a LOG line — it names every component and so
    # discriminates none.
    #
    # src/runtime/lifecycle/cli.cpp is excluded deliberately, not by oversight: its 14
    # "[NEVR] " strings are all AddArgHelpString descriptions, where the marker
    # distinguishes NEVR-added flags from the game's own in `--help` output. That is
    # a usage contract (RULINGS.md "Usage contracts"), not a log tag — docs/standards/logging.md's
    # subsystem-tag rule governs log lines. Verified 2026-07-26: every [NEVR] hit in
    # that file is a help string, zero are log calls.
    # --- N115: the login system_info block must be MEASURED, not invented --------
    # Literals ("cpu":"Wine", 4 physical cores, 8 logical, 16384 MB total, 8192
    # used) would be emitted as though read from the machine, and nothing could
    # tell, because invented data and a real reading look identical once they are
    # on the wire. Every field is read from the host (nevr_system_info::Get).
    #
    # The unit tests cover SystemInfo itself; they cannot see this format string.
    # This sensor is the half that watches the wire format.
    N115_RC=0; N115_WS=$(grep -vE '^[[:space:]]*(//|/\*|\*[[:space:]/]|\*$)' src/runtime/compat/ws_bridge.cpp) || N115_RC=$?
    sensor_stage1 "N115 login system_info measured" "src/runtime/compat/ws_bridge.cpp" "$N115_RC"
    sensor_nonempty "N115 login system_info measured" "non-comment lines of compat/ws_bridge.cpp" "$N115_WS"
    if ! grep -q 'nevr_system_info::Get()' <<<"$N115_WS"; then
        echo "verify: FAIL — N115 the login payload no longer reads measured host facts (nevr_system_info::Get)." >&2
        exit 1
    fi
    # The JSON lives inside a C string literal, so every quote in the source is
    # backslash-escaped. Matching that through a justfile recipe means three
    # layers of escaping and it WAS wrong the first time — the check silently
    # matched nothing and the break went green. Strip the backslashes once and
    # compare plain text instead of trying to out-escape the stack.
    N115_FLAT=$(tr -d '\\' <<<"$N115_WS")
    for _lit in '"num_physical_cores":4' '"num_logical_cores":8' '"memory_total":16384' '"memory_used":8192' '"cpu":"Wine"' '"video_card":"Wine D3D12"'; do
        if grep -qF "$_lit" <<<"$N115_FLAT"; then
            echo "verify: FAIL — N115 a fabricated system_info literal is back in the login payload: $_lit" >&2
            echo "Invented telemetry is worse than none: it is indistinguishable from a real reading and gets acted on. Send the measured value, or send empty/0." >&2
            exit 1
        fi
    done

    # --- N131: the Asset CDN must not fetch on a server --------------------------
    # A headless server has nothing to render, so it must not open the CDN
    # connection (it opens ServerDB + login only). initialize.cpp runs BEFORE the
    # CLI is parsed, so g_isServer is FALSE there and a gate could not distinguish
    # server from client. The call therefore lives in boot.cpp, post-CLI-parse,
    # gated on the client. Guard both facts.
    N131_RC=0; N131_BOOT=$(grep -vE '^[[:space:]]*(//|/\*|\*[[:space:]/]|\*$)' src/runtime/lifecycle/boot.cpp) || N131_RC=$?
    sensor_stage1 "N131 CDN gated off servers" "src/runtime/lifecycle/boot.cpp" "$N131_RC"
    sensor_nonempty "N131 CDN gated off servers" "non-comment lines of boot.cpp" "$N131_BOOT"
    # Whitespace-flattened and matched CONTIGUOUSLY: two separate greps ("some
    # !g_isServer exists" AND "nevr_asset_cdn::Initialize exists") passed blind, because
    # boot.cpp has OTHER !g_isServer gates — removing THIS gate while leaving the
    # call unconditional still satisfied both. Require the call inside the gate.
    N131_FLAT=$(tr -s '[:space:]' ' ' <<<"$N131_BOOT")
    if ! grep -qE 'if \( *!g_isServer *\) *\{ *nevr_asset_cdn::Initialize\(\)' <<<"$N131_FLAT"; then
        echo "verify: FAIL — N131 nevr_asset_cdn::Initialize is no longer inside an if (!g_isServer) gate in boot.cpp." >&2
        echo "Without the client-gate a headless server fetches cosmetics it never renders, opening a needless outbound connection." >&2
        exit 1
    fi
    # And it must NOT be called unconditionally from initialize.cpp (pre-CLI-parse).
    N131_RC2=0; N131_INIT=$(grep -vE '^[[:space:]]*(//|/\*|\*[[:space:]/]|\*$)' src/runtime/lifecycle/initialize.cpp) || N131_RC2=$?
    sensor_stage1 "N131 CDN not in initialize" "src/runtime/lifecycle/initialize.cpp" "$N131_RC2"
    sensor_nonempty "N131 CDN not in initialize" "non-comment lines of initialize.cpp" "$N131_INIT"
    if grep -q 'nevr_asset_cdn::Initialize' <<<"$N131_INIT"; then
        echo "verify: FAIL — N131 nevr_asset_cdn::Initialize is back in initialize.cpp, where g_isServer is not yet set." >&2
        echo "It would run before the CLI is parsed, so the server-gate cannot apply and the CDN fetches on every host." >&2
        exit 1
    fi

    # --- N129: the DLL-load hooks must report per-hook failures ------------------
    # DllLoadHook installs its four LoadLibrary hooks (they run the load filters,
    # observers and callbacks the runtime registers; they restrict no search path)
    # OUTSIDE PatchDetour, so N126/N128's reporting doesn't cover it. One "OK"/"PARTIAL"
    # would hide which variant failed and why, and loads through an unhooked variant
    # run no filter, observer or callback. It must name the variant and the MH_STATUS.
    N129_RC=0; N129_DL=$(grep -vE '^[[:space:]]*(//|/\*|\*[[:space:]/]|\*$)' src/runtime/hook/dll_load_hook.cpp) || N129_RC=$?
    sensor_stage1 "N129 dll-hook reports per-variant" "src/runtime/hook/dll_load_hook.cpp" "$N129_RC"
    sensor_nonempty "N129 dll-hook reports per-variant" "non-comment lines of dll_load_hook.cpp" "$N129_DL"
    if ! grep -q 'MH_StatusToString' <<<"$N129_DL"; then
        echo "verify: FAIL — N129 nevr_dll_load_hook::Install no longer reports the MH_STATUS on a failed LoadLibrary hook." >&2
        echo "It would revert to a bare OK/PARTIAL that hides which LoadLibrary hook failed, so loads through it would skip the load filters, observers and callbacks unseen." >&2
        exit 1
    fi
    if grep -qE 'bool ok = true;.*ok &=' <<<"$(tr '\n' ' ' <<<"$N129_DL")"; then
        echo "verify: FAIL — N129 the accumulate-and-hide 'ok &=' aggregate is back in DllLoadHook." >&2
        exit 1
    fi

    # --- N43: the boot log tee writes its file through kernel32 only ------------
    # The tee opens and writes its file during initialization, which runs on the
    # host's wrapper detour while the loader lock may be held. A CRT file call
    # (fopen, freopen, fprintf or fwrite to a FILE*, an iostream file) takes CRT
    # locks there; CreateFileA and WriteFile take none. The rule is AGENTS.md N43.
    N43_RC=0; N43_TEE=$(grep -vE '^[[:space:]]*(//|/\*|\*[[:space:]/]|\*$)' src/runtime/log/boot_log_tee.cpp) || N43_RC=$?
    sensor_stage1 "N43 boot tee kernel32 only" "src/runtime/log/boot_log_tee.cpp" "$N43_RC"
    sensor_nonempty "N43 boot tee kernel32 only" "non-comment lines of boot_log_tee.cpp" "$N43_TEE"
    if grep -qE '\b(fopen|_wfopen|fopen_s|_wfopen_s|freopen|_fsopen|fprintf|fwrite|fputs|std::ofstream|std::fstream|std::ifstream)\b' <<<"$N43_TEE"; then
        echo "verify: FAIL — N43 boot_log_tee.cpp calls a CRT file function (fopen, fprintf, fwrite, an iostream file)." >&2
        echo "The tee runs while the loader lock may be held; CRT file calls take CRT locks there. Open and write with CreateFileA and WriteFile." >&2
        exit 1
    fi
    if ! grep -qF 'CreateFileA' <<<"$N43_TEE" || ! grep -qF 'WriteFile' <<<"$N43_TEE"; then
        echo "verify: FAIL — N43 boot_log_tee.cpp no longer opens and writes its file through kernel32 (CreateFileA and WriteFile)." >&2
        exit 1
    fi

    # --- N17: a component that installs several hooks reports each outcome -----
    # Each hook reports by name, and one aggregate line at Info says how many took.
    # The aggregate is what makes a partial install visible: a Warning there, not a
    # silent success. Two components carry it: platform_compat (its three hooks) and
    # pnsrad_enabler (the echovr.exe patches). The sensor reads the aggregate only.
    N17_PC_RC=0; N17_PC=$(grep -vE '^[[:space:]]*(//|/\*|\*[[:space:]/]|\*$)' src/modules/platform-compat/src/platform_compat.cpp) || N17_PC_RC=$?
    sensor_stage1 "N17 platform_compat aggregate" "src/modules/platform-compat/src/platform_compat.cpp" "$N17_PC_RC"
    sensor_nonempty "N17 platform_compat aggregate" "non-comment lines of platform_compat.cpp" "$N17_PC"
    if ! grep -qF 'Log(okCount == 3 ? EchoVR::LogLevel::Info : EchoVR::LogLevel::Warning,' <<<"$N17_PC" || ! grep -qF 'platform_compat initialized: %d/3 hooks installed' <<<"$N17_PC" || ! grep -qF 'tls=%s createdir=%s msxml6=%s' <<<"$N17_PC"; then
        echo "verify: FAIL — N17 platform_compat no longer reports its three hooks by name with an aggregate at Info." >&2
        echo "A success that logs nothing, or an aggregate that is not Info when all three took, makes a working hook look like a silent one." >&2
        exit 1
    fi
    N17_PN_RC=0; N17_PN=$(grep -vE '^[[:space:]]*(//|/\*|\*[[:space:]/]|\*$)' src/runtime/patch/pnsrad_enabler.cpp) || N17_PN_RC=$?
    sensor_stage1 "N17 pnsrad aggregate" "src/runtime/patch/pnsrad_enabler.cpp" "$N17_PN_RC"
    sensor_nonempty "N17 pnsrad aggregate" "non-comment lines of pnsrad_enabler.cpp" "$N17_PN"
    if ! grep -qF 'Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] pnsrad init complete: %d/%d echovr.exe patches applied",' <<<"$N17_PN"; then
        echo "verify: FAIL — N17 pnsrad_enabler no longer ends its install with the aggregate line at Info (pnsrad init complete)." >&2
        echo "Without it, a pnsrad install that patched nothing reads the same as one that patched every site." >&2
        exit 1
    fi

    # --- N128: a failed hook must report WHICH MinHook error ---------------------
    # N126 made a failed detour visible; N128 made it DIAGNOSABLE. Hooking::Attach
    # captures MH_StatusToString on failure and PatchDetour surfaces it, so the log
    # names the reason (MH_ERROR_ALREADY_CREATED etc.) instead of leaving it
    # undetermined. Without this the three known double-hook collisions read as
    # "failed" with no cause, and my Wine-forwarder guess would still stand. Guard
    # both halves of the wiring.
    N128_RC=0; N128_HK=$(grep -vE '^[[:space:]]*(//|/\*|\*[[:space:]/]|\*$)' src/core/hooking.h) || N128_RC=$?
    sensor_stage1 "N128 attach captures MH_STATUS" "src/core/hooking.h" "$N128_RC"
    sensor_nonempty "N128 attach captures MH_STATUS" "non-comment lines of hooking.h" "$N128_HK"
    if ! grep -q 'MH_StatusToString' <<<"$N128_HK"; then
        echo "verify: FAIL — N128 Hooking::Attach no longer records the MH_STATUS reason." >&2
        echo "A failed hook would report 'reason=' empty again, and a double-hook collision would read as an unexplained failure." >&2
        exit 1
    fi
    N128_RC2=0; N128_PD=$(grep -vE '^[[:space:]]*(//|/\*|\*[[:space:]/]|\*$)' src/runtime/hook/patching.h) || N128_RC2=$?
    sensor_stage1 "N128 failure log surfaces reason" "src/runtime/hook/patching.h" "$N128_RC2"
    sensor_nonempty "N128 failure log surfaces reason" "non-comment lines of patching.h" "$N128_PD"
    if ! grep -q 'Hooking::LastAttachError()' <<<"$N128_PD"; then
        echo "verify: FAIL — N128 PatchDetour's failure log no longer includes the attach reason." >&2
        exit 1
    fi

    # --- N127: the Oculus-SDK block must report its real result ----------------
    # PatchBlockOculusSDK logged "Installed Oculus Platform SDK blocking hooks"
    # unconditionally while both PatchDetour calls fail under Wine — so it claimed
    # success on every boot for a feature that never worked. The success claim must
    # be gated on the actual returns.
    N127_RC=0; N127_MP=$(grep -vE '^[[:space:]]*(//|/\*|\*[[:space:]/]|\*$)' src/runtime/patch/mode_patches.cpp) || N127_RC=$?
    sensor_stage1 "N127 oculus-block reports result" "src/runtime/patch/mode_patches.cpp" "$N127_RC"
    sensor_nonempty "N127 oculus-block reports result" "non-comment lines of mode_patches.cpp" "$N127_MP"
    if grep -q 'Installed Oculus Platform SDK blocking hooks' <<<"$N127_MP"; then
        echo "verify: FAIL — N127 the unconditional 'Installed Oculus Platform SDK blocking hooks' claim is back." >&2
        echo "Both LoadLibrary detours fail under Wine; an unconditional success line claims a feature that never installed." >&2
        exit 1
    fi
    # #361: the block is a DllLoadHook filter. A second detour on LoadLibraryW/ExW fails with
    # MH_ERROR_ALREADY_CREATED (DllLoadHook owns those targets), which is how N127 went unnoticed.
    if grep -qE 'PatchDetour\(&Original_LoadLibrary' <<<"$N127_MP"; then
        echo "verify: FAIL — #361 PatchBlockOculusSDK installs its own LoadLibrary detour again." >&2
        echo "DllLoadHook already hooks LoadLibraryA/W/ExA/ExW; a second detour on the same target fails with MH_ERROR_ALREADY_CREATED." >&2
        exit 1
    fi
    if ! grep -qE 'nevr_dll_load_hook::AddLoadFilter\(' <<<"$N127_MP"; then
        echo "verify: FAIL — #361 PatchBlockOculusSDK no longer registers its load filter with DllLoadHook." >&2
        exit 1
    fi

    # --- N126: a failed hook must not be silent --------------------------------
    # PatchDetour is the one choke point every detour passes through. A failed
    # 9 of 10 mode_patches call sites ignore the return and log "installed" on the
    # next line regardless — so a failed Attach that logged nothing would leave a
    # server-critical hook uninstalled, still printing "installed", with no failure
    # signal at any level. The failure branch must Log.
    N126_RC=0; N126_PD=$(grep -vE '^[[:space:]]*(//|/\*|\*[[:space:]/]|\*$)' src/runtime/hook/patching.h) || N126_RC=$?
    sensor_stage1 "N126 failed hook is reported" "src/runtime/hook/patching.h" "$N126_RC"
    sensor_nonempty "N126 failed hook is reported" "non-comment lines of patching.h" "$N126_PD"
    if ! grep -qE 'else' <<<"$N126_PD" || ! grep -q 'hook FAILED name=' <<<"$N126_PD"; then
        echo "verify: FAIL — N126 PatchDetour no longer logs on the failure path." >&2
        echo "A failed detour would return silently again, and the 'installed' line at the call site (unconditional at 9 of 10 sites) would be the only output — claiming success on failure." >&2
        exit 1
    fi

    # --- N125: the game-loop setjmp and its longjmp live in the SAME file --------
    # g_gameLoopJmpBuf is a crash-recovery jump buffer: GameMainWrapperHook does the
    # setjmp, the VEH does the longjmp. Split across translation units by an
    # `extern`, they would leave "which file owns crash recovery?" unanswered, so
    # N125 keeps both in crash_recovery.cpp. This fails if the
    # setjmp side drifts back to mode_patches.cpp — a split nobody would notice,
    # because no run crashes the server on purpose and the smoke suite cannot reach
    # the recovery path.
    # Comment-stripped: mode_patches.cpp's own N125 breadcrumb NAMES g_gameLoopJmpBuf
    # to say it moved. A raw grep flags that breadcrumb and the sensor fires on a
    # correct tree — which it did on first write. Match only live code.
    N125_MP=$(grep -vE '^[[:space:]]*(//|/\*|\*[[:space:]/]|\*$)' src/runtime/patch/mode_patches.cpp)
    if grep -q 'g_gameLoopJmpBuf' <<<"$N125_MP"; then
        echo "verify: FAIL — N125 mode_patches.cpp references g_gameLoopJmpBuf again." >&2
        echo "The setjmp and longjmp of the crash-recovery jump buffer must stay in one file (crash_recovery.cpp). A cross-file split is coupled only by an extern and is invisible to every test, because nothing exercises the recovery path." >&2
        exit 1
    fi
    N125_RC=0; N125_CR=$(grep -vE '^[[:space:]]*(//|/\*|\*[[:space:]/]|\*$)' src/runtime/lifecycle/crash_recovery.cpp) || N125_RC=$?
    sensor_stage1 "N125 crash-recovery pair co-located" "src/runtime/lifecycle/crash_recovery.cpp" "$N125_RC"
    sensor_nonempty "N125 crash-recovery pair co-located" "non-comment lines of crash_recovery.cpp" "$N125_CR"
    if ! grep -q 'setjmp(g_gameLoopJmpBuf)' <<<"$N125_CR"; then
        echo "verify: FAIL — N125 the setjmp on g_gameLoopJmpBuf is no longer in crash_recovery.cpp." >&2
        echo "Its longjmp lives here; separating the two is the coupling N125 removed." >&2
        exit 1
    fi
    if ! grep -q 'longjmp(g_gameLoopJmpBuf' <<<"$N125_CR"; then
        echo "verify: FAIL — N125 the longjmp on g_gameLoopJmpBuf left crash_recovery.cpp." >&2
        exit 1
    fi
    # The recovery point must be REACHED, and no other sensor verifies it: the runtime's descriptor
    # must carry RuntimeGameMain as its game_main and its initialize must call ArmGameMain. A dropped
    # entry means the host never runs the setjmp, the VEH's longjmp has nowhere to land, and a server
    # crash terminates instead of recovering.
    N125_RE_RC=0; N125_RE=$(grep -vE '^[[:space:]]*(//|/\*|\*[[:space:]/]|\*$)' src/runtime/lifecycle/runtime_entry.cpp) || N125_RE_RC=$?
    sensor_stage1 "N125 runtime entry reaches the recovery point" "src/runtime/lifecycle/runtime_entry.cpp" "$N125_RE_RC"
    sensor_nonempty "N125 runtime entry reaches the recovery point" "non-comment lines of runtime_entry.cpp" "$N125_RE"
    if ! grep -qF 'api.game_main = &RuntimeGameMain;' <<<"$N125_RE" || ! grep -qF 'ArmGameMain(host);' <<<"$N125_RE"; then
        echo "verify: FAIL — N125 runtime_entry.cpp no longer hands the host RuntimeGameMain and arms it (ArmGameMain)." >&2
        echo "Without it there is no setjmp recovery point: the VEH's longjmp has nowhere to land and a server crash terminates instead of recovering." >&2
        exit 1
    fi

    # --- #515: a server's time-step arguments are removed before the game parses them ----
    # echovr.exe builds its CMainArgs in Main_ from the string GameMainWrapper passes on, before
    # PreprocessCommandLine (lifecycle/cmdline_strip.h says where). The wrapper hook is the one place
    # the removal reaches the game's parser. The decision is a pure function (PrepareForGame, GTest
    # test_cmdline_strip); no test runs the hook, so this pins that the hook makes that one call, hands
    # the game its result, records the report boot.cpp logs, and keeps the exception boundary (never
    # catch(...)). The server's two writes are pure helpers too (server_timestep.h, GTest
    # test_server_timestep); this pins that both call sites still call them with g_isServer: 8d26b1a8
    # removed this writer once, and a deleted call passes every unit test.
    # Comment-stripped (N99/N111), reusing the N125 capture of crash_recovery.cpp.
    sensor_515_need() { # $1 capture, $2 fixed-string pattern, $3 why it matters
        if ! grep -qF -- "$2" <<<"$1"; then
            echo "verify: FAIL — #515 sensor: '$2' is missing. $3" >&2
            exit 1
        fi
    }
    # Trailing `// ...` comments are stripped too (a mutant `GameMain(arg1);  // not GameMain(gameArg);` must not
    # satisfy a pattern); only a `//` preceded by whitespace, so a URL in a string survives.
    S515_STRIP_TRAILING='s#[[:space:]]+//.*$##'
    S515_CR=$(sed -E "$S515_STRIP_TRAILING" <<<"$N125_CR")
    sensor_515_need "$S515_CR" 'nevr_cmdline::PrepareForGame(' "The strip helper must make the one call that decides what the game parses; a server would give the game -fixedtimestep and -timestep."
    sensor_515_need "$S515_CR" 'report.tokensRemoved = handOff.tokensRemoved;' "boot.cpp reads the report to log removed or NOT removed; without it a failed removal is silent."
    sensor_515_need "$S515_CR" 'report.residual = handOff.residual;' "The report must say when a time-step option is left in the string the game parses."
    sensor_515_need "$S515_CR" 'report.options = handOff.optionsText;' "The report must name the removed options."
    sensor_515_need "$S515_CR" 'gameArg = reinterpret_cast<INT64>(handOff.commandLine);' "The game must be handed the result of PrepareForGame; passing the original string undoes the removal."
    sensor_515_need "$S515_CR" 'g_stripReport = std::move(report);' "The report must be published (after everything that can throw)."
    sensor_515_need "$S515_CR" 'g_stripReportReady.store(true' "The report must be published or boot.cpp sees none."
    sensor_515_need "$S515_CR" 'const INT64 gameArg = PrepareGameArgument(arg1);' "GameMainWrapperHook must get the game's argument from the helper, whose std::string and report die before GameMain."
    sensor_515_need "$S515_CR" 'GameMain(gameArg);' "The game must be called with that result."
    sensor_515_need "$S515_CR" 'catch (const std::bad_alloc&)' "The helper allocates while the game calls it; a failure must fall back to the original string."
    sensor_515_need "$S515_CR" 'catch (const std::exception&' "The helper allocates while the game calls it; a failure must fall back to the original string."
    # Order: gameArg and the report are assigned only after everything that can throw (#515 review SF1): the
    # options string is built before gameArg is assigned, and both catches put gameArg back.
    S515_OPT=$(grep -nF 'report.options = handOff.optionsText;' <<<"$S515_CR" | cut -d: -f1)
    S515_ARG=$(grep -nF 'gameArg = reinterpret_cast<INT64>(handOff.commandLine);' <<<"$S515_CR" | cut -d: -f1)
    if [ -z "$S515_OPT" ] || [ -z "$S515_ARG" ] || [ "$S515_OPT" -ge "$S515_ARG" ]; then
        echo "verify: FAIL — #515 sensor: gameArg is assigned before the report's options string is built (or one is missing)." >&2
        echo "An allocation after gameArg throws leaves the game the stripped string while the log says the original." >&2
        exit 1
    fi
    if [ "$(grep -cE '^[[:space:]]+gameArg = arg1;' <<<"$S515_CR")" -lt 2 ]; then
        echo "verify: FAIL — #515 sensor: the exception handlers no longer put gameArg back to arg1." >&2
        exit 1
    fi
    if [ "$(grep -cF 'report = nevr_timestep_log::FailedReport();' <<<"$S515_CR")" -lt 2 ]; then
        echo "verify: FAIL — #515 sensor: a handler no longer records the failure with FailedReport()." >&2
        echo "boot.cpp would not know the removal threw and would log 'NOT removed' without hook_failed=true." >&2
        exit 1
    fi
    # N2: the helper's std::string and report must be gone before GameMain, which the VEH longjmps out of.
    S515_REPORT_LINE=$(grep -nE '^[[:space:]]+TimestepStripReport [A-Za-z_]+;' <<<"$S515_CR" | cut -d: -f1 | sed -n 1p)
    S515_HOOK_LINE=$(grep -nF 'static INT64 GameMainWrapperHook(INT64 arg1) {' <<<"$S515_CR" | cut -d: -f1)
    if [ -z "$S515_REPORT_LINE" ] || [ -z "$S515_HOOK_LINE" ] || [ "$S515_REPORT_LINE" -ge "$S515_HOOK_LINE" ] || [ "$(grep -cE '^[[:space:]]+TimestepStripReport [A-Za-z_]+;' <<<"$S515_CR")" -ne 1 ]; then
        echo "verify: FAIL — #515 sensor: a TimestepStripReport local is alive in GameMainWrapperHook (or the helper moved)." >&2
        echo "A non-trivial local alive across GameMain is undefined behaviour when the VEH longjmps out of it." >&2
        exit 1
    fi
    # N2, any type: from the hook's first line through the GameMain(gameArg); line, the hook declares exactly the three trivial
    # locals below. Any other declaration (a report, a std:: or repo type, `auto`, in any form) could be alive
    # across the call the VEH longjmps out of, which is undefined behaviour for a non-trivial local.
    S515_PRE_GAMEMAIN=$(awk -v start="$S515_HOOK_LINE" 'NR>=start {print} NR>=start && /GameMain\(gameArg\);/ {exit}' <<<"$S515_CR")
    sensor_nonempty "#515 hook body before GameMain" "GameMainWrapperHook up to GameMain(gameArg);" "$S515_PRE_GAMEMAIN"
    S515_DECLS=$(grep -E '^[[:space:]]+(const[[:space:]]+)?[A-Za-z_][A-Za-z0-9_:<>,]*[[:space:]*&]+[A-Za-z_][A-Za-z0-9_]*[[:space:]]*(=|;|\{|\(|\[)' <<<"$S515_PRE_GAMEMAIN" | grep -vE '^[[:space:]]+(return|else|case|goto|delete|throw)[[:space:]]' | sed -E 's/^[[:space:]]+//' || true)
    S515_ALLOWED=$'int crashCount = setjmp(g_gameLoopJmpBuf);\nconst INT64 gameArg = PrepareGameArgument(arg1);\nconst INT64 gameResult = GameMain(gameArg);'
    if [ "$S515_DECLS" != "$S515_ALLOWED" ]; then
        echo "verify: FAIL — #515 sensor: GameMainWrapperHook's declarations through GameMain(gameArg); are not exactly the three trivial locals." >&2
        echo "Found: $S515_DECLS" >&2
        echo "A non-trivial local alive across GameMain is undefined behaviour when the VEH longjmps out of it." >&2
        exit 1
    fi
    # The allowlist reads one-line declarations only (a type split over lines, or with spaces inside its template
    # arguments, would pass it), so the hook's whole text through the GameMain(gameArg); line is also pinned: any edit
    # there fails here until a reviewer has checked it adds no non-trivial local and updates the pin.
    S515_PRE_SHA=$(sha256sum <<<"$S515_PRE_GAMEMAIN" | cut -c1-64)
    if [ "$S515_PRE_SHA" != "5c1fe4be63ca49aacae848c47932f247b5634604d1b12b8b0141546f6d1c42f1" ]; then
        echo "verify: FAIL — #515 sensor: GameMainWrapperHook's text through GameMain(gameArg); changed (sha256 $S515_PRE_SHA)." >&2
        echo "Check that the edit leaves no local with a destructor alive across GameMain, then set the pin to that hash." >&2
        exit 1
    fi
    if grep -qF 'catch (...)' <<<"$S515_CR"; then
        echo "verify: FAIL — #515 sensor: crash_recovery.cpp has catch (...). Name the exception type (C++ addendum)." >&2
        exit 1
    fi
    S515_RC=0; S515_CFG=$(grep -vE '^[[:space:]]*(//|/\*|\*[[:space:]/]|\*$)' src/runtime/lifecycle/config.cpp | sed -E "$S515_STRIP_TRAILING") || S515_RC=$?
    sensor_stage1 "#515 server step call site" "src/runtime/lifecycle/config.cpp" "$S515_RC"
    sensor_nonempty "#515 server step call site" "non-comment lines of config.cpp" "$S515_CFG"
    S515_RC=0; S515_BOOT=$(grep -vE '^[[:space:]]*(//|/\*|\*[[:space:]/]|\*$)' src/runtime/lifecycle/boot.cpp | sed -E "$S515_STRIP_TRAILING") || S515_RC=$?
    sensor_stage1 "#515 server flag call site" "src/runtime/lifecycle/boot.cpp" "$S515_RC"
    sensor_nonempty "#515 server flag call site" "non-comment lines of boot.cpp" "$S515_BOOT"
    S515_RC=0; S515_TICK=$(grep -vE '^[[:space:]]*(//|/\*|\*[[:space:]/]|\*$)' src/runtime/frame/tick.cpp | sed -E "$S515_STRIP_TRAILING") || S515_RC=$?
    sensor_stage1 "#515 read-back call site" "src/runtime/frame/tick.cpp" "$S515_RC"
    sensor_nonempty "#515 read-back call site" "non-comment lines of tick.cpp" "$S515_TICK"
    if ! grep -qE '^[[:space:]]*ApplyServerTimestep\(\);' <<<"$S515_CFG"; then
        echo "verify: FAIL — #515 sensor: LoadLocalConfigHook no longer calls ApplyServerTimestep();" >&2
        echo "The server would run on the game's own time step (#511); nothing else writes it." >&2
        exit 1
    fi
    sensor_515_need "$S515_CFG" 'nevr_server_timestep::FieldAt(state, nevr_patch_addresses::FIXED_TIMESTEP_OFFSET);' "ApplyServerTimestep must resolve the step field from the game's clock state."
    sensor_515_need "$S515_CFG" 'nevr_server_timestep::ApplyStep(g_isServer != FALSE, g_headlessTickRateHz, stepField);' "ApplyServerTimestep must pass g_isServer and the configured rate to the step writer."
    sensor_515_need "$S515_CFG" 'g_stepRecord.Store(w);' "The step outcome must be recorded: boot.cpp and the read-back report it."
    sensor_515_need "$S515_CFG" 'nevr_server_timestep::StepWrite ServerStepWrite() { return g_stepRecord.Load(); }' "The recorded outcome must be what boot.cpp and the read-back read."
    sensor_515_need "$S515_CFG" 'const bool readable = nevr_crash_recovery::IsReadableMemory(state, nevr_server_timestep::kClockReadSpan);' "The read-back must check the clock readable before it reads (N31)."
    sensor_515_need "$S515_CFG" 'nevr_server_timestep::ReadClockIf(readable, state);' "The read-back must read the clock only when the guard says it is readable."
    sensor_515_need "$S515_CFG" 'nevr_server_timestep::DecideReadBack(ServerStepWrite(), g_headlessTickRateHz, rb, tick);' "The read-back's expected step and liveness must come from the tested decision, with the recorded outcome."
    sensor_515_need "$S515_CFG" 'Log(d.live ? EchoVR::LogLevel::Info : EchoVR::LogLevel::Warning, "[NEVR.PATCH] %s", d.line.c_str());' "The read-back level must follow the decision."
    sensor_515_need "$S515_CFG" '"[NEVR.PATCH] server clock read-back failed (out of memory)"' "The read-back runs inside the per-frame dispatcher; a throw would leave its re-entrancy flag set for good, so it needs a named catch."
    sensor_515_need "$S515_BOOT" 'nevr_server_timestep::FieldAt(g_pGame, nevr_patch_addresses::GAME_TIMESTEP_FLAGS_OFFSET);' "boot.cpp must resolve the game's flags field (a null field is logged as field_missing, but only if this is the call)."
    sensor_515_need "$S515_BOOT" 'nevr_server_timestep::ApplyFlag(g_isServer != FALSE, timestepFlags);' "boot.cpp must set the server's fixed-time-step flag, with g_isServer, before the original PreprocessCommandLine (#511)."
    sensor_515_need "$S515_BOOT" 'g_flagRecord.Store(flagOutcome);' "The flag outcome must be recorded for LogTimestepArgs."
    sensor_515_need "$S515_BOOT" '  LogTimestepArgs();' "boot.cpp must report what happened to the time-step arguments."
    sensor_515_need "$S515_BOOT" 'nevr_timestep_log::BuildInputs(' "LogTimestepArgs must assemble its inputs with the tested function."
    sensor_515_need "$S515_BOOT" 'GetTimestepStripReport(), g_isServer != FALSE, nevr_cmdline::HasTimestepOption(GetCommandLineA()),' "LogTimestepArgs must pass the hook's report, boot.cpp's reading of server and the command line's time-step options."
    sensor_515_need "$S515_BOOT" 'ServerStepWrite(), g_flagRecord.Load(), g_headlessTickRateHz);' "LogTimestepArgs must pass the recorded step and flag outcomes and the rate."
    sensor_515_need "$S515_BOOT" 'nevr_timestep_log::Decide(in)' "LogTimestepArgs must log what the tested decision returns."
    sensor_515_need "$S515_BOOT" 'Log(line.level == nevr_timestep_log::Level::kWarning ? EchoVR::LogLevel::Warning : EchoVR::LogLevel::Info,' "The log level must follow the decision's level."
    sensor_515_need "$S515_BOOT" 'catch (const std::bad_alloc&)' "LogTimestepArgs allocates inside a game-called hook; it needs a named catch."
    sensor_515_need "$S515_TICK" 'nevr_server_timestep::ShouldReadBack(g_isServer != FALSE, t)) LogServerStepReadBack(t);' "The server's one clock read-back must be called from the per-frame dispatcher (N86/N110); without it the run's log cannot say whether the step is live."

    # --- N123: the login display name must be SOURCED, not a literal -------------
    # Every NEVR client announced the identical hardcoded name, so eight players in
    # one session rendered eight identical nameplates. The name was available the
    # whole time — token_auth parsed it from the auth response and persisted it to
    # the credential cache — it had simply never been exposed.
    #
    # The profile is assembled by key assignments (profile["displayname"] = ...), not
    # inside a C string literal, so these patterns match the source text directly.
    # A check that matches JSON text inside a string literal has to strip the
    # backslash before every quote first (see N115): left escaped, such a pattern
    # matches nothing and a broken tree still passes.
    N123_RC=0; N123_WS=$(grep -vE '^[[:space:]]*(//|/\*|\*[[:space:]/]|\*$)' src/runtime/compat/ws_bridge.cpp) || N123_RC=$?
    sensor_stage1 "N123 login display name sourced" "src/runtime/compat/ws_bridge.cpp" "$N123_RC"
    sensor_nonempty "N123 login display name sourced" "non-comment lines of compat/ws_bridge.cpp" "$N123_WS"
    if ! grep -qF 'profileInputs.display_name = displayName;' <<<"$N123_WS"; then
        echo "verify: FAIL — N123 ws_bridge no longer forwards the resolved display name into the login profile." >&2
        echo "A literal here makes every client announce the same name; eight players render eight identical nameplates and the service cannot tell them apart." >&2
        exit 1
    fi
    N123_PROFILE_RC=0; N123_PROFILE=$(grep -vE '^[[:space:]]*(//|/\*|\*[[:space:]/]|\*$)' src/runtime/compat/login_profile.cpp) || N123_PROFILE_RC=$?
    sensor_stage1 "N123 login profile display name" "src/runtime/compat/login_profile.cpp" "$N123_PROFILE_RC"
    sensor_nonempty "N123 login profile display name" "non-comment lines of compat/login_profile.cpp" "$N123_PROFILE"
    if ! grep -qF 'profile["displayname"] = inputs.display_name.empty()' <<<"$N123_PROFILE" || \
       ! grep -qF 'std::to_string(inputs.account_id)' <<<"$N123_PROFILE"; then
        echo "verify: FAIL — N123 login profile no longer serializes the resolved display name with the account-id fallback." >&2
        echo "Every NEVR client could announce the same literal name if this fallback is replaced with a placeholder." >&2
        exit 1
    fi
    # Anchored on the exact call form: a rename that APPENDS characters
    # (TokenAuth_GetUsernameX) still contains the bare symbol as a substring and
    # would satisfy a loose grep. Found while falsifying this very sensor.
    if ! grep -qF 'ResolveModuleProc("TokenAuth_GetUsername")' <<<"$N123_WS"; then
        echo "verify: FAIL — N123 ws_bridge no longer resolves TokenAuth_GetUsername." >&2
        echo "The field would fall back to the account id on every login even when the real name is known and cached." >&2
        exit 1
    fi
    N123_RC2=0; N123_TA=$(grep -vE '^[[:space:]]*(//|/\*|\*[[:space:]/]|\*$)' src/modules/token-auth/src/token_auth.cpp) || N123_RC2=$?
    sensor_stage1 "N123 username export" "src/modules/token-auth/src/token_auth.cpp" "$N123_RC2"
    sensor_nonempty "N123 username export" "non-comment lines of token_auth.cpp" "$N123_TA"
    if ! grep -qF 'TokenAuth_GetUsername(void)' <<<"$N123_TA"; then
        echo "verify: FAIL — N123 token_auth no longer exports TokenAuth_GetUsername." >&2
        echo "ws_bridge resolves this optionally, so its absence is silent: logins would quietly revert to the account id." >&2
        exit 1
    fi

    # --- N120: on a server, a degraded runtime must not keep running -------------
    # Four conditions must not be merely logged and stepped over. On a dedicated
    # server nobody reads a console, so "logged" means "lost", and the process
    # would run on to fail later somewhere unrelated — broken AND misattributed.
    #
    # All four are comment-stripped: a commented-out ServerFatal would satisfy a
    # plain grep and the sensor would pass on a disabled guard (N111).
    N120_RC=0; N120_LOADER=$(grep -vE '^[[:space:]]*(//|/\*|\*[[:space:]/]|\*$)' src/runtime/ext/plugin_loader.cpp) || N120_RC=$?
    sensor_stage1 "N120 server-fatal plugin guards" "src/runtime/ext/plugin_loader.cpp" "$N120_RC"
    sensor_nonempty "N120 server-fatal plugin guards" "non-comment lines of ext/plugin_loader.cpp" "$N120_LOADER"

    # The HookGuard verdict must be CONSUMED. `nevr_hook_guard::VerifyAll(filename);` as
    # a bare statement is the pre-N120 bug: the collision was detected, logged at
    # ERROR, and the plugin loaded anyway — detection that changed nothing.
    if ! grep -qE '(int|auto) +[a-z_]+ *= *nevr_hook_guard::VerifyAll' <<<"$N120_LOADER"; then
        echo "verify: FAIL — N120 nevr_hook_guard::VerifyAll's return is no longer captured." >&2
        echo "A discarded verdict means a plugin can re-hook an address this runtime owns, our patch silently stops applying, and the load continues as though nothing happened." >&2
        exit 1
    fi
    if ! grep -q 'ServerFatal' <<<"$N120_LOADER"; then
        echo "verify: FAIL — N120 plugin_loader no longer escalates to ServerFatal." >&2
        echo "A plugin that failed init, or that stole one of our hooks, would again be tolerated on a dedicated server." >&2
        exit 1
    fi

    # platform_compat returned 0 even when the TLS hook had failed. It must report failure so module_loader's
    # existing FatalError fires at the point of the defect.
    N120_RC2=0; N120_PC=$(grep -vE '^[[:space:]]*(//|/\*|\*[[:space:]/]|\*$)' src/modules/platform-compat/src/platform_compat.cpp) || N120_RC2=$?
    sensor_stage1 "N120 platform_compat reports failure" "src/modules/platform-compat/src/platform_compat.cpp" "$N120_RC2"
    sensor_nonempty "N120 platform_compat reports failure" "non-comment lines of platform_compat.cpp" "$N120_PC"
    if ! grep -q 'NEVR_MODULE_HOST_IS_SERVER' <<<"$N120_PC"; then
        echo "verify: FAIL — N120 platform_compat no longer distinguishes server from client." >&2
        echo "It must return failure ONLY on a server: module_loader treats a non-zero init as fatal in both modes, so an unconditional failure would hard-fail a client that should merely warn." >&2
        exit 1
    fi
    if ! grep -qE 'if *\( *isServer *&& *!tlsOk *\)' <<<"$N120_PC"; then
        echo "verify: FAIL — N120 platform_compat no longer fails a server run on a missing TLS hook." >&2
        echo "Without this it returns success with a degraded network stack, which is how a silently-failing TLS hook looked identical to a working one." >&2
        exit 1
    fi

    # The XPID patch's outcome was unobservable: it runs before the log filter is
    # up and its Log() output reached neither the console log nor nevr-boot.jsonl,
    # so across every captured run it reported nothing at all. `login injected
    # xpid=DSC-` does NOT cover it — ws_bridge builds that prefix itself and stays
    # green even when this patch never ran.
    N120_RC3=0; N120_XPID=$(grep -vE '^[[:space:]]*(//|/\*|\*[[:space:]/]|\*$)' src/runtime/patch/xpid_patch.cpp) || N120_RC3=$?
    sensor_stage1 "N120 xpid outcome observable" "src/runtime/patch/xpid_patch.cpp" "$N120_RC3"
    sensor_nonempty "N120 xpid outcome observable" "non-comment lines of patch/xpid_patch.cpp" "$N120_XPID"
    if [ "$(grep -c 'nevr_boot_log_tee::TeeFprintf' <<<"$N120_XPID")" -lt 2 ]; then
        echo "verify: FAIL — N120 the XPID patch no longer tees BOTH outcomes to the boot log." >&2
        echo "Success and failure must each leave a record, or whether the game's provider strings were rewritten is unknowable from a run." >&2
        exit 1
    fi
    # ServerFatal is not callable here: the patch runs before the CLI is parsed, so g_isServer is
    # still false and ServerFatal would only warn. The failure goes through g_bootHookFailed, which
    # boot.cpp checks once the mode is known (src/runtime/tests/test_behavioral.cpp XpidPatchBoot).
    if ! grep -q 'g_bootHookFailed = true' <<<"$N120_XPID"; then
        echo "verify: FAIL — N120 the XPID patch no longer escalates a validation failure." >&2
        echo "These sites are validated against literal bytes in the loaded image: a mismatch means echovr.exe is not the build this runtime targets, so every other patched address is suspect too." >&2
        exit 1
    fi
    # The flag alone is satisfied by one `g_bootHookFailed = true`, so a ServerFatal() restored on the
    # validation path would pass the check above. It must stay out of this file altogether: it runs
    # before the CLI is parsed, where ServerFatal only warns and the boot would go on.
    if grep -q 'ServerFatal(' <<<"$N120_XPID"; then
        echo "verify: FAIL — N120 xpid_patch.cpp calls ServerFatal()." >&2
        echo "The patch runs before the CLI is parsed, so g_isServer is still false and ServerFatal only warns; report through g_bootHookFailed (boot.cpp refuses to start a server)." >&2
        exit 1
    fi

    # --- N114: the plugin capability channel must stay WIRED ---------------------
    # A plugin declaring what it does is only useful if the host actually asks.
    # Declaring the export in the header and never resolving it would leave the
    # ABI looking complete while every plugin read as UNDECLARED — a decorative
    # channel, and exactly the class of half-wiring this repo keeps finding
    # (N48 shipped half-implemented for five weeks the same way).
    #
    # Comment-stripped: `// GetProcAddress(... "NvrPluginGetCapabilities")` would
    # otherwise satisfy a plain grep (N111).
    N114_RC=0; N114_LOADER=$(grep -vE '^[[:space:]]*(//|/\*|\*[[:space:]/]|\*$)' src/runtime/ext/plugin_loader.cpp) || N114_RC=$?
    sensor_stage1 "N114 plugin capabilities wired" "src/runtime/ext/plugin_loader.cpp" "$N114_RC"
    sensor_nonempty "N114 plugin capabilities wired" "non-comment lines of ext/plugin_loader.cpp" "$N114_LOADER"
    if ! grep -q 'GetProcAddress(hPlugin, "NvrPluginGetCapabilities")' <<<"$N114_LOADER"; then
        echo "verify: FAIL — N114 the host no longer resolves NvrPluginGetCapabilities." >&2
        echo "The ABI would still declare the export while every plugin read as UNDECLARED. A server cannot judge a session it cannot ask about." >&2
        exit 1
    fi
    N114_RC2=0; N114_ABI=$(grep -vE '^[[:space:]]*(//|/\*|\*[[:space:]/]|\*$)' sdk/include/nevr/plugin_interface.h) || N114_RC2=$?
    sensor_stage1 "N114 plugin capability ABI" "sdk/include/nevr/plugin_interface.h" "$N114_RC2"
    sensor_nonempty "N114 plugin capability ABI" "non-comment lines of nevr/plugin_interface.h" "$N114_ABI"
    # Pinned to the CURRENT ABI version. The pin guards against a silent REGRESSION
    # below the version where the capability channel (v3), the args channel (v4), the
    # query API (v5), the host log (v6, #598), the plugin assets (v7, #432) and the current level (v8, #1094)
    # landed — a deliberate bump updates this line, exactly as this one did.
    if ! grep -q 'NEVR_PLUGIN_API_VERSION 8' <<<"$N114_ABI"; then
        echo "verify: FAIL — N114 NEVR_PLUGIN_API_VERSION is not 8; the capability (v3) + args (v4) + query-API (v5) + host-log (v6) + asset (v7) + level (v8) channels must not regress below v8." >&2
        exit 1
    fi
    # N134 S7: the host must resolve NvrPluginInitEx — the v4 per-plugin-args init
    # export. This check is in the ABI header (typedef), the loader resolves the
    # symbol at runtime, so the static check is that the typedef exists and the
    # loader's GetProcAddress references it. Falsified: comment out the typedef;
    # rename the loader's "NvrPluginInitEx" string; revert API version to 3.
    if ! grep -q 'typedef.*NvrPluginInitEx_fn' <<<"$N114_ABI"; then
        echo "verify: FAIL — N114 S7 NvrPluginInitEx typedef removed from plugin_interface.h; the v4 args channel is dead without it." >&2
        exit 1
    fi
    if ! grep -q 'GetProcAddress(hPlugin, "NvrPluginInitEx")' <<<"$N114_LOADER"; then
        echo "verify: FAIL — N114 S7 the loader no longer resolves NvrPluginInitEx; a v4 plugin (InitEx + args_json) would fall through to Init (no args)." >&2
        exit 1
    fi
    # N134 S8: ctx_size must be set at every NvrGameContext construction site.
    # The field is how a v5+ plugin discovers the query API at runtime.
    # Falsified: remove any ctx_size = sizeof(NvrGameContext) line.
    if ! grep -q 'ctx_size = sizeof(NvrGameContext)' <<<"$N114_LOADER"; then
        echo "verify: FAIL — N134 S8 ctx_size not set in LoadPlugins; a v5 plugin cannot discover the query API." >&2
        exit 1
    fi
    if ! grep -q 'get_plugin_count = GetLoadedPluginCount' <<<"$N114_LOADER"; then
        echo "verify: FAIL — N134 S8 get_plugin_count function pointer not filled in LoadPlugins." >&2
        exit 1
    fi

    # --- N113: exactly ONE writer to CPrecisionSleep::BusyWait -------------------
    # nevr_binary_bug_fixes::Init saves the original byte before writing 0xC3 and restores
    # it on Shutdown (N33). A SECOND writer defeats that silently: if it runs first,
    # Init saves the already-patched 0xC3 as "the original" and the restore becomes
    # a no-op with the true byte lost for the process lifetime.
    #
    # A PatchServerFramePacing that blind-wrote the same VA with no validation and
    # no save would be safe only because Init happens to run first, which is an
    # ordering accident, not a design; this guard keeps it from existing.
    if grep -rn 'PatchServerFramePacing' src/runtime --include='*.cpp' --include='*.h' \
         | grep -vE ':[[:space:]]*(//|/\*|\*[[:space:]/]|\*$)' | grep .; then
        echo "verify: FAIL — N113 PatchServerFramePacing is back. It blind-writes CPrecisionSleep::BusyWait with no prologue validation and no original-byte save." >&2
        echo "patch/binary_bug_fixes.cpp is the canonical site and does both. A second writer defeats the N33 shutdown restore whenever it runs first." >&2
        exit 1
    fi
    # The canonical site must still pair the save with the write.
    N113_RC=0; N113_BODY=$(grep -vE '^[[:space:]]*(//|/\*|\*[[:space:]/]|\*$)' src/runtime/patch/binary_bug_fixes.cpp) || N113_RC=$?
    sensor_stage1 "N113 busywait save/restore" "src/runtime/patch/binary_bug_fixes.cpp" "$N113_RC"
    sensor_nonempty "N113 busywait save/restore" "non-comment lines of patch/binary_bug_fixes.cpp" "$N113_BODY"
    if ! grep -q 'memcpy(&s_busywait_original_byte' <<<"$N113_BODY"; then
        echo "verify: FAIL — N113 the BusyWait RET patch no longer saves the original byte; the N33 shutdown restore has nothing to restore." >&2
        exit 1
    fi

    # --- N110: exactly ONE per-frame dispatcher ---------------------------------
    # There were two. The second (inside PrecisionSleepWaitHook) hard-coded
    # gctx.flags = NEVR_HOST_IS_SERVER with the comment "always server at this
    # point", while hook_liveness.cpp records that same hook as "CLIENT ONLY —
    # never runs on a server (N86)". The one path that runs only on a client told
    # every plugin it was on a server, for as long as both copies existed.
    #
    # Two copies is the shape to guard against: a correct dispatcher beside a second one that
    # asserts the opposite flag. So the invariant is about COUNT, not about the flag value.
    N110_RC=0; N110_TICKS=$(grep -rn 'TickPlugins(&\|TickModules(&' src/runtime --include='*.cpp') || N110_RC=$?
    sensor_stage1 "N110 single dispatcher" "src/runtime/**/*.cpp" "$N110_RC"
    sensor_nonempty "N110 single dispatcher" "Tick*(&...) call sites" "$N110_TICKS"
    # Comment-stripped before counting: `// TickModules(&mctx);` still matches the
    # grep above, so a dispatcher commented OUT would leave the count at 2 and the
    # sensor would pass while the tick was dead. Measured 2026-07-29 — this exact
    # break went green before the filter was added. Uses the repo's :-anchored
    # stripper (the * arm requires whitespace/slash/EOL after it, so it does not
    # eat a C pointer dereference).
    N110_PROD=$(grep -v '/tests/' <<<"$N110_TICKS" \
                | grep -vE ':[[:space:]]*(//|/\*|\*[[:space:]/]|\*$)' | grep -c . || true)
    if [ "$N110_PROD" -ne 2 ]; then
        echo "verify: FAIL — N110 expected exactly 2 production Tick*(&) call sites (one TickPlugins, one TickModules, both inside DispatchPerFrameWork); found $N110_PROD." >&2
        grep -v '/tests/' <<<"$N110_TICKS" >&2
        echo "A second dispatcher is how the client path came to report NEVR_HOST_IS_SERVER. Call DispatchPerFrameWork instead of inlining another copy." >&2
        exit 1
    fi
    # ...and the surviving one must derive the host flag, never assert it.
    N110_BODY=$(awk '/^void DispatchPerFrameWork/,/^}/' src/runtime/frame/tick.cpp)
    sensor_nonempty "N110 dispatcher body" "DispatchPerFrameWork() body" "$N110_BODY"
    if ! grep -q 'gctx.flags = g_isServer ?' <<<"$N110_BODY"; then
        echo "verify: FAIL — N110 DispatchPerFrameWork no longer derives the host flag from g_isServer." >&2
        echo "Hard-coding NEVR_HOST_IS_SERVER is the original defect: this dispatcher runs from a client-only hook too." >&2
        exit 1
    fi

    # --- N109: src/runtime/ must not re-flatten its own namespace ---------------
    # The reason 44 files piled into one directory is that the target put its own
    # source dir (and gameserver/) on the include path, so any file could include
    # any other by bare basename and nothing ever forced a decision about where a
    # file belonged. Every include is now path-qualified from src/.
    #
    # Re-adding ${CMAKE_CURRENT_SOURCE_DIR} to this target restores the flat
    # namespace silently: nothing fails, and the next file lands wherever. This
    # sensor is the only thing standing between that one line and the tree we
    # just spent a reorganization undoing.
    N109_RC=0; N109_INC=$(awk '/^target_include_directories\(nevr_runtime/,/\)/' src/runtime/CMakeLists.txt) || N109_RC=$?
    sensor_stage1 "N109 runtime include roots" "src/runtime/CMakeLists.txt" "$N109_RC"
    sensor_nonempty "N109 runtime include roots" "target_include_directories(nevr_runtime …) block" "$N109_INC"
    if grep -q 'CMAKE_CURRENT_SOURCE_DIR' <<<"$N109_INC"; then
        echo "verify: FAIL — N109 src/runtime/CMakeLists.txt puts its own source dir on the include path." >&2
        echo "That re-enables bare-basename includes across all of src/runtime/, which is how 44 files ended up flat in one directory. Include path-qualified from src/ instead: \"runtime/hook/addresses.h\"." >&2
        exit 1
    fi

    # --- N108: the shared layer's dependency direction --------------------------
    # src/abi/ encodes what the GAME BINARY told us. src/core/ is what WE wrote.
    # The dependency runs core -> abi (EchoVR::LogLevel is a game enum, so logging
    # genuinely sits on the ABI layer). The reverse is forbidden: an abi/ header
    # that reaches into core/ makes the reconstruction surface depend on our own
    # conveniences, and the split stops meaning anything the first time it happens.
    #
    # pch.h is the ONE sanctioned exception and it is exempted BY NAME, not by a
    # loose pattern: it is the precompiled header, build plumbing rather than a
    # layer dependency. Widening this exemption re-merges the two directories in
    # everything but name.
    N108_RC=0; N108_HITS=$(grep -rn '#include "\(core\|extension\)/' src/abi) || N108_RC=$?
    sensor_stage1 "N108 abi->core direction" "src/abi" "$N108_RC"
    if [ "$N108_RC" -eq 0 ] && printf '%s\n' "$N108_HITS" | grep -v '#include "core/pch.h"' | grep .; then
        echo "verify: FAIL — N108 src/abi/ includes src/core/. The dependency runs core -> abi, never the reverse." >&2
        echo "The ABI layer records what the binary told us; it must not depend on our own primitives. Move the shared piece INTO abi/, or stop using it there." >&2
        exit 1
    fi
    # No "do these directories still exist" loop here on purpose: it would be
    # unfalsifiable, because emptying any of the three fails EARLIER than
    # this point and for a different reason, so the loop could never be shown to
    # be the thing that caught it. Emptying src/extension/ dies at configure with
    # `No SOURCES given to target: nevr_core`, because module_interface.h is listed
    # in src/core/CMakeLists.txt. What actually fail-closes:
    #   src/abi      -> sensor_stage1 above (grep rc>=2 on a missing subject)
    #   src/core     -> the same, via the N81/N85/N20 path sets
    #   src/extension-> CMake, since its header is in nevr_core's source list
    # --- N151: the Quest sentinel changes no code page (docs/adr/0021) ------------
    # Horizon denies execmod on a written libr15 page, so the libr15 is patched on the host at build time and the sentinel
    # stores only DATA at run time. Four things keep it so, each a scan of src/quest outside the tests:
    #   (1) a protection or mapping call (mprotect, mmap, mremap, PROT_EXEC, PROT_WRITE) appears only in the files that need
    #       one: got_hook.cpp (the GOT's data pages), libr15_attach.cpp (its trampoline pages, anonymous memory),
    #       credits_text.cpp (its anonymous buffer) and plugins/q15t/src/got_swap.cpp (the q15t GOT swap, data pages only),
    #       plus the two headers that declare those seams;
    #   (2) the run-time code writers of the old inline hooks (InstallInlineThunk, WriteWord, AllocateInReach) do not come back;
    #   (3) credits_text.cpp makes exactly one protection call, on its own buffer, and no other mprotect;
    #   (4) the STOCK build ID constant is named only by the files that need the stock file's identity. A hook that compares the
    #       stock ID where it means the patched one is a no-op on the patched libr15 that no unit test of the hook's own logic sees.
    # The allowlists are repeated in tools/tests/test_quest_no_code_writes.py, which fails when they differ.
    # BEGIN N151
    N151_PROT_OK=" plugins/q15t/src/got_swap.cpp sentinel/got_hook.cpp sentinel/libr15_attach.cpp sentinel/libr15_attach.h sentinel/credits_text.cpp sentinel/credits_text.h "
    N151_ID_OK=" diag/hwdump_run.cpp patch/libr15_patch_identity.cpp patch/libr15_extra_rows.cpp patch/libr15_patch_table.h plugin/plugin_identity.h sentinel/credits_text.cpp sentinel/data_wiring.cpp sentinel/libr15_attach.cpp sentinel/libr15_identity.cpp sentinel/pinned_targets.h "
    n151_fail() { echo "verify: FAIL — N151 $1 (docs/adr/0021: the sentinel changes no code page at run time)." >&2; exit 1; }
    N151_RC=0; N151_PROT=$(grep -rnE '\b(mprotect|mmap|mremap|pkey_mprotect)\b|PROT_(EXEC|WRITE)' src/quest --include='*.cpp' --include='*.h' --exclude-dir=tests) || N151_RC=$?
    sensor_stage1 "N151 protection calls" "src/quest" "$N151_RC"
    sensor_nonempty "N151 protection calls" "the protection calls of got_hook.cpp, libr15_attach.cpp and credits_text.cpp" "$N151_PROT"
    N151_PROT_CODE=$(grep -vE ':[[:space:]]*(//|/\*|\*[[:space:]/]|\*$)' <<<"$N151_PROT" || true)
    while IFS= read -r n151_hit; do
        [ -n "$n151_hit" ] || continue
        n151_rel="${n151_hit%%:*}"; n151_rel="${n151_rel#src/quest/}"
        case "$N151_PROT_OK" in *" $n151_rel "*) ;; *) n151_fail "$n151_rel changes or maps memory protection: $n151_hit" ;; esac
    done <<<"$N151_PROT_CODE"
    N151_RC=0; N151_BAD=$(grep -rnE 'InstallInlineThunk|\bWriteWord\b|AllocateInReach' src/quest --include='*.cpp' --include='*.h' --exclude-dir=tests) || N151_RC=$?
    sensor_stage1 "N151 run-time code writers" "src/quest" "$N151_RC"
    N151_BAD_CODE=$(grep -vE ':[[:space:]]*(//|/\*|\*[[:space:]/]|\*$)' <<<"$N151_BAD" || true)
    [ -z "$N151_BAD_CODE" ] || n151_fail "a run-time code writer is back: $N151_BAD_CODE"
    N151_RC=0; N151_CRED=$(grep -vE '^[[:space:]]*(//|/\*|\*[[:space:]/]|\*$)' src/quest/sentinel/credits_text.cpp) || N151_RC=$?
    sensor_stage1 "N151 credits install" "src/quest/sentinel/credits_text.cpp" "$N151_RC"
    sensor_nonempty "N151 credits install" "non-comment lines of src/quest/sentinel/credits_text.cpp" "$N151_CRED"
    N151_CALLS=$(grep -cE '\bprotect\(' <<<"$N151_CRED" || true)
    N151_OWN=$(grep -cE '\bprotect\(memory,' <<<"$N151_CRED" || true)
    { [ "$N151_CALLS" = "1" ] && [ "$N151_OWN" = "1" ]; } || n151_fail "credits_text.cpp makes $N151_CALLS protection call(s), $N151_OWN on its own buffer; it must make exactly one, on its buffer"
    N151_MP=$(grep -cE '\bmprotect\b' <<<"$N151_CRED" || true)
    N151_MP_DEFAULT=$(grep -cE ': &mprotect;' <<<"$N151_CRED" || true)
    [ "$N151_MP" = "$N151_MP_DEFAULT" ] || n151_fail "credits_text.cpp names mprotect other than as the default of its protection seam"
    N151_RC=0; N151_ID=$(grep -rn 'kLibR15StockBuildId' src/quest --include='*.cpp' --include='*.h' --exclude-dir=tests) || N151_RC=$?
    sensor_stage1 "N151 stock build ID users" "src/quest" "$N151_RC"
    sensor_nonempty "N151 stock build ID users" "the stock build ID's users" "$N151_ID"
    N151_ID_CODE=$(grep -vE ':[[:space:]]*(//|/\*|\*[[:space:]/]|\*$)' <<<"$N151_ID" || true)
    while IFS= read -r n151_hit; do
        [ -n "$n151_hit" ] || continue
        n151_rel="${n151_hit%%:*}"; n151_rel="${n151_rel#src/quest/}"
        case "$N151_ID_OK" in *" $n151_rel "*) ;; *) n151_fail "$n151_rel names kLibR15StockBuildId; a hook target carries sentinel::pinned::LibR15PatchedBuildId(): $n151_hit" ;; esac
    done <<<"$N151_ID_CODE"
    # END N151
    N81_RC=0; N81_HITS=$(grep -rn '"\[NEVR\] ' src/runtime src/abi src/core) || N81_RC=$?
    sensor_stage1 "N81 bare [NEVR] tag" "src/runtime src/abi src/core" "$N81_RC"
    if [ "$N81_RC" -eq 0 ] && printf '%s\n' "$N81_HITS" \
         | grep -v legacy | grep -v 'src/runtime/lifecycle/cli.cpp' | grep .; then
        echo "verify: FAIL — N81 bare [NEVR] log tag found; use a subsystem tag from the docs/standards/logging.md table." >&2
        exit 1
    fi
    # --- Tier-0 hook invariants (N83/N84) ------------------------------------
    # Static: no Wine, no game process, no execution. Catches the failure that
    # produced the whole broadcaster thread — an address given a name nobody
    # re-derived, then reasoned about by that name forever after.
    # Known bugs warn (and stay visible); anything NEW is a hard failure.
    # 1500 s, not 600: the full suite (1737 tests) took 457 s on the developer machine and about 574 s
    # for 1145 tests on the CI runner (#1004, release build 40454435804, killed at 600 s with exit 124).
    timeout -k 10 1500 python3 -m unittest discover -s tools/tests -p 'test_*.py'
    python3 tools/verify_hook_invariants.py
    # The scenario-test control endpoint can inject messages into a live session; it exists only in
    # the mingw-scenario preset. The DLL this gate just built must not carry it.
    python3 tools/verify_scenario_control_absent.py "build/{{ preset }}/bin/BugSplat64.dll" "build/{{ preset }}/bin/nevr-runtime.dll"
    # #575, #853: the game-facing PE surface of BugSplat64.dll (the host) is pinned: the exact 28-name export
    # set with its ordinals, DetoursExportPlaceholder at ordinal 1, no eager nevr-runtime.dll import, no runtime
    # code in it; nevr-runtime.dll exports the rendezvous entry and the module entry points and carries the
    # build-identity literal, and the two literals agree. The game's own BugSplat64.dll imports are cross-checked when
    # the game binary is present (it is not in CI or a fresh worktree). Fail-closed: no `|| true`.
    if [ -f echovr/bin/win10/echovr.exe ]; then
        python3 tools/verify_pe_contract.py --dll "build/{{ preset }}/bin/BugSplat64.dll" --runtime-dll "build/{{ preset }}/bin/nevr-runtime.dll" --game-exe echovr/bin/win10/echovr.exe
    else
        echo "pe-contract: NOTE game binary absent (echovr/bin/win10/echovr.exe) - game-import cross-check skipped; host export checks still run"
        python3 tools/verify_pe_contract.py --dll "build/{{ preset }}/bin/BugSplat64.dll" --runtime-dll "build/{{ preset }}/bin/nevr-runtime.dll"
    fi
    # #853 (ADR 0012 decision 4): the host shares no writable data symbol with the core or ABI library. Run on the
    # build's own UNSTRIPPED bin/BugSplat64.dll (dist strips a copy later; a stripped host has no symbols to read and
    # the tool then fails). The tool reads nm itself and fails closed: nm failing, an empty listing or a writable symbol
    # the host shares with libnevr_core.a or libnevr_abi.a all exit 1 (N93: no stage that can answer "no match" for
    # "could not look").
    python3 tools/verify_host_no_core_state.py --host-dll "build/{{ preset }}/bin/BugSplat64.dll" --lib "build/{{ preset }}/src/core/libnevr_core.a" --lib "build/{{ preset }}/src/abi/libnevr_abi.a"
    # #850: both shipped Windows files carry the pinned VERSIONINFO (Details tab), at the version the DLL's
    # own NEVR-BUILD literal states. Fail-closed: a file without the resource, a changed field or a
    # version that is not the build's exits 1.
    python3 tools/pe_version_info.py check --dll "build/{{ preset }}/bin/BugSplat64.dll" --runtime-dll "build/{{ preset }}/bin/nevr-runtime.dll" --exe "build/{{ preset }}/bin/echovr_server.exe"
    # N84 runtime counterpart. The static check above scans source, so it only
    # sees plugins in THIS tree — a third-party plugin is a DLL we never compile.
    # HookGuard detects the effect (our bytes changed) instead of the source.
    # Both call sites are wiring, invisible to the GTest, so they get a sensor.
    if ! grep -q 'nevr_hook_guard::Record(target, name)' src/runtime/hook/patching.h; then
        echo "verify: FAIL — N84 nevr_hook_guard::Record missing from PatchDetour; new detours would be unguarded." >&2
        exit 1
    fi
    if ! grep -qE 'nevr_hook_guard::VerifyAll\((filename|s\.item\.file\.c_str\(\))\)' src/runtime/ext/plugin_loader.cpp; then
        echo "verify: FAIL — N84 nevr_hook_guard::VerifyAll missing from the plugin load path; third-party re-hooks undetectable." >&2
        exit 1
    fi
    # N85: never hand ixwebsocket an empty std::function. It invokes
    # _onMessageCallback unconditionally, so nullptr throws std::bad_function_call,
    # which unwinds out of ixwebsocket's own thread, reaches the game's
    # unhandled-exception filter as GCC throw code 0x20474343, and kills the
    # dedicated server. Use a no-op lambda.
    # Match the CALL form ('->setOnMessageCallback(nullptr)') and drop comment
    # lines. A looser pattern flags prose describing the bug — the same false
    # positive the N81 sensor hit on cli.cpp's --help strings.
    N85_RC=0; N85_HITS=$(grep -rn -- '->setOnMessageCallback(nullptr)' src/runtime src/modules src/abi src/core) || N85_RC=$?
    sensor_stage1 "N85 empty ws callback" "src/runtime src/modules src/abi src/core" "$N85_RC"
    if [ "$N85_RC" -eq 0 ] && printf '%s\n' "$N85_HITS" \
         | grep -v legacy | grep -vE ':[[:space:]]*(//|/\*|\*[[:space:]/]|\*$)' | grep .; then
        echo "verify: FAIL — N85 setOnMessageCallback(nullptr) reintroduced; use a no-op lambda." >&2
        echo "An empty std::function invoked by ixwebsocket throws std::bad_function_call and kills the server." >&2
        exit 1
    fi
    # Issue #43: received ServerDB payloads reach the game through
    # CBroadcaster::ReceiveLocalEvent, which takes them as mutable. The receive
    # callback therefore hands out a writable dispatcher-owned copy, and no server
    # code casts const away from a payload pointer. test_protobuf_transport pins the
    # callback type; this catches a handler that re-declares its parameter const
    # and casts it back. :-anchored N99 comment stripper.
    I43_RC=0; I43_HITS=$(grep -rnF -- 'const_cast<VOID*>' src/runtime/server) || I43_RC=$?
    sensor_stage1 "issue #43 const_cast on received payload" "src/runtime/server" "$I43_RC"
    if [ "$I43_RC" -eq 0 ] && printf '%s\n' "$I43_HITS" \
         | grep -vE ':[[:space:]]*(//|/\*|\*[[:space:]/]|\*$)' | grep .; then
        echo "verify: FAIL — issue #43 const_cast<VOID*> is back in src/runtime/server." >&2
        echo "Take the payload as VOID* from WebSocketClient::MessageCallback; it is writable by contract." >&2
        exit 1
    fi
    # N71: the session-flags null-deref class is covered by BreakpointVEH's
    # generic null-ptr branch, NOT by per-site hooks. The VEH must call the
    # policy that enforces the low-address and game-loop-thread conditions, and
    # the attribution table must remain populated
    # (without it a fire reports a bare RVA and the class is unrecognisable).
    if ! grep -q 'if (nevr_veh_policy::ShouldRecoverNullPointerAv(' src/runtime/lifecycle/crash_recovery.cpp \
       || ! grep -q 'return isServer && target < 0x10000 && jumpBufferValid && gameLoopThreadId != 0 &&' src/runtime/lifecycle/veh_policy.h; then
        echo "verify: FAIL — N71 generic null-ptr AV branch missing from BreakpointVEH; the whole" >&2
        echo "session-flags deref class loses its only guard (no per-site hooks exist by design)." >&2
        exit 1
    fi
    # Count ENTRIES, not lines — the table packs two per line.
    N71_SITES=$(grep -oE '\{0x[0-9A-F]+, "' src/runtime/lifecycle/crash_recovery_sites.h | wc -l)
    if [ "$N71_SITES" -lt 25 ]; then
        echo "verify: FAIL — N71 known-site table has $N71_SITES entries (expected >= 25);" >&2
        echo "a crash at these RVAs would report a bare address with no class attribution." >&2
        exit 1
    fi
    # N37: ixwebsocket never sets SO_REUSEADDR and exposes no socket-option hook,
    # so a zombie LISTEN socket can refuse a bind. That gap is UNFIXABLE from here
    # (upstream vcpkg dep) and is instead made unreachable by N39: pick a random
    # ephemeral port and retry. Both properties must survive, or the old
    # hardcoded-port failure returns with no way to fix it.
    for f in src/runtime/compat/ws_bridge.cpp; do
        if ! grep -q 'uniform_int_distribution<uint16_t> dist(49152, 65535)' "$f"; then
            echo "verify: FAIL — N37/N39 random ephemeral-port selection missing from $f;" >&2
            echo "a fixed port reintroduces the zombie-socket bind failure ixwebsocket cannot recover from." >&2
            exit 1
        fi
        if ! grep -q 'kMaxBindAttempts' "$f"; then
            echo "verify: FAIL — N37/N39 bind retry loop missing from $f." >&2
            exit 1
        fi
    done
    # N62: graceful shutdown must not resolve symbols through the loader lock.
    # Strip comment lines first: the function's own comment says "NO
    # GetModuleHandleA/GetProcAddress", and a naive match flags the rule as a
    # violation of itself. Same false positive the N81 and N85 sensors hit.
    N62_RC=0; N62_BODY=$(awk '/^void PerformGracefulShutdown/,/^}/' src/runtime/lifecycle/crash_recovery.cpp) || N62_RC=$?
    sensor_stage1 "N62 shutdown loader-lock" "src/runtime/lifecycle/crash_recovery.cpp" "$N62_RC"
    sensor_nonempty "N62 shutdown loader-lock" "PerformGracefulShutdown() body in src/runtime/lifecycle/crash_recovery.cpp" "$N62_BODY"
    N62_CODE=$(grep -vE '^[[:space:]]*(//|/\*|\*[[:space:]/]|\*$)' <<<"$N62_BODY" || true)
    if grep -qE 'GetModuleHandleA|GetProcAddress' <<<"$N62_CODE"; then
        echo "verify: FAIL — N62 PerformGracefulShutdown resolves symbols at shutdown time;" >&2
        echo "both take the loader lock and may deadlock shutdown." >&2
        echo "Call StopWebSocketBridgeListener() directly." >&2
        exit 1
    fi
    if ! grep -q 'ResolveShutdownDependencies();' src/runtime/lifecycle/boot.cpp; then
        echo "verify: FAIL — N62 ResolveShutdownDependencies() call site missing." >&2
        exit 1
    fi
    # N86: the per-frame tick MUST be dispatched from a site that runs in server
    # mode. PrecisionSleep::Wait does not (measured: 0 entries over a full run),
    # which left plugins, modules and N69's stack reserve silently dead there.
    # N68's original sensor only checked that TickPlugins appeared in the file —
    # which it did, in a hook that never executed. Check the LIVE site.
    # Herestrings, not pipelines: `awk ... | grep -q` races to a false FAIL under pipefail (N101).
    N86_RC=0; N86_BODY=$(awk '/^void DispatchPerFrameWork/,/^}/' src/runtime/frame/tick.cpp) || N86_RC=$?
    sensor_stage1 "N86 per-frame dispatch" "src/runtime/frame/tick.cpp" "$N86_RC"
    sensor_nonempty "N86 per-frame dispatch" "DispatchPerFrameWork() body in src/runtime/frame/tick.cpp" "$N86_BODY"
    if ! grep -q 'TickPlugins' <<<"$N86_BODY"; then
        echo "verify: FAIL — N86 TickPlugins missing from DispatchPerFrameWork; the server-mode" >&2
        echo "per-frame tick is dead again (PrecisionSleep::Wait never runs on a server)." >&2
        exit 1
    fi
    if ! grep -q 'TickModules' <<<"$N86_BODY"; then
        echo "verify: FAIL — N86 TickModules missing from DispatchPerFrameWork." >&2
        exit 1
    fi
    if ! grep -q 'if (InterlockedExchange(&g_tickReentry, 1) != 0) return;' <<<"$N86_BODY"; then
        echo "verify: FAIL — N86 re-entrancy gate missing from DispatchPerFrameWork; a plugin" >&2
        echo "OnFrame that calls GetTimeMicroseconds would recurse without bound." >&2
        exit 1
    fi
    if ! grep -q 'nevr_frame::DispatchPerFrameWork(nowUs)' src/runtime/patch/binary_bug_fixes.cpp; then
        echo "verify: FAIL — N86 DispatchPerFrameWork call site missing from the live tick hook." >&2
        exit 1
    fi
    # N20: no platform identity may be compiled into the binary. The login
    # injection must take it from token-auth (JWT) or, in server mode, from
    # config — never a literal. Matches an 18-20 digit account/discord ID.
    N20_RC=0; N20_HITS=$(grep -rnE '\b1[0-9]{17,19}\b' src/runtime src/modules src/abi src/core) || N20_RC=$?
    sensor_stage1 "N20 identity literal" "src/runtime src/modules src/abi src/core" "$N20_RC"
    if [ "$N20_RC" -eq 0 ] && printf '%s\n' "$N20_HITS" \
         | grep -v legacy | grep -viE 'hash|symbol|0x|SYM_' | grep .; then
        echo "verify: FAIL — N20 an account/discord ID literal is compiled into source;" >&2
        echo "identity must come from the presented credential or config, never the binary." >&2
        exit 1
    fi
    # #41: config credentials reach Nakama in a URL query from two places — the
    # ServerDB registration URI (server/gameserver.cpp) and the bridge's config/
    # login connections (compat/ws_bridge.cpp). Both must go through the
    # percent-encoder in server/serverdb_uri.cpp. A raw append lets a password
    # with '&', '#', '%' or '+' rewrite the query, and if only one site encodes,
    # the two paths send different passwords for the same account.
    # Falsified 2026-10-05 against 323352b: the pattern hits gameserver.cpp:1433
    # and ws_bridge.cpp:1002-1003; on the fixed tree it exits 1.
    I41_RC=0; I41_HITS=$(grep -nE '[?&]password=%s|\+= *cfgPassword|"&password="' \
        src/runtime/server/gameserver.cpp src/runtime/server/gameserver_serverdb.cpp src/runtime/compat/ws_bridge.cpp) || I41_RC=$?
    sensor_stage1 "#41 raw URL credential" "server/gameserver.cpp server/gameserver_serverdb.cpp compat/ws_bridge.cpp" "$I41_RC"
    if [ "$I41_RC" -eq 0 ]; then
        printf '%s\n' "$I41_HITS" >&2
        echo "verify: FAIL — #41 a credential is concatenated into a URL unencoded; use ServerDbUri (server/serverdb_uri.h)." >&2
        exit 1
    fi
    if ! grep -q 'nevr_serverdb_uri::BuildLegacyUri(' src/runtime/server/gameserver.cpp src/runtime/server/gameserver_serverdb.cpp \
       || ! grep -q 'nevr_serverdb_uri::BuildBridgeCredentialUri(' src/runtime/compat/ws_bridge.cpp; then
        echo "verify: FAIL — #41 a URL-credential site no longer calls the ServerDbUri encoder." >&2
        exit 1
    fi
    # N20 (owner decision, 2026-07-27): the nevr_discord_id config fallback applies
    # in CLIENT mode too, not only server mode. Two assertions, because either one
    # alone is satisfiable by the bug — the first fires if the fallback is deleted,
    # the second if it is re-gated on server mode. Both failures are silent in
    # production: the only symptom is a WARNING line and a login as account 0.
    # RETARGETED 2026-07-28 (N105). This sensor read: "asserted against
    # src/modules/ws-bridge/ because THAT is the copy that ships". That was true
    # when written and the N92 fold INVERTED it — gamepatches became the shipping
    # copy and the module stopped being built — so from 2026-07-27 this guarantee
    # was asserted against code that never runs. The invariant does hold in the
    # shipping copy (ws_bridge.cpp:396); only the sensor was aimed wrong.
    N20A_RC=0; grep -q 'Using nevr_discord_id from config' src/runtime/compat/ws_bridge.cpp || N20A_RC=$?
    sensor_stage1 "N20 discord-id config fallback" "src/runtime/compat/ws_bridge.cpp" "$N20A_RC"
    if [ "$N20A_RC" -ne 0 ]; then
        echo "verify: FAIL — N20 nevr_discord_id config fallback missing from the shipping" >&2
        echo "ws bridge (src/runtime/compat/ws_bridge.cpp)." >&2
        exit 1
    fi
    N20B_RC=0; grep -n 'discordId == 0 &&.*g_isServer' src/runtime/compat/ws_bridge.cpp || N20B_RC=$?
    sensor_stage1 "N20 fallback not server-gated" "src/runtime/compat/ws_bridge.cpp" "$N20B_RC"
    if [ "$N20B_RC" -eq 0 ]; then
        echo "verify: FAIL — N20 the config discord-id fallback is gated on g_isServer;" >&2
        echo "the owner decision (2026-07-27) is that it applies in CLIENT mode too." >&2
        exit 1
    fi
    # N86-class: HookLiveness reports "entered=NO" for a hook that never runs.
    # If an ID is DECLARED but never Mark()ed, it reports NO forever — which is
    # indistinguishable from genuinely dead wiring, i.e. the tool would generate
    # exactly the false signal it exists to remove. Every declared ID must have
    # at least one Mark() call site.
    # Match enumerators with or without an explicit initialiser: the first is
    # written "kGetTimeMicroseconds = 0,". An earlier regex required a bare
    # "kName," and so undercounted by one, making the comparison below
    # unsatisfiable — the sensor could never fire. kCount has no trailing comma
    # and is excluded by construction.
    HL_RC=0; HL_ENUM=$(awk '/^enum Id/,/^};/' src/runtime/hook/hook_liveness.h) || HL_RC=$?
    sensor_stage1 "HookLiveness enum census" "src/runtime/hook/hook_liveness.h" "$HL_RC"
    sensor_nonempty "HookLiveness enum census" "enum Id in src/runtime/hook/hook_liveness.h" "$HL_ENUM"
    DECLARED=$(printf '%s\n' "$HL_ENUM" \
               | grep -cE '^[[:space:]]+k[A-Za-z]+([[:space:]]*=[^,]*)?,' || true)
    if [ "$DECLARED" -lt 1 ]; then
        echo "verify: FAIL — HookLiveness enum census matched zero enumerators; the enum's" >&2
        echo "spelling no longer fits the pattern and the DECLARED/MARKED comparison below" >&2
        echo "would be vacuous." >&2
        exit 1
    fi
    # MARKED needs no stage-1 capture: src/runtime is the repo itself, and a
    # zero count makes MARKED < DECLARED fail closed below (measured direction).
    MARKED=$(grep -rhoE 'nevr_hook_liveness::Mark\(nevr_hook_liveness::k[A-Za-z]+\)' src/runtime \
             | sort -u | wc -l)
    if [ "$MARKED" -lt "$DECLARED" ]; then
        echo "verify: FAIL — HookLiveness declares $DECLARED ids but only $MARKED are Mark()ed." >&2
        echo "An unmarked id reports entered=NO forever, which is indistinguishable from dead" >&2
        echo "wiring (N86) — the tool would manufacture the false alarm it exists to prevent." >&2
        exit 1
    fi
    # N89: log_filter.dll duplicates the built-in filter. Loading it installs a
    # SECOND MinHook on CLog::PrintfImpl from a different static MinHook copy,
    # which measurably killed the built-in filter's game-line capture (67 game
    # lines -> 0) and stopped max_line_length from applying.
    if ! grep -q 'kSupersededByBuiltin' src/runtime/ext/plugin_loader.cpp; then
        echo "verify: FAIL — N89 superseded-plugin skip missing from plugin_loader; a stale" >&2
        echo "log_filter.dll would silently disable the built-in log filter's file output." >&2
        exit 1
    fi
    # N95: and we shall not PRODUCE the DLL the loader above refuses. Two
    # independent sensors, because either alone is satisfiable by the bug:
    # re-adding a build target ships it again the moment any dist rule
    # globs plugins/, and re-adding a dist rule ships whatever a stale build
    # tree still holds. The target sensor reads every tracked CMake file, since
    # a target can come back in any of them. Comment lines are stripped before
    # matching so the prose forbidding the thing cannot satisfy its own check.
    N95_CMAKE=$(git ls-files -- 'CMakeLists.txt' '*/CMakeLists.txt' '*.cmake')
    sensor_nonempty "N95 log-filter build target" "tracked CMake files (git ls-files)" "$N95_CMAKE"
    N95_TGT_RC=0; N95_TGT=$(grep -vhE '^\s*#' $N95_CMAKE) || N95_TGT_RC=$?
    sensor_stage1 "N95 log-filter build target" "tracked CMake files" "$N95_TGT_RC"
    sensor_nonempty "N95 log-filter build target" "non-comment lines of the tracked CMake files" "$N95_TGT"
    if grep -qE 'log[-_]filter' <<<"$N95_TGT"; then
        echo "verify: FAIL — N95 log-filter is a build target again; plugin_loader.cpp refuses" >&2
        echo "log_filter.dll by name, so building it produces a DLL our own loader rejects (N89)." >&2
        exit 1
    fi
    N95_DIST_RC=0; N95_DIST=$(grep -vE '^\s*#' CMakeLists.txt) || N95_DIST_RC=$?
    sensor_stage1 "N95 log-filter packaging" "CMakeLists.txt" "$N95_DIST_RC"
    sensor_nonempty "N95 log-filter packaging" "non-comment lines of CMakeLists.txt" "$N95_DIST"
    if grep -q 'log_filter' <<<"$N95_DIST"; then
        echo "verify: FAIL — N95 a dist rule packages the superseded log_filter.dll." >&2
        exit 1
    fi
    # N90: pnsrad.dll links its OWN copy of CLog, so hooking echovr.exe's
    # CLog::PrintfImpl does not cover it. Without this second hook its output
    # bypasses the filter entirely — measured: 8191-byte profile dumps on console
    # while max_line_length was 500.
    if ! grep -q 'InstallPnsradHook' src/runtime/log/builtin_filter.cpp; then
        echo "verify: FAIL — N90 pnsrad log hook missing; pnsrad.dll output bypasses the filter." >&2
        exit 1
    fi
    if ! grep -q 'nevr_builtin_log_filter::InstallPnsradHook();' <<<"$TICK_CODE"; then
        echo "verify: FAIL — N90 InstallPnsradHook call site missing from the live tick; the hook" >&2
        echo "would never install, since pnsrad.dll loads long after filter init." >&2
        exit 1
    fi
    # N75: plugins must load with a restricted search path (modules are statically
    # linked and load no DLL). With
    # dwFlags=0 the search starts at the application directory, so a dll dropped
    # next to echovr.exe can satisfy a dependency ahead of the real one. N89
    # demonstrated the mechanism accidentally.
    for f in src/runtime/ext/plugin_loader.cpp; do
        if ! grep -q 'LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR' "$f"; then
            echo "verify: FAIL — N75 restricted search flags missing from $f;" >&2
            echo "dependencies would resolve from the application directory first." >&2
            exit 1
        fi
    done
    # Doc paths: every src/, sdk/, extern/, docs/, tools/, tests/, cmake/ or gen/ path claimed
    # in a root document shall resolve. Four phantom paths (src/protobufnevr,
    # extern/nevr-proto, src/server, gamepatches/patches.cpp) survived for months
    # in README.md and CLAUDE.md because nothing checked. A fact about the tree
    # asserted in prose drifts silently; this is the cheapest possible enforcement.
    python3 tools/verify_doc_paths.py
    # AGENTS.md imports the standards it makes part of its contract (#1277): an import
    # line that is lost, fenced or indented is no longer expanded; one moved below the
    # first section is no longer the first thing the contract says.
    python3 tools/verify_agents_imports.py
    # N92/N105: exactly one ws_bridge. Two divergent copies would let a fix land in
    # the one that does not run (N61's matchmaker fix did). The bridge is
    # in-process (StopWebSocketBridgeListener); this guard keeps a separate
    # module build from coming back.
    N92A_RC=0; grep -qE '^\s*add_subdirectory\(src/modules/ws-bridge\)' CMakeLists.txt || N92A_RC=$?
    sensor_stage1 "N92 ws-bridge module build" "CMakeLists.txt" "$N92A_RC"
    if [ "$N92A_RC" -eq 0 ]; then
        echo "verify: FAIL — N92 the ws-bridge MODULE is being built again. The bridge is" >&2
        echo "compiled into gamepatches; two copies is the bug (fixes land in the dead one)." >&2
        exit 1
    fi
    N92B_RC=0; grep -qE '(LoadModule|LoadLibrary[AW]?|RegisterStaticModule)\(\s*"ws_bridge' src/runtime/lifecycle/boot.cpp || N92B_RC=$?
    sensor_stage1 "N92 ws_bridge module load" "src/runtime/lifecycle/boot.cpp" "$N92B_RC"
    if [ "$N92B_RC" -eq 0 ]; then
        echo "verify: FAIL — N92 boot.cpp still loads ws_bridge as a required module; with the" >&2
        echo "DLL gone the loader fail-fasts and the server never starts." >&2
        exit 1
    fi
    if ! grep -q 'InstallWebSocketBridge();' src/runtime/lifecycle/boot.cpp; then
        echo "verify: FAIL — N92 the in-process bridge is never started; no proxy, no login." >&2
        exit 1
    fi
    # docs/standards/logging.md Rule 2: the login path SHALL log the full XPID at
    # Info. This rule existed in prose and went unenforced for months — N91 shipped
    # with the identity line at Debug, invisible in production, which is how a
    # duplicate LoginRequest and an account-id-0 login both went unnoticed. The
    # rule was right; nothing checked it.
    if ! awk '/login injected/{found=1} found && /Log\(EchoVR::LogLevel::Info/{ok=1} END{exit !ok}' \
         src/runtime/compat/ws_bridge.cpp; then
        N91_CONTEXT=$(grep -B3 'login injected' src/runtime/compat/ws_bridge.cpp || true)
        if ! grep -q 'LogLevel::Info' <<<"$N91_CONTEXT"; then
            echo "verify: FAIL — logging.md Rule 2: the login-injection line is not at Info." >&2
            echo "Identity at login shall be visible in a production log (N91)." >&2
            exit 1
        fi
    fi
    if ! grep -q 'xpid=%s' src/runtime/compat/ws_bridge.cpp; then
        echo "verify: FAIL — logging.md Rule 2: login injection does not log the XPID." >&2
        exit 1
    fi
    # N91 / #520: a game socket's role comes from the request target it asked for, not from the
    # order it connected in. The Open handler takes its slot through AdmitGameSocket with the
    # upgrade request's target (WebSocketOpenInfo::uri) and the listener; the connect-order counter
    # that once decided the roles is gone. The decision itself is tested in test_socket_role and,
    # through the real bridge, in test_behavioral (RolesByPathTest).
    I520_RC=0; I520_WS=$(grep -vE '^[[:space:]]*(//|/\*|\*[[:space:]/]|\*$)' src/runtime/compat/ws_bridge.cpp) || I520_RC=$?
    sensor_stage1 "#520 roles by request target" "src/runtime/compat/ws_bridge.cpp" "$I520_RC"
    sensor_nonempty "#520 roles by request target" "non-comment lines of compat/ws_bridge.cpp" "$I520_WS"
    if ! grep -qF 'AdmitGameSocket(&gameWs, msg->openInfo.uri, listener)' <<<"$I520_WS"; then
        echo "verify: FAIL — #520 the Open handler no longer admits a game socket by its request target" >&2
        echo "(AdmitGameSocket(&gameWs, msg->openInfo.uri, listener)): the roles would follow the connect order again." >&2
        exit 1
    fi
    if grep -qF 'g_connectionCount' <<<"$I520_WS"; then
        echo "verify: FAIL — #520 ws_bridge.cpp counts connections again (g_connectionCount): a login socket that connects before config would be labelled config." >&2
        exit 1
    fi
    # logging.md: no INFO-level logging inside a per-frame hot path (~125Hz).
    python3 tools/verify_log_rules.py
    # D1/N78: exactly ONE strong definition of ::Log. A second definition (e.g. in
    # gameserver.cpp) would make which one every call in the DLL binds to link-order
    # dependent — and if the one without the WriteLog null-check won, the
    # early-boot stderr fallback would silently not exist.
    # .cpp only (a header carries the declaration), and tests are exempt — they
    # deliberately stub Log to run without the game.
    LOG_DEFS=$(grep -rlE '^(VOID|void) Log\(EchoVR::LogLevel' \
                 --include='*.cpp' src/abi src/core src/runtime src/modules 2>/dev/null \
               | grep -v '/tests/' | wc -l)
    if [ "$LOG_DEFS" -ne 1 ]; then
        echo "verify: FAIL — D1 found $LOG_DEFS definitions of ::Log (want exactly 1)." >&2
        grep -rlE '^(VOID|void) Log\(EchoVR::LogLevel' --include='*.cpp' \
             src/abi src/core src/runtime src/modules | grep -v '/tests/' >&2
        echo "Two strong definitions is an ODR violation; the winner is link-order dependent." >&2
        exit 1
    fi
    # Nakama RPC responses are wrapped as {"payload":"<json>"} unless &unwrap is
    # passed. Commit 7a03d8b fixed this in the gamepatches copy on the same day
    # the module was extracted, and the fix never crossed — device auth would
    # silently never complete. Recovered 2026-07-27.
    for u in request poll; do
        if ! grep -q "device/auth/$u?http_key=\" + m_httpKey + \"&unwrap" \
             src/modules/token-auth/src/token_auth.cpp; then
            echo "verify: FAIL — token-auth device/auth/$u endpoint is missing &unwrap;" >&2
            echo "Nakama will wrap the response and the token parse will find nothing." >&2
            exit 1
        fi
    done
    # Token expiry shall come from the JWT, not a hardcoded value (owner
    # decision 2026-07-27). The access token is never persisted, so the old
    # unconditional 60s cap defended a threat this design does not have while
    # discarding tokens the server had issued for far longer.
    TEXP_RC=0; grep -qE 'm_tokenExpiry = static_cast<uint64_t>\(time\(nullptr\)\) \+ [0-9]+;' \
         src/modules/token-auth/src/token_auth.cpp || TEXP_RC=$?
    sensor_stage1 "token-auth hardcoded expiry" "src/modules/token-auth/src/token_auth.cpp" "$TEXP_RC"
    if [ "$TEXP_RC" -eq 0 ]; then
        echo "verify: FAIL — token-auth sets m_tokenExpiry from a hardcoded offset;" >&2
        echo "the JWT's own exp claim is the authority (GetJwtExpiry)." >&2
        exit 1
    fi
    if ! grep -q 'GetJwtExpiry()' src/modules/token-auth/src/token_auth.cpp; then
        echo "verify: FAIL — token-auth does not consult the JWT exp claim." >&2
        exit 1
    fi
    # N94: eight [NEVR.AUTH] per-step detail lines are pinned at Debug (N47
    # taxonomy, cfe96de). Merge 2f29312 reverted all nine to Info at once by
    # taking a stale branch copy of this file wholesale; a revert-by-merge shows
    # no diff against either parent, so only a content pin can catch a
    # recurrence. If a pinned message is reworded, update this list in the same
    # change — the nonempty guard makes a vanished message fail closed.
    N94_FILE=src/modules/token-auth/src/token_auth.cpp
    for msg in \
        'Loaded cached token (expires in' \
        'Access token expired, attempting refresh' \
        'Both tokens expired -- will re-authenticate' \
        'Cached token expired, no refresh token' \
        'token saved to .credentials.json' \
        'Token expires in %llus' \
        'Token expired %llus ago'; do
        N94_RC=0; N94_CTX=$(grep -B1 -F "$msg" "$N94_FILE") || N94_RC=$?
        sensor_stage1 "N94 auth log taxonomy" "$N94_FILE" "$N94_RC"
        sensor_nonempty "N94 auth log taxonomy" "message '$msg' in $N94_FILE" "$N94_CTX"
        if grep -q 'LogLevel::Info' <<<"$N94_CTX"; then
            echo "verify: FAIL — N94 the '[NEVR.AUTH] ${msg}' line is at Info; the N47 taxonomy pins it at Debug (merge 2f29312 once reverted all nine — N94)." >&2
            exit 1
        fi
    done
    # The poll-loop line lives in the platform-neutral device-code loop
    # (src/core/device_auth_flow.cpp, shared with the Quest shim); the same Debug pin
    # applies there, with the core's own level enum.
    N94_CORE_FILE=src/core/device_auth_flow.cpp
    N94_RC=0; N94_CTX=$(grep -B1 -F 'Still waiting for authorization' "$N94_CORE_FILE") || N94_RC=$?
    sensor_stage1 "N94 auth log taxonomy" "$N94_CORE_FILE" "$N94_RC"
    sensor_nonempty "N94 auth log taxonomy" "message 'Still waiting for authorization' in $N94_CORE_FILE" "$N94_CTX"
    if grep -q 'LogLevel::Info' <<<"$N94_CTX"; then
        echo "verify: FAIL — N94 the '[NEVR.AUTH] Still waiting for authorization' line is at Info; the N47 taxonomy pins it at Debug." >&2
        exit 1
    fi
    # The core logs through its own level enum; the Windows side maps it to EchoVR::LogLevel
    # in ToEchoLogLevel. Without a pin on that mapping, a swapped case would move the N94
    # Debug lines to Info while every per-line check above still passes.
    N94_MAP_FILE=src/core/auth_token_refresh.h
    for pair in 'Debug' 'Info' 'Warning' 'Error'; do
        N94_RC=0; N94_MAP=$(grep -F "case LogLevel::${pair}: return EchoVR::LogLevel::${pair};" "$N94_MAP_FILE") || N94_RC=$?
        sensor_stage1 "N94 level mapping" "$N94_MAP_FILE" "$N94_RC"
        sensor_nonempty "N94 level mapping" "ToEchoLogLevel case ${pair} in $N94_MAP_FILE" "$N94_MAP"
    done
    # Quest token auth brings OpenSSL and libcurl. The game's own libraries export about 2411 OpenSSL/curl
    # symbols (an older OpenSSL) and have the sentinel as DT_NEEDED, so a sentinel that carried and
    # exported its own copy could bind to theirs or they to ours. The tool walks the Quest CMake link
    # graph from the sentinel; if token auth, libcurl or OpenSSL is reachable it requires
    # -Wl,--exclude-libs,ALL on the sentinel and a live TestExportAllowlist. The built artifact's real
    # link line is checked by TestSentinelLinkLineGuard in `just test-android`.
    python3 tools/verify_quest_sentinel_link.py
    # C2/N84: every PatchDetour shall name its hook. The parameter is required at
    # compile time, so this is belt-and-braces against someone re-adding a default.
    # Match the DECLARATION, not prose. The first version of this check matched
    # its own explanatory comment — which quotes the very string it forbids — so
    # it failed on a correct tree. Comment lines are stripped; the pattern is
    # anchored to the PatchDetour signature.
    # --- N99: -server shall apply the game's own headless mask --------------
    # `-headless` is a NATIVE echovr.exe token. Its whole effect in the binary
    # is one instruction (0x140504566, `and dword [rbx+0x1D4], 0xFFFEFEFE`),
    # applied by the game's own arg handler ONLY when that literal token is on
    # the command line. `-server` is NEVR-invented, so nothing applied it for
    # us and bit 0 — the render/window master bit — stayed SET on every
    # -server-only run. Three assertions; any one alone is satisfiable by the
    # bug.
    N99_RC=0; N99_MP=$(grep -vE '^[[:space:]]*(//|/\*|\*[[:space:]/]|\*$)' src/runtime/patch/mode_patches.cpp) || N99_RC=$?
    sensor_stage1 "N99 headless mask" "src/runtime/patch/mode_patches.cpp" "$N99_RC"
    sensor_nonempty "N99 headless mask" "non-comment lines of src/runtime/patch/mode_patches.cpp" "$N99_MP"
    if ! grep -q 'ENGINE_FLAGS_HEADLESS_MASK' <<<"$N99_MP"; then
        echo "verify: FAIL — N99 PatchEnableHeadless no longer applies ENGINE_FLAGS_HEADLESS_MASK." >&2
        echo "-server would leave bit 0 of pGame+0x1D4 SET and the game would open a window." >&2
        exit 1
    fi
    # The mask constant shall keep the value measured in the shipped binary.
    if ! grep -q 'ENGINE_FLAGS_HEADLESS_MASK = 0xFFFEFEFEu' src/runtime/hook/addresses.h; then
        echo "verify: FAIL — N99 ENGINE_FLAGS_HEADLESS_MASK is not 0xFFFEFEFE, the value" >&2
        echo "verified byte-for-byte at 0x140504566 in the shipped echovr.exe." >&2
        exit 1
    fi
    # And boot.cpp shall not tell the operator to remove a flag that is native
    # to the game: acting on that message by removing -headless opens a
    # window on the owner's screen.
    N99_BOOT_RC=0; N99_BOOT=$(grep -vE '^[[:space:]]*(//|/\*|\*[[:space:]/]|\*$)' src/runtime/lifecycle/boot.cpp) || N99_BOOT_RC=$?
    sensor_stage1 "N99 boot flag advice" "src/runtime/lifecycle/boot.cpp" "$N99_BOOT_RC"
    sensor_nonempty "N99 boot flag advice" "non-comment lines of src/runtime/lifecycle/boot.cpp" "$N99_BOOT"
    if grep -qE 'is redundant|Remove this flag' <<<"$N99_BOOT"; then
        echo "verify: FAIL — N99 boot.cpp tells the operator a flag is redundant / to remove it." >&2
        echo "-headless and -noovr are NATIVE echovr.exe flags the game itself consumes." >&2
        exit 1
    fi
    # N96: no symbol spelled plain `ResolveVA`. Two definitions of that name once
    # coexisted in BugSplat64.dll with IDENTICAL signatures and different
    # guarantees — binary_bug_fixes.cpp's file-static did no validation,
    # nevr_common.h's did — so adding an #include or moving a line between the
    # two files would have flipped validation with no compiler diagnostic. The
    # spelling has to carry the guarantee. Comment lines are stripped so the
    # prose above cannot satisfy its own check; `_Checked`/`_Unchecked` are
    # excluded by the negative lookahead (grep -P, not -E: `-E` silently
    # overrides `-P` when both are passed and \s is not ERE syntax, which
    # produces a sensor that matches nothing and looks like it passes).
    N96_RC=0; N96_HITS=$(grep -rn --include='*.cpp' --include='*.h' -P 'ResolveVA(?!_Checked|_Unchecked)' src sdk) || N96_RC=$?
    sensor_stage1 "N96 bare ResolveVA" "src sdk" "$N96_RC"
    if [ "$N96_RC" -eq 0 ] && printf '%s\n' "$N96_HITS" \
         | grep -v legacy | grep -vE ':[[:space:]]*(//|/\*|\*[[:space:]/]|\*$)' | grep .; then
        echo "verify: FAIL — N96 a bare 'ResolveVA' spelling is back. Use" >&2
        echo "nevr::ResolveVA_Checked or nevr::ResolveVA_Unchecked — the name shall state" >&2
        echo "whether the address was validated (N96)." >&2
        exit 1
    fi
    # N97: exactly ONE definition of ValidatePrologue. Two file-local copies
    # existed; pnsrad_enabler.cpp's was a bare memcmp, so a zero length returned
    # TRUE (memcmp of 0 bytes is defined to return 0 — measured) and a null
    # address was undefined behaviour. That copy answered "the prologue matched"
    # for a comparison of nothing, to a function whose next statement writes
    # NOPs into the target, directly under the invariant that a binary patch
    # validates before it writes. Same -P (not -PE) reasoning as N96.
    N97_RC=0; N97_HITS=$(grep -rn --include='*.cpp' --include='*.h' -P '^\s*(static\s+)?(inline\s+)?bool\s+ValidatePrologue\s*\(' src sdk) || N97_RC=$?
    sensor_stage1 "N97 ValidatePrologue definitions" "src sdk" "$N97_RC"
    N97_DEFS=$(printf '%s\n' "$N97_HITS" | grep -c 'ValidatePrologue' || true)
    if [ "$N97_RC" -ne 0 ] || [ "$N97_DEFS" -ne 1 ]; then
        echo "verify: FAIL — N97 found $N97_DEFS definitions of ValidatePrologue (want exactly 1," >&2
        echo "nevr::ValidatePrologue in sdk/include/nevr/pc/nevr_common.h). A second copy is how" >&2
        echo "a bare-memcmp version returned TRUE for a zero-length check (N97)." >&2
        printf '%s\n' "$N97_HITS" >&2
        exit 1
    fi
    C2_RC=0; C2_CODE=$(grep -vE '^[[:space:]]*(//|/\*|\*[[:space:]/]|\*$)' src/runtime/hook/patching.h) || C2_RC=$?
    sensor_stage1 "C2 PatchDetour default name" "src/runtime/hook/patching.h" "$C2_RC"
    if grep -qE 'PatchDetour\(.*const char\* name\s*=' <<<"$C2_CODE"; then
        echo "verify: FAIL — C2 PatchDetour's name parameter has a default again." >&2
        echo "22 of 24 sites omitted it when it was defaultable, so HookGuard's overwrite" >&2
        echo "alarm printed name=(unnamed) for almost every address it guards." >&2
        exit 1
    fi
    # N133 S7 — config cutover invariant sensors. The cutover (S0-S5) migrated
    # all 29 NEVR config keys off the game-JSON path onto config.yaml. These
    # sensors prove the cutover IS complete and catch regressions (a reverted
    # key, a flat-map entry added without a test, a stale sample config).
    #
    # S7a — cutover integrity: no nevr_* key literal in a game-JSON read.
    # The only surviving JsonValueAsString(g_earlyConfigPtr, ...) calls are the
    # wildcard override forward (keyName variable, not a literal) and the
    # trampoline (also a variable). A literal "nevr_*" here means a key was
    # reverted or a new one was read from game JSON instead of config.yaml.
    N133_S7A_RC=0
    N133_S7A=$(grep -rn 'JsonValueAsString\|CJsonGetFloat' src/runtime/ src/modules/ \
        2>/dev/null | grep -v '/test_\|/legacy/' | grep '"nevr_') || N133_S7A_RC=$?
    sensor_stage1 "N133 S7a cutover" "src/runtime/ + src/modules/" "$N133_S7A_RC"
    if [ -n "$N133_S7A" ]; then
        echo "verify: FAIL — N133 S7a: a nevr_* key is read from the game JSON:" >&2
        printf '%s\n' "$N133_S7A" >&2
        echo "The config cutover migrated ALL nevr_* keys to config.yaml via the" >&2
        echo "flat map in service_map.cpp. This key was added or reverted to the" >&2
        echo "game-JSON path — add it to the flat map and read through NevrCfgGetFlat." >&2
        exit 1
    fi
    # S7b — flat-map structural gate: exactly 36 entries. Adding or removing a
    # flat-map entry without updating this number fails the build — a deliberate
    # reminder to also add a test for the new mapping. The 29 entries measured in
    # S0 and migrated through S3-S5b, plus nevr_allow_offline_server (#16) and
    # nevr_empty_server_ttl_s (#58) and
    # nevr_tick_rate_hz (#595), nevr_session_router (#708), nevr_invite_codes (#1156) and
    # nevr_bots_per_session (#1423).
    # nevr_max_team_size (#1422).
    N133_S7B=$(grep -cE '^\s*\{"' src/runtime/lifecycle/service_map.cpp)
    if [ "$N133_S7B" -ne 36 ]; then
        echo "verify: FAIL — N133 S7b: flat map has $N133_S7B entries, expected 36." >&2
        echo "A key was added or removed from the cutover map in service_map.cpp." >&2
        echo "If adding: also add a test to test_service_map.cpp and update this count." >&2
        echo "If removing: that key's reader must be deleted first, or it silently" >&2
        echo "falls back to the game-JSON path (exactly what the cutover prevents)." >&2
        exit 1
    fi
    # S7c — the module context config_get accessor (S5 deliverable) must remain
    # present in module_interface.h. Its removal would silently break token_auth's
    # config.yaml reads and revert modules to the game-JSON path.
    if ! grep -q 'config_get' src/extension/module_interface.h; then
        echo "verify: FAIL — N133 S7c: NvrModuleContext.config_get removed from" >&2
        echo "module_interface.h. This is the S5 deliverable that lets modules read" >&2
        echo "config.yaml. Without it token_auth reverts to game-JSON reads." >&2
        exit 1
    fi
    # S7d — the tracked sample config must remain present and contain the minimum
    # keys a server needs to boot (auth, services, identity, version sections).
    # Check the canonical fixture, not echovr/_local/config.yaml: that ignored
    # machine-local destination is created by setup and absent in fresh worktrees.
    # KNOWN-WRONG #1305: this counts headings and never parses the sample, which the parser rejects.
    N133_S7D_RC=0
    N133_S7D=$(grep -cE '^\s*(auth:|services:|identity:|version:)' \
        docs/reference/example-config.yaml) || N133_S7D_RC=$?
    sensor_stage1 "N133 S7d sample config" "docs/reference/example-config.yaml" "$N133_S7D_RC"
    if [ "$N133_S7D" -lt 4 ]; then
        echo "verify: FAIL — N133 S7d: sample config.yaml missing required sections" >&2
        echo "(found $N133_S7D of 4: auth, services, identity, version)." >&2
        echo "The tracked example is the source copied into a server's local config." >&2
        exit 1
    fi
    # #573 — the Quest reads config.yaml through the PC's nevr_config, so the shared parser and its Quest wiring are
    # part of the source. The behaviour is proved by quest_config_test (test-quest-shared) and, on the device
    # image, by check-android-static-init and tests/quest; this sensor is the tripwire for the shapes they rely on:
    #   (a) src/core/nevr_config.cpp is built into the Quest sentinel, which has no PC logger, no windows.h and no
    #       thread-local state (TestNoEmulatedTLSInHookPath): no core/logging.h, no <windows.h>, no Log( or
    #       FatalError( call, no thread_local. The PC-only half is src/core/nevr_config_policy.cpp.
    #   (b) src/quest/sentinel/quest_config.cpp parses through nevr_config (LoadFromString) and holds no JSON parser.
    #   (c) src/quest/CMakeLists.txt compiles ../core/nevr_config.cpp into nevr_quest_config and never the policy file.
    #   (d) yaml-cpp reaches the Quest only through the overlay port whose patch removes its static initializers
    #       (the sentinel allows one load-time initializer: tools/check_quest_static_init.sh).
    Q573A_RC=0; Q573A_CODE=$(grep -vE '^[[:space:]]*(//|/\*|\*[[:space:]/]|\*$)' src/core/nevr_config.cpp) || Q573A_RC=$?
    sensor_stage1 "#573 shared parser" "src/core/nevr_config.cpp" "$Q573A_RC"
    sensor_nonempty "#573 shared parser" "non-comment lines of src/core/nevr_config.cpp" "$Q573A_CODE"
    if grep -qE 'core/logging\.h|<windows\.h>|\bthread_local\b|\bLog\(|\bFatalError\(' <<<"$Q573A_CODE"; then
        echo "verify: FAIL — #573: src/core/nevr_config.cpp includes the PC logger or windows.h, calls Log/FatalError, or declares a" >&2
        echo "thread_local. It is built into the Quest sentinel, which has none of them. Put PC-only behaviour in src/core/nevr_config_policy.cpp." >&2
        exit 1
    fi
    Q573B_RC=0; Q573B_CODE=$(grep -vE '^[[:space:]]*(//|/\*|\*[[:space:]/]|\*$)' src/quest/sentinel/quest_config.cpp) || Q573B_RC=$?
    sensor_stage1 "#573 quest config" "src/quest/sentinel/quest_config.cpp" "$Q573B_RC"
    sensor_nonempty "#573 quest config" "non-comment lines of src/quest/sentinel/quest_config.cpp" "$Q573B_CODE"
    if ! grep -qF 'NevrConfig::LoadFromString' <<<"$Q573B_CODE" || grep -qE 'json::parse' <<<"$Q573B_CODE"; then
        echo "verify: FAIL — #573: src/quest/sentinel/quest_config.cpp must parse config.yaml through nevr::NevrConfig::LoadFromString and hold no JSON parser." >&2
        exit 1
    fi
    Q573C_RC=0; Q573C_CODE=$(grep -vE '^[[:space:]]*#' src/quest/CMakeLists.txt) || Q573C_RC=$?
    sensor_stage1 "#573 quest cmake" "src/quest/CMakeLists.txt" "$Q573C_RC"
    sensor_nonempty "#573 quest cmake" "non-comment lines of src/quest/CMakeLists.txt" "$Q573C_CODE"
    if ! grep -qF '../core/nevr_config.cpp' <<<"$Q573C_CODE" || grep -qF 'nevr_config_policy' <<<"$Q573C_CODE"; then
        echo "verify: FAIL — #573: nevr_quest_config must compile ../core/nevr_config.cpp and must not compile nevr_config_policy.cpp (src/quest/CMakeLists.txt)." >&2
        exit 1
    fi
    if ! grep -qF 'yaml-cpp-no-static-init.patch' src/quest/vcpkg-overlay-ports/yaml-cpp/portfile.cmake \
        || [ ! -s src/quest/vcpkg-overlay-ports/yaml-cpp/yaml-cpp-no-static-init.patch ] \
        || ! grep -qF 'vcpkg-overlay-ports' src/quest/vcpkg-configuration.json; then
        echo "verify: FAIL — #573: the Quest build must take yaml-cpp from src/quest/vcpkg-overlay-ports (portfile lists yaml-cpp-no-static-init.patch," >&2
        echo "the patch exists, src/quest/vcpkg-configuration.json names the overlay). Stock yaml-cpp adds six load-time initializers to the sentinel." >&2
        exit 1
    fi
    # (e) the parser no longer logs the unset-variable warning, so the PC caller must (service_config.cpp's NevrCfg is
    #     impure and is not linked by a unit test; LogUnsetBareVars itself is tested in test_nevr_config).
    Q573E_RC=0; Q573E_CODE=$(grep -vE '^[[:space:]]*(//|/\*|\*[[:space:]/]|\*$)' src/runtime/lifecycle/service_config.cpp) || Q573E_RC=$?
    sensor_stage1 "#573 PC unset-variable warning" "src/runtime/lifecycle/service_config.cpp" "$Q573E_RC"
    sensor_nonempty "#573 PC unset-variable warning" "non-comment lines of src/runtime/lifecycle/service_config.cpp" "$Q573E_CODE"
    if ! grep -qE 'nevr::LogUnsetBareVars\(c\)' <<<"$Q573E_CODE"; then
        echo "verify: FAIL — #573: NevrCfg() in src/runtime/lifecycle/service_config.cpp no longer calls nevr::LogUnsetBareVars(c) after the load;" >&2
        echo "the parser does not log the unset-variable warning any more, so without that call the PC never says a \${VAR} was unset." >&2
        exit 1
    fi
    # Issue #21 — _local/config.json is OPTIONAL. N48's ServerFatal on a missing
    # config.json outlived N133 (which moved every NEVR key to config.yaml) and
    # killed config.yaml-only servers at boot; the `g_earlyConfigPtr == NULL`
    # gate in RedirectServiceUrl silently disabled every service redirect without
    # one. Neither may return, and the replacement arm call — which keeps the
    # first config.yaml load from moving ahead of the bootstrap — must stay.
    I21_RC=0; I21_CODE=$(grep -hvE '^[[:space:]]*(//|/\*|\*[[:space:]/]|\*$)' \
        src/runtime/lifecycle/boot.cpp src/runtime/lifecycle/config.cpp) || I21_RC=$?
    sensor_stage1 "I21 config.json optional" "src/runtime/lifecycle/{boot,config}.cpp" "$I21_RC"
    sensor_nonempty "I21 config.json optional" "non-comment lines of boot.cpp + config.cpp" "$I21_CODE"
    if grep -qE 'g_earlyConfigPtr[[:space:]]*==[[:space:]]*(NULL|nullptr)' <<<"$I21_CODE"; then
        echo "verify: FAIL — issue #21: boot.cpp/config.cpp branch on a MISSING config.json again:" >&2
        grep -nE 'g_earlyConfigPtr[[:space:]]*==[[:space:]]*(NULL|nullptr)' \
            src/runtime/lifecycle/boot.cpp src/runtime/lifecycle/config.cpp >&2
        echo "config.json only carries keys the stock engine reads natively; NEVR's settings" >&2
        echo "are config.yaml. Its absence must not fatal a server or disable redirects." >&2
        exit 1
    fi
    if ! grep -qE '^[[:space:]]*ArmServiceRedirects\(\);' <<<"$I21_CODE"; then
        echo "verify: FAIL — issue #21: ArmServiceRedirects() is no longer called from the" >&2
        echo "bootstrap. Without it RedirectServiceUrl never fires (no login redirect)." >&2
        exit 1
    fi
    # Issue #21 — _local/config.json is OPTIONAL. N48's ServerFatal on a missing
    # config.json outlived N133 (which moved every NEVR key to config.yaml) and
    # killed config.yaml-only servers at boot; the `g_earlyConfigPtr == NULL`
    # gate in RedirectServiceUrl silently disabled every service redirect without
    # one. Neither may return, and the replacement arm call — which keeps the
    # first config.yaml load from moving ahead of the bootstrap — must stay.
    I21_RC=0; I21_CODE=$(grep -hvE '^[[:space:]]*(//|/\*|\*[[:space:]/]|\*$)' \
        src/runtime/lifecycle/boot.cpp src/runtime/lifecycle/config.cpp) || I21_RC=$?
    sensor_stage1 "I21 config.json optional" "src/runtime/lifecycle/{boot,config}.cpp" "$I21_RC"
    sensor_nonempty "I21 config.json optional" "non-comment lines of boot.cpp + config.cpp" "$I21_CODE"
    if grep -qE 'g_earlyConfigPtr[[:space:]]*==[[:space:]]*(NULL|nullptr)' <<<"$I21_CODE"; then
        echo "verify: FAIL — issue #21: boot.cpp/config.cpp branch on a MISSING config.json again:" >&2
        grep -nE 'g_earlyConfigPtr[[:space:]]*==[[:space:]]*(NULL|nullptr)' \
            src/runtime/lifecycle/boot.cpp src/runtime/lifecycle/config.cpp >&2
        echo "config.json only carries keys the stock engine reads natively; NEVR's settings" >&2
        echo "are config.yaml. Its absence must not fatal a server or disable redirects." >&2
        exit 1
    fi
    if ! grep -qE '^[[:space:]]*ArmServiceRedirects\(\);' <<<"$I21_CODE"; then
        echo "verify: FAIL — issue #21: ArmServiceRedirects() is no longer called from the" >&2
        echo "bootstrap. Without it RedirectServiceUrl never fires (no login redirect)." >&2
        exit 1
    fi
    # N112 — BuildIdentity: the client login and server registration now carry
    # NEVR build identity (version, commit, git describe, build type) and a
    # plugin manifest. These sensors prove the wiring is present and catch the
    # literals ("buildversion":631547, bare GIT_DESCRIBE) being restored by a
    # merge or refactor.
    #
    # N112a — BuildIdentity struct exists and is compiled.
    if [ ! -f src/core/build_identity.h ]; then
        echo "verify: FAIL — N112a: src/core/build_identity.h is missing." >&2
        echo "The BuildIdentity struct carries compile-time version identity into" >&2
        echo "the login payload and server registration." >&2
        exit 1
    fi
    if [ ! -f src/core/build_identity.cpp ]; then
        echo "verify: FAIL — N112a: src/core/build_identity.cpp is missing." >&2
        exit 1
    fi
    # N112b — the shared client login profile carries nevr_identity and nevr_plugins.
    N112_PROFILE_RC=0; N112_PROFILE=$(grep -vE '^[[:space:]]*(//|/\*|\*[[:space:]/]|\*$)' src/runtime/compat/login_profile.cpp) || N112_PROFILE_RC=$?
    sensor_stage1 "N112 client login identity and plugins" "src/runtime/compat/login_profile.cpp" "$N112_PROFILE_RC"
    sensor_nonempty "N112 client login identity and plugins" "non-comment lines of compat/login_profile.cpp" "$N112_PROFILE"
    if ! grep -qF 'profile["nevr_identity"]' <<<"$N112_PROFILE"; then
        echo "verify: FAIL — N112b: shared login profile does not carry" >&2
        echo "nevr_identity. The client login must send NEVR version info (N112)." >&2
        exit 1
    fi
    if ! grep -qF 'profile["nevr_plugins"]' <<<"$N112_PROFILE"; then
        echo "verify: FAIL — N112b: shared login profile does not carry" >&2
        echo "nevr_plugins. The client login must send a plugin manifest (N112)." >&2
        exit 1
    fi
    # N112c — server registration uses BuildIdentity (not bare GIT_DESCRIBE).
    if ! grep -q 'nevr_build_identity::Get()' src/runtime/server/gameserver.cpp src/runtime/server/gameserver_serverdb.cpp; then
        echo "verify: FAIL — N112c: gameserver.cpp does not call nevr_build_identity::Get()." >&2
        echo "The server registration version field must be enriched with commit" >&2
        echo "hash and build type, not just bare GIT_DESCRIBE (N112)." >&2
        exit 1
    fi
    # N112d — BuildPluginManifestJson exists.
    if ! grep -q 'BuildPluginManifestJson' src/runtime/ext/plugin_loader.cpp; then
        echo "verify: FAIL — N112d: BuildPluginManifestJson() not found in" >&2
        echo "plugin_loader.cpp. The plugin manifest builder is required (N112)." >&2
        exit 1
    fi
    # Static token_auth exports are registered by the host, not token_auth_Init.
    # Keep all three registrations pinned to boot so ws_bridge can resolve its ABI.
    for export in TokenAuth_GetToken TokenAuth_GetDiscordId TokenAuth_GetUsername; do
        if ! grep -q "RegisterModuleProc(\"$export\"" src/runtime/lifecycle/boot.cpp; then
            echo "verify: FAIL — missing static token_auth export registration: $export" >&2
            exit 1
        fi
    done
    # N112e — the hardcoded buildversion literal is still the game's 631547
    # (we send NEVR version info alongside it, so the game field is unchanged),
    # but a second NEVR buildversion field would be a duplicate. The sensor
    # asserts the NEVR identity is in its own nevr_identity sub-object.
    #   (No sensor — the presence of nevr_identity is already checked in N112b.)
    # Wave 10: a deleted unit test must be visible to the closed-loop gate.
    # The floor is the count at the base commit, read from git (#1343); a deliberate drop carries a
    # `GTest-Drop: <reason>` trailer.
    # #451: the self-check wiring goes through the tested gate functions (source check).
    python3 tools/verify_self_check_wiring.py
    python3 tools/verify_gtest_floor.py
    # Wave 10.2: PATCHES_SOURCES is the compiled runtime patch inventory. A
    # patch addition/removal requires a reviewed update to its pinned list.
    python3 tools/verify_patch_source_inventory.py
    # Wave 5/10: every concrete mode_patches.cpp byte rewrite must have a
    # PE-backed assertRVABytes row. Detours/import hooks are deliberately not
    # included: they are not ordinary prologue rewrites.
    python3 tools/verify_mode_patch_ground_truth.py
    echo "verify: OK ({{ preset }})"

# Generate combat override files from echomod build output
generate-combat-overrides build_dir:
    python tools/echomod/generate_resources.py \
        --build-dir {{build_dir}} \
        --output-dir echovr/bin/win10/_overrides/combat

# --- Code Signing ---

# Generate the CA hierarchy (root → intermediate → code-signing)
generate-certs:
    ./certs/generate-ca.sh

# Renew intermediate + code-signing certs (root CA stays in pass)
renew-certs:
    ./certs/generate-ca.sh --renew

# Build the stripped archives with required local Authenticode signing.
sign:
    #!/usr/bin/env bash
    set -euo pipefail
    cert_dir="{{ justfile_directory() }}/certs"
    if [[ ! -f "$cert_dir/code-signing.key" || ! -f "$cert_dir/chain.pem" || ! -f "$cert_dir/root-ca.crt" ]]; then
        echo "ERROR: No signing certs found. Run: just generate-certs" >&2
        exit 1
    fi
    export CODESIGN_CERT="$cert_dir/chain.pem"
    export CODESIGN_KEY="$cert_dir/code-signing.key"
    export CODESIGN_CA_FILE="$cert_dir/root-ca.crt"
    export CODESIGN_SIGNER_SHA256="$(openssl x509 -in "$cert_dir/chain.pem" -noout -fingerprint -sha256 | cut -d= -f2 | tr -d ':')"
    export CODESIGN_ROOT_SHA256="$(openssl x509 -in "$cert_dir/root-ca.crt" -noout -fingerprint -sha256 | cut -d= -f2 | tr -d ':')"
    export CODESIGN_TSURL="http://timestamp.digicert.com"
    cmake --preset {{ preset }} -DNEVR_CODESIGN_REQUIRED=ON
    build_status=0
    cmake --build --preset {{ preset }} --target dist || build_status=$?
    cmake --preset {{ preset }} -DNEVR_CODESIGN_REQUIRED=OFF > /dev/null
    exit "$build_status"

# Verify Authenticode signature on a file
verify-sign file:
    osslsigncode verify -CAfile certs/root-ca.crt -in {{ file }}

# --- Internal ---

# Fail unless the local ~/.vcpkg is on the revision CI builds (.vcpkg-commit). Bumping that file is a
# deliberate dependency upgrade: the vcpkg ports, and so the library versions, change with it.
vcpkg-pin-check:
    #!/usr/bin/env bash
    set -euo pipefail
    want=$(tr -d '[:space:]' < "{{ justfile_directory() }}/.vcpkg-commit")
    have=$(git -C "$HOME/.vcpkg" rev-parse HEAD)
    if [ "$have" != "$want" ]; then
        echo "vcpkg-pin-check: FAIL — ~/.vcpkg is at $have, CI builds $want (.vcpkg-commit)." >&2
        echo "Fix: git -C ~/.vcpkg checkout --detach $want   (or bump .vcpkg-commit on purpose, in its own PR)." >&2
        exit 1
    fi
    # The pinned ixwebsocket port links -lCrypt32; the MinGW package ships libcrypt32.a only and ld on
    # Linux is case-sensitive. CI creates the link; a development machine needs it too (#294).
    lib=${NEVR_MINGW_LIB:-/usr/x86_64-w64-mingw32/lib}
    if [ ! -e "$lib/libCrypt32.a" ]; then
        echo "vcpkg-pin-check: FAIL — $lib/libCrypt32.a is missing; nevr-runtime.dll will not link ixwebsocket (cannot find -lCrypt32)." >&2
        echo "Fix: sudo ln -s libcrypt32.a $lib/libCrypt32.a   (CI does the same, see .github/workflows/build.yml)" >&2
        exit 1
    fi
    echo "vcpkg-pin-check: OK ($have, libCrypt32.a present)"

# Install vcpkg dependencies for MinGW cross-compilation (runs only for mingw presets)
[private]
_vcpkg-mingw:
    #!/usr/bin/env bash
    set -euo pipefail
    if [[ "{{ preset }}" == mingw-* ]]; then
        mkdir -p build/{{ preset }}/vcpkg_installed
        # This checkout's own vcpkg root (tools/vcpkg_root.sh), so concurrent builds in other
        # worktrees do not wait on a shared root's lock; the binary cache is still shared.
        root="$("{{ justfile_directory() }}/tools/vcpkg_root.sh")"
        cd "$root"
        unset VCPKG_ROOT
        # Fixed PATH: the triplet passes PATH through (VCPKG_ENV_PASSTHROUGH), so the caller's PATH
        # would be hashed into every package ABI and miss the shared binary cache.
        env PATH=/usr/local/bin:/usr/bin ./vcpkg install --triplet=x64-mingw-static --host-triplet=x64-linux \
            --x-manifest-root="{{ justfile_directory() }}" \
            --x-install-root="{{ justfile_directory() }}/build/{{ preset }}/vcpkg_installed"
    fi
