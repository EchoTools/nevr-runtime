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
    # N34: mechanical guard — src/gameserver/ is dead code. The compiled path is
    # src/gamepatches/gameserver/. If add_subdirectory(src/gameserver) is ever
    # uncommented (not preceded by #), fail the verify gate. Sensor over lock:
    # the dead path must stay dead; re-enabling it silently compiles the wrong copy.
    if grep -Pn '^\s*add_subdirectory\s*\(\s*src/gameserver\s*\)' CMakeLists.txt; then
        echo "verify: FAIL — src/gameserver/ is DEAD CODE (N34). Compiled path is src/gamepatches/gameserver/." >&2
        echo "Uncommenting add_subdirectory(src/gameserver) compiles the WRONG copy. Remove this line or route the fix to src/gamepatches/gameserver/." >&2
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
    if awk '/^LONG WINAPI BreakpointVEH/,/^}/' src/gamepatches/crash_recovery.cpp | grep -qF 'Log(EchoVR::LogLevel'; then
        echo "verify: FAIL — N70 BreakpointVEH calls Log(); crash path must use VehPrintf (raw WriteFile)." >&2
        echo "Log() reaches std::lock_guard(g_file_mutex) — a crash during log emission would self-deadlock." >&2
        exit 1
    fi
    if awk '/^static void WriteCrashDump/,/^}/' src/gamepatches/crash_recovery.cpp | grep -qF 'Log(EchoVR::LogLevel'; then
        echo "verify: FAIL — N70 WriteCrashDump calls Log(); crash path must use VehPrintf (raw WriteFile)." >&2
        exit 1
    fi
    # N70: the handler must never enumerate modules (loader lock). The snapshot is
    # taken at init by CacheModuleTable and read from the cache during a crash.
    if awk '/^static void WriteCrashDump/,/^}/' src/gamepatches/crash_recovery.cpp | grep -q 'EnumProcessModules'; then
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
    LOGS_IN_INIT=$(awk '/^VOID Initialize\(\)/,/^}/' src/gamepatches/initialize.cpp | grep -cF 'Log(EchoVR::LogLevel' || true)
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
    if grep -rn '"\[NEVR\] ' src/gamepatches src/common src/gameserver 2>/dev/null \
         | grep -v legacy | grep -v 'src/gamepatches/cli.cpp'; then
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
    if grep -rn -- '->setOnMessageCallback(nullptr)' src/gamepatches src/modules src/common 2>/dev/null \
         | grep -v legacy | grep -vE ':[[:space:]]*(//|\*)'; then
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
    if awk '/^void PerformGracefulShutdown/,/^}/' src/gamepatches/crash_recovery.cpp \
         | grep -vE '^[[:space:]]*(//|\*|/\*)' \
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
    if grep -rnE '\b1[0-9]{17,19}\b' src/gamepatches src/modules src/common 2>/dev/null \
         | grep -v legacy | grep -viE 'hash|symbol|0x|SYM_'; then
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
    DECLARED=$(awk '/^enum Id/,/^};/' src/gamepatches/hook_liveness.h \
               | grep -cE '^[[:space:]]+k[A-Za-z]+([[:space:]]*=[^,]*)?,')
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
