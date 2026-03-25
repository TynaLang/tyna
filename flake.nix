{
  description = "Tiny LLVM language dev env";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  };

  outputs = { self, nixpkgs }: {
    devShells.x86_64-linux.default =
      let
      pkgs = import nixpkgs { system = "x86_64-linux"; };
    in pkgs.mkShell {
      buildInputs = [
        pkgs.llvmPackages_latest.clang
          pkgs.llvmPackages_latest.llvm
          pkgs.cmake
          pkgs.lld
          pkgs.pkg-config
          pkgs.gdb
          pkgs.just
          pkgs.glibc.dev
      ];

      shellHook = ''
        export C_INCLUDE_PATH=${pkgs.glibc.dev}/include
        export CPLUS_INCLUDE_PATH=${pkgs.glibc.dev}/include

        echo "LLVM dev environment ready"
        llvm-config --version
        '';
    };
  };
}
