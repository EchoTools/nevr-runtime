# The toolchain image the CI jobs (build.yml, vcpkg-cache.yml, defender-scan.yml) run in, so a run
# does not reinstall it. It holds what those jobs installed per run before: the pacman packages
# (Arch's MinGW-w64 GCC, the same toolchain development uses), osslsigncode 2.14, buf 1.73.0 and
# the case-folded libCrypt32.a that the pinned ixwebsocket port links.
#
# The image tag is the first 12 hex digits of this file's sha256 (`sha256sum .github/builder/Dockerfile | cut -c1-12`), and the three
# workflows name that tag literally; tools/tests/test_builder_image.py fails when they disagree.
# Packages are whatever Arch ships when the image is built, so to refresh the toolchain change this
# file (the line below) and rebuild with the builder-image workflow.
# refresh: 1
FROM archlinux:base-devel

LABEL org.opencontainers.image.source="https://github.com/EchoTools/nevr-runtime"
LABEL org.opencontainers.image.description="Build toolchain for the nevr-runtime CI jobs"

# git must be present before actions/checkout, or it falls back to the REST API (no submodules).
RUN pacman -Syu --noconfirm --needed \
      git mingw-w64-gcc ninja cmake python python-yaml go just zstd openssl wine \
      curl zip unzip tar pkgconf zlib \
    && pacman -Scc --noconfirm

# The pinned vcpkg revision's ixwebsocket port links `-lCrypt32`; Arch's MinGW package ships
# libcrypt32.a only, and ld on Linux is case-sensitive.
RUN ln -s libcrypt32.a /usr/x86_64-w64-mingw32/lib/libCrypt32.a

# osslsigncode is not in Arch's official repositories; build a pinned upstream release so the
# throwaway-certificate signing test in tools/tests runs instead of skipping.
RUN set -eux; \
    git clone --depth 1 --branch 2.14 https://github.com/mtrojnar/osslsigncode.git /tmp/osslsigncode; \
    cmake -S /tmp/osslsigncode -B /tmp/osslsigncode/build -G Ninja -DCMAKE_BUILD_TYPE=Release; \
    cmake --build /tmp/osslsigncode/build; \
    cmake --install /tmp/osslsigncode/build; \
    rm -rf /tmp/osslsigncode

# Buf takes about a minute to compile from source.
RUN set -eux; \
    GOBIN=/usr/local/bin go install github.com/bufbuild/buf/cmd/buf@v1.73.0; \
    go clean -cache -modcache; \
    rm -rf /root/go /root/.cache
