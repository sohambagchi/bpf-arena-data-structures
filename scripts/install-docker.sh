#!/usr/bin/env bash
set -Eeuo pipefail

# USAGE-START
# scripts/install-docker.sh -- install Docker Engine on Ubuntu for this artifact
#
# Follows https://docs.docker.com/engine/install/ubuntu/ (the apt repository
# method) plus https://docs.docker.com/engine/install/linux-postinstall/.
# Every step is skipped when it is already done, so re-running is safe.
#
# usage: scripts/install-docker.sh [options]
#
#   -y, --yes            do not ask for confirmation
#       --version VER    install a specific Docker Engine version, either a
#                        plain version (28.3.0) or a full apt version string
#                        (5:28.3.0-1~ubuntu.24.04~noble)
#       --suite NAME     Ubuntu suite (codename) for the apt repository,
#                        e.g. noble -- needed on Ubuntu derivatives whose own
#                        codename Docker does not publish
#       --configure-only Docker is already installed; only run the service,
#                        group and verification steps
#       --user NAME      user to add to the docker group (default: the invoking
#                        user, or $SUDO_USER when run under sudo)
#       --no-group       do not touch the docker group; keep using sudo docker
#       --no-hello-world skip the `docker run hello-world` check (no network)
#       --dry-run        print the commands instead of running them
#   -h, --help           this text
#
# exit codes:
#   2  unsupported platform (not Linux, or not Ubuntu / an Ubuntu derivative)
#   3  missing prerequisite (apt-get, curl, sudo)
#   4  a conflicting Docker package is installed and could not be removed
#   5  the apt repository could not be configured
#   6  the packages failed to install
#   7  the daemon did not start
#   8  the docker group could not be configured
#   9  verification failed
# USAGE-END

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------
DOCKER_REPO="https://download.docker.com/linux/ubuntu"
GPG_URL="$DOCKER_REPO/gpg"
KEYRING="/etc/apt/keyrings/docker.asc"
SOURCES="/etc/apt/sources.list.d/docker.sources"
LEGACY_SOURCES="/etc/apt/sources.list.d/docker.list"

# The packages upstream tells you to remove first: distro forks and the
# unofficial packaging, all of which own files docker-ce also wants.
CONFLICTS=(docker.io docker-compose docker-compose-v2 docker-doc docker-buildx
           podman-docker containerd runc)

# docker-ce is the daemon, docker-ce-cli the client, containerd.io the runtime;
# the two plugins are what `docker buildx` and `docker compose` dispatch to.
PACKAGES=(docker-ce docker-ce-cli containerd.io docker-buildx-plugin
          docker-compose-plugin)

SUPPORTED_ARCHES=(amd64 armhf arm64 s390x ppc64le)

REPO_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"

# ---------------------------------------------------------------------------
# Defaults, overridable by flags
# ---------------------------------------------------------------------------
ASSUME_YES=0
CONFIGURE_ONLY=0
DRY_RUN=0
NO_GROUP=0
HELLO_WORLD=1
DOCKER_VERSION=""
SUITE=""
TARGET_USER=""

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
skip()  { printf '%s    --:%s %s\n' "$C_DIM" "$C_OFF" "$*"; }

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

# run: the single choke point for everything that mutates the system, so
# --dry-run is honoured without a second code path.
run() {
    if ((DRY_RUN)); then
        printf '    %s+ %s%s\n' "$C_DIM" "$*" "$C_OFF"
        return 0
    fi
    "$@"
}

