# The toolchain image the CI jobs (build.yml, vcpkg-cache.yml, defender-scan.yml) run in, so a run
# does not reinstall it. It holds what those jobs installed per run before: the pacman packages
# (Arch's MinGW-w64 GCC, the same toolchain development uses), osslsigncode 2.14, buf 1.73.0 and
# the case-folded libCrypt32.a that the pinned ixwebsocket port links, and a JRE for `keytool`, which
# tools/tests/test_quest_keystore.py runs to make a throwaway key (#717).
#
# The image tag is the first 12 hex digits of this file's sha256 (`sha256sum .github/builder/Dockerfile | cut -c1-12`), and the three
# workflows name that tag literally; tools/tests/test_builder_image.py fails when they disagree.
#
# Nothing here moves on its own (#695). The base image is a dated tag pinned by digest. pacman reads one dated
# snapshot of Arch's repositories from the Arch Linux Archive (ARCH_SNAPSHOT), not the live mirrors, and every
# package below is installed as name=version; the same versions are asserted by the "Toolchain versions" step
# in build.yml (tools/tests/test_builder_image.py keeps the two lists equal). To change the toolchain, change
# the versions (and the snapshot date when a version is newer than the snapshot) in this file and in that step,
# and rebuild with the builder-image workflow: `gh workflow run builder-image.yml --ref <branch>`.
# refresh: 1
FROM archlinux:base-devel-20261018.0.612668@sha256:0ccc8b2e55c0c26a6e501e892cc6b1cdea7cfdf77e432941320148b4dabd2bff

LABEL org.opencontainers.image.source="https://github.com/EchoTools/nevr-runtime"
LABEL org.opencontainers.image.description="Build toolchain for the nevr-runtime CI jobs"

# The archive keeps one copy of the repositories per day. -Syyuu syncs the databases again and lets pacman move
# a package of the base image back to the snapshot's version, so the result is the snapshot and nothing else.
ARG ARCH_SNAPSHOT=2026/10/20
RUN echo "Server = https://archive.archlinux.org/repos/${ARCH_SNAPSHOT}/\$repo/os/\$arch" > /etc/pacman.d/mirrorlist \
    && pacman -Syyuu --noconfirm

# git must be present before actions/checkout, or it falls back to the REST API (no submodules).
RUN pacman -S --noconfirm --needed \
      git=2.56.0-1 \
      mingw-w64-gcc=16.2.0-2 mingw-w64-binutils=2.47-1 mingw-w64-headers=14.0.0-1 \
      mingw-w64-crt=14.0.0-1 mingw-w64-winpthreads=14.0.0-1 \
      ninja=1.13.2-3 cmake=4.4.4-1 python=3.14.8-1 python-yaml=6.0.3-2 go=2:1.27.2-1 \
      zstd=1.5.7-3 openssl=3.6.5-1 wine=11.20-1 curl=8.23.0-1 zip=3.0-14 unzip=6.0-23 \
      tar=1.35-5 pkgconf=3.0.10-1 zlib=1:1.3.2-4 jre-openjdk-headless=27.u35-1 \
    && pacman -Scc --noconfirm

# just is pinned, not Arch's current one: build.yml asserts `just 1.58.0` (the version the release was built with;
# PATH-visible tools are part of every mingw package ABI). /usr/local/bin precedes /usr/bin. The digest is the one
# build.yml's quest job checks for the same tarball.
RUN set -eux; \
    curl -fsSL -o /tmp/just.tgz https://github.com/casey/just/releases/download/1.58.0/just-1.58.0-x86_64-unknown-linux-musl.tar.gz; \
    echo "4a5cc2f53e6f0f8c59092a6cc38291eb729d46a7dd95d3ae582008881b84931d  /tmp/just.tgz" | sha256sum -c -; \
    tar -xzf /tmp/just.tgz -C /usr/local/bin just; \
    rm /tmp/just.tgz; \
    test "$(just --version)" = "just 1.58.0"

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
