#!/usr/bin/env bash
#
# One-click kernel build for bpf-arena-data-structures.
#
# Downloads Linux 6.18.44 from cdn.kernel.org, verifies it, configures it with
# this repo's BPF fragment, builds it, and installs it.
#
#   scripts/build-kernel.sh                 # defconfig + BPF fragment, then install
#   scripts/build-kernel.sh --base running  # start from THIS machine's config
#   scripts/build-kernel.sh --base full     # the paper's reference .config
#   scripts/build-kernel.sh --no-install    # build only, touch nothing outside the tree
#   scripts/build-kernel.sh -y              # no confirmation prompt
#
# Exit codes:
#   1  usage error
#   2  unsupported platform
#   3  missing build dependency
#   4  insufficient disk space
#   5  download or checksum failure
#   6  extract failure
#   7  kernel configuration failure
#   8  resulting .config does not satisfy the artifact's requirements
#   9  compile failure
#  10  install failure
#
set -Eeuo pipefail

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------
KERNEL_VERSION="6.18.44"
KERNEL_URL="https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-${KERNEL_VERSION}.tar.xz"
# From https://cdn.kernel.org/pub/linux/kernel/v6.x/sha256sums.asc
KERNEL_SHA256="0f72d938f06828e82c90405174fe572287db7bfe089e2fc46572a99a7f240d43"

# Distinguishes the built kernel from anything already installed, so
# /lib/modules/<ver> and /boot entries can never collide with the running one.
LOCALVERSION="-bpf-arena"

# ~20 GB per ae.tex, plus slack for modules_install.
REQUIRED_GB=22

REPO_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
DELTA_CONFIG="${REPO_ROOT}/kernel/configs/delta-bpf-arena.config"
FULL_CONFIG="${REPO_ROOT}/kernel/configs/full-6.18.2-bpf.config"
CHECK_KCONFIG="${REPO_ROOT}/scripts/check_kconfig.py"

# ---------------------------------------------------------------------------
# Defaults, overridable by flags
# ---------------------------------------------------------------------------
BASE="defconfig"
WORKDIR="${REPO_ROOT}/kernel/build"
JOBS="$(nproc 2>/dev/null || echo 4)"
DO_INSTALL=1
ASSUME_YES=0
KEEP_TARBALL=1

# ---------------------------------------------------------------------------
# Output helpers
# ---------------------------------------------------------------------------
if [[ -t 1 ]] && [[ -z "${NO_COLOR:-}" ]]; then
    C_RED=$'\033[1;31m'; C_YEL=$'\033[1;33m'; C_GRN=$'\033[1;32m'
    C_BLU=$'\033[1;34m'; C_DIM=$'\033[2m'; C_OFF=$'\033[0m'
else
    C_RED=""; C_YEL=""; C_GRN=""; C_BLU=""; C_DIM=""; C_OFF=""
fi

STEP_N=0
# Set once the flags are parsed: 7 with the install step, 6 without. A fixed
# denominator would end a --no-install run at "[6/7]", which reads as a failure.
TOTAL_STEPS=7
step()  { STEP_N=$((STEP_N + 1)); printf '\n%s==> [%d/%d] %s%s\n' "$C_BLU" "$STEP_N" "$TOTAL_STEPS" "$*" "$C_OFF"; }
info()  { printf '    %s\n' "$*"; }
warn()  { printf '%swarning:%s %s\n' "$C_YEL" "$C_OFF" "$*" >&2; }
ok()    { printf '%s    ok:%s %s\n' "$C_GRN" "$C_OFF" "$*"; }

