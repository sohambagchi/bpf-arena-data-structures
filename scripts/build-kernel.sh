#!/usr/bin/env bash
set -Eeuo pipefail

# build-kernel.sh -- download, configure, build and install Linux 6.18.44
#                    with this repo's BPF-arena config fragment.
#
# usage: scripts/build-kernel.sh [options]
#
#   --base defconfig   'make defconfig', then merge the fragment (default)
#   --base running     this machine's current config, then merge the fragment
#   --base full        the paper's reference 6.18.2 .config, verbatim
#   --config PATH      an explicit .config (or config.gz) as the base, then
#                      merge the fragment. Use this inside a container, where
#                      the host's config is not visible: see KERNEL_CONFIG.
#   -j, --jobs N       parallelism (default: nproc)
#   -w, --workdir DIR  where to download and build (default: kernel/build)
#   --no-install       build only; touch nothing outside the source tree
#   --no-keep          delete the tarball after extracting
#   -y, --yes          skip the confirmation prompt
#   -h, --help         this text
#
# environment:
#   KERNEL_CONFIG      same as --config; the flag wins if both are given
#
# exit codes:
#   1 usage      2 platform    3 dependencies   4 disk      5 download
#   6 extract    7 configure   8 config check   9 compile  10 install
#
# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------
KERNEL_VERSION="6.18.44"
KERNEL_URL="https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-${KERNEL_VERSION}.tar.xz"
KERNEL_SHA256="0f72d938f06828e82c90405174fe572287db7bfe089e2fc46572a99a7f240d43"

LOCALVERSION="-bpf-arena"

REQUIRED_GB=22

REPO_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
DELTA_CONFIG="${REPO_ROOT}/kernel/configs/delta-bpf-arena.config"
FULL_CONFIG="${REPO_ROOT}/kernel/configs/full-6.18.2-bpf.config"
CHECK_KCONFIG="${REPO_ROOT}/scripts/check_kconfig.py"

# ---------------------------------------------------------------------------
# Defaults, overridable by flags
# ---------------------------------------------------------------------------
BASE="defconfig"
BASE_CONFIG="${KERNEL_CONFIG:-}"
[[ -n "$BASE_CONFIG" ]] && BASE="file"
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
TOTAL_STEPS=7
step()  { STEP_N=$((STEP_N + 1)); printf '\n%s==> [%d/%d] %s%s\n' "$C_BLU" "$STEP_N" "$TOTAL_STEPS" "$*" "$C_OFF"; }
info()  { printf '    %s\n' "$*"; }
warn()  { printf '%swarning:%s %s\n' "$C_YEL" "$C_OFF" "$*" >&2; }
ok()    { printf '%s    ok:%s %s\n' "$C_GRN" "$C_OFF" "$*"; }

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
    printf '\n%sinternal error:%s command failed (rc=%d) at %s:%d\n' \
        "$C_RED" "$C_OFF" "$rc" "${BASH_SOURCE[0]}" "$line" >&2
    printf '  This is a bug in build-kernel.sh -- the failure was not handled.\n' >&2
    exit "$rc"
}
trap 'on_err $LINENO' ERR

usage() {
    sed -n '4,27p' "${BASH_SOURCE[0]}" | sed 's/^#\s\?//'
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
        --config)
            [[ -n "${2:-}" ]] || die 1 "--config needs a path to a kernel .config"
            BASE="file"; BASE_CONFIG="$2"; shift 2 ;;
        --config=*) BASE="file"; BASE_CONFIG="${1#*=}"; shift ;;
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
    defconfig|running|full|file) ;;
    *) die 1 "unknown --base value: '$BASE'" \
        "Valid values:" \
        "  defconfig  'make defconfig', then append the BPF fragment (default)" \
        "  running    this machine's current config, then append the fragment" \
        "  full       the paper's reference 6.18.2 .config, used verbatim" \
        "" \
        "To start from a config that is not one of those, pass it directly:" \
        "  $0 --config /path/to/config" ;;
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
    lines+=("" "Install them with one of:" ""
        "  Debian/Ubuntu:  sudo apt-get install -y ${debs[*]}"
        "  Fedora/RHEL:    sudo dnf install -y ${rpms[*]}"
        "  Arch:           sudo pacman -S --needed ${arches[*]}"
        "" "Or get all of them at once with:  nix develop")
    die 3 "missing build dependencies" "${lines[@]}"
fi

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

