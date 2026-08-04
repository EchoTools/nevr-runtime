# NEVR Runtime Build System

# Default preset: mingw-release on Linux, release on Windows
default_preset := if os() == "linux" { "mingw-release" } else { "release" }
preset := env("PRESET", default_preset)

# Show available recipes (default)
default:
    @just --list

# Regenerate the EVR symbol cache C++ source from the nakama Go source.
# Runs automatically as a dependency of configure; the generated file is committed
# to git so a missing evrcat tree does not block the build on other machines.
generate-symcache:
    #!/usr/bin/env bash
    set -euo pipefail
    repo_root="{{ justfile_directory() }}"
    go_file="/home/andrew/src/evrcat/vendor/github.com/heroiclabs/nakama/v3/server/evr/core_hash_lookup.go"
    if [[ -f "$go_file" ]]; then
        bash "$repo_root/tools/generate-symcache.sh"
    else
        echo "generate-symcache: Go source not found at $go_file — skipping (using committed symcache_data.cpp)" >&2
    fi

# Configure CMake
configure: generate-symcache _vcpkg-mingw
    @unset VCPKG_ROOT && cmake --preset {{ preset }} > /dev/null 2>&1 || (unset VCPKG_ROOT && cmake --preset {{ preset }})

# Build all components
build: configure
    @cmake --build --preset {{ preset }} 2>&1 | grep -E '(error|Error|ERROR|fatal|FAILED)' || true

# Build only the echovr_server.exe launcher
launcher: configure
    cmake --build --preset {{ preset }} --target echovr_server

# Build with full compiler output
verbose-build: configure
    cmake --build --preset {{ preset }}

# Create distribution packages
dist: build
    @cmake --build --preset {{ preset }} --target dist 2>&1 | \
        grep -vE '(^\[|^ninja|Creating.*\.(tar\.zst|zip)|Preparing distribution|Running utility|^===)' | \
        grep -E '(error|Error|ERROR|fatal|FAILED)' || true

# Create distribution with full output
verbose-dist: build
    cmake --build --preset {{ preset }} --target dist

# Build stripped binaries without debug symbols
dist-lite: build
    cmake --build --preset {{ preset }} --target dist-lite

# Create legacy distribution package (v1 DLLs only)
dist-legacy: build
    cmake --build --preset {{ preset }} --target dist-legacy

# Regenerate C++ protobuf from BSR (buf.build/echotools/nevr-api).
# Uses vcpkg protoc to match the runtime version. Run `just configure` first.
# PINNED to a specific BSR commit so the generated code is reproducible and
# matches nevr-stream's Go module revision (N132). Bump this when the BSR schema
# changes: `buf registry commit list buf.build/echotools/nevr-api --page-size 1`.
proto:
    PATH="{{ justfile_directory() }}/build/{{ preset }}/vcpkg_installed/x64-linux/tools/protobuf:$PATH" buf generate buf.build/echotools/nevr-api:04fc66c864ef47d2b792c4199e4c0ad3

# Remove build and dist directories
clean:
    rm -rf build/ dist/

# --- Android / Quest (arm64-v8a) ---
# Self-contained NDK sub-project at src/quest (root CMake is MinGW-only).
# NDK: r26d by default; override with ANDROID_NDK_HOME.

ndk := env("ANDROID_NDK_HOME", "/home/andrew/src/android-ndk-r26d")

# Configure the Android arm64-v8a build (NDK r26d, API 26, static libc++)
configure-android:
    cd src/quest && ANDROID_NDK_HOME="{{ ndk }}" cmake --preset android-arm64

# Build the Android arm64-v8a crash-reporter .so
build-android: configure-android
    ANDROID_NDK_HOME="{{ ndk }}" cmake --build build/android-arm64 -j

# Build with full compiler output
verbose-build-android: configure-android
    ANDROID_NDK_HOME="{{ ndk }}" cmake --build build/android-arm64 -v

# Run the Quest .so ground-truth (ELF-shape) tests
test-android: build-android
    cd tests/quest && go test -v ./...

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
# forwarding to the renamed original. Produces a zipaligned, DEBUG-signed APK and
# verifies both libs' ELF shape from the SIGNED output (no headset needed).
# All work under build/android-arm64/repack — never mutates the source APK or shim.
# `apk` is required (the store APK path); the debug keystore is throwaway, NOT prod.
android-repack-apk apk shim="build/android-arm64/sentinel/libovrplatformloader.so" ks="build/android-arm64/repack/debug.keystore":
    #!/usr/bin/env bash
    set -euo pipefail
    root="$(pwd)"
    apk="{{ apk }}"; shim="{{ shim }}"; ks="{{ ks }}"
    work="$root/build/android-arm64/repack"
    for t in unzip patchelf zip zipalign apksigner readelf nm keytool; do
        command -v "$t" >/dev/null || { echo "MISSING required tool: $t" >&2; exit 1; }
    done
    [ -f "$apk" ]  || { echo "APK not found: $apk" >&2; exit 1; }
    [ -f "$shim" ] || { echo "shim not found (build it: just build-android): $shim" >&2; exit 1; }
    mkdir -p "$work/stage/lib/arm64-v8a" "$work/orig"
    # Debug keystore — generated once, self-signed, for sideload only (never prod).
    if [ ! -f "$ks" ]; then
        mkdir -p "$(dirname "$ks")"
        keytool -genkeypair -v -keystore "$ks" -alias nevrdebug -keyalg RSA -keysize 2048 \
            -validity 10000 -storepass android -keypass android \
            -dname "CN=NEVR Debug, OU=nevr-runtime, O=Sprock, C=US"
    fi
    # 1. extract the real original loader from the store APK
    unzip -o -q "$apk" lib/arm64-v8a/libovrplatformloader.so -d "$work/orig"
    # 2. rename original + patch soname so the NEEDED string matches the file on disk
    patchelf --set-soname libovrplatformloader_orig.so \
        --output "$work/stage/lib/arm64-v8a/libovrplatformloader_orig.so" \
        "$work/orig/lib/arm64-v8a/libovrplatformloader.so"
    # 3. drop our shim in under the hijacked name
    cp "$shim" "$work/stage/lib/arm64-v8a/libovrplatformloader.so"
    # 4. fresh APK copy, strip the store's v1 signature, inject both libs STORED (-0)
    unsigned="$work/r15-repacked.apk"
    aligned="$work/r15-repacked-aligned.apk"
    signed="$work/r15_nevr-sentinel_signed.apk"
    cp -f "$apk" "$unsigned"
    zip -q -d "$unsigned" 'META-INF/*.RSA' 'META-INF/*.SF' 'META-INF/*.MF' || true
    ( cd "$work/stage" && zip -q -0 -X "$unsigned" \
        lib/arm64-v8a/libovrplatformloader.so lib/arm64-v8a/libovrplatformloader_orig.so )
    # 5. page-align uncompressed .so, then sign (v2/v3) — align BEFORE sign
    zipalign -p -f 4 "$unsigned" "$aligned"
    apksigner sign --ks "$ks" --ks-key-alias nevrdebug \
        --ks-pass pass:android --key-pass pass:android --out "$signed" "$aligned"
    apksigner verify --print-certs "$signed" | head -1
    # 6. VERIFY from the SIGNED apk — the headless packaging proof
    rm -rf "$work/verify"; mkdir -p "$work/verify"
    unzip -o -q "$signed" 'lib/arm64-v8a/libovrplatformloader*.so' -d "$work/verify"
    echo "== shim  lib/arm64-v8a/libovrplatformloader.so =="
    readelf -d "$work/verify/lib/arm64-v8a/libovrplatformloader.so" | grep -iE 'soname|needed'
    nm -D "$work/verify/lib/arm64-v8a/libovrplatformloader.so" | grep nevr_sentinel_marker
    echo "== renamed original  lib/arm64-v8a/libovrplatformloader_orig.so =="
    readelf -d "$work/verify/lib/arm64-v8a/libovrplatformloader_orig.so" | grep -i soname
    echo ""
    echo "Signed sideload APK ready -> $signed"
    echo "Install with: adb install -r \"$signed\""

