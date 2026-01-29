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
	cd $(HOME)/.vcpkg && ./vcpkg install --triplet=x64-mingw-static \
		--x-manifest-root=$(CURDIR) \
		--x-install-root=$(CURDIR)/build/$(PRESET)/vcpkg_installed

# Configure with automatic vcpkg install for MinGW presets
configure:
	@if echo "$(PRESET)" | grep -q "^mingw-"; then \
		echo "Detected MinGW preset, installing vcpkg dependencies..."; \
		$(MAKE) vcpkg-mingw; \
	fi
	cmake --preset $(PRESET)

build: configure
	cmake --build --preset $(PRESET)

dist: build
	cmake --build --preset $(PRESET) --target dist

clean:
	rm -rf build/ dist/

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
