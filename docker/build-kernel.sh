#!/usr/bin/env bash
# Fetch, configure and build the reference kernel inside the container, then
# print the QEMU line that boots it.
#
# The container cannot *run* this kernel -- it shares the host's. It can build
# it and boot it in a VM, which is the only way to get Linux 6.18 + this repo's
# config out of a Docker-based workflow.
#
#   build-kernel                 # 6.18.2, full reference .config
#   build-kernel --delta         # host-ish defconfig + delta fragment only
#   KERNEL_VERSION=6.18.40 build-kernel
#
set -euo pipefail

KERNEL_VERSION="${KERNEL_VERSION:-6.18.2}"
KERNEL_SRC="${KERNEL_SRC:-/usr/src/linux}"
ARTIFACT="${ARTIFACT:-/artifact}"
JOBS="${JOBS:-$(nproc)}"
MODE="full"

[[ "${1:-}" == "--delta" ]] && MODE="delta"

FULL_CONFIG="${ARTIFACT}/kernel/configs/full-6.18.2-bpf.config"
DELTA_CONFIG="${ARTIFACT}/kernel/configs/delta-bpf-arena.config"

for f in "${FULL_CONFIG}" "${DELTA_CONFIG}"; do
    [[ -r "${f}" ]] || { echo "error: ${f} not readable -- mount the repo at ${ARTIFACT}" >&2; exit 1; }
done

major="${KERNEL_VERSION%%.*}"
tarball="linux-${KERNEL_VERSION}.tar.xz"
url="https://cdn.kernel.org/pub/linux/kernel/v${major}.x/${tarball}"

mkdir -p "$(dirname "${KERNEL_SRC}")"
if [[ ! -d "${KERNEL_SRC}" ]]; then
    echo ">> fetching ${url}"
    wget -q --show-progress -O "/tmp/${tarball}" "${url}"
    echo ">> unpacking"
    mkdir -p "${KERNEL_SRC}"
    tar -xf "/tmp/${tarball}" -C "${KERNEL_SRC}" --strip-components=1
    rm -f "/tmp/${tarball}"
fi

cd "${KERNEL_SRC}"

if [[ "${MODE}" == "full" ]]; then
    echo ">> using the full reference config (${FULL_CONFIG})"
    cp "${FULL_CONFIG}" .config
else
    echo ">> defconfig + delta fragment (${DELTA_CONFIG})"
    make defconfig
    ./scripts/kconfig/merge_config.sh -m .config "${DELTA_CONFIG}"
fi

# olddefconfig, not oldconfig: no tty here to answer prompts, and the reference
# config was generated against 6.18.2, so a different point release will have
# symbols it has never heard of.
make olddefconfig

echo ">> verifying the resulting .config against the artifact's requirements"
python3 "${ARTIFACT}/scripts/check_kconfig.py" .config

echo ">> building with -j${JOBS} (expect 20-60 minutes)"
make -j"${JOBS}" bzImage modules
make INSTALL_MOD_PATH=/usr/src/modroot modules_install

bzimage="${KERNEL_SRC}/arch/x86/boot/bzImage"
echo
echo "built: ${bzimage}"
cat <<EOF

To boot it, from inside this container (needs 'docker run --device /dev/kvm'):

  qemu-system-x86_64 \\
    -enable-kvm -cpu host -smp 4 -m 4096 -nographic \\
    -kernel ${bzimage} \\
    -append "console=ttyS0 root=/dev/vda rw lsm=landlock,lockdown,yama,loadpin,safesetid,integrity,bpf" \\
    -drive file=rootfs.img,if=virtio,format=raw \\
    -virtfs local,path=${ARTIFACT},mount_tag=artifact,security_model=none

You still need a rootfs.img with a userland in it. If that is more assembly
than you want, the flake in this repo does the whole thing in one command:

  nix run .#vm

which builds this same kernel from the same .config and boots it with the repo
mounted at /repo, root autologin, and the toolchain already on PATH.
EOF
