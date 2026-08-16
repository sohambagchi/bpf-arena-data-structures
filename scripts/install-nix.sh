#!/usr/bin/env bash
set -Eeuo pipefail

# USAGE-START
# scripts/install-nix.sh -- install Nix and enable the flakes support this
#                           repo's `nix develop` needs
#
# Every step is skipped when it is already done, so re-running is safe.
#
# usage: scripts/install-nix.sh [options]
#
#   -y, --yes            do not ask for confirmation
#       --no-daemon      single-user install (~/.config/nix/nix.conf)
#       --configure-only Nix is installed; only enable flakes and restart it
#   -h, --help           this text
#
# exit codes:
#   2  unsupported platform    3  missing prerequisite    4  install failed
#   5  could not enable flakes  6  daemon restart failed  7  verification failed
# USAGE-END

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------
INSTALLER_URL="https://nixos.org/nix/install"

REQUIRED_NIX_VERSION="2.27"

REQUIRED_FEATURES=(nix-command flakes)

REPO_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"

# ---------------------------------------------------------------------------
# Defaults, overridable by flags
# ---------------------------------------------------------------------------
DAEMON=1
ASSUME_YES=0
CONFIGURE_ONLY=0

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
TOTAL_STEPS=5
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

# ---------------------------------------------------------------------------
# Flags
# ---------------------------------------------------------------------------
while (($# > 0)); do
    case "$1" in
        -y|--yes)         ASSUME_YES=1 ;;
        --no-daemon)      DAEMON=0 ;;
        --daemon)         DAEMON=1 ;;
        --configure-only) CONFIGURE_ONLY=1 ;;
        -h|--help)        usage 0 ;;
        *) printf 'unknown option: %s\n\n' "$1" >&2; usage 1 ;;
    esac
    shift
done

# ---------------------------------------------------------------------------
# Environment probes
# ---------------------------------------------------------------------------
OS="$(uname -s)"
case "$OS" in
    Linux|Darwin) ;;
    *) die 2 "unsupported platform: $OS" "Nix supports Linux and macOS." ;;
esac

IS_NIXOS=0
[[ -e /etc/NIXOS ]] && IS_NIXOS=1

IS_DETERMINATE=0
[[ -e /nix/receipt.json ]] && IS_DETERMINATE=1

load_nix_profile() {
    local p
    for p in /etc/profile.d/nix.sh \
             /nix/var/nix/profiles/default/etc/profile.d/nix-daemon.sh \
             "$HOME/.nix-profile/etc/profile.d/nix.sh"; do
        if [[ -r "$p" ]]; then
            set +u; # shellcheck disable=SC1090
            . "$p"; set -u
        fi
    done
}
load_nix_profile

have_nix() { command -v nix >/dev/null 2>&1; }

nix_version() { nix --version 2>/dev/null | grep -oE '[0-9]+\.[0-9]+(\.[0-9]+)?' | head -n1; }

version_ge() { [[ "$(printf '%s\n%s\n' "$2" "$1" | sort -V | head -n1)" == "$2" ]]; }

if ((DAEMON)); then
    NIX_CONF="/etc/nix/nix.conf"
else
    NIX_CONF="${XDG_CONFIG_HOME:-$HOME/.config}/nix/nix.conf"
fi

# ---------------------------------------------------------------------------
# Plan + confirmation
# ---------------------------------------------------------------------------
printf '%s\n' "${C_BLU}Nix setup for bpf-arena-data-structures${C_OFF}"
info "repo        : $REPO_ROOT"
info "platform    : $OS$( ((IS_NIXOS)) && printf ' (NixOS)' )$( ((IS_DETERMINATE)) && printf ' (Determinate)' )"
info "mode        : $( ((DAEMON)) && echo 'multi-user (--daemon)' || echo 'single-user' )"
info "nix.conf    : $NIX_CONF"
info "features    : ${REQUIRED_FEATURES[*]}"
if have_nix; then
    info "existing nix: $(nix_version)"
else
    info "existing nix: not installed"