# ---------------------------------------------------------------------------
# Flags
# ---------------------------------------------------------------------------
need_arg() { [[ $# -ge 2 && -n "$2" ]] || { printf '%s needs an argument\n\n' "$1" >&2; usage 1; }; }

while (($# > 0)); do
    case "$1" in
        -y|--yes)             ASSUME_YES=1 ;;
        --version)            need_arg "$@"; DOCKER_VERSION="$2"; shift ;;
        --version=*)          DOCKER_VERSION="${1#*=}" ;;
        --suite)              need_arg "$@"; SUITE="$2"; shift ;;
        --suite=*)            SUITE="${1#*=}" ;;
        --user)               need_arg "$@"; TARGET_USER="$2"; shift ;;
        --user=*)             TARGET_USER="${1#*=}" ;;
        --configure-only)     CONFIGURE_ONLY=1 ;;
        --no-group)           NO_GROUP=1 ;;
        --no-hello-world)     HELLO_WORLD=0 ;;
        --dry-run)            DRY_RUN=1 ;;
        -h|--help)            usage 0 ;;
        *) printf 'unknown option: %s\n\n' "$1" >&2; usage 1 ;;
    esac
    shift
done

# ---------------------------------------------------------------------------
# Environment probes
# ---------------------------------------------------------------------------
[[ "$(uname -s)" == Linux ]] || die 2 "unsupported platform: $(uname -s)" \
    "Docker Engine is Linux-only."

[[ -r /etc/os-release ]] || die 2 "no /etc/os-release; cannot identify the distribution" \
    "This script targets Ubuntu. Other distributions:" \
    "  https://docs.docker.com/engine/install/"

# shellcheck disable=SC1091
. /etc/os-release

DISTRO_ID="${ID:-unknown}"
DISTRO_NAME="${PRETTY_NAME:-$DISTRO_ID}"
# Ubuntu derivatives (Mint, Pop!_OS, Zorin, elementary) keep their own
# VERSION_CODENAME and expose the Ubuntu base as UBUNTU_CODENAME. Docker only
# publishes the Ubuntu suites, so the base is what the repository needs.
DETECTED_SUITE="${UBUNTU_CODENAME:-${VERSION_CODENAME:-}}"
UBUNTU_VERSION="${VERSION_ID:-}"

if [[ "$DISTRO_ID" != ubuntu ]]; then
    if [[ -n "${UBUNTU_CODENAME:-}" ]]; then
        warn "$DISTRO_NAME is not Ubuntu, but is based on Ubuntu ${UBUNTU_CODENAME}; using that repository"
    elif [[ "$DISTRO_ID" == debian || " ${ID_LIKE:-} " == *" debian "* ]]; then
        die 2 "$DISTRO_NAME is Debian-based but not Ubuntu" \
            "This script only handles the Ubuntu repository:" \
            "  https://docs.docker.com/engine/install/debian/"
    else
        die 2 "unsupported distribution: $DISTRO_NAME" \
            "This script follows https://docs.docker.com/engine/install/ubuntu/." \
            "Instructions for other distributions:" \
            "  https://docs.docker.com/engine/install/"
    fi
fi

[[ -n "$SUITE" ]] || SUITE="$DETECTED_SUITE"
[[ -n "$SUITE" ]] || die 2 "could not determine the Ubuntu codename" \
    "/etc/os-release has neither UBUNTU_CODENAME nor VERSION_CODENAME." \
    "Pass one explicitly, e.g.:  scripts/install-docker.sh --suite noble"

if [[ "$(id -u)" == 0 ]]; then SUDO=""; else SUDO="sudo"; fi

ARCH=""
command -v dpkg >/dev/null 2>&1 && ARCH="$(dpkg --print-architecture)"

HAS_SYSTEMD=0
[[ -d /run/systemd/system ]] && HAS_SYSTEMD=1

IS_WSL=0
grep -qiE 'microsoft|wsl' /proc/version 2>/dev/null && IS_WSL=1

IN_CONTAINER=0
{ [[ -e /.dockerenv ]] || grep -qE '(docker|containerd|lxc)' /proc/1/cgroup 2>/dev/null; } && IN_CONTAINER=1

have_docker() { command -v docker >/dev/null 2>&1; }
docker_version() { docker --version 2>/dev/null | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -n1; }

pkg_installed() {
    [[ "$(dpkg-query -W -f='${db:Status-Status}' "$1" 2>/dev/null)" == installed ]]
}

