# NEVR Runtime Build System

# Default preset: mingw-release on Linux, release on Windows
default_preset := if os() == "linux" { "mingw-release" } else { "release" }
preset := env("PRESET", default_preset)

# Show available recipes (default)
default:
    @just --list

# Configure CMake
configure: _vcpkg-mingw
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

# Regenerate C++ protobuf from BSR (buf.build/echotools/nevr-api)
# Uses vcpkg protoc to match the runtime version. Run `just configure` first.
proto:
    PATH="{{ justfile_directory() }}/build/{{ preset }}/vcpkg_installed/x64-linux/tools/protobuf:$PATH" buf generate buf.build/echotools/nevr-api

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
    cmake --build --preset {{ preset }} --target test_xpid_patch --target test_parse_endpoint --target test_behavioral
    bin="build/{{ preset }}/bin/test_xpid_patch.exe"
    if [[ ! -f "$bin" ]]; then
        echo "ERROR: GTest binary not found: $bin" >&2
        echo "       (is 'gtest' available in vcpkg for triplet x64-mingw-static?)" >&2
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
        echo "verify: FAIL — src/gameserver/ was removed (N103). The compiled path is src/gamepatches/gameserver/." >&2
        echo "Re-adding that tree recreates the two-copy split that let N48 ship half-implemented. Route the change to src/gamepatches/gameserver/." >&2
        exit 1
    fi
    # Wave I — source-level verifiers: each fix must have its call site present.
    # These are automated red→green tests: they fail when the call site is missing
    # (the bug state) and pass when the fix is in place.
    # N59: PatchDscProvider call site in initialize.cpp
    if ! grep -q 'PatchDscProvider()' src/gamepatches/initialize.cpp; then
        echo "verify: FAIL — N59 PatchDscProvider call site missing from initialize.cpp" >&2
        exit 1
    fi
    # N68: TickPlugins/TickModules wired into per-frame hook
    if ! grep -q 'TickPlugins' src/gamepatches/wave0_instrumentation.cpp; then
        echo "verify: FAIL — N68 TickPlugins call site missing from per-frame hook" >&2
        exit 1
    fi
    if ! grep -q 'TickModules' src/gamepatches/wave0_instrumentation.cpp; then
        echo "verify: FAIL — N68 TickModules call site missing from per-frame hook" >&2
        exit 1
    fi
    if ! grep -q 'NotifyModulesStateChange' src/gamepatches/state_machine.cpp; then
        echo "verify: FAIL — N68 NotifyModulesStateChange call site missing from state path" >&2
        exit 1
    fi
    # N60: stop() must be called OUTSIDE g_pairsMutex lock (deadlock prevention)
    if ! grep -q 'remoteToStop->stop()' src/gamepatches/ws_bridge.cpp; then
        echo "verify: FAIL — N60 stop()-outside-lock pattern missing from ws_bridge" >&2
        exit 1
    fi
    # N61: matchmaker must register independent callback on shared remote
    if ! grep -q 'g_loginRemoteWs->setOnMessageCallback' src/gamepatches/ws_bridge.cpp; then
        echo "verify: FAIL — N61 matchmaker callback registration missing from ws_bridge" >&2
        exit 1
    fi
    # N63: re-entry gate must call ForceFatalExit, not return
    if grep -q 'Sleep(100);\s*$' src/gamepatches/crash_recovery.cpp; then
        echo "verify: FAIL — N63 re-entry gate still returns (Sleep+return), should be ForceFatalExit" >&2
        exit 1
    fi
    # N64: BeginGracefulShutdown must call WsBridge_Shutdown before ForceFatalExit
    if ! grep -q 'WsBridge_Shutdown' src/gamepatches/gameserver/gameserver.cpp; then
        echo "verify: FAIL — N64 WsBridge_Shutdown call missing from BeginGracefulShutdown" >&2
        exit 1
    fi
    # N102: the two fail-fast sites N48 introduced must live in the copy that
    # SHIPS. 647cca7 wrote them into src/gameserver/ — a tree that stopped being
    # built on 2026-06-26 — so the feature was recorded as done while production
    # had no fatal path at all for either condition. Assert on the compiled file
    # (src/gamepatches/gameserver/), never the dead one.
    N102_RC=0; N102_GS=$(grep -vE '^[[:space:]]*(//|/\*|\*[[:space:]/]|\*$)' src/gamepatches/gameserver/gameserver.cpp) || N102_RC=$?
    sensor_stage1 "N102 gameserver fail-fast" "src/gamepatches/gameserver/gameserver.cpp" "$N102_RC"
    sensor_nonempty "N102 gameserver fail-fast" "non-comment lines of src/gamepatches/gameserver/gameserver.cpp" "$N102_GS"
    for site in 'registration rejected by ServerDB' 'no valid token for ServerDB connection'; do
        if ! grep -qF "$site" <<<"$N102_GS"; then
            echo "verify: FAIL — N102 the fail-fast for '${site}' is missing from the SHIPPING gameserver." >&2
            echo "Without it the server runs degraded for hours instead of exiting with a cause (BUGS.md N102)." >&2
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
    # src/gamepatches, and every real call site (the remainder are prose in comments).
    # Deliberately NOT a word-boundary regex: `\bLog\(` also matches the phrase "Log()"
    # in the explanatory comments inside these very functions. Deliberately NOT the
    # `(^|[^a-zA-Z_])Log\(` idiom either — GNU grep's ERE mishandles `^` inside an
    # alternation group and that pattern silently matches nothing (verified 2026-07-26).
    N70_VEH_RC=0; N70_VEH_BODY=$(awk '/^LONG WINAPI BreakpointVEH/,/^}/' src/gamepatches/crash_recovery.cpp) || N70_VEH_RC=$?
    sensor_stage1 "N70 BreakpointVEH" "src/gamepatches/crash_recovery.cpp" "$N70_VEH_RC"
    sensor_nonempty "N70 BreakpointVEH" "BreakpointVEH() body in src/gamepatches/crash_recovery.cpp" "$N70_VEH_BODY"
    if grep -qF 'Log(EchoVR::LogLevel' <<<"$N70_VEH_BODY"; then
        echo "verify: FAIL — N70 BreakpointVEH calls Log(); crash path must use VehPrintf (raw WriteFile)." >&2
        echo "Log() reaches std::lock_guard(g_file_mutex) — a crash during log emission would self-deadlock." >&2
        exit 1
    fi
    N70_WCD_RC=0; N70_WCD_BODY=$(awk '/^static void WriteCrashDump/,/^}/' src/gamepatches/crash_recovery.cpp) || N70_WCD_RC=$?
    sensor_stage1 "N70 WriteCrashDump" "src/gamepatches/crash_recovery.cpp" "$N70_WCD_RC"
    sensor_nonempty "N70 WriteCrashDump" "WriteCrashDump() body in src/gamepatches/crash_recovery.cpp" "$N70_WCD_BODY"
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
    if ! grep -q 'CacheModuleTable()' src/gamepatches/crash_recovery.cpp; then
        echo "verify: FAIL — N70 CacheModuleTable() call site missing; handler would have no module snapshot." >&2
        exit 1
    fi
    # N69: stack reserve must be claimed, or the overflow handler has no room to run.
    if ! grep -q 'SetThreadStackGuarantee' src/gamepatches/crash_recovery.cpp; then
        echo "verify: FAIL — N69 SetThreadStackGuarantee missing; stack-overflow handler cannot run." >&2
        exit 1
    fi
    if ! grep -q 'EnsureStackReserve()' src/gamepatches/wave0_instrumentation.cpp; then
        echo "verify: FAIL — N69 EnsureStackReserve() missing from per-frame hook; game threads uncovered." >&2
        exit 1
    fi
    # N67: the crash flags are written from any thread and read from the faulting
    # thread. VERIFIED-BY-TYPE only binds the artifact that ships — this grep asserts
    # the type is on THIS file, the one the linker consumes (the earlier fix landed
    # in plugins/crash-handler/, which CMake does not build).
    if ! grep -q 'std::atomic<bool> g_crashReporterSuppressed' src/gamepatches/crash_recovery.cpp; then
        echo "verify: FAIL — N67 g_crashReporterSuppressed is not std::atomic<bool> in the SHIPPING path." >&2
        exit 1
    fi
    if ! grep -q 'std::atomic<bool> g_justSuppressedCrash' src/gamepatches/crash_recovery.cpp; then
        echo "verify: FAIL — N67 g_justSuppressedCrash is not std::atomic<bool> in the SHIPPING path." >&2
        exit 1
    fi
    # N36: Log() is unsafe under the loader lock. Initialize() runs from DllMain, and
    # the Log() fallback to stderr stops applying the moment
    # InitializeFunctionPointers() makes EchoVR::WriteLog non-null. Everything after
    # that point in Initialize() must use BootLogTee::TeeFprintf. Exactly one Log()
    # call is permitted — the final line, emitted after BootLogTee::Close().
    N36_RC=0; N36_BODY=$(awk '/^VOID Initialize\(\)/,/^}/' src/gamepatches/initialize.cpp) || N36_RC=$?
    sensor_stage1 "N36 Initialize Log census" "src/gamepatches/initialize.cpp" "$N36_RC"
    sensor_nonempty "N36 Initialize Log census" "Initialize() body in src/gamepatches/initialize.cpp" "$N36_BODY"
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
    if ! grep -q 'if (IsNevrLine(message)) return false;' src/gamepatches/builtin_log_filter.cpp; then
        echo "verify: FAIL — N77 NEVR-line exemption missing from ShouldSuppress; game-noise patterns can delete NEVR output." >&2
        exit 1
    fi
    # N77: these two patterns must never return to the blanket suppress list.
    if grep -qF '"Finished initializing engine",' src/gamepatches/builtin_log_filter.cpp; then
        echo "verify: FAIL — N77 'Finished initializing engine' is suppressed again; that string is the headless-boot witness (N7/N8/N10)." >&2
        exit 1
    fi
    if grep -qF '"ExitProcess(",' src/gamepatches/builtin_log_filter.cpp; then
        echo "verify: FAIL — N77 'ExitProcess(' is suppressed again; it masks real process-exit reports. Use rate limiting (N78)." >&2
        exit 1
    fi
    # N78: frequent lines are collapsed into a counted summary, never deleted.
    if ! grep -q 'RateVerdict::Collapse' src/gamepatches/builtin_log_filter.cpp; then
        echo "verify: FAIL — N78 rate-limited summarisation missing; filter can only delete, not summarise (docs/standards/logging.md Rule 4)." >&2
        exit 1
    fi
    # N79: filter health must be emitted while the process is alive. Shutdown() is
    # unreachable in production — every server exit is TerminateProcess, which runs
    # no DLL detach.
    if ! grep -q 'MaybeEmitHealth();' src/gamepatches/builtin_log_filter.cpp; then
        echo "verify: FAIL — N79 periodic health emission missing; suppression ratio only reachable at a shutdown that never runs." >&2
        exit 1
    fi
    # N80: both log writers must stamp the shared run ID, or the two files cannot be joined.
    if ! grep -q 'GetRunId()' src/gamepatches/boot_log_tee.cpp; then
        echo "verify: FAIL — N80 run ID missing from BootLogTee; boot log cannot be correlated or split by run." >&2
        exit 1
    fi
    if ! grep -q 'GetRunId()' src/gamepatches/builtin_log_filter.cpp; then
        echo "verify: FAIL — N80 run ID missing from the runtime log writer." >&2
        exit 1
    fi
    # N81/N45: no bare [NEVR] tag on a LOG line — it names every component and so
    # discriminates none.
    #
    # src/gamepatches/cli.cpp is excluded deliberately, not by oversight: its 14
    # "[NEVR] " strings are all AddArgHelpString descriptions, where the marker
    # distinguishes NEVR-added flags from the game's own in `--help` output. That is
    # a usage contract (RULINGS.md "Usage contracts"), not a log tag — docs/standards/logging.md's
    # subsystem-tag rule governs log lines. Verified 2026-07-26: every [NEVR] hit in
    # that file is a help string, zero are log calls.
    N81_RC=0; N81_HITS=$(grep -rn '"\[NEVR\] ' src/gamepatches src/common) || N81_RC=$?
    sensor_stage1 "N81 bare [NEVR] tag" "src/gamepatches src/common" "$N81_RC"
    if [ "$N81_RC" -eq 0 ] && printf '%s\n' "$N81_HITS" \
         | grep -v legacy | grep -v 'src/gamepatches/cli.cpp' | grep .; then
        echo "verify: FAIL — N81 bare [NEVR] log tag found; use a subsystem tag from the docs/standards/logging.md table." >&2
        exit 1
    fi
    # --- Tier-0 hook invariants (N83/N84) ------------------------------------
    # Static: no Wine, no game process, no execution. Catches the failure that
    # produced the whole broadcaster thread — an address given a name nobody
    # re-derived, then reasoned about by that name forever after.
    # Known bugs warn (and stay visible); anything NEW is a hard failure.
    python3 tools/verify_hook_invariants.py
    # N84 runtime counterpart. The static check above scans source, so it only
    # sees plugins in THIS tree — a third-party plugin is a DLL we never compile.
    # HookGuard detects the effect (our bytes changed) instead of the source.
    # Both call sites are wiring, invisible to the GTest, so they get a sensor.
    if ! grep -q 'HookGuard::Record(target, name)' src/gamepatches/gamepatches_internal.h; then
        echo "verify: FAIL — N84 HookGuard::Record missing from PatchDetour; new detours would be unguarded." >&2
        exit 1
    fi
    if ! grep -q 'HookGuard::VerifyAll(filename)' src/gamepatches/plugin_loader.cpp; then
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
    N85_RC=0; N85_HITS=$(grep -rn -- '->setOnMessageCallback(nullptr)' src/gamepatches src/modules src/common) || N85_RC=$?
    sensor_stage1 "N85 empty ws callback" "src/gamepatches src/modules src/common" "$N85_RC"
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
    if ! grep -q 'target < 0x10000' src/gamepatches/crash_recovery.cpp; then
        echo "verify: FAIL — N71 generic null-ptr AV branch missing from BreakpointVEH; the whole" >&2
        echo "session-flags deref class loses its only guard (no per-site hooks exist by design)." >&2
        exit 1
    fi
    # Count ENTRIES, not lines — the table packs two per line.
    N71_SITES=$(grep -oE '\{0x[0-9A-F]+, "' src/gamepatches/crash_recovery.cpp | wc -l)
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
    for f in src/modules/ws-bridge/src/ws_bridge.cpp src/gamepatches/ws_bridge.cpp; do
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
    N62_RC=0; N62_BODY=$(awk '/^void PerformGracefulShutdown/,/^}/' src/gamepatches/crash_recovery.cpp) || N62_RC=$?
    sensor_stage1 "N62 shutdown loader-lock" "src/gamepatches/crash_recovery.cpp" "$N62_RC"
    sensor_nonempty "N62 shutdown loader-lock" "PerformGracefulShutdown() body in src/gamepatches/crash_recovery.cpp" "$N62_BODY"
    if printf '%s\n' "$N62_BODY" \
         | grep -vE '^[[:space:]]*(//|/\*|\*[[:space:]/]|\*$)' \
         | grep -qE 'GetModuleHandleA|GetProcAddress'; then
        echo "verify: FAIL — N62 PerformGracefulShutdown resolves symbols at shutdown time;" >&2
        echo "both take the loader lock, so a signal arriving while it is held deadlocks shutdown." >&2
        echo "Use the pointer cached by ResolveShutdownDependencies()." >&2
        exit 1
    fi
    if ! grep -q 'ResolveShutdownDependencies();' src/gamepatches/boot.cpp; then
        echo "verify: FAIL — N62 ResolveShutdownDependencies() call site missing; the cached" >&2
        echo "WsBridge_Shutdown pointer stays null and the listener leaks on every shutdown." >&2
        exit 1
    fi
    if ! grep -q 'volatile sig_atomic_t g_inSignalContext' src/gamepatches/crash_recovery.cpp; then
        echo "verify: FAIL — N62 signal-context flag is not volatile sig_atomic_t (the only type" >&2
        echo "a signal handler may portably touch)." >&2
        exit 1
    fi
    # N86: the per-frame tick MUST be dispatched from a site that runs in server
    # mode. PrecisionSleep::Wait does not (measured: 0 entries over a full run),
    # which left plugins, modules and N69's stack reserve silently dead there.
    # N68's original sensor only checked that TickPlugins appeared in the file —
    # which it did, in a hook that never executed. Check the LIVE site.
    if ! awk '/^static void DispatchPerFrameWork/,/^}/' src/gamepatches/wave0_instrumentation.cpp \
         | grep -q 'TickPlugins'; then
        echo "verify: FAIL — N86 TickPlugins missing from DispatchPerFrameWork; the server-mode" >&2
        echo "per-frame tick is dead again (PrecisionSleep::Wait never runs on a server)." >&2
        exit 1
    fi
    if ! awk '/^static void DispatchPerFrameWork/,/^}/' src/gamepatches/wave0_instrumentation.cpp \
         | grep -q 'TickModules'; then
        echo "verify: FAIL — N86 TickModules missing from DispatchPerFrameWork." >&2
        exit 1
    fi
    if ! awk '/^static void DispatchPerFrameWork/,/^}/' src/gamepatches/wave0_instrumentation.cpp \
         | grep -q 'if (InterlockedExchange(&g_tickReentry, 1) != 0) return;'; then
        echo "verify: FAIL — N86 re-entrancy gate missing from DispatchPerFrameWork; a plugin" >&2
        echo "OnFrame that calls GetTimeMicroseconds would recurse without bound." >&2
        exit 1
    fi
    if ! grep -q 'DispatchPerFrameWork(nowUs)' src/gamepatches/wave0_instrumentation.cpp; then
        echo "verify: FAIL — N86 DispatchPerFrameWork call site missing from the live tick hook." >&2
        exit 1
    fi
    # N20: no platform identity may be compiled into the binary. The login
    # injection must take it from token-auth (JWT) or, in server mode, from
    # config — never a literal. Matches an 18-20 digit account/discord ID.
    N20_RC=0; N20_HITS=$(grep -rnE '\b1[0-9]{17,19}\b' src/gamepatches src/modules src/common) || N20_RC=$?
    sensor_stage1 "N20 identity literal" "src/gamepatches src/modules src/common" "$N20_RC"
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
    # Asserted against src/modules/ws-bridge/ because THAT is the copy that ships —
    # InstallWebSocketBridge has exactly one call site, in the module's
    # NvrModuleInit; the src/gamepatches/ws_bridge.cpp copy compiles and never runs
    # (N92). A sensor on the gamepatches copy would watch dead code.
    if ! grep -q 'Using nevr_discord_id from config' src/modules/ws-bridge/src/ws_bridge.cpp; then
        echo "verify: FAIL — N20 nevr_discord_id config fallback missing from the shipping" >&2
        echo "ws-bridge module (src/modules/ws-bridge/src/ws_bridge.cpp)." >&2
        exit 1
    fi
    if grep -n 'discordId == 0 &&.*g_isServer' src/modules/ws-bridge/src/ws_bridge.cpp; then
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
    HL_RC=0; HL_ENUM=$(awk '/^enum Id/,/^};/' src/gamepatches/hook_liveness.h) || HL_RC=$?
    sensor_stage1 "HookLiveness enum census" "src/gamepatches/hook_liveness.h" "$HL_RC"
    sensor_nonempty "HookLiveness enum census" "enum Id in src/gamepatches/hook_liveness.h" "$HL_ENUM"
    DECLARED=$(printf '%s\n' "$HL_ENUM" \
               | grep -cE '^[[:space:]]+k[A-Za-z]+([[:space:]]*=[^,]*)?,' || true)
    if [ "$DECLARED" -lt 1 ]; then
        echo "verify: FAIL — HookLiveness enum census matched zero enumerators; the enum's" >&2
        echo "spelling no longer fits the pattern and the DECLARED/MARKED comparison below" >&2
        echo "would be vacuous." >&2
        exit 1
    fi
    # MARKED needs no stage-1 capture: src/gamepatches is the repo itself, and a
    # zero count makes MARKED < DECLARED fail closed below (measured direction).
    MARKED=$(grep -rhoE 'HookLiveness::Mark\(HookLiveness::k[A-Za-z]+\)' src/gamepatches \
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
    if ! grep -q 'kSupersededByBuiltin' src/gamepatches/plugin_loader.cpp; then
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
    if ! grep -q 'InstallPnsradHook' src/gamepatches/builtin_log_filter.cpp; then
        echo "verify: FAIL — N90 pnsrad log hook missing; pnsrad.dll output bypasses the filter." >&2
        exit 1
    fi
    if ! grep -q 'BuiltinLogFilter::InstallPnsradHook();' src/gamepatches/wave0_instrumentation.cpp; then
        echo "verify: FAIL — N90 InstallPnsradHook call site missing from the live tick; the hook" >&2
        echo "would never install, since pnsrad.dll loads long after filter init." >&2
        exit 1
    fi
    # N75: plugins and modules must load with a restricted search path. With
    # dwFlags=0 the search starts at the application directory, so a dll dropped
    # next to echovr.exe can satisfy a dependency ahead of the real one. N89
    # demonstrated the mechanism accidentally.
    for f in src/gamepatches/plugin_loader.cpp src/gamepatches/module_loader.cpp; do
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
    # N92: exactly one ws_bridge. Two divergent copies existed for months — only
    # the module ran, while N61's matchmaker fix landed in the gamepatches copy
    # that never did. The fold is only safe while the module cannot come back.
    if grep -qE '^\s*add_subdirectory\(src/modules/ws-bridge\)' CMakeLists.txt; then
        echo "verify: FAIL — N92 the ws-bridge MODULE is being built again. The bridge is" >&2
        echo "compiled into gamepatches; two copies is the bug (fixes land in the dead one)." >&2
        exit 1
    fi
    if grep -q 'LoadModule("ws_bridge"' src/gamepatches/boot.cpp; then
        echo "verify: FAIL — N92 boot.cpp still loads ws_bridge as a required module; with the" >&2
        echo "DLL gone the loader fail-fasts and the server never starts." >&2
        exit 1
    fi
    if ! grep -q 'InstallWebSocketBridge();' src/gamepatches/boot.cpp; then
        echo "verify: FAIL — N92 the in-process bridge is never started; no proxy, no login." >&2
        exit 1
    fi
    # docs/standards/logging.md Rule 2: the login path SHALL log the full XPID at
    # Info. This rule existed in prose and went unenforced for months — N91 shipped
    # with the identity line at Debug, invisible in production, which is how a
    # duplicate LoginRequest and an account-id-0 login both went unnoticed. The
    # rule was right; nothing checked it.
    if ! awk '/login injected/{found=1} found && /Log\(EchoVR::LogLevel::Info/{ok=1} END{exit !ok}' \
         src/gamepatches/ws_bridge.cpp; then
        if ! grep -B3 'login injected' src/gamepatches/ws_bridge.cpp | grep -q 'LogLevel::Info'; then
            echo "verify: FAIL — logging.md Rule 2: the login-injection line is not at Info." >&2
            echo "Identity at login shall be visible in a production log (N91)." >&2
            exit 1
        fi
    fi
    if ! grep -q 'xpid=%s' src/gamepatches/ws_bridge.cpp; then
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
                 --include='*.cpp' src/common src/gamepatches src/modules 2>/dev/null \
               | grep -v '/tests/' | wc -l)
    if [ "$LOG_DEFS" -ne 1 ]; then
        echo "verify: FAIL — D1 found $LOG_DEFS definitions of ::Log (want exactly 1)." >&2
        grep -rlE '^(VOID|void) Log\(EchoVR::LogLevel' --include='*.cpp' \
             src/common src/gamepatches src/modules | grep -v '/tests/' >&2
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
    if grep -qE 'm_tokenExpiry = static_cast<uint64_t>\(time\(nullptr\)\) \+ [0-9]+;' \
         src/modules/token-auth/src/token_auth.cpp; then
        echo "verify: FAIL — token-auth sets m_tokenExpiry from a hardcoded offset;" >&2
        echo "the JWT's own exp claim is the authority (GetJwtExpiry)." >&2
        exit 1
    fi
    if ! grep -q 'GetJwtExpiry()' src/modules/token-auth/src/token_auth.cpp; then
        echo "verify: FAIL — token-auth does not consult the JWT exp claim." >&2
        exit 1
    fi
    # N94: nine [NEVR.AUTH] per-step detail lines are pinned at Debug (N47
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
        'Token expired %llus ago' \
        'Token refreshed successfully'; do
        N94_RC=0; N94_CTX=$(grep -B1 -F "$msg" "$N94_FILE") || N94_RC=$?
        sensor_stage1 "N94 auth log taxonomy" "$N94_FILE" "$N94_RC"
        sensor_nonempty "N94 auth log taxonomy" "message '$msg' in $N94_FILE" "$N94_CTX"
        if grep -q 'LogLevel::Info' <<<"$N94_CTX"; then
            echo "verify: FAIL — N94 the '[NEVR.AUTH] ${msg}' line is at Info; the N47 taxonomy pins it at Debug (merge 2f29312 once reverted all nine — BUGS.md N94)." >&2
            exit 1
        fi
    done
    # C2/N84: every PatchDetour shall name its hook. The parameter is required at
    # compile time, so this is belt-and-braces against someone re-adding a default.
    # Match the DECLARATION, not prose. The first version of this check matched
    # its own explanatory comment — which quotes the very string it forbids — so
    # it failed on a correct tree. Comment lines are stripped; the pattern is
    # anchored to the PatchDetour signature.
    # N100: docs/standards/verification.md:44 requires every "fixed"/"closed"
    # claim to state its evidence rank. Nothing checked it. Scope is entries
    # ADDED on or after 2026-07-26, when that standard landed; earlier entries
    # are pre-standard and stand as written. That date is not readable from file
    # content, so the sensor uses N-ID as a proxy — and the proxy is MEASURED,
    # not assumed (first appearance of each heading in BUGS.md's own history):
    #     N83  a51966b  2026-07-26
    #     N79  548df84  2026-07-25
    # so N-ID >= 83 is exactly "added on or after 2026-07-26".
    # Only rows that NAME a verification method are in scope: an entry that
    # claims no method (N83 "Guard measured", N84 "RE-OPENED") has no rank to
    # state. Vacuity guard below, because a scope that matched nothing would
    # pass forever.
    # One awk pass emits both the offender list and the scope size, tagged, so
    # there is no temp file (scratch shall not go in /tmp — CLAUDE.md).
    N100_RC=0; N100_RAW=$(awk '
        /^### N[0-9]+\./ { id = $2; sub(/^N/, "", id); sub(/\./, "", id); cur = id }
        /^\| \*\*Status\*\*/ {
            if (cur + 0 >= 83) {
                inscope++
                if ($0 ~ /Verification|SYSTEM-TEST|SENSOR|falsif/ && $0 !~ /[Rr]ank[ ]*([0-9]|N\/A)/)
                    print "OFFENDER N" cur
            }
        }
        END { print "INSCOPE " inscope + 0 }
    ' BUGS.md) || N100_RC=$?
    sensor_stage1 "N100 ledger evidence rank" "BUGS.md" "$N100_RC"
    sensor_nonempty "N100 ledger evidence rank" "N-entry Status rows in BUGS.md" "$N100_RAW"
    N100_INSCOPE=$(printf '%s\n' "$N100_RAW" | sed -n 's/^INSCOPE //p')
    N100_OFFENDERS=$(printf '%s\n' "$N100_RAW" | sed -n 's/^OFFENDER //p')
    if [ -z "$N100_INSCOPE" ] || [ "$N100_INSCOPE" -lt 1 ]; then
        echo "verify: FAIL — sensor 'N100 ledger evidence rank': the scope is EMPTY, so the check" >&2
        echo "could never fire. Either the heading/Status shapes changed or the awk broke." >&2
        exit 1
    fi
    if [ -n "$N100_OFFENDERS" ]; then
        echo "verify: FAIL — N100 these N-entry Status rows name a verification method but no" >&2
        echo "evidence rank (docs/standards/verification.md:44). State the ladder rank:" >&2
        printf '%s\n' "$N100_OFFENDERS" >&2
        exit 1
    fi
    # --- N99: -server shall apply the game's own headless mask --------------
    # `-headless` is a NATIVE echovr.exe token. Its whole effect in the binary
    # is one instruction (0x140504566, `and dword [rbx+0x1D4], 0xFFFEFEFE`),
    # applied by the game's own arg handler ONLY when that literal token is on
    # the command line. `-server` is NEVR-invented, so nothing applied it for
    # us and bit 0 — the render/window master bit — stayed SET on every
    # -server-only run. Three assertions; any one alone is satisfiable by the
    # bug.
    N99_RC=0; N99_MP=$(grep -vE '^[[:space:]]*(//|/\*|\*[[:space:]/]|\*$)' src/gamepatches/mode_patches.cpp) || N99_RC=$?
    sensor_stage1 "N99 headless mask" "src/gamepatches/mode_patches.cpp" "$N99_RC"
    sensor_nonempty "N99 headless mask" "non-comment lines of src/gamepatches/mode_patches.cpp" "$N99_MP"
    if ! grep -q 'ENGINE_FLAGS_HEADLESS_MASK' <<<"$N99_MP"; then
        echo "verify: FAIL — N99 PatchEnableHeadless no longer applies ENGINE_FLAGS_HEADLESS_MASK." >&2
        echo "-server would leave bit 0 of pGame+0x1D4 SET and the game would open a window." >&2
        exit 1
    fi
    # The mask constant shall keep the value measured in the shipped binary.
    if ! grep -q 'ENGINE_FLAGS_HEADLESS_MASK = 0xFFFEFEFEu' src/gamepatches/patch_addresses.h; then
        echo "verify: FAIL — N99 ENGINE_FLAGS_HEADLESS_MASK is not 0xFFFEFEFE, the value" >&2
        echo "verified byte-for-byte at 0x140504566 in the shipped echovr.exe." >&2
        exit 1
    fi
    # And boot.cpp shall not tell the operator to remove a flag that is native
    # to the game. A unit believed that message, removed -headless, and a
    # window opened on the owner's screen.
    N99_BOOT_RC=0; N99_BOOT=$(grep -vE '^[[:space:]]*(//|/\*|\*[[:space:]/]|\*$)' src/gamepatches/boot.cpp) || N99_BOOT_RC=$?
    sensor_stage1 "N99 boot flag advice" "src/gamepatches/boot.cpp" "$N99_BOOT_RC"
    sensor_nonempty "N99 boot flag advice" "non-comment lines of src/gamepatches/boot.cpp" "$N99_BOOT"
    if grep -qE 'is redundant|Remove this flag' <<<"$N99_BOOT"; then
        echo "verify: FAIL — N99 boot.cpp tells the operator a flag is redundant / to remove it." >&2
        echo "-headless and -noovr are NATIVE echovr.exe flags the game itself consumes." >&2
        exit 1
    fi
    # N96: no symbol spelled plain `ResolveVA`. Two definitions of that name once
    # coexisted in BugSplat64.dll with IDENTICAL signatures and different
    # guarantees — wave0_instrumentation.cpp's file-static did no validation,
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
        echo "whether the address was validated (BUGS.md N96)." >&2
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
        echo "a bare-memcmp version returned TRUE for a zero-length check (BUGS.md N97)." >&2
        printf '%s\n' "$N97_HITS" >&2
        exit 1
    fi
    C2_RC=0; C2_CODE=$(grep -vE '^[[:space:]]*(//|/\*|\*[[:space:]/]|\*$)' src/gamepatches/gamepatches_internal.h) || C2_RC=$?
    sensor_stage1 "C2 PatchDetour default name" "src/gamepatches/gamepatches_internal.h" "$C2_RC"
    if printf '%s\n' "$C2_CODE" \
         | grep -qE 'PatchDetour\(.*const char\* name\s*='; then
        echo "verify: FAIL — C2 PatchDetour's name parameter has a default again." >&2
        echo "22 of 24 sites omitted it when it was defaultable, so HookGuard's overwrite" >&2
        echo "alarm printed name=(unnamed) for almost every address it guards." >&2
        exit 1
    fi
    echo "verify: OK ({{ preset }})"

# ServerDB token-auth BAC smoke test (live backend; reads echovr/_local/config.json)
test-token-auth config="echovr/_local/config.json":
    bash tests/token-auth-smoke.sh {{config}}

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
