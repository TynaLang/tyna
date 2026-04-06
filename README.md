# Tyl

Tyl is a tiny programming language implemented using LLVM. It features functions, basic types, and can either JIT, compile and run or emit LLVM IR.

## Prerequisites

- **LLVM** (Preferably the latest version)
- **Clang/C Compiler**
- **CMake** (3.15+)
- **Just** (Case-sensitive command runner)

## Installation & Running

### Nix (NixOS or standalone)

If you have Nix installed with flakes enabled, you can enter the development environment directly:

```bash
nix develop
```

Once inside the shell, you can use `just` to build and run:

```bash
just run examples/test.tyl
```

### Arch Linux

Install the necessary dependencies:

```bash
sudo pacman -S llvm clang cmake just
```

To build and run the project:

```bash
just configure
just build
# Run an example
./build/tyl examples/test.tyl
```

### Usage

```bash
Usage: tyl [options] <file>
Options:
  -c, --compile   Compile to executable
  -j, --jit       JIT compile and run (default)
  -e, --emit-ir   Emit LLVM IR to file
  -d, --dump      Dump AST and LLVM IR to stdout
  -h, --help      Show this help message
```

You can also use the `just` commands:

- `just run <file>`: Build and run a file.

## Feature Set

Tyl supports the following language features:

- **Scalar types:** `int`, `float`, `double`, `char`, `bool`, `string`, with `const` and `let` declarations
- **Type casting:** Explicit and implicit promotion (e.g., `int` → `float`)
- **Arithmetic & binary operators:** `+`, `-`, `*`, `/`, `%`, `^`, `==`, `!=`, `<`, `>`, `<=`, `>=`, `&&`, `||`
- **Control flow:** `if`, `else`, ternary operator (`? :`)
- **Functions:** Definition, recursion, mutual recursion, return values
- **Arrays:** Declaration, initialization, indexing, multi-dimensional (partial)
- **Defer:** Cleanup blocks (like Go's `defer`)
- **Print:** Variadic, supports multiple types
- **Error handling:** Compile-time and runtime errors, bounds checking (partial)
- **String operations:** Equality, length, subscripting, `to_array`, `from_array`
- **Increment/decrement:** `++`, `--`
- **Comments:** `//` single-line
- **Edge cases:** Out-of-bounds, type errors, const assignment

See `examples/tests/` for comprehensive language and feature tests.