# die <exit-code> <headline> [remediation lines...]
die() {
    local code="$1" headline="$2"; shift 2
    printf '\n%serror:%s %s\n' "$C_RED" "$C_OFF" "$headline" >&2
    if (($# > 0)); then
        printf '\n' >&2
        local line
        for line in "$@"; do printf '  %s\n' "$line" >&2; done
    fi
    printf '\n%s(exit %d; see the exit-code table at the top of %s)%s\n' \
        "$C_DIM" "$code" "${BASH_SOURCE[0]}" "$C_OFF" >&2
    exit "$code"
}

on_err() {
    local rc=$? line=$1
    # Anything that reaches here escaped an explicit check; make that obvious
    # rather than exiting silently under `set -e`.
    printf '\n%sinternal error:%s command failed (rc=%d) at %s:%d\n' \
        "$C_RED" "$C_OFF" "$rc" "${BASH_SOURCE[0]}" "$line" >&2
    printf '  This is a bug in build-kernel.sh -- the failure was not handled.\n' >&2
    exit "$rc"
}
trap 'on_err $LINENO' ERR

usage() {
    sed -n '3,20p' "${BASH_SOURCE[0]}" | sed 's/^#\s\?//'
    exit "${1:-0}"
}

# ---------------------------------------------------------------------------
# Argument parsing
# ---------------------------------------------------------------------------
while (($# > 0)); do
    case "$1" in
        --base)
            [[ -n "${2:-}" ]] || die 1 "--base needs a value" \
                "Valid values: defconfig, running, full"
            BASE="$2"; shift 2 ;;
        --base=*)  BASE="${1#*=}"; shift ;;
        -j|--jobs)
            [[ "${2:-}" =~ ^[0-9]+$ ]] || die 1 "--jobs needs a positive integer" \
                "Got: '${2:-}'"
            JOBS="$2"; shift 2 ;;
        --jobs=*)  JOBS="${1#*=}"; shift ;;
        -w|--workdir)
            [[ -n "${2:-}" ]] || die 1 "--workdir needs a path"
            WORKDIR="$2"; shift 2 ;;
        --workdir=*) WORKDIR="${1#*=}"; shift ;;
        --no-install) DO_INSTALL=0; shift ;;
        --no-keep)    KEEP_TARBALL=0; shift ;;
        -y|--yes)     ASSUME_YES=1; shift ;;
        -h|--help)    usage 0 ;;
        *) die 1 "unknown option: $1" "Run '$0 --help' for usage." ;;
    esac
done

case "$BASE" in
    defconfig|running|full) ;;
    *) die 1 "unknown --base value: '$BASE'" \
        "Valid values:" \
        "  defconfig  'make defconfig', then append the BPF fragment (default)" \
        "  running    this machine's current config, then append the fragment" \
        "  full       the paper's reference 6.18.2 .config, used verbatim" ;;
esac

((DO_INSTALL)) || TOTAL_STEPS=6

TARBALL="${WORKDIR}/linux-${KERNEL_VERSION}.tar.xz"
SRCDIR="${WORKDIR}/linux-${KERNEL_VERSION}"

# ===========================================================================
# 1. Preflight
# ===========================================================================
step "Preflight checks"

[[ "$(uname -s)" == "Linux" ]] || die 2 "this script builds a Linux kernel and must run on Linux" \
    "Detected: $(uname -s)" \
    "On macOS or Windows, use the QEMU path instead:  nix run .#vm"

ARCH="$(uname -m)"
[[ "$ARCH" == "x86_64" ]] || die 2 "unsupported architecture: ${ARCH}" \
    "The configs in kernel/configs/ are x86 configs and this script assumes" \
    "arch/x86/boot/bzImage. Cross-compiling is out of scope here." \
    "" \
    "ae.tex states the artifact targets x86-64 Linux."

# NixOS has no writable /boot layout, no /sbin/installkernel, and an immutable
# /etc; `make install` would half-succeed and leave a confusing mess.
if [[ -e /etc/NIXOS ]] && ((DO_INSTALL)); then
    die 2 "this looks like NixOS, where 'make modules_install install' does not work" \
        "NixOS manages kernels declaratively; there is no /boot layout for" \
        "installkernel to write into." \
        "" \
        "Use the flake instead -- it builds this same kernel from the same config:" \
        "" \
        "  nix build .#kernel     # bzImage + modules in the store" \
        "  nix run   .#vm         # boot it in QEMU, repo mounted at /repo" \
        "" \
        "To boot it on the host, set in your configuration.nix:" \
        "  boot.kernelPackages = pkgs.linuxPackagesFor <this flake>.packages.\${system}.kernel;" \
        "" \
        "Or re-run this script with --no-install to build the tree only."