fi

if ((!ASSUME_YES)); then
    printf '\nThis installs Nix (if missing), edits %s, and restarts the daemon.\n' "$NIX_CONF"
    read -r -p "Continue? [y/N] " reply
    [[ "$reply" =~ ^[Yy]$ ]] || { echo "aborted"; exit 0; }
fi

# ---------------------------------------------------------------------------
# 1. Prerequisites
# ---------------------------------------------------------------------------
step "Checking prerequisites"
missing=()
have_nix || command -v curl >/dev/null 2>&1 || missing+=(curl)
command -v sudo >/dev/null 2>&1 || [[ "$(id -u)" == 0 ]] || missing+=(sudo)
if ((${#missing[@]} > 0)); then
    die 3 "missing: ${missing[*]}" \
        "Install them with your distribution's package manager, e.g." \
        "  apt install ${missing[*]}   /   dnf install ${missing[*]}   /   pacman -S ${missing[*]}"
fi
ok "curl and sudo available"

# ---------------------------------------------------------------------------
# 2. Install Nix
# ---------------------------------------------------------------------------
step "Installing Nix"
if ((IS_NIXOS)); then
    skip "NixOS already provides Nix"
elif have_nix; then
    skip "already installed: $(nix --version)"
elif ((CONFIGURE_ONLY)); then
    die 4 "--configure-only was given but no 'nix' is on PATH" \
        "Open a new login shell (the installer only edits shell startup files)," \
        "or re-run without --configure-only."
else
    installer_args=( "$( ((DAEMON)) && echo '--daemon' || echo '--no-daemon' )" )
    ((ASSUME_YES)) && installer_args+=( --yes )
    info "curl -L $INSTALLER_URL | sh -s -- ${installer_args[*]}"
    info "(the installer prints its own plan and asks for sudo)"
    if ! curl --proto '=https' --tlsv1.2 -sSf -L "$INSTALLER_URL" | sh -s -- "${installer_args[@]}"; then
        die 4 "the Nix installer failed" \
            "See https://nixos.org/download/ (a partially removed /nix is the" \
            "usual cause; remove it first)."
    fi
    load_nix_profile
    have_nix || die 4 "Nix installed but 'nix' is not on PATH in this shell" \
        "Open a new login shell and re-run:  scripts/install-nix.sh --configure-only"
    ok "installed $(nix --version)"
fi

# ---------------------------------------------------------------------------
# 3. Enable nix-command + flakes
# ---------------------------------------------------------------------------
step "Enabling experimental features (${REQUIRED_FEATURES[*]})"

features_active() {
    local shown f
    shown=" $(nix --extra-experimental-features nix-command \
                  config show experimental-features 2>/dev/null) " || return 1
    for f in "${REQUIRED_FEATURES[@]}"; do
        [[ "$shown" == *" $f "* ]] || return 1
    done
}

if ((IS_NIXOS)); then
    if features_active; then
        ok "already enabled"
    else
        warn "on NixOS, /etc/nix/nix.conf is generated -- add this to configuration.nix:"
        printf '\n      nix.settings.experimental-features = [ "nix-command" "flakes" ];\n\n'
        info "then: sudo nixos-rebuild switch"
        die 5 "cannot edit a NixOS-managed $NIX_CONF"
    fi
elif ((IS_DETERMINATE)) && features_active; then
    skip "Determinate Nix enables these by default"
elif features_active; then
    skip "already enabled"
else
    existing=""
    if [[ -f "$NIX_CONF" ]]; then
        existing="$(grep -E '^[[:space:]]*experimental-features[[:space:]]*=' "$NIX_CONF" | tail -n1 || true)"
        [[ -n "$existing" ]] && info "existing line: $existing"
    fi

    # shellcheck disable=SC2206
    wanted=( ${existing#*=} )
    for f in "${REQUIRED_FEATURES[@]}"; do
        [[ " ${wanted[*]} " == *" $f "* ]] || wanted+=("$f")
    done
    line="experimental-features = ${wanted[*]}"
    info "adding to $NIX_CONF:  $line"

    write_conf() {
        if ((DAEMON)); then
            sudo mkdir -p "$(dirname "$NIX_CONF")" || return 1
            [[ -f "$NIX_CONF" ]] && sudo cp -n "$NIX_CONF" "${NIX_CONF}.bak-bpf-arena"
            printf '\n# added by bpf-arena-data-structures/scripts/install-nix.sh\n%s\n' "$line" \
                | sudo tee -a "$NIX_CONF" >/dev/null
        else
            mkdir -p "$(dirname "$NIX_CONF")" || return 1
            [[ -f "$NIX_CONF" ]] && cp -n "$NIX_CONF" "${NIX_CONF}.bak-bpf-arena"
            printf '\n# added by bpf-arena-data-structures/scripts/install-nix.sh\n%s\n' "$line" \
                >> "$NIX_CONF"
        fi
    }
    write_conf || die 5 "could not write $NIX_CONF" \
        "Add this line to it by hand:" \
        "  $line"
    ok "wrote $NIX_CONF$( [[ -f "${NIX_CONF}.bak-bpf-arena" ]] && printf ' (backup: %s.bak-bpf-arena)' "$NIX_CONF" )"
fi

# ---------------------------------------------------------------------------
# 4. Restart the daemon so it picks up the new nix.conf
# ---------------------------------------------------------------------------
step "Restarting nix-daemon"
if ((!DAEMON)); then
    skip "single-user install has no daemon"
elif ((IS_NIXOS)); then
    skip "handled by nixos-rebuild"
elif [[ "$OS" == Darwin ]]; then
    if sudo launchctl kickstart -k system/org.nixos.nix-daemon 2>/dev/null; then
        ok "kickstarted org.nixos.nix-daemon"
    else
        warn "could not kickstart the daemon; log out and back in"
    fi
elif command -v systemctl >/dev/null 2>&1 && systemctl list-unit-files nix-daemon.service >/dev/null 2>&1; then
    sudo systemctl restart nix-daemon \
        || die 6 "systemctl restart nix-daemon failed" \
                "Check: systemctl status nix-daemon"
    ok "nix-daemon restarted"
else
    warn "no systemd nix-daemon.service found; restart the daemon by hand if the check below fails"
fi

# ---------------------------------------------------------------------------
# 5. Verify
# ---------------------------------------------------------------------------
step "Verifying"
have_nix || die 7 "'nix' is still not on PATH" \
    "Open a new login shell, then re-run:  scripts/install-nix.sh --configure-only"

ver="$(nix_version)"
[[ -n "$ver" ]] || die 7 "could not parse 'nix --version'"
if version_ge "$ver" "$REQUIRED_NIX_VERSION"; then
    ok "nix $ver (>= $REQUIRED_NIX_VERSION)"
else
    die 7 "nix $ver is too old; this flake needs >= $REQUIRED_NIX_VERSION" \
        "inputs.self.submodules is a 2.27 feature. Upgrade:  sudo -i nix upgrade-nix"
fi

features_active \
    || die 7 "flakes are still disabled" \
            "Confirm the setting is live:  nix config show experimental-features" \
            "and that it is set in $NIX_CONF, then restart nix-daemon."
ok "nix-command + flakes enabled"

if [[ -d "$REPO_ROOT/.git" || -f "$REPO_ROOT/.git" ]]; then
    if [[ -e "$REPO_ROOT/third_party/libbpf/src/libbpf.c" ]]; then
        ok "submodules present"
    else
        warn "submodules not checked out -- run: git submodule update --init --recursive"
    fi
fi

printf '\n%sNix is ready.%s Next:\n\n' "$C_GRN" "$C_OFF"
printf '  cd %s\n' "$REPO_ROOT"
printf '  git submodule update --init --recursive\n'
printf '  nix develop\n\n'
printf '%sIf nix is not found in a fresh terminal, open a new login shell.%s\n' "$C_DIM" "$C_OFF"
