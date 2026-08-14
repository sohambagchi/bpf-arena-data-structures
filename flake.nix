{
  description = "bpf-arena-data-structures: artifact toolchain, reference 6.18 kernel, and a bootable QEMU VM";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  # libbpf, bpftool and blazesym live in third_party/ as git submodules. Without
  # this, `self` is the superproject tree only and those directories arrive
  # empty, so packages.artifact cannot build libbpf.a or bootstrap bpftool.
  # Requires Nix >= 2.27; run `git submodule update --init --recursive` first.
  inputs.self.submodules = true;

  outputs = { self, nixpkgs }:
    let
      # The artifact is x86-64 Linux (ae.tex: "Binary: built from source; x86-64
      # Linux"). aarch64 is carried for the devShell only -- the kernel and VM
      # outputs are pinned to x86_64-linux because the reference .config is an
      # x86 config.
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system:
        f system (import nixpkgs { inherit system; }));

      # ---------------------------------------------------------------------
      # Toolchain
      # ---------------------------------------------------------------------
      # Makefile defaults to CLANG ?= clang-20 and falls back to `clang` when
      # clang-20 is not on PATH, so shipping llvmPackages_20.clang as plain
      # `clang` is enough. It must be the *wrapped* clang: the Makefile derives
      # CLANG_BPF_SYS_INCLUDES from `clang -v -E -`, and only the wrapper puts
      # the libc include dirs in that search list (asm/types.h et al).
      artifactTools = pkgs: with pkgs; [
        llvmPackages_20.clang # BPF target + CLANG_BPF_SYS_INCLUDES
        llvmPackages_20.llvm  # llvm-objdump, llvm-strip
        gcc                   # userspace side, and libbpf/bpftool bootstrap
        gnumake
        binutils              # objdump, for `make asm`
        pkg-config
        elfutils              # libelf, required by libbpf and bpftool
        zlib
        openssl               # third_party/bpftool/src/sign.c, even for `bpftool bootstrap`
        git                   # submodules; bpftool's version stamping
        (python3.withPackages (ps: [ ps.networkx ])) # runner.py/usertests.py + calculus
      ];

      # Everything needed to build a kernel from kernel/configs/*.config by
      # hand, inside the shell or the container. pahole is not optional:
      # CONFIG_DEBUG_INFO_BTF=y fails the build without it.
      kernelTools = pkgs: with pkgs; [
        bison flex bc perl rsync cpio kmod
        openssl ncurses
        pahole                # /sys/kernel/btf/vmlinux
        zstd xz gzip
        qemu_kvm              # boot the result without touching the host
      ];

      # ---------------------------------------------------------------------
      # Kernel: Linux 6.18 built from kernel/configs/full-6.18.2-bpf.config
      # ---------------------------------------------------------------------
      # nixpkgs' linux_6_18 tracks whatever 6.18.x is current in the locked
      # nixpkgs; the checked-in .config was generated against 6.18.2. That is a
      # stable point-release delta, so the kernel's own oldconfig pass fills in
      # whatever moved. If you need the source to match the config exactly,
      # replace the `src`/`version` pair below with a fetchurl of the 6.18.2
      # tarball from cdn.kernel.org.
      exactKernelFor = pkgs: pkgs.linuxManualConfig {
        inherit (pkgs.linux_6_18) src;
        version = pkgs.linux_6_18.version;
        configfile = ./kernel/configs/full-6.18.2-bpf.config;
        # linuxManualConfig has to read the resulting .config back to learn
        # whether the kernel has modules, which arch it is, etc.
        allowImportFromDerivation = true;
      };

      # Fallback path: take the nixpkgs kernel as-is and merge only the delta
      # fragment (kernel/configs/delta-bpf-arena.config) into it. Cheaper to
      # reason about and immune to the reference .config drifting away from
      # whatever NixOS itself needs, at the cost of not being bit-identical to
      # the kernel the paper's numbers were taken on.
      deltaKernelFor = pkgs: pkgs.linux_6_18.override {
        ignoreConfigErrors = true;
        structuredExtraConfig = with pkgs.lib.kernel; {
          # arena (there is no CONFIG_BPF_ARENA; it follows from BPF_SYSCALL)
          BPF_SYSCALL = yes;
          BPF_JIT = yes;
          # lsm.s/inode_create producer
          SECURITY = yes;
          BPF_LSM = yes;
          LSM = freeform "landlock,lockdown,yama,loadpin,safesetid,integrity,bpf";
          # uprobe.s consumer
          PERF_EVENTS = yes;
          KPROBES = yes;
          KPROBE_EVENTS = yes;
          UPROBE_EVENTS = yes;
          # BPF trampolines + trace_pipe for bpf_printk()
          FTRACE = yes;
          FUNCTION_TRACER = yes;
          DYNAMIC_FTRACE = yes;
          # BTF/CO-RE -- a runtime requirement, not just a build one
          DEBUG_INFO_BTF = yes;
          DEBUG_INFO_BTF_MODULES = yes;
          # optional comparison subsystems; KCOV_INSTRUMENT_ALL stays off so it
          # does not skew the ds_metrics latency numbers
          IO_URING = yes;
          KCOV = yes;
        };
      };
    in
    {
      # =====================================================================
      # devShells
      # =====================================================================
      devShells = forAllSystems (system: pkgs: {
        # nix develop  ->  git submodule update --init --recursive && make
        #
        # Deliberately does NOT pull in kernelTools: every -dev input lands in
        # clang's include search list, which the Makefile turns into
        # CLANG_BPF_SYS_INCLUDES and feeds to the BPF compile as -idirafter.
        # Keeping ncurses/openssl/qemu headers out of that keeps the BPF
        # objects reproducible. Use `nix develop .#kernel` to build a kernel.
        default = pkgs.mkShell {
          name = "bpf-arena-ds";
          packages = artifactTools pkgs;

          # nixpkgs' cc-wrapper injects hardening flags that the BPF backend
          # rejects outright: -fzero-call-used-regs=used-gpr is an error for
          # target bpf, and -fstack-protector-strong is silently ignored. They
          # would also perturb the userspace latency numbers, so drop them for
          # the whole build rather than just the BPF half.
          hardeningDisable = [ "all" ];

          shellHook = ''
            # The Makefile's clang-20 probe finds no clang-20 here and falls
            # back to `clang`, which is LLVM 20. Pin it explicitly anyway so a
            # stray clang-20 on the host PATH cannot win.
            export CLANG=clang
            export CC=gcc
            echo "bpf-arena-data-structures dev shell"
            echo "  clang : $(clang --version | head -n1)"
            echo "  gcc   : $(gcc  --version | head -n1)"
            echo "  python: $(python3 --version)"
            echo
            echo "  build : git submodule update --init --recursive && make"
            echo "  test  : python3 scripts/usertests.py --build"
            echo "  kconf : python3 scripts/check_kconfig.py"
            echo "  vm    : nix run .#vm     (boots Linux ${pkgs.linux_6_18.version} with this repo's config)"
          '';
        };

        # nix develop .#kernel  ->  everything above, plus what it takes to
        # configure, build and boot a kernel from kernel/configs/.
        kernel = pkgs.mkShell {
          name = "bpf-arena-ds-kernel";
          packages = artifactTools pkgs ++ kernelTools pkgs;
          hardeningDisable = [ "all" ];
          shellHook = ''
            export CLANG=clang
            cat <<'EOF'
            bpf-arena-data-structures kernel shell

              # exact reference config, built by nix:
              nix build .#kernel

              # or by hand, against your own linux tree:
              cd /path/to/linux
              cp ${toString ./kernel/configs/full-6.18.2-bpf.config} .config
              make olddefconfig && make -j$(nproc)

              # or merge just the delta into an existing .config:
              ./scripts/kconfig/merge_config.sh -m .config \
                  ${toString ./kernel/configs/delta-bpf-arena.config}
              make olddefconfig && make -j$(nproc)
            EOF
          '';
        };
      });

      # =====================================================================
      # packages
      # =====================================================================
      packages = forAllSystems (system: pkgs:
        {
          default = self.packages.${system}.artifact;

          # The 14 binaries, built hermetically from the vendored submodules.
          # Building needs no network: libbpf, bpftool and vmlinux.h are all
          # in-tree under third_party/.
          artifact = pkgs.stdenv.mkDerivation {
            pname = "bpf-arena-data-structures";
            version = "0.1.0";
            src = self;

            nativeBuildInputs = artifactTools pkgs;
            buildInputs = with pkgs; [ elfutils zlib ];

            # See the devShell comment: the BPF backend errors on
            # -fzero-call-used-regs=used-gpr, which cc-wrapper adds by default.
            hardeningDisable = [ "all" ];

            postPatch = ''
              patchShebangs scripts third_party/libbpf third_party/bpftool
            '';

            # CC must be gcc, not the stdenv default `cc`: clang's setup hook
            # makes `cc` resolve to clang, and the C11 userspace variants use
            # READ_ONCE() on plain __u64 fields, which clang rejects as
            # "address argument to atomic operation must be a pointer to
            # _Atomic type" while gcc accepts it. Only the BPF half is clang.
            makeFlags = [ "CLANG=clang" "CC=gcc" ];
            enableParallelBuilding = true;

            installPhase = ''
              runHook preInstall
              mkdir -p $out/bin
              cp build/* $out/bin/
              runHook postInstall
            '';

            meta = {
              description = "Lock-free data structures over BPF_MAP_TYPE_ARENA";
              license = with pkgs.lib.licenses; [ gpl2Only bsd2 ];
              platforms = [ "x86_64-linux" ];
            };
          };
        }
        // pkgs.lib.optionalAttrs (system == "x86_64-linux") {
          # nix build .#kernel  ->  bzImage + modules, from the reference config
          kernel = exactKernelFor pkgs;

          # nix build .#kernel-delta  ->  nixpkgs kernel + delta fragment only
          kernel-delta = deltaKernelFor pkgs;

          # nix run .#vm  ->  QEMU guest running that kernel, repo mounted at
          # /repo, root autologin. This is the only one of these environments
          # that actually gives you the 6.18 kernel at runtime.
          vm = self.nixosConfigurations.ae-vm.config.system.build.vm;
        });

      # =====================================================================
      # apps
      # =====================================================================
      apps = forAllSystems (system: pkgs:
        pkgs.lib.optionalAttrs (system == "x86_64-linux") {
          default = self.apps.${system}.vm;
          vm = {
            type = "app";
            program = "${self.packages.${system}.vm}/bin/run-ae-vm-vm";
          };
        });

      # =====================================================================
      # nixosConfigurations -- the kernel-in-an-environment answer
      # =====================================================================
      nixosConfigurations.ae-vm = nixpkgs.lib.nixosSystem {
        system = "x86_64-linux";
        modules = [
          "${nixpkgs}/nixos/modules/virtualisation/qemu-vm.nix"
          ({ config, pkgs, lib, ... }: {
            system.stateVersion = "25.11";
            networking.hostName = "ae-vm";

            # ---- the kernel under test -------------------------------------
            # Swap to deltaKernelFor if the reference .config turns out to be
            # missing something NixOS asserts on.
            boot.kernelPackages = pkgs.linuxPackagesFor (exactKernelFor pkgs);

            # CONFIG_LSM lists bpf, but say it on the command line too: the
            # single most common reason lsm.s/inode_create fails to attach on a
            # kernel that otherwise looks correct is bpf missing from the active
            # LSM list, and this needs no rebuild to fix.
            boot.kernelParams = [
              "lsm=landlock,lockdown,yama,loadpin,safesetid,integrity,bpf"
              "console=ttyS0"
            ];

            # ---- guest environment -----------------------------------------
            environment.systemPackages =
              artifactTools pkgs ++ (with pkgs; [ bpftools strace trace-cmd ]);

            # systemd mounts debugfs at /sys/kernel/debug on its own, which is
            # where scripts/runner.py reads trace_pipe from.

            # scripts/check_kconfig.py looks for /boot/config-$(uname -r) and
            # falls back to /proc/config.gz. The reference config sets neither
            # CONFIG_IKCONFIG nor CONFIG_IKCONFIG_PROC, and NixOS has no /boot
            # in a VM, so without this the artifact's own first command fails
            # inside the very environment built to satisfy it. Point it at the
            # post-olddefconfig .config the kernel was actually built from.
            systemd.tmpfiles.rules =
              let k = config.boot.kernelPackages.kernel; in [
                "L+ /boot/config-${k.modDirVersion} - - - - ${k.dev}/lib/modules/${k.modDirVersion}/build/.config"
              ];

            services.getty.autologinUser = "root";
            users.users.root.password = "";
            security.sudo.wheelNeedsPassword = false;

            # ---- host repo, live, at /repo ---------------------------------
            virtualisation = {
              memorySize = 4096;
              cores = 4;
              diskSize = 8192;
              graphics = false;
              sharedDirectories.repo = {
                # Expanded by the generated run-ae-vm-vm shell script, so this
                # defaults to wherever `nix run .#vm` was invoked from. Set
                # AE_REPO to point it elsewhere.
                source = "\${AE_REPO:-$PWD}";
                target = "/repo";
                # passthrough semantics without chown failures: the 9p server
                # runs as the invoking host user, who owns the checkout, while
                # the guest side is root. The mapped-xattr default would
                # instead scribble user.virtfs.* xattrs onto the host tree.
                securityModel = "none";
              };
            };

            environment.etc."motd".text = ''

              bpf-arena-data-structures artifact VM
              kernel: ${config.boot.kernelPackages.kernel.version}

                cd /repo
                python3 scripts/check_kconfig.py     # expect: OK: all requirements met
                make
                python3 scripts/usertests.py --build
                python3 scripts/runner.py

              Run the VM with AE_REPO=$PWD so /repo points at your checkout.

            '';
          })
        ];
      };

      # =====================================================================
      # checks -- `nix flake check` builds these
      # =====================================================================
      checks = forAllSystems (system: pkgs: {
        inherit (self.packages.${system}) artifact;

        # Static analysis only: no kernel is built or booted.
        kconfig-reference = pkgs.runCommand "kconfig-reference-check"
          {
            nativeBuildInputs = [ pkgs.python3 ];
          } ''
          cd ${self}
          python3 scripts/check_kconfig.py kernel/configs/full-6.18.2-bpf.config
          touch $out
        '';
      });

      formatter = forAllSystems (system: pkgs: pkgs.nixpkgs-fmt);
    };
}