fi

# Tools. Each entry is  binary:why:debian-package:fedora-package:arch-package
REQUIRED_TOOLS=(
    "tar:unpack the source tarball:tar:tar:tar"
    "xz:decompress the .tar.xz:xz-utils:xz:xz"
    "make:kbuild:make:make:make"
    "gcc:compile the kernel:gcc:gcc:gcc"
    "ld:link the kernel:binutils:binutils:binutils"
    "bison:parse kconfig grammars:bison:bison:bison"
    "flex:lex kconfig grammars:flex:flex:flex"
    "bc:kernel/timeconst arithmetic:bc:bc:bc"
    "depmod:module dependency index:kmod:kmod:kmod"
    "python3:verify the resulting .config:python3:python3:python"
)
# CONFIG_DEBUG_INFO_BTF=y is set by every path this script can take, and it is
# a hard build dependency on pahole -- not a warning, the build stops.
REQUIRED_TOOLS+=("pahole:generate BTF (CONFIG_DEBUG_INFO_BTF=y):dwarves:dwarves:pahole")

missing=()
for entry in "${REQUIRED_TOOLS[@]}"; do
    IFS=: read -r bin why deb rpm arch <<<"$entry"
    command -v "$bin" >/dev/null 2>&1 || missing+=("$bin|$why|$deb|$rpm|$arch")
done

