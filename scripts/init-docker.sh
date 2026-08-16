#!/usr/bin/env bash
set -Eeuo pipefail

# USAGE-START
# scripts/init-docker.sh -- run this artifact in a BPF-capable container
#
# A plain `docker run` cannot load BPF programs however root you are inside it.
# This wrapper adds what it leaves out: the capabilities (and the seccomp
# profile lifted, since it blocks bpf(2)), the /sys/kernel/debug, /sys/fs/bpf,
# /sys/kernel/security and /boot mounts, and the host PID namespace.
#
# It cannot supply a kernel: a container shares the host's, so the host must
# already satisfy scripts/check_kconfig.py -- which the preflight below checks.
#
# usage: scripts/init-docker.sh [options] [-- command ...]
#
#   with no command you get an interactive shell in /artifact; with one, it
#   runs and the container exits:
#
#       scripts/init-docker.sh -- python3 scripts/run_all.py
#
#       --restricted     drop every capability and add back only BPF, PERFMON,
#                        SYS_ADMIN, DAC_OVERRIDE and SYS_RESOURCE, instead of
#                        --privileged
#       --image NAME     image to run (default: bpf-arena-ds)
#       --build          docker build the image first
#       --no-check       run even if the host preflight fails
#       --dry-run        print the docker command instead of running it
#   -h, --help           this text
#
# exit codes:
#   2  not Linux    3  docker missing or unreachable
#   4  the host kernel cannot run the relays    5  no such image
# USAGE-END

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------
REPO_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"

# Each of these was observed to be load-bearing: BPF/PERFMON load the programs
# and open the perf events, SYS_ADMIN is additionally required for the uprobe
# perf events, DAC_OVERRIDE lets uid 0 write the bind-mounted checkout, and
# SYS_RESOURCE covers RLIMIT_MEMLOCK on kernels older than 6.18.
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
    "On macOS/Windows the container runs against Docker Desktop's LinuxKit VM," \
    "not a kernel you built."

step "Checking Docker"
command -v docker >/dev/null 2>&1 || die 3 \
    "docker is not installed" \
    "On Ubuntu: scripts/install-docker.sh" \
    "Otherwise see DOCKER_NIX_INSTALL.md"

if ! docker info >/dev/null 2>&1; then
    die 3 "the Docker daemon is unreachable" \
        "Start it (sudo systemctl start docker), or check that you are in the" \
        "docker group. Check with: docker info"
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
        PROBLEMS+=("The BPF LSM is inactive, so no lsm.s program can attach. Get the"
                   "  kernel command line to add with:"
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
    PROBLEMS+=("/sys/kernel/btf/vmlinux is missing; the host kernel needs"
               "  CONFIG_DEBUG_INFO_BTF=y.")
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
               "  neither exists here. Point it at a copy with --config path/to/.config")
fi

if ((${#PROBLEMS[@]} > 0)); then
    if ((NO_CHECK)); then
        warn "host preflight failed; continuing because --no-check was given"
    else
        die 4 "this host cannot run the relays" "${PROBLEMS[@]}" \
            "" \
            "A container shares the host's kernel and cannot fix these. Build and" \
            "boot one first:  ./scripts/build-kernel.sh --base running -y" \
            "For the compile and usertest halves only, pass --no-check."
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
        "  scripts/init-docker.sh --build" \
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
    info "root in /artifact:  make && python3 scripts/run_all.py"
fi

exec docker "${DOCKER_ARGS[@]}"