if command -v sha256sum >/dev/null 2>&1; then SHA_CMD=(sha256sum)
elif command -v shasum >/dev/null 2>&1; then SHA_CMD=(shasum -a 256)
else
    die 3 "no sha256 tool found (sha256sum or shasum)" \
        "The tarball checksum is verified before use; this script will not" \
        "skip that step." \
        "" \
        "  Debian/Ubuntu:  sudo apt-get install -y coreutils"
fi

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
probe_header "gelf.h"             "build objtool and other kbuild host tools" "libelf-dev" "elfutils-libelf-devel" "libelf"
probe_header "openssl/opensslv.h" "sign modules and build host tools"  "libssl-dev"    "openssl-devel"         "openssl"

ok "toolchain: gcc $(gcc -dumpversion), $(pahole --version 2>&1 | awk 'NR == 1 { first = $0 } END { print first }'), make $(make --version | awk 'NR == 1 { version = $3 } END { print version }')"

case "$BASE" in
    defconfig|running|file)
        [[ -r "$DELTA_CONFIG" ]] || die 1 "cannot read ${DELTA_CONFIG}" \
            "This script must run from inside a bpf-arena-data-structures checkout." \
            "Detected repo root: ${REPO_ROOT}" ;;
    full)
        [[ -r "$FULL_CONFIG" ]] || die 1 "cannot read ${FULL_CONFIG}" \
            "This script must run from inside a bpf-arena-data-structures checkout." \
            "Detected repo root: ${REPO_ROOT}" ;;
esac

# True for Docker, Podman and most other container runtimes. A container shares
# the host's kernel, so 'uname -r' names a kernel whose /boot is not mounted.
in_container() {
    [[ -f /.dockerenv ]] || [[ -f /run/.containerenv ]] \
        || [[ -n "${container:-}" ]] \
        || grep -qE '(docker|containerd|libpod|kubepods)' /proc/1/cgroup 2>/dev/null
}

# Absolute, because step 5 runs after 'cd $SRCDIR'.
if [[ -n "$BASE_CONFIG" ]] && [[ -e "$BASE_CONFIG" ]]; then
    BASE_CONFIG="$(cd -- "$(dirname -- "$BASE_CONFIG")" && pwd)/$(basename -- "$BASE_CONFIG")"
fi

if [[ "$BASE" == "file" ]]; then
    [[ -r "$BASE_CONFIG" ]] || die 7 "cannot read the config given with --config: ${BASE_CONFIG}" \
        "Give a path to a kernel .config, or to a gzipped one (config.gz)." \
        "" \
        "Inside a container, copy the host's config in before building:" \
        "  # on the host" \
        "  cp \"/boot/config-\$(uname -r)\" ./host.config" \
        "  # in the container, with the repo bind-mounted" \
        "  ./scripts/build-kernel.sh --config ./host.config --no-install"
    RUNNING_CONFIG="$BASE_CONFIG"
    ok "base config: ${RUNNING_CONFIG}"
fi

if [[ "$BASE" == "running" ]]; then
    RUNNING_CONFIG=""
    # KERNEL_CONFIG and /host/boot let a container be pointed at the host's
    # config, which it cannot otherwise see. /proc/config.gz does work in a
    # container -- same kernel -- but only if the host set CONFIG_IKCONFIG_PROC.
    for cand in "${BASE_CONFIG:-}" \
                "/host/boot/config-$(uname -r)" \
                "/boot/config-$(uname -r)" \
                /proc/config.gz \
                "/lib/modules/$(uname -r)/build/.config"; do
        [[ -n "$cand" ]] && [[ -r "$cand" ]] && { RUNNING_CONFIG="$cand"; break; }
    done
    if [[ -z "$RUNNING_CONFIG" ]] && in_container; then
        die 7 "--base running does not work in a container: the host's config is not visible here" \
            "A container shares the host's kernel but not its /boot, so" \
            "'/boot/config-\$(uname -r)' names a file that is not mounted." \
            "" \
            "Pick one:" \
            "" \
            "  1. Mount the host's /boot read-only when starting the container:" \
            "       docker run --rm -it -v \"\$PWD:/artifact\" -v /boot:/host/boot:ro bpf-arena-ds" \
            "     then '--base running' finds it at /host/boot/ by itself." \
            "" \
            "  2. Copy the config in and point at it:" \
            "       cp \"/boot/config-\$(uname -r)\" ./host.config     # on the host" \
            "       ./scripts/build-kernel.sh --config ./host.config  # in the container" \
            "" \
            "  3. Use '--base full' (the paper's reference config) or '--base defconfig'," \
            "     which need nothing from the host." \
            "" \
            "Note that installing is a host operation too -- add --no-install here and" \
            "boot the resulting bzImage under QEMU, or install it from the host."
    fi
    [[ -n "$RUNNING_CONFIG" ]] || die 7 "--base running, but this machine's kernel config is not readable" \
        "Looked for:" \
        "  /boot/config-$(uname -r)" \
        "  /proc/config.gz  (needs CONFIG_IKCONFIG_PROC=y)" \
        "  /lib/modules/$(uname -r)/build/.config" \
        "" \
        "Point at it directly with '--config PATH' if it lives somewhere else," \
        "or use '--base defconfig' or '--base full' instead."
    ok "base config: ${RUNNING_CONFIG}"
