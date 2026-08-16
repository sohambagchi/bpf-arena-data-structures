# Installing Nix or Docker by hand

`scripts/install-nix.sh` and `scripts/install-docker.sh` do all of this for
you. These are the same steps, spelled out. You only need one of the two.

---

## Nix

### 1. Install

```bash
curl --proto '=https' --tlsv1.2 -L https://nixos.org/nix/install | sh -s -- --daemon
```

`--daemon` is the multi-user install; it asks for `sudo`. The installer edits
your shell startup files, so open a new login shell afterwards, or:

```bash
. /etc/profile.d/nix.sh
nix --version                    # must be >= 2.27
```

2.27 is a hard requirement: `flake.nix` sets `inputs.self.submodules = true`,
which is what pulls `third_party/` into the flake source.

### 2. Enable flakes

A stock install leaves them off, which is why `nix develop` fails with
`experimental Nix feature 'nix-command' is disabled`:

```bash
echo 'experimental-features = nix-command flakes' | sudo tee -a /etc/nix/nix.conf
```

If the file already has an `experimental-features` line, edit that line instead
of appending a second one — the last occurrence wins.

### 3. Restart the daemon

It reads `/etc/nix/nix.conf` at startup, and it is what evaluates and builds.

```bash
sudo systemctl restart nix-daemon
```

### 4. Verify

```bash
nix --extra-experimental-features nix-command \
    config show experimental-features      # must list nix-command and flakes
```

Ask Nix what it resolved rather than reading `nix.conf` back — that is what
catches a duplicate line or a daemon that was never restarted.

### 5. Use it

```bash
cd /path/to/bpf-arena-data-structures
git submodule update --init --recursive
nix develop
```

---

## Docker

For Ubuntu, following <https://docs.docker.com/engine/install/ubuntu/>.

### 1. Remove conflicting packages

Ubuntu's own `docker.io` and the unofficial packages own files `docker-ce` also
wants. This does not touch `/var/lib/docker`:

```bash
for pkg in docker.io docker-compose docker-compose-v2 docker-doc docker-buildx \
           podman-docker containerd runc; do
    sudo apt-get remove -y $pkg
done
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

### 3. Install

```bash
sudo apt-get install -y docker-ce docker-ce-cli containerd.io \
     docker-buildx-plugin docker-compose-plugin
```

`docker-buildx-plugin` is not optional here: this repo's `Dockerfile` starts
with `# syntax=docker/dockerfile:1.7` and uses `RUN --mount=type=cache`, which
the legacy builder cannot parse.

### 4. Start the daemon

```bash
sudo systemctl enable --now docker.service containerd.service
systemctl is-active docker.service          # -> active
```

### 5. Non-root access

```bash
sudo usermod -aG docker "$USER"
newgrp docker                    # this shell only; otherwise log out and back in
```

The `docker` group is root-equivalent on this host: the daemon runs as root and
will bind-mount any path into a container. Skip this step and use `sudo docker`
if that matters here.

### 6. Verify

```bash
docker version                   # both Client and Server sections
docker buildx version
docker run --rm hello-world
```

Only a `Client` section means the daemon is not running (step 4) or your user
cannot reach its socket (step 5).

### 7. Use it

```bash
cd /path/to/bpf-arena-data-structures
git submodule update --init --recursive
docker build -t bpf-arena-ds .
scripts/init-docker.sh
```

Build *inside* the container rather than reusing a `build/` populated on the
host: host binaries are linked against the host's loader and fail at exec. The
converse is that a container `make` leaves root-owned files in the bind-mounted
`build/` and `.output/`, so clean those from a container too.
