# Makefile for NEVR Server
# Provides a simple interface for building and distributing the project

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

.PHONY: all build dist clean help configure vcpkg-mingw

all: build

# Install vcpkg dependencies for MinGW cross-compilation
vcpkg-mingw:
	@echo "Installing vcpkg dependencies for MinGW..."
	@mkdir -p build/$(PRESET)/vcpkg_installed
	cd $(HOME)/.vcpkg && unset VCPKG_ROOT && ./vcpkg install --triplet=x64-mingw-static \
		--x-manifest-root=$(CURDIR) \
		--x-install-root=$(CURDIR)/build/$(PRESET)/vcpkg_installed

# Configure with automatic vcpkg install for MinGW presets
configure:
	@if echo "$(PRESET)" | grep -q "^mingw-"; then \
		echo "Detected MinGW preset, installing vcpkg dependencies..."; \
		$(MAKE) vcpkg-mingw; \
	fi
	unset VCPKG_ROOT && cmake --preset $(PRESET)

build: configure
	cmake --build --preset $(PRESET)

dist: build
	cmake --build --preset $(PRESET) --target dist

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
	@echo "NEVR Server Build System"
	@echo "========================"
	@echo ""
	@echo "Available targets:"
	@echo "  all (default) - Build the project"
	@echo "  vcpkg-mingw   - Install vcpkg dependencies for MinGW"
	@echo "  configure     - Configure the project (cmake --preset)"
	@echo "  build         - Build the project"
	@echo "  dist          - Build and create dist/ with renamed DLLs"
	@echo "  clean         - Remove build and dist directories"
	@echo "  help          - Show this help message"
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