if ((${#missing[@]} > 0)); then
    lines=("The following tools are not on PATH:" "")
    debs=(); rpms=(); arches=()
    for m in "${missing[@]}"; do
        IFS='|' read -r bin why deb rpm arch <<<"$m"
        lines+=("$(printf '  %-10s %s' "$bin" "$why")")
        debs+=("$deb"); rpms+=("$rpm"); arches+=("$arch")
    done
    # Built with pure shell expansion on purpose: an error message must not
    # depend on external commands, which may be exactly what is missing.
    lines+=("" "Install them with one of:" ""
        "  Debian/Ubuntu:  sudo apt-get install -y ${debs[*]}"
        "  Fedora/RHEL:    sudo dnf install -y ${rpms[*]}"
        "  Arch:           sudo pacman -S --needed ${arches[*]}"
        "" "Or get all of them at once with:  nix develop .#kernel")
    die 3 "missing build dependencies" "${lines[@]}"
fi

# A downloader -- either is fine.
DOWNLOADER=""
if command -v curl >/dev/null 2>&1; then DOWNLOADER="curl"
elif command -v wget >/dev/null 2>&1; then DOWNLOADER="wget"
else
    die 3 "neither curl nor wget is available" \
        "One of them is needed to fetch ${KERNEL_URL}" \
        "" \
        "  Debian/Ubuntu:  sudo apt-get install -y curl" \
        "  Fedora/RHEL:    sudo dnf install -y curl" \
        "  Arch:           sudo pacman -S --needed curl"
fi

# A checksum tool -- refusing to skip verification is deliberate.
if command -v sha256sum >/dev/null 2>&1; then SHA_CMD=(sha256sum)
elif command -v shasum >/dev/null 2>&1; then SHA_CMD=(shasum -a 256)
else
    die 3 "no sha256 tool found (sha256sum or shasum)" \
        "The tarball checksum is verified before use; this script will not" \
        "skip that step." \
        "" \
        "  Debian/Ubuntu:  sudo apt-get install -y coreutils"
fi

# Header-only dependencies that kbuild reports late and cryptically. Probe them
# now with a real compile so the failure is actionable.
probe_header() {
    local header="$1" why="$2" deb="$3" rpm="$4" arch="$5"
    printf '#include <%s>\nint main(void){return 0;}\n' "$header" \
        | gcc -x c - -o /dev/null -c 2>/dev/null && return 0
    die 3 "missing development headers: <${header}>" \
        "Needed to ${why}." \
        "" \
        "  Debian/Ubuntu:  sudo apt-get install -y ${deb}" \
        "  Fedora/RHEL:    sudo dnf install -y ${rpm}" \
        "  Arch:           sudo pacman -S --needed ${arch}"
}
probe_header "elf.h"              "build the kbuild host tools"        "libelf-dev"    "elfutils-libelf-devel" "libelf"
probe_header "openssl/opensslv.h" "sign modules and build host tools"  "libssl-dev"    "openssl-devel"         "openssl"

ok "toolchain: gcc $(gcc -dumpversion), $(pahole --version 2>&1 | head -n1), make $(make --version | head -n1 | awk '{print $3}')"

# Config fragment must exist -- the whole point of the exercise.
case "$BASE" in
    defconfig|running)
        [[ -r "$DELTA_CONFIG" ]] || die 1 "cannot read ${DELTA_CONFIG}" \
            "This script must run from inside a bpf-arena-data-structures checkout." \
            "Detected repo root: ${REPO_ROOT}" ;;
    full)
        [[ -r "$FULL_CONFIG" ]] || die 1 "cannot read ${FULL_CONFIG}" \
            "This script must run from inside a bpf-arena-data-structures checkout." \
            "Detected repo root: ${REPO_ROOT}" ;;
esac

if [[ "$BASE" == "running" ]]; then
    RUNNING_CONFIG=""
    for cand in "/boot/config-$(uname -r)" /proc/config.gz; do
        [[ -r "$cand" ]] && { RUNNING_CONFIG="$cand"; break; }
    done
    [[ -n "$RUNNING_CONFIG" ]] || die 7 "--base running, but this machine's kernel config is not readable" \
        "Looked for:" \
        "  /boot/config-$(uname -r)" \
        "  /proc/config.gz  (needs CONFIG_IKCONFIG_PROC=y)" \
        "" \
        "Use '--base defconfig' or '--base full' instead."
    ok "base config: ${RUNNING_CONFIG}"
fi

# Disk space.
mkdir -p "$WORKDIR" 2>/dev/null || die 4 "cannot create work directory: ${WORKDIR}" \
    "Check permissions, or pick another location with --workdir DIR"
avail_gb="$(df -BG --output=avail "$WORKDIR" 2>/dev/null | tail -n1 | tr -dc '0-9')"
if [[ -n "$avail_gb" ]] && ((avail_gb < REQUIRED_GB)); then
    die 4 "not enough free disk space in ${WORKDIR}" \
        "Available: ${avail_gb} GB" \
        "Required:  ${REQUIRED_GB} GB (ae.tex budgets ~20 GB for a kernel build)" \
        "" \
        "Free some space, or build elsewhere:  $0 --workdir /path/with/room"
fi
ok "disk: ${avail_gb:-?} GB available in ${WORKDIR} (need ${REQUIRED_GB})"
ok "jobs: -j${JOBS}"

# ===========================================================================
# 2. Confirm, if we are going to modify the system
# ===========================================================================
if ((DO_INSTALL)) && ((!ASSUME_YES)); then
    cat <<EOF

${C_YEL}This will build Linux ${KERNEL_VERSION}${LOCALVERSION} and then install it:${C_OFF}

  source     ${SRCDIR}
  base       ${BASE}
  modules -> /lib/modules/${KERNEL_VERSION}${LOCALVERSION}/
  kernel  -> /boot/  (via 'make install', which runs your distro's
             installkernel hook and usually regenerates the bootloader menu)

Your current kernel is NOT removed or overwritten -- the new one installs
alongside it under a distinct version string, and you pick it at boot.

Expect 20-60 minutes and a sudo password prompt at the install step.
EOF
    if [[ "$BASE" == "defconfig" ]]; then
        cat <<EOF

${C_RED}Note on --base defconfig:${C_OFF} defconfig is a generic, minimal x86 config.
It may lack drivers your root filesystem or storage controller needs, in which
case the installed kernel will not boot on this machine (your existing kernel
still will). If you intend to boot this on real hardware, prefer:

  $0 --base running

which starts from the config this machine is running now.
EOF
    fi
    printf '\nProceed? [y/N] '
    read -r reply </dev/tty || die 1 "no terminal available to confirm" \
        "Re-run with -y to skip this prompt, or with --no-install."
    [[ "$reply" =~ ^[Yy]$ ]] || { printf 'Aborted.\n'; exit 0; }
fi

# ===========================================================================
# 3. Download
# ===========================================================================
step "Fetching linux-${KERNEL_VERSION}.tar.xz"

verify_tarball() {
    local actual
    actual="$("${SHA_CMD[@]}" "$TARBALL" | awk '{print $1}')"
    [[ "$actual" == "$KERNEL_SHA256" ]]
}

if [[ -f "$TARBALL" ]] && verify_tarball; then
    ok "already downloaded and verified: ${TARBALL}"
else
    [[ -f "$TARBALL" ]] && { warn "existing tarball failed checksum; re-downloading"; rm -f "$TARBALL"; }
    info "${KERNEL_URL}"
    info "~150 MB"
    case "$DOWNLOADER" in
        curl) curl -fL --retry 3 --retry-delay 2 --progress-bar -o "${TARBALL}.part" "$KERNEL_URL" ;;
        wget) wget -q --show-progress --tries=3 -O "${TARBALL}.part" "$KERNEL_URL" ;;
    esac || die 5 "download failed" \
        "URL: ${KERNEL_URL}" \
        "" \
        "Check network access and whether cdn.kernel.org is reachable:" \
        "  ${DOWNLOADER} -I ${KERNEL_URL}" \
        "" \
        "If you already have the tarball, drop it at:" \
        "  ${TARBALL}"
    mv -f "${TARBALL}.part" "$TARBALL"

    if ! verify_tarball; then
        actual="$("${SHA_CMD[@]}" "$TARBALL" | awk '{print $1}')"
        rm -f "$TARBALL"
        die 5 "SHA-256 mismatch -- the download is corrupt or tampered with" \
            "expected: ${KERNEL_SHA256}" \
            "actual:   ${actual}" \
            "" \
            "The bad file has been deleted. Re-run to try again." \
            "Upstream checksums: https://cdn.kernel.org/pub/linux/kernel/v6.x/sha256sums.asc"
    fi
    ok "downloaded and SHA-256 verified"
