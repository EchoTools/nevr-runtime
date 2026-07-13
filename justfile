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

# Run C++ auth unit tests under Wine (cross-compiled GTest)
test-auth-unit: build
    wine build/{{preset}}/bin/test_token_auth.exe 2>/dev/null || echo "GTest binary not found (build with -DBUILD_TESTING=ON)"

# Run auth integration tests (needs game binary + MCP harness)
test-auth-integration:
    cd tests/system && go test -v -run "TestAuth" ./...

# Run all auth tests
test-auth: test-auth-groundtruth test-auth-unit

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
