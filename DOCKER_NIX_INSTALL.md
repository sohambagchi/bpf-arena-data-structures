# Installing Nix (and Docker) for this artifact

`README.md` step 1 assumes one of the two packaged environments is already on
the machine. This document is the part before that: getting Nix or Docker
installed on a fresh system and configured so this repo's `nix develop` and
`docker build` actually work.

The Docker half is not written yet — see [Docker](#docker) at the end.

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
artifact, and bison/flex/bc/pahole/QEMU for `scripts/build-kernel.sh`. Inside
it, `README.md` steps 2 and 4 work as written. (`nix develop .#kernel` still
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

Not written yet. For now use the upstream instructions —
<https://docs.docker.com/engine/install/> for Linux, Docker Desktop at
<https://docs.docker.com/desktop/> for macOS and Windows — and then
`README.md`'s step 1:

```bash
docker build -t bpf-arena-ds .
docker run --rm -it -v "$PWD:/artifact" bpf-arena-ds
```

Two things worth knowing before you go that route: the container gives you the
toolchain but not a kernel, so only the userspace half of the artifact runs
under it (`README.md`'s FAQ, "Why does the BPF half not work under Docker?"),
and the post-install step that lets a non-root user talk to the daemon
(`usermod -aG docker $USER`) grants root-equivalent access on the host.
