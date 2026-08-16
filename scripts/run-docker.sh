#!/usr/bin/env bash
set -Eeuo pipefail

# USAGE-START
# scripts/run-docker.sh -- run this artifact in a BPF-capable container
#
# `docker run -v "$PWD:/artifact" bpf-arena-ds` gives you the toolchain and
# nothing else: the relays fail there because the container is isolated from
# the very things they need, not because the image is missing a package.
# Being root inside the container is not enough.  This wrapper adds the four
# things a plain `docker run` leaves out:
#
#   1. privileges -- CAP_BPF/CAP_PERFMON for bpf(2), and the default seccomp
#      profile lifted, since it blocks bpf(2) outright
#   2. /sys/kernel/debug -- trace_pipe, which runner.py reads and clears
#   3. /sys/fs/bpf, /sys/kernel/security -- bpffs, and the LSM list that
#      check_kconfig.py reads to prove the BPF LSM is actually active
#   4. /boot -- so check_kconfig.py can find /boot/config-$(uname -r); without
#      it the pipeline stops on "no config given and none found"
#
# It cannot supply a kernel.  A container shares the host's, so the *host* must
# already satisfy scripts/check_kconfig.py; the preflight below says so up
# front instead of letting eight relays fail one at a time.
#
# usage: scripts/run-docker.sh [options] [-- command ...]
#
#   with no command, you get an interactive shell in /artifact; with one, it
#   runs and the container exits, e.g.
#
#       scripts/run-docker.sh -- python3 scripts/run_all.py
#
#       --restricted     instead of --privileged, drop every capability and add
#                        back only BPF, PERFMON, SYS_ADMIN, DAC_OVERRIDE and
#                        SYS_RESOURCE, with seccomp and AppArmor unconfined
#       --image NAME     image to run (default: bpf-arena-ds)
#       --build          docker build the image first
#       --no-check       run even if the host preflight fails
#       --dry-run        print the docker command instead of running it
#   -h, --help           this text
#
# exit codes:
#   2  unsupported platform (not Linux)
#   3  docker missing, or the daemon is unreachable
#   4  the host kernel cannot run the relays (see the report)
#   5  the image does not exist and --build was not given
# USAGE-END

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------
REPO_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"

# --restricted drops every capability and adds back only these, so each one is
# here because removing it was observed to break something:
#
#   BPF, PERFMON    load the programs and open the perf events
#   SYS_ADMIN       the documented BPF+PERFMON pair is *not* enough: with only
#                   those two the programs load and then attach dies on
#                   "failed to create uprobe '/proc/self/exe:0x8174' perf
#                   event: -EACCES"
#   DAC_OVERRIDE    root in the container is uid 0, but the bind-mounted
#                   checkout belongs to your host user; without it runner.py's
#                   workers fail on "touch: cannot touch 'file0.tmp':
#                   Permission denied" and no CSV is written
#   SYS_RESOURCE    RLIMIT_MEMLOCK, which libbpf still raises on its legacy
#                   path.  The relays passed without it on 6.18 (BPF memory is
#                   memcg-accounted there); kept for older kernels, where that
#                   limit does gate map creation.
RESTRICTED_CAPS=(BPF PERFMON SYS_ADMIN DAC_OVERRIDE SYS_RESOURCE)

# ---------------------------------------------------------------------------
# Defaults, overridable by flags
# ---------------------------------------------------------------------------
IMAGE="bpf-arena-ds"
RESTRICTED=0
DO_BUILD=0
NO_CHECK=0
DRY_RUN=0

# ---------------------------------------------------------------------------
# Output helpers
# ---------------------------------------------------------------------------
if [[ -t 1 ]] && [[ -z "${NO_COLOR:-}" ]]; then
    C_RED=$'\033[1;31m'; C_YEL=$'\033[1;33m'; C_GRN=$'\033[1;32m'
    C_BLU=$'\033[1;34m'; C_DIM=$'\033[2m'; C_OFF=$'\033[0m'
else
    C_RED=""; C_YEL=""; C_GRN=""; C_BLU=""; C_DIM=""; C_OFF=""
fi

step() { printf '\n%s==> %s%s\n' "$C_BLU" "$*" "$C_OFF"; }
info() { printf '    %s\n' "$*"; }
warn() { printf '%swarning:%s %s\n' "$C_YEL" "$C_OFF" "$*" >&2; }
ok()   { printf '%s    ok:%s %s\n' "$C_GRN" "$C_OFF" "$*"; }
bad()  { printf '%s   bad:%s %s\n' "$C_RED" "$C_OFF" "$*"; }

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

usage() {
    sed -n '/^# USAGE-START/,/^# USAGE-END/p' "${BASH_SOURCE[0]}" \
        | sed -e '1d' -e '$d' -e 's/^# \{0,1\}//'
    exit "${1:-0}"
}

