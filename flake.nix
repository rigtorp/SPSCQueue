{
  description = "SPSCQueue benchmark environment with optional comparison queues";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    mpmcqueue = {
      url = "github:rigtorp/MPMCQueue";
      flake = false;
    };
    dro-spsc-queue = {
      url = "github:drogalis/SPSC-Queue";
      flake = false;
    };
    atomic-queue = {
      url = "github:max0x7ba/atomic_queue";
      flake = false;
    };
  };

  outputs = {
    self,
    nixpkgs,
    mpmcqueue,
    dro-spsc-queue,
    atomic-queue,
  }: let
    supportedSystems = ["x86_64-linux" "aarch64-linux"];
    forAllSystems = nixpkgs.lib.genAttrs supportedSystems;
  in {
    packages = forAllSystems (system: let
      pkgs = nixpkgs.legacyPackages.${system};
      benchmark = pkgs.stdenv.mkDerivation {
        pname = "spscqueue-benchmark";
        version = "2.0.0";
        src = ./.;

        hardeningDisable = ["all"];

        nativeBuildInputs = [pkgs.cmake];
        buildInputs = [
          pkgs.boost.dev
          pkgs.dpdk
          pkgs.folly.dev
          pkgs.readerwriterqueue
        ];

        cmakeFlags = [
          "-DCMAKE_BUILD_TYPE=Release"
          "-DBUILD_TESTING=ON"
        ];

        env.NIX_CFLAGS_COMPILE = "-isystem ${mpmcqueue}/include -isystem ${dro-spsc-queue}/include -isystem ${atomic-queue}/include";

        buildFlags = ["SPSCQueueBenchmark"];

        installPhase = ''
          runHook preInstall
          mkdir -p $out/bin
          cp SPSCQueueBenchmark $out/bin/SPSCQueueBenchmark
          runHook postInstall
        '';
      };
    in {
      inherit benchmark;
      default = benchmark;
    });

    apps = forAllSystems (system: {
      benchmark = {
        type = "app";
        program = "${self.packages.${system}.benchmark}/bin/SPSCQueueBenchmark";
      };
      default = self.apps.${system}.benchmark;
    });

    devShells = forAllSystems (system: let
      pkgs = nixpkgs.legacyPackages.${system};
    in {
      default = pkgs.mkShell {
        packages = [
          pkgs.cmake
          pkgs.ninja
          pkgs.boost.dev
          pkgs.dpdk
          pkgs.folly.dev
          pkgs.readerwriterqueue
        ];
        shellHook = ''
          export NIX_CFLAGS_COMPILE="''${NIX_CFLAGS_COMPILE:-} -isystem ${mpmcqueue}/include -isystem ${dro-spsc-queue}/include -isystem ${atomic-queue}/include"
        '';
      };
    });
  };
}
