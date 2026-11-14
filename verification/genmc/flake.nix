{
  description = "Pinned GenMC environment for bounded SPSCQueue verification";

  inputs.nixpkgs.url = "https://releases.nixos.org/nixpkgs/nixpkgs-26.11pre1109744.4fe25e12eb9f/nixexprs.tar.zst";

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forAllSystems = nixpkgs.lib.genAttrs systems;
      package = system:
        let
          pkgs = import nixpkgs { inherit system; };
          llvm = pkgs.llvmPackages_20;
        in pkgs.stdenv.mkDerivation {
          pname = "genmc";
          version = "0.20.0";
          src = pkgs.fetchFromGitHub {
            owner = "MPI-SWS";
            repo = "genmc";
            rev = "v0.20.0";
            hash = "sha256-kVuz+L2NbKSPsrJXGjmkC5fs/MM42S32uYUSkEp1yW0=";
          };
          nativeBuildInputs = [ pkgs.cmake pkgs.ninja ];
          buildInputs = [ llvm.llvm.dev pkgs.libffi pkgs.zlib pkgs.libedit ];
          # LLVM and Clang live in separate Nix store paths. Use the wrapped
          # compiler so it also finds the matching C++ standard-library headers.
          postPatch = ''
            substituteInPlace CMakeLists.txt \
              --replace-fail 'find_program(CLANGPATH clang PATHS ''${LLVM_TOOLS_BINARY_DIR} NO_CACHE NO_DEFAULT_PATH REQUIRED)' \
              'set(CLANGPATH "${llvm.clang}/bin/clang")'
            substituteInPlace CMakeLists.txt \
              --replace-fail 'find_program(LLVM_CONFIG_PATH llvm-config PATHS ''${LLVM_TOOLS_BINARY_DIR} NO_CACHE NO_DEFAULT_PATH REQUIRED)' \
              'set(LLVM_CONFIG_PATH "${llvm.llvm.dev}/bin/llvm-config")'
          '';
          cmakeFlags = [
            "-DBUILD_TESTS=OFF"
            "-DBUILD_DOC=OFF"
            # GenMC prefixes this directory with CMAKE_INSTALL_PREFIX itself.
            "-DCMAKE_INSTALL_INCLUDEDIR=include"
          ];
        };
    in {
      packages = forAllSystems (system: {
        default = package system;
      });
      devShells = forAllSystems (system:
        let pkgs = import nixpkgs { inherit system; };
        in {
          default = pkgs.mkShell {
            packages = [ self.packages.${system}.default pkgs.python3 ];
          };
        });
    };
}