# /proc/mounts rather than mountpoint(1), which is in util-linux and not
# guaranteed to be installed on a minimal host.
is_mounted() { awk -v d="$1" '$2 == d { found = 1 } END { exit !found }' /proc/mounts; }

# ---------------------------------------------------------------------------
# Flags
# ---------------------------------------------------------------------------
need_arg() { [[ $# -ge 2 && -n "$2" ]] || { printf '%s needs an argument\n\n' "$1" >&2; usage 1; }; }

COMMAND=()
while (($# > 0)); do
    case "$1" in
        --restricted) RESTRICTED=1 ;;
        --image)      need_arg "$@"; IMAGE="$2"; shift ;;
        --image=*)    IMAGE="${1#*=}" ;;
        --build)      DO_BUILD=1 ;;
        --no-check)   NO_CHECK=1 ;;
        --dry-run)    DRY_RUN=1 ;;
        -h|--help)    usage 0 ;;
        --)           shift; COMMAND=("$@"); break ;;
        *) printf 'unknown option: %s\n\n' "$1" >&2; usage 1 ;;
    esac
    shift
done

# ---------------------------------------------------------------------------
# Step 1: docker itself
# ---------------------------------------------------------------------------
[[ "$(uname -s)" == "Linux" ]] || die 2 \
    "this wrapper only works on a Linux host (this one is $(uname -s))" \
    "A container shares the host's kernel, so on macOS or Windows the relays" \
    "would run against Docker Desktop's LinuxKit VM, not a kernel built from" \
    "kernel/configs/delta-bpf-arena.config.  Use a Linux machine, or the Nix" \
    "route: nix run .#vm"

step "Checking Docker"
command -v docker >/dev/null 2>&1 || die 3 \
    "docker is not installed" \
    "On Ubuntu: scripts/install-docker.sh" \
    "Otherwise see DOCKER_NIX_INSTALL.md"

if ! docker info >/dev/null 2>&1; then
    die 3 "the Docker daemon is unreachable" \
        "Either it is not running (sudo systemctl start docker), or your user" \
        "is not in the docker group (log out and back in after being added)." \
        "Check with: docker info"
fi
ok "docker $(docker version --format '{{.Server.Version}}' 2>/dev/null || echo '(version unknown)')"

# ---------------------------------------------------------------------------
# Step 2: host preflight
#
# Everything the container cannot provide for itself.  Failures are collected
# rather than fatal-on-first, so one run tells you everything that is wrong.
# ---------------------------------------------------------------------------
step "Checking the host kernel ($(uname -r))"

PROBLEMS=()

if [[ -r /sys/kernel/security/lsm ]]; then
    if grep -qw bpf /sys/kernel/security/lsm; then
        ok "BPF LSM active: $(cat /sys/kernel/security/lsm)"
    else
        bad "BPF LSM not active: $(cat /sys/kernel/security/lsm)"
        PROBLEMS+=("The BPF LSM is not in the active list, so every lsm.s program"
                   "  will fail to attach.  An lsm= kernel command line replaces"
                   "  CONFIG_LSM outright; get the line to add with:"
                   "    python3 scripts/check_kconfig.py --print-lsm-fix")
    fi
elif [[ -d /sys/kernel/security ]]; then
    warn "/sys/kernel/security/lsm is not readable by this user; deferring to the container"
else
    bad "securityfs is not mounted"
    PROBLEMS+=("securityfs is not mounted, so neither this script nor"
               "  check_kconfig.py can confirm the BPF LSM is active:"
               "    sudo mount -t securityfs none /sys/kernel/security")
fi

if [[ -e /sys/kernel/btf/vmlinux ]]; then
    ok "host BTF present (/sys/kernel/btf/vmlinux)"
else
    bad "no host BTF"
    PROBLEMS+=("/sys/kernel/btf/vmlinux is missing, so the lsm.s and uprobe.s"
               "  programs cannot be attached.  The host kernel needs"
               "  CONFIG_DEBUG_INFO_BTF=y -- it is not something the image can add.")
fi

if is_mounted /sys/kernel/debug || [[ -d /sys/kernel/debug/tracing ]]; then
    ok "debugfs mounted (/sys/kernel/debug)"
else
    bad "debugfs is not mounted"
    PROBLEMS+=("runner.py reads and clears /sys/kernel/debug/tracing/trace_pipe:"
               "    sudo mount -t debugfs none /sys/kernel/debug")
fi

if is_mounted /sys/fs/bpf; then
    ok "bpffs mounted (/sys/fs/bpf)"
else
    bad "bpffs is not mounted"
    PROBLEMS+=("bpffs is where pinned maps and programs live:"
               "    sudo mount -t bpf none /sys/fs/bpf")
fi

