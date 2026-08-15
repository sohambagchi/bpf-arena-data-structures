# syntax=docker/dockerfile:1.7

# ---------------------------------------------------------------------------
# Stage 1: toolchain -- everything `make` needs, and nothing else
# ---------------------------------------------------------------------------
FROM ubuntu:24.04 AS toolchain

ARG LLVM_VERSION=20
ARG DEBIAN_FRONTEND=noninteractive

SHELL ["/bin/bash", "-o", "pipefail", "-c"]

# hadolint ignore=DL3008
RUN --mount=type=cache,target=/var/cache/apt,sharing=locked \
    --mount=type=cache,target=/var/lib/apt/lists,sharing=locked \
    apt-get update && apt-get install --no-install-recommends -y \
        ca-certificates \
        curl \
        gnupg \
        lsb-release \
        software-properties-common

RUN --mount=type=cache,target=/var/cache/apt,sharing=locked \
    --mount=type=cache,target=/var/lib/apt/lists,sharing=locked \
    curl -fsSL https://apt.llvm.org/llvm.sh -o /tmp/llvm.sh \
    && chmod +x /tmp/llvm.sh \
    && /tmp/llvm.sh "${LLVM_VERSION}" \
    && rm -f /tmp/llvm.sh

# hadolint ignore=DL3008
RUN --mount=type=cache,target=/var/cache/apt,sharing=locked \
    --mount=type=cache,target=/var/lib/apt/lists,sharing=locked \
    apt-get update && apt-get install --no-install-recommends -y \
        build-essential \
        gcc \
        make \
        pkg-config \
        binutils \
        git \
        libelf-dev \
        zlib1g-dev \
        libssl-dev \
        binutils-dev \
        libcap-dev \
        python3 \
        python3-networkx \
    && rm -rf /var/lib/apt/lists/*

ENV CLANG=clang-${LLVM_VERSION}
ENV CC=gcc

WORKDIR /artifact

RUN "${CLANG}" --version && gcc --version && python3 --version

# ---------------------------------------------------------------------------
# Stage 2: verify -- compile the artifact so a broken image cannot ship
# ---------------------------------------------------------------------------
FROM toolchain AS verify

COPY . /artifact
# hadolint ignore=DL4006
RUN make \
    && test -x build/skeleton_msqueue \
    && test -x build/usertest_msqueue \
    && ./build/usertest_msqueue | tail -n 1 | grep -q '^done: produced=' \
    && make clean

# ---------------------------------------------------------------------------
# Stage 3: kernel-tools -- build and boot Linux 6.18 with this repo's config
# ---------------------------------------------------------------------------
FROM toolchain AS kernel-tools

ARG DEBIAN_FRONTEND=noninteractive

# hadolint ignore=DL3008
RUN --mount=type=cache,target=/var/cache/apt,sharing=locked \
    --mount=type=cache,target=/var/lib/apt/lists,sharing=locked \
    apt-get update && apt-get install --no-install-recommends -y \
        bison \
        flex \
        bc \
        perl \
        rsync \
        cpio \
        kmod \
        libncurses-dev \
        dwarves \
        zstd \
        xz-utils \
        gzip \
        file \
        qemu-system-x86 \
        qemu-utils \
        busybox-static \
        wget \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /artifact

CMD ["/bin/bash"]