fi

# ===========================================================================
# 4. Extract
# ===========================================================================
step "Extracting to ${SRCDIR}"

if [[ -f "${SRCDIR}/Makefile" ]] && [[ -d "${SRCDIR}/kernel" ]]; then
    ok "source tree already present, reusing it"
    info "delete ${SRCDIR} to force a clean extract"
else
    if [[ -e "$SRCDIR" ]]; then
        die 6 "${SRCDIR} exists but is not a usable kernel tree" \
            "Refusing to extract on top of it." \
            "" \
            "Remove it and re-run:  rm -rf '${SRCDIR}'"
    fi
    mkdir -p "$SRCDIR"
    tar -xf "$TARBALL" -C "$SRCDIR" --strip-components=1 || die 6 "extract failed" \
        "Tarball: ${TARBALL}" \
        "" \
        "The archive may be truncated. Delete it and re-run:" \
        "  rm -f '${TARBALL}'"
    ok "extracted"
fi

if ((!KEEP_TARBALL)) && [[ -f "$TARBALL" ]]; then
    rm -f "$TARBALL"
    ok "removed tarball (--no-keep); a re-run will download it again"
fi

cd "$SRCDIR"

# ===========================================================================
# 5. Configure
# ===========================================================================
step "Configuring (base: ${BASE})"