# ---------------------------------------------------------------------------
# Plan + confirmation
# ---------------------------------------------------------------------------
printf '%s\n' "${C_BLU}Docker Engine setup for bpf-arena-data-structures${C_OFF}"
info "repo        : $REPO_ROOT"
info "distro      : $DISTRO_NAME$( ((IS_WSL)) && printf ' (WSL)' )$( ((IN_CONTAINER)) && printf ' (inside a container)' )"
info "suite       : $SUITE${ARCH:+ / $ARCH}"
info "repository  : $DOCKER_REPO"
info "packages    : ${PACKAGES[*]}"
[[ -n "$DOCKER_VERSION" ]] && info "version     : $DOCKER_VERSION"
info "init        : $( ((HAS_SYSTEMD)) && echo systemd || echo 'no systemd (sysv service)' )"
if have_docker; then
    info "existing    : $(docker --version 2>/dev/null || echo 'docker on PATH, version unknown')"
else
    info "existing    : not installed"
fi
((DRY_RUN)) && info "dry-run     : nothing will be changed"

if ((IN_CONTAINER)); then
    warn "this looks like a container; installing Docker inside one needs a mounted"
    warn "docker socket or --privileged, and is not what README.md step 1 asks for"
fi

if ((!ASSUME_YES)) && ((!DRY_RUN)); then
    printf '\nThis removes conflicting packages, adds %s, installs Docker Engine,\n' "$SOURCES"
    printf 'enables the daemon, and adds a user to the docker group.\n'
    read -r -p "Continue? [y/N] " reply
    [[ "$reply" =~ ^[Yy]$ ]] || { echo "aborted"; exit 0; }
fi

export DEBIAN_FRONTEND=noninteractive

# ---------------------------------------------------------------------------
# 1. Prerequisites
# ---------------------------------------------------------------------------
step "Checking prerequisites"

command -v apt-get >/dev/null 2>&1 || die 3 "apt-get not found" \
    "This script installs from Docker's apt repository, which needs a" \
    "Debian-family system. See https://docs.docker.com/engine/install/"

if [[ -n "$SUDO" ]] && ! command -v sudo >/dev/null 2>&1; then
    die 3 "not root and no sudo" \
        "Re-run as root, or install sudo first."
fi

if [[ -n "$ARCH" ]] && [[ " ${SUPPORTED_ARCHES[*]} " != *" $ARCH "* ]]; then
    warn "architecture '$ARCH' is not one Docker publishes (${SUPPORTED_ARCHES[*]}); the repository may 404"
fi

