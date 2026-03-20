# Makefile for NEVR Runtime
# Provides a simple interface for building and distributing the project

# Silence make's directory messages
MAKEFLAGS += --no-print-directory

# Detect OS
UNAME_S := $(shell uname -s)

# Default build preset based on OS
ifeq ($(UNAME_S),Linux)
    PRESET ?= mingw-release
else
    PRESET ?= release
endif

# Extract build type from preset (debug or release)
BUILD_TYPE := $(shell echo $(PRESET) | grep -o 'debug\|release')

.PHONY: all build dist clean help configure vcpkg-mingw verbose-build verbose-dist proto

all: build

# Verbose variants - show full output
verbose-build: configure
	cmake --build --preset $(PRESET)

verbose-dist: build
	cmake --build --preset $(PRESET) --target dist

# Install vcpkg dependencies for MinGW cross-compilation
vcpkg-mingw:
	@mkdir -p build/$(PRESET)/vcpkg_installed
	@cd $(HOME)/.vcpkg && unset VCPKG_ROOT && ./vcpkg install --triplet=x64-mingw-static \
		--x-manifest-root=$(CURDIR) \
		--x-install-root=$(CURDIR)/build/$(PRESET)/vcpkg_installed > /dev/null 2>&1 || true

# Configure with automatic vcpkg install for MinGW presets
configure:
	@if echo "$(PRESET)" | grep -q "^mingw-"; then \
		$(MAKE) vcpkg-mingw; \
	fi
	@unset VCPKG_ROOT && cmake --preset $(PRESET) > /dev/null 2>&1 || (unset VCPKG_ROOT && cmake --preset $(PRESET))

build: configure
	@cmake --build --preset $(PRESET) 2>&1 | grep -E '(error|Error|ERROR|fatal|FAILED)' || true

dist: build
	@cmake --build --preset $(PRESET) --target dist 2>&1 | \
		grep -vE '(^\[|^ninja|Creating.*\.(tar\.zst|zip)|Preparing distribution|Running utility|^===)' | \
		grep -E '(error|Error|ERROR|fatal|FAILED)' || true

# Regenerate C++ protobuf from BSR (buf.build/echotools/nevr-api)
proto:
	buf generate buf.build/echotools/nevr-api

clean:
	rm -rf build/ dist/

# System Tests
.PHONY: test-system test-system-short test-system-dll test-system-verbose

test-system:
	@echo "Running system tests (full mode)..."
	cd tests/system && go test -v ./...

test-system-short:
	@echo "Running system tests (short mode)..."
	cd tests/system && go test -v -short ./...

test-system-dll:
	@echo "Running DLL loading tests..."
	cd tests/system && go test -v -short -run ".*DLL.*" ./...

test-system-verbose:
	@echo "Running system tests (verbose, no cache)..."
	cd tests/system && go test -v -count=1 ./...

help:
	@echo "NEVR Runtime Build System"
	@echo "========================"
	@echo ""
	@echo "Available targets:"
	@echo "  all (default)  - Build the project (silent)"
	@echo "  vcpkg-mingw    - Install vcpkg dependencies for MinGW"
	@echo "  configure      - Configure the project (cmake --preset)"
	@echo "  build          - Build the project (silent)"
	@echo "  dist           - Build and create dist/ with renamed DLLs (silent)"
	@echo "  verbose-build  - Build with full output"
	@echo "  verbose-dist   - Create dist with full output"
	@echo "  clean          - Remove build and dist directories"
	@echo "  help           - Show this help message"
	@echo ""
	@echo "Test targets:"
	@echo "  test-system       - Run all system tests (full mode)"
	@echo "  test-system-short - Run quick system tests only (skips slow operations)"
	@echo "  test-system-dll   - Run DLL loading tests only"
	@echo "  test-system-verbose - Run system tests with verbose output (no cache)"
	@echo ""
	@echo "Build Presets:"
	@echo "  mingw-debug         - MinGW cross-compile (debug)"
	@echo "  mingw-release       - MinGW cross-compile (release) [Linux default]"
	@echo "  linux-wine-debug    - Wine/MSVC cross-compile (debug)"
	@echo "  linux-wine-release  - Wine/MSVC cross-compile (release)"
	@echo "  debug               - Native build (debug) [Windows default]"
	@echo "  release             - Native build (release) [Windows default]"
	@echo ""
	@echo "Dependencies:"
	@echo "  - vcpkg (installed at ~/.vcpkg/)"
	@echo "  - protobuf (installed via vcpkg automatically for native builds)"
	@echo ""
	@echo "First-time MinGW setup:"
	@echo "  make vcpkg-mingw   # Install vcpkg deps for MinGW (run once)"
	@echo ""
	@echo "Examples:"
	@echo "  make                           # Build with default preset"
	@echo "  make dist                      # Build and create distribution"
	@echo "  make dist PRESET=mingw-debug   # Build debug distribution"
	@echo "  make configure PRESET=linux-wine-release  # Configure Wine/MSVC build"
	@echo "  make test-system-short         # Run quick system tests"
