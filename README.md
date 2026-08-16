# BPF Arena Data Structures

Lock-free data structures backed by `BPF_MAP_TYPE_ARENA`, measured with a
kernel -> userspace -> kernel relay: a BPF program produces into a kernel->user
lane on `lsm.s/inode_create`, a userspace thread relays into a user->kernel
lane, and a uprobe-triggered kernel consumer drains it.

Eight relays (`build/skeleton_*`), six of which also have a userspace-only
pthread test (`build/usertest_*`):

| Data structure | Header | Relay | Usertest |
| --- | --- | --- | --- |
| Michael-Scott queue | `include/ds_msqueue.h` | `skeleton_msqueue` | yes |
| Vyukhov bounded MPMC | `include/ds_vyukhov.h` | `skeleton_vyukhov` | yes |
| Folly SPSC ring | `include/ds_folly_spsc.h` | `skeleton_folly_spsc` | yes |
| CK FIFO SPSC | `include/ds_ck_fifo_spsc.h` | `skeleton_ck_fifo_spsc` | yes |
| CK ring SPSC | `include/ds_ck_ring_spsc.h` | `skeleton_ck_ring_spsc` | yes |
| CK stack UPMC | `include/ds_ck_stack_upmc.h` | `skeleton_ck_stack_upmc` | yes |
| io_uring-style ring | `include/ds_io_uring.h` | `skeleton_io_uring` | no |
| kcov-style buffer | `include/ds_kcov.h` | `skeleton_kcov` | no |

`io_uring` and `kcov` model those subsystems' memory layouts in the arena; they
do not call into the real subsystems.

## Reproducing the results

Eight steps. Step 4 takes 20-60 minutes and needs ~22 GB free; step 5 reboots
the machine.

### 1. Pick your poison: Nix or Docker

Both give the same toolchain (clang-20, gcc, libelf, zlib, libbfd, libcap,
python3) plus the kernel build tools. Differences that matter:

- **Nix** runs everything, including the kernel build. Not on NixOS, which
  manages kernels declaratively — use `nix build .#kernel` / `nix run .#vm`
  there.
- **Docker** compiles the code and runs the userspace tests, but a container
  shares the host's kernel: the relays need the host to be running the kernel
  from step 4, and they need `scripts/init-docker.sh`, which grants the
  capabilities and mounts a plain `docker run` leaves out.

```bash
git submodule update --init --recursive    # required by both
```

### 2. Install Nix or Docker

```bash
./scripts/install-nix.sh        # installs Nix and enables flakes
./scripts/install-docker.sh     # installs Docker Engine on Ubuntu
```

Both are re-runnable and skip what is already done. `DOCKER_NIX_INSTALL.md` has
the manual steps.

### 3. Launch the environment

```bash
# Nix
nix develop

# Docker: build the image; the container itself is not needed until step 6,
# because steps 4 and 5 are host operations
docker build -t bpf-arena-ds .
```

### 4. Build and install kernel 6.18.44-bpf-arena

Downloads Linux 6.18.44, verifies its SHA-256, merges
`kernel/configs/delta-bpf-arena.config` into this machine's config, builds it,
and installs it as `6.18.44-bpf-arena` alongside your current kernel:

```bash
./scripts/build-kernel.sh --base running -y
```

