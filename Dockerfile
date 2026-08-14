# syntax=docker/dockerfile:1.7
#
# Build/eval environment for bpf-arena-data-structures.
#
#   docker build -t bpf-arena-ds .                      # full image (default)
#   docker build -t bpf-arena-ds:lean --target toolchain .   # no kernel tooling
#
# ---------------------------------------------------------------------------
# What this image can and cannot do
# ---------------------------------------------------------------------------
# It CAN:
#   * build all 14 binaries (8 BPF relay apps + 6 userspace pthread tests)
#   * run the userspace pthread tests -- the artifact appendix's "basic test"
#   * build a 6.18 kernel from kernel/configs/*.config  (--target kernel-tools)
#   * boot that kernel in QEMU and run the BPF experiments inside the guest
#
# It CANNOT supply a kernel to itself. A container shares the host's kernel;
# there is no such thing as "a Docker image with Linux 6.18 in it". So the BPF
# half of the artifact has exactly two options:
#
#   (a) Run against the HOST kernel, if the host satisfies check_kconfig.py:
#         docker run --rm -it --privileged --pid=host \
#           -v "$PWD:/artifact" \
#           -v /sys/kernel/debug:/sys/kernel/debug \
#           -v /sys/fs/bpf:/sys/fs/bpf \
#           bpf-arena-ds
#         # then, inside:  python3 scripts/check_kconfig.py && make && \
#         #                python3 scripts/runner.py
#       CAP_BPF/CAP_PERFMON plus debugfs are why this needs --privileged; the
#       default seccomp profile also blocks bpf(2).
#
#   (b) Build the reference kernel here and boot it in QEMU, which gives you a
#       real 6.18 kernel regardless of what the host runs. See the
#       kernel-tools stage below. Needs /dev/kvm passed through to be fast:
#         docker run --rm -it --device /dev/kvm -v "$PWD:/artifact" bpf-arena-ds
#
# On Docker Desktop (macOS/Windows) only (b) is possible -- the host "kernel"
# there is Docker's own LinuxKit VM, which has neither CONFIG_LSM=...,bpf nor
# the BTF the loader needs.
#
# The flake in this repo does (b) for you in one command: `nix run .#vm`.
# ===========================================================================

# ---------------------------------------------------------------------------
# Stage 1: toolchain -- everything `make` needs, and nothing else
# ---------------------------------------------------------------------------
FROM ubuntu:24.04 AS toolchain

# ae.tex: "Clang/LLVM 15+ with BPF target (clang-20 recommended)".
# Ubuntu 24.04 ships clang-18, so clang-20 comes from apt.llvm.org.
ARG LLVM_VERSION=20
ARG DEBIAN_FRONTEND=noninteractive

SHELL ["/bin/bash", "-o", "pipefail", "-c"]

# DL3008 is disabled throughout: pinning Ubuntu package versions makes the
# image un-buildable the moment the archive rotates a point release, which is
# worse for an artifact reviewer than the reproducibility it buys. The flake is
# the reproducible path; this is the convenient one.
# hadolint ignore=DL3008
RUN --mount=type=cache,target=/var/cache/apt,sharing=locked \
    --mount=type=cache,target=/var/lib/apt/lists,sharing=locked \
    apt-get update && apt-get install --no-install-recommends -y \
        ca-certificates \
        curl \
        gnupg \
        lsb-release \
        software-properties-common

# LLVM's own apt repo, for a clang new enough to have a usable BPF backend
# with __BPF_FEATURE_ADDR_SPACE_CAST.
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
        # userspace side of every app, plus libbpf's and bpftool's own build
        build-essential \
        gcc \
        make \
        pkg-config \
        binutils \
        git \
        # libbpf: libelf + zlib.  bpftool: those plus libssl (src/sign.c
        # includes <openssl/opensslv.h> even for the `bootstrap` target, which
        # is the only target this Makefile builds).
        libelf-dev \
        zlib1g-dev \
        libssl-dev \
        # scripts/usertests.py, scripts/runner.py, scripts/check_kconfig.py.
        # networkx is for the sibling `calculus` artifact's calculus.py; it is
        # unused here but costs nothing and lets one image serve both.
        python3 \
        python3-networkx \
    && rm -rf /var/lib/apt/lists/*

# The Makefile probes for clang-20 and falls back to plain `clang`; apt.llvm.org
# installs only the versioned name, so make the probe succeed as intended.
ENV CLANG=clang-${LLVM_VERSION}
ENV CC=gcc

WORKDIR /artifact

# Sanity: fail the build here rather than three layers later.
RUN "${CLANG}" --version && gcc --version && python3 --version

# ---------------------------------------------------------------------------
# Stage 2: verify -- compile the artifact so a broken image cannot ship
# ---------------------------------------------------------------------------
# Not part of the runtime image. `docker build --target verify .` builds all 14
# binaries from a copy of the tree; the default build below does not run it.
FROM toolchain AS verify

COPY . /artifact
# DL4006: the toolchain stage already set SHELL to bash -o pipefail, which
# child stages inherit; hadolint does not track that across FROM.
# hadolint ignore=DL4006
RUN make -j"$(nproc)" \
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
        # kbuild
        bison \
        flex \
        bc \
        perl \
        rsync \
        cpio \
        kmod \
        libncurses-dev \
        # CONFIG_DEBUG_INFO_BTF=y is a hard build dependency on pahole; without
        # it the kernel build fails, and without BTF libbpf cannot attach the
        # lsm.s and uprobe.s programs at runtime.
        dwarves \
        # initramfs/compression
        zstd \
        xz-utils \
        gzip \
        file \
        # boot the result without touching the host kernel
        qemu-system-x86 \
        qemu-utils \
        # minimal userland to put in an initramfs, if you roll your own
        busybox-static \
        wget \
    && rm -rf /var/lib/apt/lists/*

# Reference kernel for the paper's numbers. ae.tex: "The reference kernel is
# 6.18.2; its complete .config is included". Override at build or run time.
ENV KERNEL_VERSION=6.18.2
ENV KERNEL_SRC=/usr/src/linux

COPY docker/build-kernel.sh /usr/local/bin/build-kernel
RUN chmod +x /usr/local/bin/build-kernel

WORKDIR /artifact

CMD ["/bin/bash"]