# --- Tests ---

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
    cmake --preset {{ preset }} -DBUILD_TESTING=ON > /dev/null 2>&1 \
        || cmake --preset {{ preset }} -DBUILD_TESTING=ON
    cmake --build --preset {{ preset }} --target test_xpid_patch --target test_parse_endpoint --target test_behavioral --target test_token_auth --target test_messages --target test_crash_recovery --target test_nevr_config --target test_service_map --target test_plugin_load_plan
    bin="build/{{ preset }}/bin/test_xpid_patch.exe"
    if [[ ! -f "$bin" ]]; then
        echo "ERROR: GTest binary not found: $bin" >&2
        echo "       (is 'gtest' available in vcpkg for triplet x64-mingw-static?)" >&2
        exit 1
    fi
    wine "$bin"
    bin="build/{{ preset }}/bin/test_token_auth.exe"
    if [[ ! -f "$bin" ]]; then
        echo "ERROR: GTest binary not found: $bin" >&2
        exit 1
    fi
    wine "$bin"
    bin="build/{{ preset }}/bin/test_messages.exe"
    if [[ ! -f "$bin" ]]; then
        echo "ERROR: GTest binary not found: $bin" >&2
        exit 1
    fi
    wine "$bin"
    bin="build/{{ preset }}/bin/test_crash_recovery.exe"
    if [[ ! -f "$bin" ]]; then
        echo "ERROR: GTest binary not found: $bin" >&2
        exit 1
    fi
    wine "$bin"
    bin="build/{{ preset }}/bin/test_parse_endpoint.exe"
    if [[ ! -f "$bin" ]]; then
        echo "ERROR: GTest binary not found: $bin" >&2
        echo "       (is 'gtest' available in vcpkg for triplet x64-mingw-static?)" >&2
        exit 1
    fi
    wine "$bin"
    bin="build/{{ preset }}/bin/test_behavioral.exe"
    if [[ ! -f "$bin" ]]; then
        echo "ERROR: GTest binary not found: $bin" >&2
        echo "       (is 'gtest' available in vcpkg for triplet x64-mingw-static?)" >&2
        exit 1
    fi
    wine "$bin"
    bin="build/{{ preset }}/bin/test_nevr_config.exe"
    if [[ ! -f "$bin" ]]; then
        echo "ERROR: GTest binary not found: $bin" >&2
        echo "       (is 'gtest'/'yaml-cpp' available in vcpkg for triplet x64-mingw-static?)" >&2
        exit 1
    fi
    wine "$bin"
    bin="build/{{ preset }}/bin/test_service_map.exe"
    if [[ ! -f "$bin" ]]; then
        echo "ERROR: GTest binary not found: $bin" >&2
        echo "       (is 'gtest'/'yaml-cpp' available in vcpkg for triplet x64-mingw-static?)" >&2
        exit 1
    fi
    wine "$bin"
    bin="build/{{ preset }}/bin/test_plugin_load_plan.exe"
    if [[ ! -f "$bin" ]]; then
        echo "ERROR: GTest binary not found: $bin" >&2
        echo "       (is 'gtest'/'yaml-cpp' available in vcpkg for triplet x64-mingw-static?)" >&2
        exit 1
    fi
    wine "$bin"
    # test_broadcaster_bridge / test_broadcaster_guards moved to
    # ~/src/nevr-runtime-plugins with the broadcaster-bridge plugin (2026-07-26).
    # The N72/N73 guards they cover now live and are tested there.

# Run auth integration tests (needs game binary + MCP harness)
test-auth-integration:
    cd tests/system && go test -v -run "TestAuth" ./...

# Run all auth tests
test-auth: test-auth-groundtruth test-auth-unit

# --- Verify (closed-loop gate) ---

