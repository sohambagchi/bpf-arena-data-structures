# Installing Nix (and Docker) for this artifact

`README.md` step 1 assumes one of the two packaged environments is already on
the machine. This document is the part before that: getting Nix or Docker
installed on a fresh system and configured so this repo's `nix develop` and
`docker build` actually work.

Two independent halves: [Nix](#nix) and [Docker](#docker). You only need one.
Each has a one-command script (`scripts/install-nix.sh`,
`scripts/install-docker.sh`) and the same thing spelled out by hand underneath.

---

## Nix

### The one-command path

From the repo root:

```bash
./scripts/install-nix.sh
```

It installs Nix if it is missing, enables the two experimental features this
repo's flake needs, restarts `nix-daemon`, and verifies the result. It is safe
to re-run: every step is skipped if it is already done. Useful flags:

| flag | effect |
| --- | --- |
| `-y` | skip the script's own confirmation prompt |
| `--no-daemon` | single-user install (`~/.config/nix/nix.conf`) instead of the daemon |
| `--configure-only` | Nix is already installed; just enable flakes and restart the daemon |

Then:

```bash
git submodule update --init --recursive
nix develop
```

The rest of this section is the same thing done by hand, plus what each step is
for. Do this if you would rather not run an installer script that runs another
installer script.

### 1. Install Nix

```bash
curl --proto '=https' --tlsv1.2 -L https://nixos.org/nix/install | sh -s -- --daemon
```

`--daemon` is the multi-user install: it creates the `nixbld` users, the
`nix-daemon` systemd service, and a system-wide `/etc/nix/nix.conf`. It needs
`sudo` (the installer asks) and it prints its plan before touching anything.
Use `--no-daemon` instead only on a machine where you cannot get root; the
single-user install keeps everything under `/nix` owned by you and reads
`~/.config/nix/nix.conf` rather than `/etc/nix/nix.conf`.

The installer edits your shell startup files, so **open a new login shell**
afterwards, or source the profile snippet in the current one:

```bash
. /etc/profile.d/nix.sh          # or: . ~/.nix-profile/etc/profile.d/nix.sh
nix --version                    # must be >= 2.27
```

Nix >= 2.27 is a hard requirement: `flake.nix` sets `inputs.self.submodules =
true`, which is what pulls `third_party/libbpf`, `third_party/bpftool` and
`third_party/vmlinux` into the flake's source tree. Older Nix ignores it and
`nix build .#artifact` fails on a missing `libbpf.a`. Upgrade an existing
install with `sudo -i nix upgrade-nix`.

### 2. Enable `nix-command` and `flakes`

**This is the step a fresh install does not do for you, and the reason
`nix develop` fails out of the box** with:

```
error: experimental Nix feature 'nix-command' is disabled; add '--extra-experimental-features nix-command' to enable it
```

Flakes are still gated behind an opt-in, so add to `/etc/nix/nix.conf`
(multi-user) or `~/.config/nix/nix.conf` (single-user):

```
experimental-features = nix-command flakes
```

```bash
echo 'experimental-features = nix-command flakes' | sudo tee -a /etc/nix/nix.conf
```

If the file already has an `experimental-features` line, edit that line to
include both values rather than appending a second one — Nix lets the last
occurrence win, so a second line silently replaces the first.

### 3. Restart the daemon

The daemon reads `/etc/nix/nix.conf` at startup, and it — not your shell — is
what evaluates and builds. Skip this on a single-user install; there is no
daemon.

```bash
sudo systemctl restart nix-daemon          # Linux, systemd
sudo launchctl kickstart -k system/org.nixos.nix-daemon    # macOS
```

### 4. Verify

```bash
nix --version                              # >= 2.27
nix --extra-experimental-features nix-command \
    config show experimental-features      # must list both nix-command and flakes
```

Ask Nix what it resolved rather than reading `nix.conf` back — that is what
catches a duplicate `experimental-features` line overriding yours, or a daemon
that was never restarted. (`nix flake --help` is not a test: recent versions
print the help text whether or not the feature is enabled.)

### 5. Use it

```bash
cd /path/to/bpf-arena-data-structures
git submodule update --init --recursive    # the flake needs these checked out
nix develop
```

`nix develop` is a single shell with everything: clang-20 and gcc for the
artifact, libbfd/libopcodes and libcap for a full `bpftool` build (see
`README.md`, *Requirements → libbfd and libcap*, for what a bpftool built
without them silently loses), and bison/flex/bc/pahole/QEMU for
`scripts/build-kernel.sh`. Inside
it, `README.md` steps 2, 4 and 5 work as written. (`nix develop .#kernel` still
resolves, as an alias for the default shell.) The QEMU guest is separate —
`nix run .#vm` — because it is a booted machine rather than a set of tools.

### Alternatives and special cases

**Determinate Systems installer.** Enables flakes by default, and can uninstall
itself cleanly, which the upstream installer cannot:

```bash
curl --proto '=https' --tlsv1.2 -sSf -L https://install.determinate.systems/nix | sh -s -- install
```

Steps 2 and 3 above are unnecessary with it. Note that it manages `/etc/nix`
itself; edit `/etc/nix/nix.custom.conf` rather than `nix.conf` there.

**Enable flakes for one command only,** without editing any config — handy when
you are on someone else's machine:

```bash
nix --extra-experimental-features 'nix-command flakes' develop
```

**NixOS.** Nix is already installed and `/etc/nix/nix.conf` is generated, so
step 2 goes in `configuration.nix` instead:

```nix
nix.settings.experimental-features = [ "nix-command" "flakes" ];
```

then `sudo nixos-rebuild switch`. No daemon restart needed; the rebuild does it.

**Enable flakes at install time,** collapsing steps 1–3 into one command:

```bash
echo 'experimental-features = nix-command flakes' > /tmp/nix-extra.conf
curl --proto '=https' --tlsv1.2 -L https://nixos.org/nix/install \
  | sh -s -- --daemon --nix-extra-conf-file /tmp/nix-extra.conf
```

**WSL2.** Install inside the WSL distribution, not on Windows. The daemon
install needs systemd, which means `systemd=true` under `[boot]` in
`/etc/wsl.conf` and a `wsl --shutdown` from Windows first. Without systemd, use
`--no-daemon`.

**macOS.** Everything above works, but only the userspace half of this artifact
is meaningful there: the relay apps need a Linux 6.10+ kernel with
`CONFIG_BPF_ARENA`, and `nix build .#kernel` / `nix run .#vm` are pinned to
`x86_64-linux`.

### Troubleshooting

| symptom | cause and fix |
| --- | --- |
| `experimental Nix feature 'nix-command' is disabled` | step 2 was skipped, or the daemon was not restarted after it (step 3) |
| the setting is in `nix.conf` but `nix config show` disagrees | a later `experimental-features` line in the same file is overriding it, or you edited the file the *other* install mode reads (`/etc/nix` vs `~/.config/nix`) |
| `nix: command not found` in a fresh terminal | the installer's profile snippet is only picked up by a login shell; open a new one, or source `/etc/profile.d/nix.sh` |
| `error: cannot find flake attribute` / missing `libbpf.a` | Nix is older than 2.27, or the submodules are not checked out — `git submodule update --init --recursive` |
| `warning: Git tree ... is dirty` | harmless; the flake builds from the working tree |
| `directory /nix already exists` during install | a previous install was removed only partially. The upstream installer has no uninstaller; the Determinate installer does (`/nix/nix-installer uninstall`) |
| build fails on a read-only `/nix/store` | you are on NixOS or a locked-down host; nothing to fix, that store is managed |

### Uninstalling

The upstream multi-user install spreads itself over `/nix`, the `nixbld` users,
`/etc/nix`, the systemd unit and your shell profiles; removing it by hand is
documented in the Nix manual under *Uninstalling Nix*. If you expect to remove
it later, prefer the Determinate installer, which records a receipt
(`/nix/receipt.json`) and reverses everything with `/nix/nix-installer
uninstall`.

---

## Docker

This section targets **Ubuntu** (22.04 jammy, 24.04 noble, 26.04 resolute, and
Ubuntu-based derivatives), following
<https://docs.docker.com/engine/install/ubuntu/> and
<https://docs.docker.com/engine/install/linux-postinstall/>. For other
distributions the shape is identical, only the repository differs —
<https://docs.docker.com/engine/install/>.

Before you start, two things worth knowing about this route: the container
gives you the toolchain but not a kernel, so only the userspace half of the
artifact runs under it (`README.md`'s FAQ, "Why does the BPF half not work
under Docker?"), and the post-install step that lets a non-root user talk to
the daemon (`usermod -aG docker $USER`) grants root-equivalent access on the
host.

### The one-command path

From the repo root:

```bash
./scripts/install-docker.sh
```

It removes the conflicting distro packages, adds Docker's apt repository and
signing key, installs Engine + CLI + containerd + the buildx and compose
plugins, enables the daemon, adds you to the `docker` group, and verifies the
result with `docker run hello-world`. Like the Nix script, every step is
skipped when it is already done, so it is safe to re-run. Useful flags:

| flag | effect |
| --- | --- |
| `-y` | skip the script's own confirmation prompt |
| `--dry-run` | print every command it would run, change nothing |
| `--version 28.3.0` | pin a version instead of taking the newest |
| `--suite noble` | force the Ubuntu suite (needed on some derivatives) |
| `--no-group` | do not add anyone to the `docker` group; keep using `sudo docker` |
| `--user NAME` | add someone other than the invoking user to the group |
| `--convenience-script` | install via `get.docker.com` instead of the apt repository |
| `--configure-only` | Docker is installed; only start the daemon, fix the group, verify |
| `--no-hello-world` | skip the image pull at the end (no network) |

Then:

```bash
git submodule update --init --recursive
docker build -t bpf-arena-ds .
docker run --rm -it -v "$PWD:/artifact" bpf-arena-ds
```

The rest of this section is the same thing by hand, plus what each step is for.

### 1. Remove conflicting packages

Ubuntu ships its own `docker.io`, and several unofficial packages own files
that `docker-ce` also wants. Remove them first — this does **not** touch images
or volumes under `/var/lib/docker`:

```bash
for pkg in docker.io docker-compose docker-compose-v2 docker-doc docker-buildx \
           podman-docker containerd runc; do
    sudo apt-get remove -y $pkg
done
```

Also check for the snap, which upstream's instructions do not mention but which
shadows `docker` on the PATH with its own daemon:

```bash
snap list docker 2>/dev/null && sudo snap remove docker
```

### 2. Add Docker's apt repository

```bash
sudo apt-get update
sudo apt-get install -y ca-certificates curl
sudo install -m 0755 -d /etc/apt/keyrings
sudo curl -fsSL https://download.docker.com/linux/ubuntu/gpg -o /etc/apt/keyrings/docker.asc
sudo chmod a+r /etc/apt/keyrings/docker.asc

sudo tee /etc/apt/sources.list.d/docker.sources >/dev/null <<EOF
Types: deb
URIs: https://download.docker.com/linux/ubuntu
Suites: $(. /etc/os-release && echo "${UBUNTU_CODENAME:-$VERSION_CODENAME}")
Components: stable
Architectures: $(dpkg --print-architecture)
Signed-By: /etc/apt/keyrings/docker.asc
EOF

sudo apt-get update
```

`Signed-By` scopes Docker's key to this one repository rather than trusting it
for everything. The `UBUNTU_CODENAME:-VERSION_CODENAME` fallback matters on
derivatives: Linux Mint's own `VERSION_CODENAME` is `xia`, which Docker does
not publish; its `UBUNTU_CODENAME` is `noble`, which it does. Docker publishes
`amd64`, `armhf`, `arm64`, `s390x` and `ppc64le`.

If you followed older instructions before, delete the one-line
`/etc/apt/sources.list.d/docker.list` — with `docker.sources` alongside it, apt
sees the same repository twice and errors out.

### 3. Install

```bash
sudo apt-get install -y docker-ce docker-ce-cli containerd.io \
     docker-buildx-plugin docker-compose-plugin
```

`docker-ce` is the daemon, `docker-ce-cli` the client, `containerd.io` the
runtime underneath. **`docker-buildx-plugin` is not optional here:** this
repo's `Dockerfile` starts with `# syntax=docker/dockerfile:1.7` and uses
`RUN --mount=type=cache`, which the legacy builder cannot parse.

To pin a version instead, list what the repository has and name it exactly:

```bash
apt-cache madison docker-ce | head           # or: apt list --all-versions docker-ce
VERSION_STRING=5:28.3.0-1~ubuntu.24.04~noble
sudo apt-get install -y docker-ce="$VERSION_STRING" docker-ce-cli="$VERSION_STRING" \
     containerd.io docker-buildx-plugin docker-compose-plugin
```

### 4. Start the daemon

The Ubuntu packages enable and start it already; this is the explicit form, and
what to run if you are on an image where the postinst was masked:

```bash
sudo systemctl enable --now docker.service containerd.service
systemctl is-active docker.service          # -> active
```

### 5. Non-root access

```bash
sudo groupadd docker                        # usually already created by the package
sudo usermod -aG docker "$USER"
newgrp docker                               # this shell only; otherwise log out and back in
```

**The `docker` group is root on this host.** The daemon runs as root and will
bind-mount any path into a container, so anyone in the group can read or write
anything. On a machine where that matters, skip this step and use `sudo docker`
(`scripts/install-docker.sh --no-group`), or set up rootless mode —
<https://docs.docker.com/engine/security/rootless/>.

If you ran `sudo docker` before doing this, `~/.docker` is owned by root and
the now-unprivileged client cannot write it:

```bash
sudo chown -R "$USER":"$USER" "$HOME/.docker"
sudo chmod -R g+rwx "$HOME/.docker"
```

### 6. Verify

```bash
docker version                              # both Client and Server sections
docker buildx version                       # required by this repo's Dockerfile
docker run --rm hello-world
```

`docker version` printing only a `Client` section means the daemon is not
running or you cannot reach its socket — the two failures step 4 and step 5
address respectively.

### 7. Use it

```bash
cd /path/to/bpf-arena-data-structures
git submodule update --init --recursive     # the build copies third_party/ in
docker build -t bpf-arena-ds .
docker run --rm -it -v "$PWD:/artifact" bpf-arena-ds
```

The image's `CMD` is a shell, not the pipeline. Inside it you are root, so run
`make` and then `python3 scripts/run_all.py` without `sudo`. The build's
`verify` stage compiles the artifact and runs a userspace test, so a broken
toolchain fails the build rather than the run.

### Alternatives and special cases

**Convenience script.** One command, no repository setup; upstream explicitly
calls it unsuitable for production, but it is fine for a throwaway VM or a CI
image. It configures the same apt repository behind the scenes:

```bash
curl -fsSL https://get.docker.com -o get-docker.sh
sudo sh ./get-docker.sh --dry-run           # prints its plan
sudo sh ./get-docker.sh
```

`scripts/install-docker.sh --convenience-script` takes this path and still does
the group and verification steps afterwards.

**Offline / air-gapped, from `.deb` files.** Download the five packages for
your suite and architecture from
<https://download.docker.com/linux/ubuntu/dists/> (under
`<suite>/pool/stable/<arch>/`) and install them together so dependencies
resolve:

```bash
sudo dpkg -i ./containerd.io_<version>_<arch>.deb \
  ./docker-ce_<version>_<arch>.deb \
  ./docker-ce-cli_<version>_<arch>.deb \
  ./docker-buildx-plugin_<version>_<arch>.deb \
  ./docker-compose-plugin_<version>_<arch>.deb
sudo systemctl start docker
```

You still need base images, which the build pulls from Docker Hub — on a truly
disconnected machine, `docker save`/`docker load` the `ubuntu:24.04` image and
build with `--pull=false`.

**Docker Desktop (macOS, Windows).** <https://docs.docker.com/desktop/>. The
build works; the BPF half cannot, because the "host kernel" there is Docker's
own LinuxKit VM rather than a kernel you control. Use the Nix path plus
`nix run .#vm` if you need the relays to actually run.

**WSL2.** Either install Docker Desktop on Windows and enable its WSL
integration, or install Engine inside the distribution with the steps above. If
you do the latter, the daemon needs systemd: `systemd=true` under `[boot]` in
`/etc/wsl.conf`, then `wsl --shutdown` from Windows. Without it, start the
daemon per session with `sudo service docker start`.

**Rootless mode.** Removes the root-equivalent `docker` group at the cost of
some isolation features. <https://docs.docker.com/engine/security/rootless/>.
Fine for this artifact's build, which needs nothing privileged.

**Firewall.** Published container ports (`-p`) are inserted ahead of `ufw`
rules and bypass them. This repo's flow never publishes a port, so it does not
come up here, but it surprises people on a shared host. Docker requires
`iptables-nft` or `iptables-legacy`; pure `nft` rulesets are unsupported.

### Troubleshooting

| symptom | cause and fix |
| --- | --- |
| `permission denied while trying to connect to the Docker daemon socket` | step 5 was skipped, or the group is not in this shell yet — `newgrp docker`, or log out and back in |
| `Cannot connect to the Docker daemon. Is the docker daemon running?` | step 4 — `sudo systemctl status docker.service`, then `journalctl -xeu docker.service` |
| `the ... file is configured multiple times` from apt | both `docker.list` and `docker.sources` exist; delete the old `docker.list` |
| `404 Not Found` on `download.docker.com/linux/ubuntu/dists/<name>` | the suite is your derivative's codename, not Ubuntu's — use `UBUNTU_CODENAME`, or `--suite noble` |
| `NO_PUBKEY` / `signatures couldn't be verified` | `/etc/apt/keyrings/docker.asc` is missing or truncated; re-run step 2's `curl` |
| `unknown flag: --mount` or a syntax error on the first `RUN` | buildx is missing (step 3) or `DOCKER_BUILDKIT=0` is set |
| `docker: command not found` after installing | the `docker` snap was removed but its PATH entry lingers, or the install actually failed — `dpkg -l docker-ce-cli` |
| the build works but the relays report `bpf(2)` failures | expected: the container has the host's kernel. `check_kconfig.py` is checking the *host*. See README.md's FAQ |
| `no space left on device` mid-build | `/var/lib/docker` filled up — `docker system prune -a` |

### Uninstalling

```bash
sudo apt-get purge -y docker-ce docker-ce-cli containerd.io \
     docker-buildx-plugin docker-compose-plugin docker-ce-rootless-extras
sudo rm -rf /var/lib/docker /var/lib/containerd
sudo rm -f /etc/apt/sources.list.d/docker.sources /etc/apt/keyrings/docker.asc
sudo groupdel docker                        # optional
```

`purge` leaves images, containers and volumes behind on purpose; the `rm -rf`
is what actually deletes them, and it is not reversible.
