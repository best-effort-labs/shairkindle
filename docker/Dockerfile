# One image, both toolchains: zig (static ARMv6 cross-build) + JDK8/ecj (kindlet).
FROM eclipse-temurin:8-jdk-jammy

ARG ZIG_VERSION=0.13.0
# TARGETARCH is provided by BuildKit (amd64 on CI runners, arm64 on Apple Silicon).
# zig is a cross-compiler, so the K3 (arm-linux-musleabi) output is identical either
# way — we only need a zig that RUNS on the build host.
ARG TARGETARCH

# build-essential gives the native cc/c++ the host test suite needs (`make check`,
# which also compiles host BearSSL from vendor/bearssl/src); zig handles only the
# ARMv6 cross-build.
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential make file ecj zip unzip python3 xz-utils ca-certificates curl \
 && rm -rf /var/lib/apt/lists/*

# Hermetic zig (bundles its own musl + libc++ for the cross-build). The tarball is the
# one network-fetched thing that compiles device code, so verify its published SHA-256
# (from ziglang.org/download/index.json) before trusting it — HTTPS alone won't catch a
# CDN/upstream swap. Bump both hashes when ZIG_VERSION changes.
RUN case "$TARGETARCH" in \
      amd64) ZARCH=x86_64; ZSHA=d45312e61ebcc48032b77bc4cf7fd6915c11fa16e4aad116b66c9468211230ea ;; \
      arm64) ZARCH=aarch64; ZSHA=041ac42323837eb5624068acd8b00cd5777dac4cf91179e8dad7a7e90dd0c556 ;; \
      *) echo "unsupported TARGETARCH=$TARGETARCH" >&2; exit 1 ;; \
    esac; \
    curl -fsSL -o /tmp/zig.tar.xz "https://ziglang.org/download/${ZIG_VERSION}/zig-linux-${ZARCH}-${ZIG_VERSION}.tar.xz" \
 && echo "${ZSHA}  /tmp/zig.tar.xz" | sha256sum -c - \
 && tar -xJf /tmp/zig.tar.xz -C /opt && rm /tmp/zig.tar.xz \
 && ln -s "/opt/zig-linux-${ZARCH}-${ZIG_VERSION}/zig" /usr/local/bin/zig

WORKDIR /work
ENTRYPOINT ["sh", "docker/build.sh"]
