# The toolchain image the CI jobs (build.yml, vcpkg-cache.yml, defender-scan.yml) run in, so a run
# does not reinstall it. It holds what those jobs installed per run before: the pacman packages
# (Arch's MinGW-w64 GCC, the same toolchain development uses), osslsigncode 2.14, buf 1.74.0 and a
# JRE for `keytool`, which tools/tests/test_quest_keystore.py runs to make a throwaway key (#717).
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
      git mingw-w64-gcc ninja cmake python python-yaml go zstd openssl wine \
      curl zip unzip tar pkgconf zlib jre-openjdk-headless \
    && pacman -Scc --noconfirm

# just is pinned, not Arch's current one: build.yml asserts `just 1.59.0` (the version the release was built with;
# PATH-visible tools are part of every mingw package ABI). /usr/local/bin precedes /usr/bin. The digest is the one
# build.yml's quest job checks for the same tarball.
RUN set -eux; \
    curl -fsSL -o /tmp/just.tgz https://github.com/casey/just/releases/download/1.59.0/just-1.59.0-x86_64-unknown-linux-musl.tar.gz; \
    echo "409044617b6f06517c1035e55ca38c8c856ff8b650e41bc2aa1ef82d5903a23e  /tmp/just.tgz" | sha256sum -c -; \
    tar -xzf /tmp/just.tgz -C /usr/local/bin just; \
    rm /tmp/just.tgz; \
    test "$(just --version)" = "just 1.59.0"

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
    GOBIN=/usr/local/bin go install github.com/bufbuild/buf/cmd/buf@v1.74.0; \
    go clean -cache -modcache; \
    rm -rf /root/go /root/.cache
