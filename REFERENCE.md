# ExtendedC Language Reference

ExtendedC (`.ec`) is a statically-typed, object-oriented, garbage-collected programming language that compiles to LLVM IR and links to native x86-64 binaries via `clang`. It features classes with single inheritance and vtable dispatch, closures, exceptions, type-erased collections, `async`/`await` multithreading, and a complete standard library.

---

## Table of Contents

1. [Quick Start](#1-quick-start)
2. [Source Files and Build Pipeline](#2-source-files-and-build-pipeline)
3. [Types](#3-types)
4. [Literals](#4-literals)
5. [Variables and Declarations](#5-variables-and-declarations)
6. [Operators](#6-operators)
7. [Control Flow](#7-control-flow)
8. [Functions](#8-functions)
9. [Classes](#9-classes)
10. [Enums](#10-enums)
11. [Structs and Unions](#11-structs-and-unions)
12. [Arrays](#12-arrays)
13. [Strings](#13-strings)
14. [Closures and Lambdas](#14-closures-and-lambdas)
15. [Exceptions](#15-exceptions)
16. [Imports and Modules](#16-imports-and-modules)
17. [Type Aliases — using](#17-type-aliases--using)
18. [Resource Blocks — with](#18-resource-blocks--with)
19. [Export](#19-export)
20. [Async / Await and Multithreading](#20-async--await-and-multithreading)
21. [Garbage Collector](#21-garbage-collector)
22. [Standard Library — std.io](#22-standard-library--stdio)
23. [Standard Library — std.string](#23-standard-library--stdstring)
24. [Standard Library — std.math](#24-standard-library--stdmath)
25. [Standard Library — std.collections](#25-standard-library--stdcollections)
26. [Terminal Primitives](#26-terminal-primitives)
27. [Compiler Reference](#27-compiler-reference)
28. [Memory and Object Model](#28-memory-and-object-model)
29. [LLVM IR ABI](#29-llvm-ir-abi)
30. [Self-Hosting Status](#30-self-hosting-status)
31. [ecvim Editor](#31-ecvim-editor)
32. [Bugs Fixed](#32-bugs-fixed)

---

## 1. Quick Start

```ec
import "std.io";
import "std.string";

fun int main() {
    println("Hello, ExtendedC!");
    int x = 42;
    println("x = " + toString(x));
    return 0;
}
```

**Compile and run:**

```bash
cd ~/Project/ExtendedC/ExtendedC_dist
sudo make install                       # installs /exec/ec and /exec/runtime.o

/exec/ec hello.ec -o hello.ll -I /exec/stdlib
clang -O2 hello.ll /exec/runtime.o -lm -o hello
./hello
```

---

## 2. Source Files and Build Pipeline

### File extensions

| Extension | Description |
|---|---|
| `.ec` | ExtendedC source file |
| `.ll` | LLVM IR output (text, intermediate) |
| `.o` | Object file |

### Compilation stages

```
source.ec ──► Lexer ──► Parser ──► TypeChecker ──► CodeGen ──► source.ll
source.ll + /exec/runtime.o ──► clang -O2 ──► native binary
```

### Compiler invocation

```bash
ec [options] file.ec [file2.ec ...]
```

| Option | Description |
|---|---|
| `-o <file>` | Output `.ll` path (default: `output.ll`) |
| `-I <dir>` | Add import search directory; repeatable |
| `--ast` | Print parsed AST to stdout before codegen |
| `--ir` | Print generated IR to stdout after writing |
| `--verbose` | Show module loading and resolution details |
| `--no-stop` | Collect all errors instead of stopping at the first |
| `--sample` | Compile and emit the built-in demo program |
| `--string "<src>"` | Compile a source string directly from the command line |
| `-h` / `--help` | Show usage |

### Import search order

When `import "mymod"` is encountered the compiler searches for `mymod.ec` in:

1. The directory containing the importing file
2. Each `-I <dir>` path in the order provided
3. The current working directory

Dot notation converts to path separators: `import "utils.math"` resolves to `utils/math.ec`.

### Multi-file compilation

All source files are merged into a single program before type-checking and codegen. There are no separate namespaces between files — all top-level names from all imported files share one global scope.

```bash
ec main.ec utils.ec models.ec -o app.ll -I /exec/stdlib -I ./lib
clang -O2 app.ll /exec/runtime.o -lm -o app
```

---
[...]