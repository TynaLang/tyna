{
  description = "Tiny LLVM language dev env";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  };

  outputs =
    { self, nixpkgs }:
    {
      devShells.x86_64-linux.default =
        let
          pkgs = import nixpkgs { system = "x86_64-linux"; };
        in
        pkgs.mkShell {
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
            export LLVM_PATH="${pkgs.llvmPackages_latest.libllvm.dev}"
            export LLVM_INCLUDE_DIR="$LLVM_PATH/include"
            export C_INCLUDE_PATH=${pkgs.glibc.dev}/include
            export CPLUS_INCLUDE_PATH=${pkgs.glibc.dev}/include
            export CLANGD_PATH="${pkgs.llvmPackages_latest.clang}/bin/clangd" 
            cat <<EOF > .clangd
            CompileFlags:
              Add:
                - "-I${pkgs.llvmPackages_latest.libllvm.dev}/include"
                - "-I${pkgs.glibc.dev}/include"
                - "-L${pkgs.llvmPackages_latest.libllvm.lib}/lib"
            EOF

            echo "LLVM dev environment ready"
            llvm-config --version
          '';
        };
    };
}