# Aggregate verify gate for the all-the-way-down canon: build everything, then run
# the hardened C++ GTest suite under Wine. Fail-close: exits nonzero on any failure.
# Success derives from the real compiler/linker + test artifacts, not a proxy
# [day-one-kit §5]. EXCLUDES the Go integration suites — the evr-test-harness
# dependency is excised [RULINGS.md 2026-07-20 "Test harness excised"].
verify:
    #!/usr/bin/env bash
    set -euo pipefail
    unset VCPKG_ROOT
    just build
    # `just build` filters its output through grep and always exits 0, so its exit
    # code is a proxy, not a pass/fail signal. Re-run the real build to derive success
    # from the compiler/linker itself — a no-op when green, nonzero when truly broken.
    cmake --build --preset {{ preset }}
    just test-auth-unit
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
    # N34/N103: src/gameserver/ was DELETED on 2026-07-28 after its one piece of
    # stranded work (N48's fail-fast) was recovered as N102. This guard remains
    # so the tree cannot be recreated and wired: a second gameserver copy is how
    # N48 shipped half-implemented for five weeks. CMake would now hard-fail on a
    # missing directory, but this names the reason instead of the symptom.
    if grep -Pn '^\s*add_subdirectory\s*\(\s*src/gameserver\s*\)' CMakeLists.txt; then
        echo "verify: FAIL — src/gameserver/ was removed (N103). The compiled path is src/runtime/server/." >&2
        echo "Re-adding that tree recreates the two-copy split that let N48 ship half-implemented. Route the change to src/runtime/server/." >&2
        exit 1
    fi
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
    if ! grep -q 'g_loginRemoteWs->setOnMessageCallback' src/runtime/compat/ws_bridge.cpp; then
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
    # password auth. Nothing else refreshes in server mode: TokenAuth::Init
    # returns early on is_server, before the background refresh thread starts.
    # This exchange is the ONLY place a dedicated server can mint an access token.
    N106_RC=0; N106_GS=$(grep -vE '^[[:space:]]*(//|/\*|\*[[:space:]/]|\*$)' src/runtime/server/gameserver.cpp) || N106_RC=$?
    sensor_stage1 "N106 OAuth2 refresh reachable" "src/runtime/server/gameserver.cpp" "$N106_RC"
    sensor_nonempty "N106 OAuth2 refresh reachable" "non-comment lines of gameserver.cpp" "$N106_GS"
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
    # The old sensor matched the STRING 'WsBridge_Shutdown', which a
    # GetProcAddress returning null satisfies perfectly — and did, on every run
    # from the N92 fold until 2026-07-28. Assert the DIRECT call instead: a
    # symbol the linker must resolve, not a name looked up at runtime.
    if ! grep -q 'StopWebSocketBridgeListener()' src/runtime/server/gameserver.cpp; then
        echo "verify: FAIL — N64/N105 BeginGracefulShutdown does not call StopWebSocketBridgeListener();" >&2
        echo "the ws bridge listening socket leaks as a wineserver zombie (N37: no SO_REUSEADDR)." >&2
        exit 1
    fi
    # The graceful signal path must release it too, and must not reintroduce a
    # runtime symbol lookup on a path that cannot take the loader lock (N62).
    if ! grep -q 'StopWebSocketBridgeListener()' src/runtime/lifecycle/crash_recovery.cpp; then
        echo "verify: FAIL — N105 PerformGracefulShutdown does not call StopWebSocketBridgeListener()." >&2
        exit 1
    fi
    # Scoped to ResolveShutdownDependencies, NOT the whole file: crash_recovery.cpp
    # legitimately resolves the four kernel32 hooks (CreateProcessA/W, ExitProcess,
    # TerminateProcess) at INIT, where the loader lock is fine. The constraint is
    # that the shutdown-dependency resolver no longer performs a lookup at all —
    # the bridge is in-process now. PerformGracefulShutdown's own body is covered
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
    N102_RC=0; N102_GS=$(grep -vE '^[[:space:]]*(//|/\*|\*[[:space:]/]|\*$)' src/runtime/server/gameserver.cpp) || N102_RC=$?
    sensor_stage1 "N102 gameserver fail-fast" "src/runtime/server/gameserver.cpp" "$N102_RC"
    sensor_nonempty "N102 gameserver fail-fast" "non-comment lines of src/runtime/server/gameserver.cpp" "$N102_GS"
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
    # N72/N73: the broadcaster ingress guards moved with the plugin to
    # ~/src/nevr-runtime-plugins on 2026-07-26 (this repo is PUBLIC and the
    # plugin is a broadcaster injection tool). Their sensors moved with them —
    # asserting on a path that no longer exists here would fail-closed forever,
    # and asserting nothing would silently drop the guarantee. Recorded in the
    # N72/N73 ledger entries instead, which name the new owning repo.
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
    # the type is on THIS file, the one the linker consumes (the earlier fix landed
    # in plugins/crash-handler/, which CMake does not build).
    if ! grep -q 'std::atomic<bool> g_crashReporterSuppressed' src/runtime/lifecycle/crash_recovery.cpp; then
        echo "verify: FAIL — N67 g_crashReporterSuppressed is not std::atomic<bool> in the SHIPPING path." >&2
        exit 1
    fi
    if ! grep -q 'std::atomic<bool> g_justSuppressedCrash' src/runtime/lifecycle/crash_recovery.cpp; then
        echo "verify: FAIL — N67 g_justSuppressedCrash is not std::atomic<bool> in the SHIPPING path." >&2
        exit 1
    fi
    # N36: Log() is unsafe under the loader lock. Initialize() runs from DllMain, and
    # the Log() fallback to stderr stops applying the moment
    # InitializeFunctionPointers() makes EchoVR::WriteLog non-null. Everything after
    # that point in Initialize() must use BootLogTee::TeeFprintf. Exactly one Log()
    # call is permitted — the final line, emitted after BootLogTee::Close().
    N36_RC=0; N36_BODY=$(awk '/^VOID Initialize\(\)/,/^}/' src/runtime/lifecycle/initialize.cpp) || N36_RC=$?
    sensor_stage1 "N36 Initialize Log census" "src/runtime/lifecycle/initialize.cpp" "$N36_RC"
    sensor_nonempty "N36 Initialize Log census" "Initialize() body in src/runtime/lifecycle/initialize.cpp" "$N36_BODY"
    LOGS_IN_INIT=$(printf '%s\n' "$N36_BODY" | grep -cF 'Log(EchoVR::LogLevel' || true)
    if [ "$LOGS_IN_INIT" -gt 1 ]; then
        echo "verify: FAIL — N36 Initialize() contains $LOGS_IN_INIT Log() calls (max 1, the final line)." >&2
        echo "Initialize() runs under the DllMain loader lock; after InitializeFunctionPointers() the" >&2
        echo "stderr fallback in logging.cpp no longer fires and Log() enters the game logger. Use BootLogTee::TeeFprintf." >&2
        exit 1
    fi
    # --- Observability invariants (N77/N78/N79/N80/N81) ----------------------
    # N77: NEVR's own lines must be exempt from game-noise suppression patterns.
    # Without the guard, a substring rule aimed at echovr noise silently deletes
    # NEVR structured output — it previously erased "Finished initializing engine"
    # (the witness cited in the N7/N8/N10 closes) and masked real ExitProcess reports.
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
    # --- N116: pre-reorg audit records must keep their pre-reorg paths -----------
    # An audit states what was measured on a date, so its file:line citations
    # describe the tree AS IT WAS. Rewriting them to today's layout does not
    # modernise the record — it falsifies it, pairing a current path with a line
    # number from months ago, both halves looking authoritative.
    #
    # This happened: on 2026-07-29 the N108/N109 mechanical path rewriter walked
    # this directory and changed 21 citations across two records. Nothing caught
    # it, because verify_doc_paths.py deliberately does NOT scan docs/audits/ —
    # the exclusion that keeps old paths from failing the build also meant nothing
    # noticed them being rewritten. This sensor closes that specific gap.
    #
    # Scoped by NAME to the records that predate the reorganisation. A future
    # audit written after 2026-07-29 will legitimately cite src/runtime/ and must
    # not be caught by this.
    for _rec in docs/audits/fable-consistency-hunt-2026-07-23.md docs/audits/recon-owner-bug-batch-RESULTS.md; do
        if [ ! -f "$_rec" ]; then
            echo "verify: FAIL — N116 audit record $_rec is missing. Records are immutable; if it was deliberately removed, cite <sha>:<path> in docs/audits/README.md and drop it from this list in the same commit." >&2
            exit 1
        fi
        if grep -qE 'src/(runtime|abi|core|extension)/' "$_rec"; then
            echo "verify: FAIL — N116 $_rec cites a POST-reorganisation path, but it records measurements taken before the reorganisation." >&2
            echo "Its line numbers are from the old tree, so a new path makes the citation wrong in a way that still looks authoritative. Revert the record and use the mapping table in docs/audits/README.md to follow old citations." >&2
            exit 1
        fi
    done

    # --- N115: the login system_info block must be MEASURED, not invented --------
    # It used to be literals — "cpu":"Wine", 4 physical cores, 8 logical, 16384 MB
    # total, 8192 used — emitted as though read from the machine. Measured on this
    # host, the truth was 16/32 cores and 32000 MB. Every field was wrong, and
    # nothing could tell, because invented data and a real reading look identical
    # once they are on the wire.
    #
    # The unit tests cover SystemInfo itself; they cannot see this format string.
    # This sensor is the half that watches the wire format.
    N115_RC=0; N115_WS=$(grep -vE '^[[:space:]]*(//|/\*|\*[[:space:]/]|\*$)' src/runtime/compat/ws_bridge.cpp) || N115_RC=$?
    sensor_stage1 "N115 login system_info measured" "src/runtime/compat/ws_bridge.cpp" "$N115_RC"
    sensor_nonempty "N115 login system_info measured" "non-comment lines of compat/ws_bridge.cpp" "$N115_WS"
    if ! grep -q 'SystemInfo::Get()' <<<"$N115_WS"; then
        echo "verify: FAIL — N115 the login payload no longer reads measured host facts (SystemInfo::Get)." >&2
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
    # connection (it opens ServerDB + login only). AssetCDN::Initialize used to run
    # unconditionally from initialize.cpp BEFORE the CLI was parsed, so g_isServer
    # was FALSE there and a gate could not distinguish server from client. The call
    # now lives in boot.cpp, post-CLI-parse, gated on the client. Guard both facts.
    N131_RC=0; N131_BOOT=$(grep -vE '^[[:space:]]*(//|/\*|\*[[:space:]/]|\*$)' src/runtime/lifecycle/boot.cpp) || N131_RC=$?
    sensor_stage1 "N131 CDN gated off servers" "src/runtime/lifecycle/boot.cpp" "$N131_RC"
    sensor_nonempty "N131 CDN gated off servers" "non-comment lines of boot.cpp" "$N131_BOOT"
    # Whitespace-flattened and matched CONTIGUOUSLY: two separate greps ("some
    # !g_isServer exists" AND "AssetCDN::Initialize exists") passed blind, because
    # boot.cpp has OTHER !g_isServer gates — removing THIS gate while leaving the
    # call unconditional still satisfied both. Require the call inside the gate.
    N131_FLAT=$(tr -s '[:space:]' ' ' <<<"$N131_BOOT")
    if ! grep -qE 'if \( *!g_isServer *\) *\{ *AssetCDN::Initialize\(\)' <<<"$N131_FLAT"; then
        echo "verify: FAIL — N131 AssetCDN::Initialize is no longer inside an if (!g_isServer) gate in boot.cpp." >&2
        echo "Without the client-gate a headless server fetches cosmetics it never renders, opening a needless outbound connection." >&2
        exit 1
    fi
    # And it must NOT be called unconditionally from initialize.cpp (pre-CLI-parse).
    N131_RC2=0; N131_INIT=$(grep -vE '^[[:space:]]*(//|/\*|\*[[:space:]/]|\*$)' src/runtime/lifecycle/initialize.cpp) || N131_RC2=$?
    sensor_stage1 "N131 CDN not in initialize" "src/runtime/lifecycle/initialize.cpp" "$N131_RC2"
    sensor_nonempty "N131 CDN not in initialize" "non-comment lines of initialize.cpp" "$N131_INIT"
    if grep -q 'AssetCDN::Initialize' <<<"$N131_INIT"; then
        echo "verify: FAIL — N131 AssetCDN::Initialize is back in initialize.cpp, where g_isServer is not yet set." >&2
        echo "It would run before the CLI is parsed, so the server-gate cannot apply and the CDN fetches on every host." >&2
        exit 1
    fi

    # --- N129: the DLL-load hardening must report per-hook failures --------------
    # DllLoadHook (N75/N89 search-path hardening) installs its four LoadLibrary
    # hooks OUTSIDE PatchDetour, so N126/N128's reporting doesn't cover it. It used
    # to accumulate ok &= and log one "OK"/"PARTIAL", hiding which variant failed
    # and why — the worst place to be vague, since a silently-unhooked LoadLibrary
    # is a DLL-hijack gap. It must name the variant and the MH_STATUS on failure.
    N129_RC=0; N129_DL=$(grep -vE '^[[:space:]]*(//|/\*|\*[[:space:]/]|\*$)' src/runtime/hook/dll_load_hook.cpp) || N129_RC=$?
    sensor_stage1 "N129 dll-hook reports per-variant" "src/runtime/hook/dll_load_hook.cpp" "$N129_RC"
    sensor_nonempty "N129 dll-hook reports per-variant" "non-comment lines of dll_load_hook.cpp" "$N129_DL"
    if ! grep -q 'MH_StatusToString' <<<"$N129_DL"; then
        echo "verify: FAIL — N129 DllLoadHook::Install no longer reports the MH_STATUS on a failed LoadLibrary hook." >&2
        echo "It would revert to a bare OK/PARTIAL that hides which search-path hook failed — a silent DLL-hijack gap." >&2
        exit 1
    fi
    if grep -qE 'bool ok = true;.*ok &=' <<<"$(tr '\n' ' ' <<<"$N129_DL")"; then
        echo "verify: FAIL — N129 the accumulate-and-hide 'ok &=' aggregate is back in DllLoadHook." >&2
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
    if ! grep -qE '(BOOL|auto) +[a-zA-Z]+ *= *PatchDetour\(&Original_LoadLibraryW' <<<"$N127_MP"; then
        echo "verify: FAIL — N127 PatchBlockOculusSDK no longer captures the LoadLibraryW detour result." >&2
        echo "Without checking the return it cannot report FAILED, and the silent-success regression returns." >&2
        exit 1
    fi

    # --- N126: a failed hook must not be silent --------------------------------
    # PatchDetour is the one choke point every detour passes through. A failed
    # Attach used to return FALSE and log nothing, and 9 of 10 mode_patches call
    # sites ignore the return and log "installed" on the next line regardless — so
    # a server-critical hook that never installed still printed "installed" and
    # produced no failure signal at any level. The failure branch must Log.
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
    # setjmp, the VEH does the longjmp. They used to sit in different translation
    # units coupled by an `extern`, which is the seam that reads as "which file owns
    # crash recovery?". N125 co-located them in crash_recovery.cpp. This fails if the
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
    # The recovery hook must still be INSTALLED — the move surfaced that nothing
    # verified this. A dropped InstallGameMainHook() call means the server no longer
    # sets up its longjmp point and dies on the first crash it was built to survive.
    if ! grep -q 'InstallGameMainHook()' src/runtime/lifecycle/initialize.cpp; then
        echo "verify: FAIL — N125 initialize.cpp no longer calls InstallGameMainHook()." >&2
        echo "Without it there is no setjmp recovery point: the VEH's longjmp has nowhere to land and a server crash terminates instead of recovering." >&2
        exit 1
    fi

    # --- N123: the login display name must be SOURCED, not a literal -------------
    # Every NEVR client announced the identical hardcoded name, so eight players in
    # one session rendered eight identical nameplates. The name was available the
    # whole time — token_auth parsed it from the auth response and persisted it to
    # the credential cache — it had simply never been exposed.
    #
    # Flattened before matching (tr -d '\\'): the JSON lives inside a C string
    # literal, so every quote is backslash-escaped and matching it through a
    # justfile recipe means three layers of escaping. N115's sensor silently
    # matched NOTHING for exactly this reason and its falsification went green.
    N123_RC=0; N123_WS=$(grep -vE '^[[:space:]]*(//|/\*|\*[[:space:]/]|\*$)' src/runtime/compat/ws_bridge.cpp) || N123_RC=$?
    sensor_stage1 "N123 login display name sourced" "src/runtime/compat/ws_bridge.cpp" "$N123_RC"
    sensor_nonempty "N123 login display name sourced" "non-comment lines of compat/ws_bridge.cpp" "$N123_WS"
    N123_FLAT=$(tr -d '\\' <<<"$N123_WS")
    # N146: displayname is now set via nlohmann_json, not a hand-built snprintf
    # format string.  The old pattern was "\"displayname\":\"%s\""; the new one
    # is j["displayname"] = resolvedName (where resolvedName traces back to
    # TokenAuth_GetUsername, verified by the second check below).
    if ! grep -qE 'displayname.*=.*resolvedName|displayname.*=.*displayName' <<<"$N123_FLAT"; then
        echo "verify: FAIL — N123 the login displayname is no longer sourced from a variable." >&2
        echo "A literal here makes every client announce the same name; eight players render eight identical nameplates and the service cannot tell them apart." >&2
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
    # Four conditions used to be logged and stepped over. On a dedicated server
    # nobody reads a console, so "logged" means "lost", and the process ran on to
    # fail later somewhere unrelated — broken AND misattributed.
    #
    # All four are comment-stripped: a commented-out ServerFatal would satisfy a
    # plain grep and the sensor would pass on a disabled guard (N111).
    N120_RC=0; N120_LOADER=$(grep -vE '^[[:space:]]*(//|/\*|\*[[:space:]/]|\*$)' src/runtime/ext/plugin_loader.cpp) || N120_RC=$?
    sensor_stage1 "N120 server-fatal plugin guards" "src/runtime/ext/plugin_loader.cpp" "$N120_RC"
    sensor_nonempty "N120 server-fatal plugin guards" "non-comment lines of ext/plugin_loader.cpp" "$N120_LOADER"

    # The HookGuard verdict must be CONSUMED. `HookGuard::VerifyAll(filename);` as
    # a bare statement is the pre-N120 bug: the collision was detected, logged at
    # ERROR, and the plugin loaded anyway — detection that changed nothing.
    if ! grep -qE '(int|auto) +[a-z_]+ *= *HookGuard::VerifyAll' <<<"$N120_LOADER"; then
        echo "verify: FAIL — N120 HookGuard::VerifyAll's return is no longer captured." >&2
        echo "A discarded verdict means a plugin can re-hook an address this runtime owns, our patch silently stops applying, and the load continues as though nothing happened." >&2
        exit 1
    fi
    if ! grep -q 'ServerFatal' <<<"$N120_LOADER"; then
        echo "verify: FAIL — N120 plugin_loader no longer escalates to ServerFatal." >&2
        echo "A plugin that failed init, or that stole one of our hooks, would again be tolerated on a dedicated server." >&2
        exit 1
    fi

    # platform_compat returned 0 even when the hook its own comment calls
    # silent-but-fatal had failed. It must report failure so module_loader's
    # existing FatalError fires at the point of the defect.
    N120_RC2=0; N120_PC=$(grep -vE '^[[:space:]]*(//|/\*|\*[[:space:]/]|\*$)' src/modules/platform-compat/src/platform_compat.cpp) || N120_RC2=$?
    sensor_stage1 "N120 platform_compat reports failure" "src/modules/platform-compat/src/platform_compat.cpp" "$N120_RC2"
    sensor_nonempty "N120 platform_compat reports failure" "non-comment lines of platform_compat.cpp" "$N120_PC"
    if ! grep -q 'NEVR_MODULE_HOST_IS_SERVER' <<<"$N120_PC"; then
        echo "verify: FAIL — N120 platform_compat no longer distinguishes server from client." >&2
        echo "It must return failure ONLY on a server: module_loader treats a non-zero init as fatal in both modes, so an unconditional failure would hard-fail a client that should merely warn." >&2
        exit 1
    fi
    if ! grep -qE 'if *\( *isServer *&& *\( *!tlsOk *\|\| *!httpOk *\) *\)' <<<"$N120_PC"; then
        echo "verify: FAIL — N120 platform_compat no longer fails a server run on a missing TLS or WinHTTP hook." >&2
        echo "Without this it returns success with a degraded network stack, which is how a silently-failing WinHTTP hook looked identical to a working one." >&2
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
    if [ "$(grep -c 'BootLogTee::TeeFprintf' <<<"$N120_XPID")" -lt 2 ]; then
        echo "verify: FAIL — N120 the XPID patch no longer tees BOTH outcomes to the boot log." >&2
        echo "Success and failure must each leave a record, or whether the game's provider strings were rewritten is unknowable from a run." >&2
        exit 1
    fi
    if ! grep -q 'ServerFatal' <<<"$N120_XPID"; then
        echo "verify: FAIL — N120 the XPID patch no longer escalates a validation failure." >&2
        echo "These sites are validated against literal bytes in the loaded image: a mismatch means echovr.exe is not the build this runtime targets, so every other patched address is suspect too." >&2
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
    N114_RC2=0; N114_ABI=$(grep -vE '^[[:space:]]*(//|/\*|\*[[:space:]/]|\*$)' src/extension/plugin_interface.h) || N114_RC2=$?
    sensor_stage1 "N114 plugin capability ABI" "src/extension/plugin_interface.h" "$N114_RC2"
    sensor_nonempty "N114 plugin capability ABI" "non-comment lines of extension/plugin_interface.h" "$N114_ABI"
    # Pinned to the CURRENT ABI version (N134 S6 bumped it 3->4 for the additive
    # NvrPluginInitEx per-plugin-args export). The pin guards against a silent
    # REGRESSION below the version where the capability channel (v3) and the args
    # channel (v4) landed — a deliberate bump updates this line, exactly as this
    # one did.
    if ! grep -q 'NEVR_PLUGIN_API_VERSION 5' <<<"$N114_ABI"; then
        echo "verify: FAIL — N114 NEVR_PLUGIN_API_VERSION is not 5; the capability (v3) + args (v4) + query-API (v5) channels must not regress below v5." >&2
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
    # BinaryBugFixes::Init saves the original byte before writing 0xC3 and restores
    # it on Shutdown (N33). A SECOND writer defeats that silently: if it runs first,
    # Init saves the already-patched 0xC3 as "the original" and the restore becomes
    # a no-op with the true byte lost for the process lifetime.
    #
    # That was the live arrangement until 2026-07-29 — PatchServerFramePacing
    # blind-wrote the same VA with no validation and no save. It was safe only
    # because Init happened to run first, which is an ordering accident, not a
    # design. It stays deleted.
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
    # Two copies is the shape, not the typo: N86 measured the truth, added a
    # correct dispatcher, and left the old one asserting the opposite. So the
    # invariant is about COUNT, not about the flag value.
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
    # No "do these directories still exist" loop here on purpose. It was written,
    # then removed as unfalsifiable: emptying any of the three fails EARLIER than
    # this point and for a different reason, so the loop could never be shown to
    # be the thing that caught it. Measured 2026-07-29 — emptying src/extension/
    # dies at configure with `No SOURCES given to target: nevr_core`, because the
    # headers are listed in src/core/CMakeLists.txt. What actually fail-closes:
    #   src/abi      -> sensor_stage1 above (grep rc>=2 on a missing subject)
    #   src/core     -> the same, via the N81/N85/N20 path sets
    #   src/extension-> CMake, since its headers are in nevr_core's source list
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
    python3 -m unittest discover -s tools/tests -p 'test_*.py'
    python3 tools/verify_hook_invariants.py
    # N84 runtime counterpart. The static check above scans source, so it only
    # sees plugins in THIS tree — a third-party plugin is a DLL we never compile.
    # HookGuard detects the effect (our bytes changed) instead of the source.
    # Both call sites are wiring, invisible to the GTest, so they get a sensor.
    if ! grep -q 'HookGuard::Record(target, name)' src/runtime/hook/patching.h; then
        echo "verify: FAIL — N84 HookGuard::Record missing from PatchDetour; new detours would be unguarded." >&2
        exit 1
    fi
    if ! grep -qE 'HookGuard::VerifyAll\((filename|s\.item\.file\.c_str\(\))\)' src/runtime/ext/plugin_loader.cpp; then
        echo "verify: FAIL — N84 HookGuard::VerifyAll missing from the plugin load path; third-party re-hooks undetectable." >&2
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
    # N71: the session-flags null-deref class is covered by BreakpointVEH's
    # generic null-ptr branch, NOT by per-site hooks. Two things must hold: the
    # generic branch still exists, and the attribution table is still populated
    # (without it a fire reports a bare RVA and the class is unrecognisable).
    if ! grep -q 'target < 0x10000' src/runtime/lifecycle/crash_recovery.cpp; then
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
    # N62: the SIGINT/SIGTERM path must not deadlock. Two sources were removed —
    # the stderr FILE lock (Log -> vfprintf) and the loader lock
    # (GetModuleHandleA/GetProcAddress). Both must stay out of PerformGracefulShutdown.
    # Strip comment lines first: the function's own comment says "NO
    # GetModuleHandleA/GetProcAddress", and a naive match flags the rule as a
    # violation of itself. Same false positive the N81 and N85 sensors hit.
    N62_RC=0; N62_BODY=$(awk '/^void PerformGracefulShutdown/,/^}/' src/runtime/lifecycle/crash_recovery.cpp) || N62_RC=$?
    sensor_stage1 "N62 shutdown loader-lock" "src/runtime/lifecycle/crash_recovery.cpp" "$N62_RC"
    sensor_nonempty "N62 shutdown loader-lock" "PerformGracefulShutdown() body in src/runtime/lifecycle/crash_recovery.cpp" "$N62_BODY"
    if printf '%s\n' "$N62_BODY" \
         | grep -vE '^[[:space:]]*(//|/\*|\*[[:space:]/]|\*$)' \
         | grep -qE 'GetModuleHandleA|GetProcAddress'; then
        echo "verify: FAIL — N62 PerformGracefulShutdown resolves symbols at shutdown time;" >&2
        echo "both take the loader lock, so a signal arriving while it is held deadlocks shutdown." >&2
        echo "Use the pointer cached by ResolveShutdownDependencies()." >&2
        exit 1
    fi
    if ! grep -q 'ResolveShutdownDependencies();' src/runtime/lifecycle/boot.cpp; then
        echo "verify: FAIL — N62 ResolveShutdownDependencies() call site missing; the cached" >&2
        echo "WsBridge_Shutdown pointer stays null and the listener leaks on every shutdown." >&2
        exit 1
    fi
    if ! grep -q 'volatile sig_atomic_t g_inSignalContext' src/runtime/lifecycle/crash_recovery.cpp; then
        echo "verify: FAIL — N62 signal-context flag is not volatile sig_atomic_t (the only type" >&2
        echo "a signal handler may portably touch)." >&2
        exit 1
    fi
    # N86: the per-frame tick MUST be dispatched from a site that runs in server
    # mode. PrecisionSleep::Wait does not (measured: 0 entries over a full run),
    # which left plugins, modules and N69's stack reserve silently dead there.
    # N68's original sensor only checked that TickPlugins appeared in the file —
    # which it did, in a hook that never executed. Check the LIVE site.
    if ! awk '/^void DispatchPerFrameWork/,/^}/' src/runtime/frame/tick.cpp \
         | grep -q 'TickPlugins'; then
        echo "verify: FAIL — N86 TickPlugins missing from DispatchPerFrameWork; the server-mode" >&2
        echo "per-frame tick is dead again (PrecisionSleep::Wait never runs on a server)." >&2
        exit 1
    fi
    if ! awk '/^void DispatchPerFrameWork/,/^}/' src/runtime/frame/tick.cpp \
         | grep -q 'TickModules'; then
        echo "verify: FAIL — N86 TickModules missing from DispatchPerFrameWork." >&2
        exit 1
    fi
    if ! awk '/^void DispatchPerFrameWork/,/^}/' src/runtime/frame/tick.cpp \
         | grep -q 'if (InterlockedExchange(&g_tickReentry, 1) != 0) return;'; then
        echo "verify: FAIL — N86 re-entrancy gate missing from DispatchPerFrameWork; a plugin" >&2
        echo "OnFrame that calls GetTimeMicroseconds would recurse without bound." >&2
        exit 1
    fi
    if ! grep -q 'Frame::DispatchPerFrameWork(nowUs)' src/runtime/patch/binary_bug_fixes.cpp; then
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
    MARKED=$(grep -rhoE 'HookLiveness::Mark\(HookLiveness::k[A-Za-z]+\)' src/runtime \
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
    # re-adding the build target ships it again the moment any dist rule
    # globs plugins/, and re-adding a dist rule ships whatever a stale build
    # tree still holds. Comment lines are stripped before matching so the
    # prose forbidding the thing cannot satisfy its own check.
    N95_TGT_RC=0; N95_TGT=$(grep -vE '^\s*#' plugins/CMakeLists.txt) || N95_TGT_RC=$?
    sensor_stage1 "N95 log-filter build target" "plugins/CMakeLists.txt" "$N95_TGT_RC"
    sensor_nonempty "N95 log-filter build target" "non-comment lines of plugins/CMakeLists.txt" "$N95_TGT"
    if grep -q 'add_subdirectory(log-filter)' <<<"$N95_TGT"; then
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
    if ! grep -q 'BuiltinLogFilter::InstallPnsradHook();' <<<"$TICK_CODE"; then
        echo "verify: FAIL — N90 InstallPnsradHook call site missing from the live tick; the hook" >&2
        echo "would never install, since pnsrad.dll loads long after filter init." >&2
        exit 1
    fi
    # N75: plugins and modules must load with a restricted search path. With
    # dwFlags=0 the search starts at the application directory, so a dll dropped
    # next to echovr.exe can satisfy a dependency ahead of the real one. N89
    # demonstrated the mechanism accidentally.
    for f in src/runtime/ext/plugin_loader.cpp src/runtime/ext/module_loader.cpp; do
        if ! grep -q 'LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR' "$f"; then
            echo "verify: FAIL — N75 restricted search flags missing from $f;" >&2
            echo "dependencies would resolve from the application directory first." >&2
            exit 1
        fi
    done
    # Doc paths: every src/, plugins/, extern/, docs/, tools/ or gen/ path claimed
    # in a root document shall resolve. Four phantom paths (src/protobufnevr,
    # extern/nevr-proto, src/server, gamepatches/patches.cpp) survived for months
    # in README.md and CLAUDE.md because nothing checked. A fact about the tree
    # asserted in prose drifts silently; this is the cheapest possible enforcement.
    python3 tools/verify_doc_paths.py
    # N92/N105: exactly one ws_bridge. Two divergent copies existed for months —
    # only the module ran, while N61's matchmaker fix landed in the gamepatches
    # copy that never did. The module tree was DELETED 2026-07-28 once its last
    # unique symbol (WsBridge_Shutdown) was brought in-process as
    # StopWebSocketBridgeListener. This guard keeps it from coming back.
    N92A_RC=0; grep -qE '^\s*add_subdirectory\(src/modules/ws-bridge\)' CMakeLists.txt || N92A_RC=$?
    sensor_stage1 "N92 ws-bridge module build" "CMakeLists.txt" "$N92A_RC"
    if [ "$N92A_RC" -eq 0 ]; then
        echo "verify: FAIL — N92 the ws-bridge MODULE is being built again. The bridge is" >&2
        echo "compiled into gamepatches; two copies is the bug (fixes land in the dead one)." >&2
        exit 1
    fi
    N92B_RC=0; grep -q 'LoadModule("ws_bridge"' src/runtime/lifecycle/boot.cpp || N92B_RC=$?
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
        if ! grep -B3 'login injected' src/runtime/compat/ws_bridge.cpp | grep -q 'LogLevel::Info'; then
            echo "verify: FAIL — logging.md Rule 2: the login-injection line is not at Info." >&2
            echo "Identity at login shall be visible in a production log (N91)." >&2
            exit 1
        fi
    fi
    if ! grep -q 'xpid=%s' src/runtime/compat/ws_bridge.cpp; then
        echo "verify: FAIL — logging.md Rule 2: login injection does not log the XPID." >&2
        exit 1
    fi
    # logging.md: no INFO-level logging inside a per-frame hot path (~125Hz).
    python3 tools/verify_log_rules.py
    # D1/N78: exactly ONE strong definition of ::Log. gameserver.cpp used to define
    # a second one with no WriteLog null-check, so which definition every call in
    # the DLL bound to was link-order dependent — and if that one won, the
    # early-boot stderr fallback silently did not exist.
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
        'Still waiting for authorization' \
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
    # C2/N84: every PatchDetour shall name its hook. The parameter is required at
    # compile time, so this is belt-and-braces against someone re-adding a default.
    # Match the DECLARATION, not prose. The first version of this check matched
    # its own explanatory comment — which quotes the very string it forbids — so
    # it failed on a correct tree. Comment lines are stripped; the pattern is
    # anchored to the PatchDetour signature.
    # N100 sensor removed 2026-08-02: its subject (BUGS.md) is being purged from
    # the public repo. The evidence-rank rule it enforced lives on in
    # docs/standards/verification.md; the N-ledger entries it checked are now
    # git history or migrated to ADRs.
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
    # to the game. A unit believed that message, removed -headless, and a
    # window opened on the owner's screen.
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
    N96_RC=0; N96_HITS=$(grep -rn --include='*.cpp' --include='*.h' -P 'ResolveVA(?!_Checked|_Unchecked)' src plugins) || N96_RC=$?
    sensor_stage1 "N96 bare ResolveVA" "src plugins" "$N96_RC"
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
    N97_RC=0; N97_HITS=$(grep -rn --include='*.cpp' --include='*.h' -P '^\s*(static\s+)?(inline\s+)?bool\s+ValidatePrologue\s*\(' src plugins) || N97_RC=$?
    sensor_stage1 "N97 ValidatePrologue definitions" "src plugins" "$N97_RC"
    N97_DEFS=$(printf '%s\n' "$N97_HITS" | grep -c 'ValidatePrologue' || true)
    if [ "$N97_RC" -ne 0 ] || [ "$N97_DEFS" -ne 1 ]; then
        echo "verify: FAIL — N97 found $N97_DEFS definitions of ValidatePrologue (want exactly 1," >&2
        echo "nevr::ValidatePrologue in plugins/common/include/nevr_common.h). A second copy is how" >&2
        echo "a bare-memcmp version returned TRUE for a zero-length check (N97)." >&2
        printf '%s\n' "$N97_HITS" >&2
        exit 1
    fi
    C2_RC=0; C2_CODE=$(grep -vE '^[[:space:]]*(//|/\*|\*[[:space:]/]|\*$)' src/runtime/hook/patching.h) || C2_RC=$?
    sensor_stage1 "C2 PatchDetour default name" "src/runtime/hook/patching.h" "$C2_RC"
    if printf '%s\n' "$C2_CODE" \
         | grep -qE 'PatchDetour\(.*const char\* name\s*='; then
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
    # S7b — flat-map structural gate: exactly 29 entries. Adding or removing a
    # flat-map entry without updating this number fails the build — a deliberate
    # reminder to also add a test for the new mapping. The 29 entries are the
    # complete key surface measured in S0, verified migrated through S3-S5b.
    N133_S7B=$(grep -cE '^\s*\{"' src/runtime/lifecycle/service_map.cpp)
    if [ "$N133_S7B" -ne 29 ]; then
        echo "verify: FAIL — N133 S7b: flat map has $N133_S7B entries, expected 29." >&2
        echo "A key was added or removed from the cutover map in service_map.cpp." >&2
        echo "If adding: also add a test to test_service_map.cpp and update this count." >&2
        echo "If removing: that key's reader must be deleted first, or it silently" >&2
        echo "falls back to the old game-JSON path (exactly what the cutover prevents)." >&2
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
    # S7d — the sample config.yaml must remain valid YAML and contain the minimum
    # keys a server needs to boot (auth, services, identity sections). A missing
    # or corrupted sample breaks launch-server.sh with an inscrutable YAML error.
    N133_S7D_RC=0
    N133_S7D=$(grep -cE '^\s*(auth:|services:|identity:|version:)' \
        echovr/_local/config.yaml) || N133_S7D_RC=$?
    sensor_stage1 "N133 S7d sample config" "echovr/_local/config.yaml" "$N133_S7D_RC"
    if [ "$N133_S7D" -lt 4 ]; then
        echo "verify: FAIL — N133 S7d: sample config.yaml missing required sections" >&2
        echo "(found $N133_S7D of 4: auth, services, identity, version)." >&2
        echo "launch-server.sh reads this file; a broken sample breaks every server boot." >&2
        exit 1
    fi
    # N112 — BuildIdentity: the client login and server registration now carry
    # NEVR build identity (version, commit, git describe, build type) and a
    # plugin manifest. These sensors prove the wiring is present and catch the
    # case where the old literals ("buildversion":631547, bare GIT_DESCRIBE)
    # are restored by a merge or refactor.
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
    # N112b — client login carries nevr_identity and nevr_plugins.
    if ! grep -q 'nevr_identity' src/runtime/compat/ws_bridge.cpp; then
        echo "verify: FAIL — N112b: ws_bridge.cpp login JSON does not carry" >&2
        echo "nevr_identity. The client login must send NEVR version info (N112)." >&2
        exit 1
    fi
    if ! grep -q 'nevr_plugins' src/runtime/compat/ws_bridge.cpp; then
        echo "verify: FAIL — N112b: ws_bridge.cpp login JSON does not carry" >&2
        echo "nevr_plugins. The client login must send a plugin manifest (N112)." >&2
        exit 1
    fi
    # N112c — server registration uses BuildIdentity (not bare GIT_DESCRIBE).
    if ! grep -q 'BuildIdentity::Get()' src/runtime/server/gameserver.cpp; then
        echo "verify: FAIL — N112c: gameserver.cpp does not call BuildIdentity::Get()." >&2
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
    # The floor is deliberately derived from the current, production-linked
    # suite; raising it is part of adding tests, while a drop is always a
    # regression that needs an explicit sensor update and review.
    TEST_COUNT=$(grep -hE '^TEST(_F)?\(' src/runtime/tests/*.cpp | wc -l)
    if [ "$TEST_COUNT" -lt 176 ]; then
        echo "verify: FAIL — runtime GTest count fell to $TEST_COUNT (floor 176)." >&2
        exit 1
    fi
    echo "verify: runtime GTest declarations=$TEST_COUNT (floor 176)"
    # Wave 10.2: PATCHES_SOURCES is the compiled runtime patch inventory. A
    # patch addition/removal requires a reviewed update to its pinned list.
    python3 tools/verify_patch_source_inventory.py
    # Wave 5/10: every concrete mode_patches.cpp byte rewrite must have a
    # PE-backed assertRVABytes row. Detours/import hooks are deliberately not
    # included: they are not ordinary prologue rewrites.
    python3 tools/verify_mode_patch_ground_truth.py
    echo "verify: OK ({{ preset }})"

# ServerDB token-auth BAC smoke test removed 2026-08-02: the test script
# (tests/token-auth-smoke.sh) was deleted — superseded by just verify's
# test-auth-unit and the auth ground-truth tests.

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

# Sign all DLLs in dist/ with Authenticode
sign: dist
    #!/usr/bin/env bash
    set -euo pipefail
    cert_dir="{{ justfile_directory() }}/certs"
    if [[ ! -f "$cert_dir/code-signing.key" || ! -f "$cert_dir/chain.pem" ]]; then
        echo "ERROR: No signing certs found. Run: just generate-certs" >&2
        exit 1
    fi
    shopt -s nullglob globstar
    dlls=(dist/**/*.dll dist/**/*.exe)
    if [[ ${#dlls[@]} -eq 0 ]]; then
        echo "No DLLs/EXEs found in dist/" >&2
        exit 1
    fi
    for f in "${dlls[@]}"; do
        echo "Signing $f"
        osslsigncode sign \
            -certs "$cert_dir/chain.pem" \
            -key "$cert_dir/code-signing.key" \
            -n "nEVR Runtime" \
            -t http://timestamp.digicert.com \
            -in "$f" \
            -out "$f.signed"
        mv "$f.signed" "$f"
    done
    echo "Signed ${#dlls[@]} file(s)"

# Verify Authenticode signature on a file
verify-sign file:
    osslsigncode verify -CAfile certs/root-ca.crt -in {{ file }}

# --- Internal ---

# Install vcpkg dependencies for MinGW cross-compilation (runs only for mingw presets)
[private]
_vcpkg-mingw:
    #!/usr/bin/env bash
    if [[ "{{ preset }}" == mingw-* ]]; then
        mkdir -p build/{{ preset }}/vcpkg_installed
        cd "$HOME/.vcpkg" && unset VCPKG_ROOT && ./vcpkg install --triplet=x64-mingw-static \
            --x-manifest-root="{{ justfile_directory() }}" \
            --x-install-root="{{ justfile_directory() }}/build/{{ preset }}/vcpkg_installed" > /dev/null 2>&1 || true
    fi