# ca-certificates and curl are needed to fetch the signing key over TLS.
missing=()
pkg_installed ca-certificates || missing+=(ca-certificates)
command -v curl >/dev/null 2>&1 || missing+=(curl)
if ((${#missing[@]} > 0)); then
    info "installing: ${missing[*]}"
    run $SUDO apt-get update -qq \
        || die 3 "apt-get update failed" "Check network access and /etc/apt/sources.list."
    run $SUDO apt-get install -y "${missing[@]}" \
        || die 3 "could not install: ${missing[*]}"
fi
ok "apt-get, curl, ca-certificates${SUDO:+, sudo} available"

# ---------------------------------------------------------------------------
# 2. Remove the packages that conflict with docker-ce
# ---------------------------------------------------------------------------
step "Removing conflicting packages"

if ((CONFIGURE_ONLY)); then
    skip "--configure-only"
else
    if command -v snap >/dev/null 2>&1 && snap list docker >/dev/null 2>&1; then
        die 4 "the 'docker' snap is installed" \
            "It provides its own /snap/bin/docker and its own daemon, which will" \
            "shadow or fight with docker-ce. Remove it first:" \
            "  sudo snap remove docker"
    fi

    present=()
    for p in "${CONFLICTS[@]}"; do
        pkg_installed "$p" && present+=("$p")
    done

    if ((${#present[@]} == 0)); then
        skip "none of: ${CONFLICTS[*]}"
    else
        info "removing: ${present[*]}"
        info "(images and volumes under /var/lib/docker are left alone)"
        run $SUDO apt-get remove -y "${present[@]}" \
            || die 4 "could not remove: ${present[*]}" \
                    "Remove them by hand, then re-run:" \
                    "  sudo apt-get remove ${present[*]}"
        ok "removed ${#present[@]} conflicting package(s)"
    fi
fi

# ---------------------------------------------------------------------------
# 3. Docker's apt repository
# ---------------------------------------------------------------------------
step "Configuring the apt repository"

if ((CONFIGURE_ONLY)); then
    skip "--configure-only"
else
    # Fail early and legibly rather than on a 404 from apt-get update.
    if ! curl -fsSL -o /dev/null "$DOCKER_REPO/dists/$SUITE/Release"; then
        die 5 "Docker publishes no '$SUITE' suite" \
            "Checked: $DOCKER_REPO/dists/$SUITE/Release" \
            "Pick one explicitly, e.g.:  scripts/install-docker.sh --suite noble"
    fi
    ok "$DOCKER_REPO/dists/$SUITE exists"

    if [[ -f "$LEGACY_SOURCES" ]]; then
        # A one-line docker.list from the pre-deb822 instructions and the
        # docker.sources below are the same repository twice; apt errors out.
        warn "found the older $LEGACY_SOURCES; moving it aside"
        run $SUDO mv "$LEGACY_SOURCES" "${LEGACY_SOURCES}.bak-bpf-arena"
    fi

    if [[ -s "$KEYRING" ]]; then
        skip "keyring already present: $KEYRING"
    else
        info "fetching the signing key: $GPG_URL"
        if ((DRY_RUN)); then
            run $SUDO install -m 0755 -d /etc/apt/keyrings
            run curl -fsSL "$GPG_URL" -o "$KEYRING"
            run $SUDO chmod a+r "$KEYRING"
        else
            tmpkey="$(mktemp)"
            trap 'rm -f "${tmpkey:-}"' EXIT
            curl -fsSL --proto '=https' --tlsv1.2 "$GPG_URL" -o "$tmpkey" \
                || die 5 "could not download $GPG_URL"
            grep -q 'BEGIN PGP PUBLIC KEY BLOCK' "$tmpkey" \
                || die 5 "$GPG_URL did not return a PGP key" \
                        "A captive portal or proxy may be intercepting the download."
            $SUDO install -m 0755 -d /etc/apt/keyrings \
                || die 5 "could not create /etc/apt/keyrings"
            $SUDO install -m 0644 "$tmpkey" "$KEYRING" \
                || die 5 "could not write $KEYRING"
            rm -f "$tmpkey"; trap - EXIT
            ok "wrote $KEYRING"
        fi
    fi

    # deb822 format, matching the current upstream instructions. Signed-By
    # pins this repository to Docker's key instead of trusting it globally.
    sources_body="Types: deb
URIs: $DOCKER_REPO
Suites: $SUITE
Components: stable
Architectures: ${ARCH:-amd64}
Signed-By: $KEYRING"

    if [[ -f "$SOURCES" ]] && [[ "$(cat "$SOURCES" 2>/dev/null)" == "$sources_body" ]]; then
        skip "$SOURCES already correct"
    else
        info "writing $SOURCES"
        if ((DRY_RUN)); then
            printf '%s\n' "$sources_body" | sed "s/^/      ${C_DIM}| /;s/$/${C_OFF}/"
        else
            [[ -f "$SOURCES" ]] && $SUDO cp -n "$SOURCES" "${SOURCES}.bak-bpf-arena"
            printf '%s\n' "$sources_body" | $SUDO tee "$SOURCES" >/dev/null \
                || die 5 "could not write $SOURCES"
            ok "wrote $SOURCES"
        fi
    fi

    run $SUDO apt-get update \
        || die 5 "apt-get update failed after adding the repository" \
                "Inspect the output above; a signature error means $KEYRING is wrong."
    ok "package lists updated"
fi

# ---------------------------------------------------------------------------
# 4. Install Docker Engine
# ---------------------------------------------------------------------------
step "Installing Docker Engine"

if ((CONFIGURE_ONLY)); then
    have_docker || die 6 "--configure-only was given but no 'docker' is on PATH" \
        "Re-run without --configure-only."
    skip "already installed: $(docker --version)"
else
    install_pkgs=("${PACKAGES[@]}")
    if [[ -n "$DOCKER_VERSION" ]]; then
        # Upstream's version strings look like 5:28.3.0-1~ubuntu.24.04~noble;
        # accept a bare 28.3.0 and build that form.
        if [[ "$DOCKER_VERSION" == *:* || "$DOCKER_VERSION" == *~* ]]; then
            version_string="$DOCKER_VERSION"
        else
            version_string="5:${DOCKER_VERSION}-1~ubuntu.${UBUNTU_VERSION}~${SUITE}"
        fi
        info "version string: $version_string"
        if ! ((DRY_RUN)) && ! apt-cache madison docker-ce 2>/dev/null | grep -qF "$version_string"; then
            available="$(apt-cache madison docker-ce 2>/dev/null | awk -F'|' '{print "  " $2}' | head -n 10 || true)"
            die 6 "docker-ce=$version_string is not in the repository" \
                "Available (10 most recent):" "${available:-  (none -- is the repository configured?)}" \
                "List them yourself with:  apt-cache madison docker-ce"
        fi
        install_pkgs=("docker-ce=$version_string" "docker-ce-cli=$version_string"
                      containerd.io docker-buildx-plugin docker-compose-plugin)
    fi

    info "apt-get install ${install_pkgs[*]}"
    run $SUDO apt-get install -y "${install_pkgs[@]}" \
        || die 6 "the packages failed to install" \
                "If apt reports held or conflicting packages, remove the distro's" \
                "docker.io/containerd first:  sudo apt-get remove ${CONFLICTS[*]}"
    ((DRY_RUN)) || ok "installed $(docker --version)"
fi

# ---------------------------------------------------------------------------
# 5. Start the daemon
# ---------------------------------------------------------------------------
step "Starting the daemon"

if ((HAS_SYSTEMD)); then
    # On Ubuntu the packages enable the units already; this makes it explicit
    # and covers images where the postinst was masked.
    run $SUDO systemctl enable --now docker.service containerd.service \
        || die 7 "systemctl enable --now docker.service failed" \
                "Check:  systemctl status docker.service" \
                "        journalctl -xeu docker.service"
    if ((DRY_RUN)); then
        skip "dry run"
    elif systemctl is-active --quiet docker.service; then
        ok "docker.service active and enabled at boot"
    else
        die 7 "docker.service is not active" \
            "Check:  systemctl status docker.service" \
            "        journalctl -xeu docker.service"
    fi
elif ((IS_WSL)); then
    warn "no systemd -- WSL2 needs 'systemd=true' under [boot] in /etc/wsl.conf"
    warn "and a 'wsl --shutdown' from Windows before the daemon starts on boot"
    info "starting it for this session instead"
    run $SUDO service docker start || die 7 "'service docker start' failed" \
        "Start it in the foreground to see why:  sudo dockerd"
    ok "dockerd started (this session only)"
else
    warn "no systemd found; starting the sysv service"
    run $SUDO service docker start || die 7 "'service docker start' failed" \
        "Start it in the foreground to see why:  sudo dockerd"
    ok "dockerd started"
fi

# ---------------------------------------------------------------------------
# 6. Non-root access (the docker group)
# ---------------------------------------------------------------------------
step "Configuring non-root access"

[[ -n "$TARGET_USER" ]] || TARGET_USER="${SUDO_USER:-$(id -un)}"
GROUP_ADDED=0

if ((NO_GROUP)); then
    skip "--no-group; keep prefixing commands with sudo"
elif [[ "$TARGET_USER" == root ]]; then
    skip "target user is root, which already reaches the socket"
elif ! id "$TARGET_USER" >/dev/null 2>&1; then
    die 8 "no such user: $TARGET_USER" \
        "Pass an existing account:  scripts/install-docker.sh --user \$USER"
elif id -nG "$TARGET_USER" 2>/dev/null | tr ' ' '\n' | grep -qx docker; then
    skip "$TARGET_USER is already in the docker group"
else
    warn "the 'docker' group is root-equivalent on this host; --no-group opts out"
    getent group docker >/dev/null 2>&1 || run $SUDO groupadd docker \
        || die 8 "groupadd docker failed"
    run $SUDO usermod -aG docker "$TARGET_USER" \
        || die 8 "usermod -aG docker $TARGET_USER failed"
    GROUP_ADDED=1
    ok "added $TARGET_USER to the docker group"

    # A previous 'sudo docker' run leaves ~/.docker owned by root, which the
    # now-unprivileged client cannot write.
    home_dir="$(getent passwd "$TARGET_USER" | cut -d: -f6 || true)"
    if [[ -n "$home_dir" && -d "$home_dir/.docker" ]] \
       && [[ "$(stat -c '%U' "$home_dir/.docker" 2>/dev/null)" == root ]]; then
        info "fixing root-owned $home_dir/.docker (left by an earlier sudo docker)"
        run $SUDO chown -R "$TARGET_USER:$TARGET_USER" "$home_dir/.docker"
        run $SUDO chmod -R g+rwx "$home_dir/.docker"
    fi
fi

# ---------------------------------------------------------------------------
# 7. Verify
# ---------------------------------------------------------------------------
step "Verifying"

if ((DRY_RUN)); then
    skip "dry run: nothing was installed to verify"
    printf '\n%sDry run complete.%s Re-run without --dry-run to apply.\n\n' "$C_GRN" "$C_OFF"
    exit 0
fi

have_docker || die 9 "'docker' is not on PATH" \
    "The packages installed but the client is missing; check:  dpkg -l docker-ce-cli"
ok "client $(docker_version)"

$SUDO docker version --format '{{.Server.Version}}' >/dev/null 2>&1 \
    || die 9 "the client cannot reach the daemon" \
            "Check:  sudo systemctl status docker.service" \
            "        sudo docker version"
ok "daemon $($SUDO docker version --format '{{.Server.Version}}' 2>/dev/null)"

if $SUDO docker buildx version >/dev/null 2>&1; then
    ok "buildx present ($($SUDO docker buildx version | head -n1))"
else
    warn "docker buildx is missing; 'docker build' falls back to the legacy builder"
    warn "and the Dockerfile's cache mounts (RUN --mount=type=cache) will fail"
fi

if $SUDO docker compose version >/dev/null 2>&1; then
    ok "compose present ($($SUDO docker compose version | head -n1))"
else
    skip "docker compose plugin not installed (this repo does not need it)"
fi

if ((HELLO_WORLD)); then
    info "docker run --rm hello-world"
    if $SUDO docker run --rm hello-world >/dev/null 2>&1; then
        ok "pulled and ran hello-world"
    else
        die 9 "'docker run hello-world' failed" \
            "The daemon is up but could not pull or run an image (no network, or" \
            "a full /var/lib/docker). See:  sudo docker run --rm hello-world"
    fi
else
    skip "--no-hello-world"
fi

if ((GROUP_ADDED)); then
    warn "the new group membership is not in this shell yet"
    info "start a new login session, or for this shell only:  newgrp docker"
elif ((!NO_GROUP)) && [[ "$TARGET_USER" == "$(id -un)" && "$TARGET_USER" != root ]] \
     && ! docker info >/dev/null 2>&1; then
    warn "$TARGET_USER still cannot reach the daemon without sudo"
    info "log out and back in, or:  newgrp docker"
fi

# ---------------------------------------------------------------------------
# Next steps
# ---------------------------------------------------------------------------
printf '\n%sDocker is ready.%s Next:\n\n' "$C_GRN" "$C_OFF"
printf '  cd %s\n' "$REPO_ROOT"
printf '  git submodule update --init --recursive\n'
printf '  docker build -t bpf-arena-ds .\n'
printf '  scripts/init-docker.sh\n\n'
printf '%sThe container shares the host kernel: the relays need the host running\n' "$C_DIM"
printf 'the kernel from README step 4.%s\n' "$C_OFF"
