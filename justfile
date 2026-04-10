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

# Regenerate C++ protobuf from BSR (buf.build/echotools/nevr-api)
# Uses vcpkg protoc to match the runtime version. Run `just configure` first.
proto:
    PATH="{{ justfile_directory() }}/build/{{ preset }}/vcpkg_installed/x64-linux/tools/protobuf:$PATH" buf generate buf.build/echotools/nevr-api

# Remove build and dist directories
clean:
    rm -rf build/ dist/

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

# Generate combat override files from echomod build output
generate-combat-overrides build_dir:
    python tools/echomod/generate_resources.py \
        --build-dir {{build_dir}} \
        --output-dir echovr/bin/win10/_overrides/combat

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
