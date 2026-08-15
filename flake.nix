{
  description = "bpf-arena-data-structures: artifact toolchain, reference 6.18 kernel, and a bootable QEMU VM";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  inputs.self.submodules = true;

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system:
        f system (import nixpkgs { inherit system; }));

      # ---------------------------------------------------------------------
      # Toolchain
      # ---------------------------------------------------------------------
      artifactTools = pkgs: with pkgs; [
        llvmPackages_20.clang
        llvmPackages_20.llvm
        gcc
        gnumake
        binutils
        pkg-config
        elfutils
        zlib
        openssl
        git
        (python3.withPackages (ps: [ ps.networkx ]))
      ];

      bpftoolFullTools = pkgs: with pkgs; [
        libbfd
        libopcodes
        libcap
      ];

      kernelTools = pkgs: with pkgs; [
        bison flex bc perl rsync cpio kmod
        openssl ncurses
        pahole
        zstd xz gzip
        curl cacert
        qemu_kvm
      ];

      # ---------------------------------------------------------------------
      # Kernel: Linux 6.18 built from kernel/configs/full-6.18.2-bpf.config
      # ---------------------------------------------------------------------
      exactKernelFor = pkgs: pkgs.linuxManualConfig {
        inherit (pkgs.linux_6_18) src;
        version = pkgs.linux_6_18.version;
        configfile = ./kernel/configs/full-6.18.2-bpf.config;
        allowImportFromDerivation = true;
      };

      deltaKernelFor = pkgs: pkgs.linux_6_18.override {
        ignoreConfigErrors = true;
        structuredExtraConfig = with pkgs.lib.kernel; {
          BPF_SYSCALL = yes;
          BPF_JIT = yes;
          SECURITY = yes;
          BPF_LSM = yes;
          LSM = freeform "landlock,lockdown,yama,loadpin,safesetid,integrity,bpf";
          PERF_EVENTS = yes;
          KPROBES = yes;
          KPROBE_EVENTS = yes;
          UPROBE_EVENTS = yes;
          FTRACE = yes;
          FUNCTION_TRACER = yes;
          DYNAMIC_FTRACE = yes;
          DEBUG_INFO_BTF = yes;
          DEBUG_INFO_BTF_MODULES = yes;
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
        default = pkgs.mkShell {
          name = "bpf-arena-ds";
          packages = artifactTools pkgs ++ kernelTools pkgs ++ bpftoolFullTools pkgs;

          hardeningDisable = [ "all" ];

          shellHook = ''
            export CLANG=clang
            export CC=gcc

            export CLANG_BPF_SYS_INCLUDES="$(
              NIX_CFLAGS_COMPILE="" NIX_CFLAGS_COMPILE_BEFORE="" \
                clang -v -E - </dev/null 2>&1 \
                | sed -n '/<...> search starts here:/,/End of search list./{ s| \(/.*\)|-idirafter \1|p }' \
                | tr '\n' ' '
            )"

            echo "bpf-arena-data-structures dev shell"
            echo "  clang : $(clang --version | head -n1)"
            echo "  gcc   : $(gcc  --version | head -n1)"
            echo "  python: $(python3 --version)"
            echo "  kernel: bison flex bc pahole qemu on PATH"
            echo "  libs  : libbfd/libopcodes + libcap (full bpftool: jited dump, feature probe)"
            echo
            echo "  build : git submodule update --init --recursive && make"
            echo "  run   : sudo python3 scripts/run_all.py   (build first: it compiles nothing)"
            echo "  test  : python3 scripts/usertests.py --build"
            echo "  kconf : python3 scripts/check_kconfig.py"
            echo "  kbuild: ./scripts/build-kernel.sh --base running -y"
            echo "  vm    : nix run .#vm     (boots Linux ${pkgs.linux_6_18.version} with this repo's config)"
          '';
        };

        kernel = self.devShells.${system}.default;
      });

      # =====================================================================
      # packages
      # =====================================================================
      packages = forAllSystems (system: pkgs:
        {
          default = self.packages.${system}.artifact;

          artifact = pkgs.stdenv.mkDerivation {
            pname = "bpf-arena-data-structures";
            version = "0.1.0";
            src = self;

            nativeBuildInputs = artifactTools pkgs;
            buildInputs = with pkgs; [ elfutils zlib ];

            hardeningDisable = [ "all" ];

            postPatch = ''
              patchShebangs scripts third_party/libbpf third_party/bpftool
            '';

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
          kernel = exactKernelFor pkgs;

          kernel-delta = deltaKernelFor pkgs;

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
            boot.kernelPackages = pkgs.linuxPackagesFor (exactKernelFor pkgs);

            boot.kernelParams = [
              "lsm=landlock,lockdown,yama,loadpin,safesetid,integrity,bpf"
              "console=ttyS0"
            ];

            # ---- guest environment -----------------------------------------
            environment.systemPackages =
              artifactTools pkgs ++ bpftoolFullTools pkgs
              ++ (with pkgs; [ bpftools strace trace-cmd ]);

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
                source = "\${AE_REPO:-$PWD}";
                target = "/repo";
                securityModel = "none";
              };
            };

            environment.etc."motd".text = ''

              bpf-arena-data-structures artifact VM
              kernel: ${config.boot.kernelPackages.kernel.version}

              Build on the HOST first, inside `nix develop`:  make
              /repo is your checkout and the guest shares the host's Nix store,
              so build/ is already populated and runnable here. Do not `make`
              in the guest: a NixOS system environment carries no -dev outputs
              (no libelf.h), which is the same failure `sudo make` gives.

                cd /repo
                python3 scripts/run_all.py     # kconfig -> usertests -> runner
                                               # already root; it compiles nothing

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