case "$BASE" in
    defconfig)
        make defconfig >/dev/null || die 7 "'make defconfig' failed" \
            "Re-run verbosely to see why:  cd '${SRCDIR}' && make defconfig"
        ok "make defconfig"
        ;;
    running)
        cp_cmd=(cp "$RUNNING_CONFIG" .config)
        [[ "$RUNNING_CONFIG" == *.gz ]] && cp_cmd=(sh -c "zcat '$RUNNING_CONFIG' > .config")
        "${cp_cmd[@]}" || die 7 "could not install ${RUNNING_CONFIG} as .config"
        # Distro configs reference signing/trusted keys this build has no
        # access to; clearing them is what every out-of-tree rebuild has to do.
        ./scripts/config --disable SYSTEM_TRUSTED_KEYS \
                         --disable SYSTEM_REVOCATION_KEYS \
                         --set-str CONFIG_MODULE_SIG_KEY "" || true
        ok "seeded from ${RUNNING_CONFIG}"
        ;;
    full)
        cp "$FULL_CONFIG" .config || die 7 "could not install ${FULL_CONFIG} as .config"
        ok "seeded from the reference 6.18.2 config"
        ;;
esac

if [[ "$BASE" != "full" ]]; then
    info "merging ${DELTA_CONFIG#"${REPO_ROOT}/"}"
    # -m merges without running olddefconfig; we do that ourselves next so a
    # failure there is attributable.
    ./scripts/kconfig/merge_config.sh -m .config "$DELTA_CONFIG" >/dev/null \
        || die 7 "merging the BPF config fragment failed" \
            "Fragment: ${DELTA_CONFIG}" \
            "" \
            "Re-run verbosely:" \
            "  cd '${SRCDIR}' && ./scripts/kconfig/merge_config.sh -m .config '${DELTA_CONFIG}'"
    ok "BPF fragment merged"
fi

# Distinct version string, so nothing already installed is overwritten.
./scripts/config --disable LOCALVERSION_AUTO --set-str LOCALVERSION "$LOCALVERSION" \
    || die 7 "could not set CONFIG_LOCALVERSION"

# olddefconfig resolves everything the fragment implies and takes defaults for
# symbols the base config never heard of. Never plain 'oldconfig' -- that
# prompts, and there is nobody here to answer.
make olddefconfig >/dev/null || die 7 "'make olddefconfig' failed" \
    "Re-run verbosely:  cd '${SRCDIR}' && make olddefconfig"
ok "olddefconfig resolved"

# ===========================================================================
# 6. Verify the config actually satisfies the artifact
# ===========================================================================
step "Verifying .config against the artifact's requirements"

if ! python3 "$CHECK_KCONFIG" .config; then
    die 8 "the resulting .config does not satisfy the artifact's requirements" \
        "check_kconfig.py named the missing symbols above." \
        "" \
        "This usually means a symbol in the fragment was overridden by a" \
        "dependency it does not select. Inspect and fix interactively:" \
        "" \
        "  cd '${SRCDIR}' && make menuconfig" \
        "" \
        "then re-run this script -- it reuses the extracted tree."
fi
ok "all requirements met"

# ===========================================================================
# 7. Build
# ===========================================================================
step "Building (-j${JOBS}) -- this takes 20-60 minutes"

BUILD_LOG="${WORKDIR}/build-${KERNEL_VERSION}.log"
info "full log: ${BUILD_LOG}"

if ! make -j"$JOBS" 2>&1 | tee "$BUILD_LOG" | grep -E --line-buffered '^\s*(LD|AR|BTF|LINK|OBJCOPY|Kernel:)' ; then
    # tee's status, not grep's -- grep exits 1 when it simply matched nothing.
    if [[ "${PIPESTATUS[0]}" -ne 0 ]]; then
        die 9 "kernel compile failed" \
            "Last errors from ${BUILD_LOG}:" \
            "" \
            "$(grep -iE 'error:|Error [0-9]' "$BUILD_LOG" | tail -n 10 | sed 's/^/  /')" \
            "" \
            "Full log: ${BUILD_LOG}" \
            "Retry serially for a clearer message:  cd '${SRCDIR}' && make -j1"
    fi