# check_kconfig.py wants /boot/config-$(uname -r) and falls back to
# /proc/config.gz.  /boot is commonly 0700 root, so "cannot read it as this
# user" is not a failure -- the container runs as root.  Only "neither source
# exists at all" is.
KERNEL_CONFIG_SRC=""
if [[ -r "/boot/config-$(uname -r)" ]]; then
    KERNEL_CONFIG_SRC="/boot/config-$(uname -r)"
elif [[ -r /proc/config.gz ]]; then
    KERNEL_CONFIG_SRC="/proc/config.gz"
elif [[ -d /boot ]] && ! [[ -x /boot ]]; then
    KERNEL_CONFIG_SRC="/boot (not readable by this user; the container is root)"
fi

if [[ -n "$KERNEL_CONFIG_SRC" ]]; then
    ok "kernel config readable: $KERNEL_CONFIG_SRC"
else
    bad "no kernel config to check"
    PROBLEMS+=("check_kconfig.py needs /boot/config-\$(uname -r) or /proc/config.gz;"
               "  neither exists on this host.  Point it at a copy instead:"
               "    scripts/run-docker.sh -- python3 scripts/run_all.py --config path/to/.config")
fi

if ((${#PROBLEMS[@]} > 0)); then
    if ((NO_CHECK)); then
        warn "host preflight failed; continuing because --no-check was given"
    else
        die 4 "this host cannot run the relays" "${PROBLEMS[@]}" \
            "" \
            "A container cannot fix any of these: it shares the host's kernel." \
            "Build and boot a compliant one first (README step 2):" \
            "  ./scripts/build-kernel.sh --base running -y" \
            "" \
            "To run anyway -- for the compile and usertest halves, which do not" \
            "need any of the above -- pass --no-check, or skip this wrapper:" \
            "  docker run --rm -it -v \"\$PWD:/artifact\" $IMAGE"
    fi
fi

# ---------------------------------------------------------------------------
# Step 3: the image
# ---------------------------------------------------------------------------
if ((DO_BUILD)); then
    step "Building $IMAGE"
    if ((DRY_RUN)); then
        info "+ docker build -t $IMAGE $REPO_ROOT"
    else
        docker build -t "$IMAGE" "$REPO_ROOT"
    fi
elif ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    die 5 "no such image: $IMAGE" \
        "Build it first:" \
        "  scripts/run-docker.sh --build" \
        "or by hand:" \
        "  docker build -t $IMAGE $REPO_ROOT"
fi

# ---------------------------------------------------------------------------
# Step 4: run
# ---------------------------------------------------------------------------
DOCKER_ARGS=(run --rm)

# -t only when there is a terminal to attach it to, so this stays usable from
# a script or a CI job.
[[ -t 0 ]] && DOCKER_ARGS+=(-i)
[[ -t 1 ]] && DOCKER_ARGS+=(-t)

if ((RESTRICTED)); then
    DOCKER_ARGS+=(--cap-drop ALL)
    for cap in "${RESTRICTED_CAPS[@]}"; do
        DOCKER_ARGS+=(--cap-add "$cap")
    done
    # The default seccomp profile blocks bpf(2) outright, and the default
    # AppArmor profile blocks writes to the debugfs mount below; both are
    # denials no capability can lift.
    DOCKER_ARGS+=(--security-opt seccomp=unconfined --security-opt apparmor=unconfined)
else
    DOCKER_ARGS+=(--privileged)
fi

# The uprobes attach to the touch(1) processes runner.py spawns; a shared PID
# namespace keeps their PIDs the same on both sides, which is what the trace
# validation compares against.
DOCKER_ARGS+=(--pid=host)

DOCKER_ARGS+=(
    -v "$REPO_ROOT:/artifact"
    -v /sys/kernel/debug:/sys/kernel/debug
    -v /sys/fs/bpf:/sys/fs/bpf
    -w /artifact
)
[[ -d /sys/kernel/security ]] && DOCKER_ARGS+=(-v /sys/kernel/security:/sys/kernel/security:ro)
[[ -d /boot ]] && DOCKER_ARGS+=(-v /boot:/boot:ro)

DOCKER_ARGS+=("$IMAGE")
((${#COMMAND[@]} > 0)) && DOCKER_ARGS+=("${COMMAND[@]}")

step "Starting $IMAGE ($( ((RESTRICTED)) && echo restricted || echo privileged ))"
info "docker ${DOCKER_ARGS[*]}"

if ((DRY_RUN)); then
    exit 0
fi

if ((${#COMMAND[@]} == 0)); then
    info ""
    info "You are root in /artifact.  The pipeline is:"
    info "  make                        # if build/ is empty"
    info "  python3 scripts/run_all.py  # no sudo needed, you are already root"
fi

exec docker "${DOCKER_ARGS[@]}"