fi

# Bind-mounting the host's /boot and /lib/modules into the container makes the
# install step meaningful; without that it writes into a filesystem that is
# discarded when the container exits.
host_dir_mounted() {
    local dir="$1" root_dev dir_dev
    root_dev="$(stat -c %d / 2>/dev/null)" || return 1
    dir_dev="$(stat -c %d "$dir" 2>/dev/null)" || return 1
    [[ "$dir_dev" != "$root_dev" ]]
}

if ((DO_INSTALL)) && in_container \
   && ! { host_dir_mounted /boot && host_dir_mounted /lib/modules; }; then
    die 2 "refusing to install a kernel from inside a container" \
        "'make modules_install install' would write to the container's own" \
        "/lib/modules and /boot, which vanish with the container -- and the" \
        "container cannot reboot the host into the result anyway." \
        "" \
        "If you did mean to install onto the host, bind-mount both first:" \
        "  docker run --rm -it -v \"\$PWD:/artifact\" \\" \
        "    -v /boot:/boot -v /lib/modules:/lib/modules bpf-arena-ds" \
        "" \
        "Build here and install from the host:" \
        "  ./scripts/build-kernel.sh --base ${BASE} --no-install --workdir /artifact/kernel/build" \
        "" \
        "then, on the host, in the same directory:" \
        "  cd kernel/build/linux-${KERNEL_VERSION} && sudo make modules_install && sudo make install" \
        "" \
        "Or boot the bzImage under QEMU without installing it at all."
fi

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
  base       ${BASE}${RUNNING_CONFIG:+  (${RUNNING_CONFIG})}
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
    running|file)
        cp_cmd=(cp "$RUNNING_CONFIG" .config)
        [[ "$RUNNING_CONFIG" == *.gz ]] && cp_cmd=(sh -c "zcat '$RUNNING_CONFIG' > .config")
        "${cp_cmd[@]}" || die 7 "could not install ${RUNNING_CONFIG} as .config"
        ./scripts/config --disable SYSTEM_TRUSTED_KEYS \
                         --disable SYSTEM_REVOCATION_KEYS \
                         --disable MODULE_SIG_ALL \
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
    ./scripts/kconfig/merge_config.sh -m .config "$DELTA_CONFIG" >/dev/null \
        || die 7 "merging the BPF config fragment failed" \
            "Fragment: ${DELTA_CONFIG}" \
            "" \
            "Re-run verbosely:" \
            "  cd '${SRCDIR}' && ./scripts/kconfig/merge_config.sh -m .config '${DELTA_CONFIG}'"
    ok "BPF fragment merged"
fi

./scripts/config --disable LOCALVERSION_AUTO --set-str LOCALVERSION "$LOCALVERSION" \
    || die 7 "could not set CONFIG_LOCALVERSION"

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
    if readelf -S vmlinux 2>/dev/null | grep '\.BTF' >/dev/null; then
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
  cd '${SRCDIR}' && sudo make -j\$(nproc) modules_install && sudo make install
EOF
    exit 0
fi

step "Installing"

SUDO=""
if [[ "$(id -u)" -ne 0 ]]; then
    command -v sudo >/dev/null 2>&1 || die 10 "not root, and sudo is not available" \
        "Re-run this script as root, or install only the build with --no-install" \
        "and do the install step yourself:" \
        "  cd '${SRCDIR}' && make -j\$(nproc) modules_install && make install"
    SUDO="sudo"
    info "requesting sudo for modules_install and install"
fi

$SUDO make -j"$JOBS" modules_install || die 10 "'make modules_install' failed" \
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
  cd '${REPO_ROOT}' && make && sudo python3 scripts/run_all.py

If 'bpf' is missing from the LSM list after reboot, add it to your kernel
command line -- no rebuild needed:

  lsm=landlock,lockdown,yama,loadpin,safesetid,integrity,bpf
EOF