fi
[[ -f arch/x86/boot/bzImage ]] || die 9 "build finished but arch/x86/boot/bzImage is missing" \
    "Full log: ${BUILD_LOG}"

ok "bzImage: $(du -h arch/x86/boot/bzImage | cut -f1)"

if [[ -f vmlinux ]] && command -v readelf >/dev/null 2>&1; then
    if readelf -S vmlinux 2>/dev/null | grep -q '\.BTF'; then
        ok "BTF section present in vmlinux"
    else
        warn "no .BTF section in vmlinux -- libbpf will not be able to attach the"
        warn "lsm.s and uprobe.s programs. Check that pahole ran correctly."
    fi
fi

# ===========================================================================
# 8. Install
# ===========================================================================
if ((!DO_INSTALL)); then
    cat <<EOF

${C_GRN}Build complete.${C_OFF} Nothing outside the source tree was modified.

  bzImage  ${SRCDIR}/arch/x86/boot/bzImage
  config   ${SRCDIR}/.config

To install it later:
  cd '${SRCDIR}' && sudo make modules_install install
EOF
    exit 0
fi

step "Installing"

SUDO=""
if [[ "$(id -u)" -ne 0 ]]; then
    command -v sudo >/dev/null 2>&1 || die 10 "not root, and sudo is not available" \
        "Re-run this script as root, or install only the build with --no-install" \
        "and do the install step yourself:" \
        "  cd '${SRCDIR}' && make modules_install install"
    SUDO="sudo"
    info "requesting sudo for modules_install and install"
fi

$SUDO make modules_install || die 10 "'make modules_install' failed" \
    "Target: /lib/modules/${KERNEL_VERSION}${LOCALVERSION}/" \
    "" \
    "Common causes: /lib/modules on a full or read-only filesystem." \
    "Check with:  df -h /lib/modules"

$SUDO make install || die 10 "'make install' failed" \
    "This step calls your distribution's installkernel hook, which copies the" \
    "kernel into /boot and usually regenerates the bootloader configuration." \
    "" \
    "Common causes:" \
    "  - /boot is too small (check:  df -h /boot)" \
    "  - no /sbin/installkernel and no kernel-install on this distro" \
    "" \
    "The modules are already installed, so you can finish by hand:" \
    "  cd '${SRCDIR}'" \
    "  sudo cp arch/x86/boot/bzImage /boot/vmlinuz-${KERNEL_VERSION}${LOCALVERSION}" \
    "  sudo cp System.map /boot/System.map-${KERNEL_VERSION}${LOCALVERSION}" \
    "  sudo cp .config /boot/config-${KERNEL_VERSION}${LOCALVERSION}" \
    "  # then regenerate your bootloader menu (update-grub, grub2-mkconfig, ...)"

cat <<EOF

${C_GRN}Installed Linux ${KERNEL_VERSION}${LOCALVERSION}.${C_OFF}

  modules  /lib/modules/${KERNEL_VERSION}${LOCALVERSION}/
  kernel   /boot/vmlinuz-${KERNEL_VERSION}${LOCALVERSION}
  config   /boot/config-${KERNEL_VERSION}${LOCALVERSION}

If your bootloader menu was not regenerated automatically, do it now:

  Debian/Ubuntu:  sudo update-grub
  Fedora/RHEL:    sudo grubby --info=ALL   # verify the entry exists
  Arch:           sudo grub-mkconfig -o /boot/grub/grub.cfg

Then reboot into it and confirm:

  uname -r                                  # ${KERNEL_VERSION}${LOCALVERSION}
  cat /sys/kernel/security/lsm              # must contain 'bpf'
  python3 scripts/check_kconfig.py          # OK: all requirements met
  cd '${REPO_ROOT}' && make && sudo python3 scripts/runner.py

If 'bpf' is missing from the LSM list after reboot, add it to your kernel
command line -- no rebuild needed:

  lsm=landlock,lockdown,yama,loadpin,safesetid,integrity,bpf
EOF