Run it as a normal user; it requests `sudo` only for the install step. It
checks every dependency up front and names what is missing — see
[Installing the dependencies by hand](#installing-the-dependencies-by-hand) if
you are running it outside `nix develop` or the image.

Other bases: `--base full` (the reference `.config` verbatim), `--base
defconfig`, or `--config PATH`. Also `-j N`, `--no-install`.

On the Docker route, the container can do the *compile* — `--base full` and
`--base defconfig` need nothing from the host, and `--base running` works if
you bind-mount `/boot` — but it cannot install the result or reboot into it.
Pass `--no-install` and finish from the host side of the bind mount:

```bash
# host: not init-docker.sh, whose preflight wants the kernel you are about to build
docker run --rm -it -v "$PWD:/artifact" -v /boot:/host/boot:ro bpf-arena-ds

# container
./scripts/build-kernel.sh --base running --no-install

# host
cd kernel/build/linux-6.18.44
sudo make modules_install && sudo make install
```

That way the host needs only `make`, `kmod` and its usual boot tooling, not the
full kernel toolchain. Building on the host instead is the simpler path if you
already have those.

### 5. Reboot

Boot the `6.18.44-bpf-arena` entry, then confirm:

```bash
uname -r                        # 6.18.44-bpf-arena
cat /sys/kernel/security/lsm    # must contain 'bpf'
```

If `bpf` is missing, the fix is a kernel command line change, not a rebuild:
`python3 scripts/check_kconfig.py --print-lsm-fix` prints the `lsm=` line for
this machine.

### 6. Launch the environment again

```bash
nix develop                      # Nix
scripts/init-docker.sh           # Docker: checks the host kernel, then opens a
                                 # shell in /artifact with BPF privileges
```

### 7. Compile the data structures

```bash
make                            # 14 binaries into build/
```

Build as your normal user, never under `sudo`: `sudo` resets the environment,
so the dev shell's compiler settings are lost and the build fails on
`libelf.h: No such file or directory`. If that already happened, recover with
`sudo make clean`.

### 8. Run them all

```bash
sudo python3 scripts/run_all.py
```

Runs three stages against the binaries from step 7 — kernel config check,
userspace tests, then the eight relays — and ends with a ranking plus
`build/runner_metrics_<timestamp>.csv` (one row per data structure and relay
lane, plus an `ALL` aggregate). Exit status is 0 only if every stage passed.

Inside the Docker container you are already root, so drop the `sudo`.

Useful variations:

```bash
python3 scripts/run_all.py --only usertests     # one stage (repeatable)
python3 scripts/run_all.py --skip runner
sudo python3 scripts/run_all.py --keep-going    # do not stop at the first failure
sudo python3 scripts/run_all.py skeleton_msqueue skeleton_ck_ring_spsc
```

## Running one relay by hand

```bash
sudo build/skeleton_msqueue -v
```

In another shell, create files to trigger `inode_create` events:

```bash
touch /tmp/bpf-arena-relay-{1..20}
```

`Ctrl+C` triggers the uprobe consumer and prints the statistics table. All
relays accept `-v` (verify both lanes on exit), `-s` (statistics, on by
default) and `-h`.

## Installing the dependencies by hand

`nix develop` and the Docker image already carry all of this. You need it only
where you run a step outside them — in practice step 4, if you build the kernel
on the host rather than in the container. The commands below are for Ubuntu.

### To build the data structures (step 7)

```bash
sudo apt-get update
sudo apt-get install -y \
    build-essential pkg-config git python3 \
    libelf-dev zlib1g-dev
```

`build-essential` supplies gcc and make; `pkg-config` is how libbpf finds
libelf and zlib; `libelf` and `zlib` are also what the relays link against.

Then Clang with BPF target support. The Makefile wants `clang-20` and falls
back to `clang`, so Ubuntu's own package works if it is recent enough:

```bash
sudo apt-get install -y clang
```

To install LLVM 20 from upstream instead:

```bash
sudo apt-get install -y ca-certificates curl gnupg lsb-release \
    software-properties-common                       # what llvm.sh needs
curl -fsSL https://apt.llvm.org/llvm.sh -o llvm.sh
chmod +x llvm.sh
sudo ./llvm.sh 20
```

### To build the kernel (step 4)

Everything above, plus:

```bash
sudo apt-get install -y \
    bison flex bc kmod dwarves libssl-dev libncurses-dev \
    rsync cpio perl tar xz-utils zstd gzip file curl
```

`dwarves` provides `pahole`, which generates the BTF that
`CONFIG_DEBUG_INFO_BTF=y` needs — without it the relays cannot attach.
`libncurses-dev` is only for `make menuconfig`, which you need if you ever have
to fix the generated `.config` by hand.

### For a full `bpftool` (optional)

Not needed by `make`, which builds only `bpftool bootstrap` — the minimal host
build used to emit skeletons. These matter if you want to build the complete
`bpftool` from `third_party/bpftool`, whose Makefile silently drops features
when they are absent: JIT disassembly (`bpftool prog dump jited`) without
`libbfd`, and the capability half of `bpftool feature probe` without `libcap`.

```bash
sudo apt-get install -y binutils-dev libcap-dev llvm-dev
```

bpftool looks for an *unversioned* `llvm-config`, which Ubuntu's `llvm-dev`
provides but `apt.llvm.org` does not. If you installed LLVM 20 from upstream,
put its bin directory on `PATH`:

```bash
export PATH="$PATH:/usr/lib/llvm-20/bin"
```

## Repository layout

- `include/` data structure headers, arena API, metrics
- `src/` BPF relay pairs (`skeleton_*.bpf.c` + `skeleton_*.c`)
- `usertest/` userspace-only pthread tests
- `scripts/` install, kernel build, kernel config check, and the pipeline
- `kernel/configs/` the BPF fragment and the reference `.config`
- `docs/` architecture, memory-ordering and design notes
- `flake.nix`, `Dockerfile` the two packaged environments
