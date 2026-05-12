# ExtendedC Compiler

A statically-typed, object-oriented, ahead-of-time compiled language.
The compiler is written in C++17 and produces LLVM IR, which `clang` links
into a native binary.

---

## Quick start

```bash
make                          # build compiler + runtime
make run                      # compile and run the built-in demo
make compile-file SRC=my.lang # compile your own file
```

**Requirements:** `clang` (to link IR) and `g++` or `clang++` (to build the
compiler). On Ubuntu/Debian:

```bash
sudo apt install clang g++
```

---

## CLI

```
build/ec [options] file.lang [file2.lang ...]

  -o <file>       Output .ll file          (default: build/output.ll)
  -I <dir>        Add import search path
  --sample        Compile built-in demo
  --ast           Print AST
  --ir            Print IR to stdout
  --verbose       Show module loading
  --string "<s>"  Compile source string
  --help          Usage
```

---

## Language

### Types

| Type     | Description                        |
|----------|------------------------------------|
| `bool`   | true / false                       |
| `char`   | 8-bit character                    |
| `int`    | 32-bit signed integer              |
| `long`   | 64-bit signed integer              |
| `float`  | 32-bit float                       |
| `double` | 64-bit float                       |
| `string` | GC-managed null-terminated string  |
| `T[]`    | GC-managed array of T              |
| `var`    | type-inferred local variable       |

### Functions

```
fun ReturnType name(Type param, ...) [throws] { body }
```

### Classes, inheritance, vtable dispatch

```
class Animal {
    protected string name;
    public fun void init(string n) { this.name = n; }
    public fun void speak()        { println("..."); }
}

class Dog extends Animal {
    public fun void init(string n) { super.init(n); }
    public fun void speak()        { println("Woof!"); }
}

Animal a = new Dog("Rex");   // polymorphic upcast
a.speak();                   // dispatches through vtable → Dog_speak
```

Every non-private, non-static method is virtual. `init` is always called
statically. `super.method()` is a direct call.

### instanceof and cast

```
if (a instanceof Dog) {
    Dog d = a as Dog;        // safe downcast
    d.speak();
}
```

### Arrays

```
int[] nums = new int[5];
nums[0] = 42;
int n = nums.length;

int[] lit = {1, 2, 3};
```

### Strings

```
string s = "Hello";
int  n = s.length;           // ec_str_len
char c = s[0];               // ec_char_at → char
string t = toLower(s);
string u = replace(s, "Hello", "Hi");
```

### Closures

```
int base = 10;
var adder = (int x) => x + base;
int result = adder(5);       // 15 — indirect call through fat pointer
```

Closures capture by value into a GC-allocated env struct. The fat pointer
is `{ fn_ptr, env_ptr }` (16 bytes).

### Exceptions

```
fun int divide(int a, int b) throws {
    if (b == 0) throw "division by zero";
    return a / b;
}

try {
    int r = divide(10, 0);
} catch (Exception e) {
    println("Caught: " + e);
}
```

### Multi-file imports

```
import "std.io";
import "std.collections";
import "mylib";              // loads mylib.lang from search path
```

---

## Standard library

All stdlib modules live in `stdlib/`. Pass `-I stdlib` to the compiler.

### std.io

I/O, file I/O, GC API, system:

```
print(s)  println(s)  readLine()  readInt()  readFloat()  prompt(msg)
readFile(path)  writeFile(path, data)  fileExists(path)
gcCollect()  gcStats()  gcLiveBytes()  gcLiveObjects()
exit(code)  timeMs()
```

### std.string

String operations and char operations:

```
toString(n)  parseInt(s)  parseFloat(s)  charToString(c)  boolToString(b)
stringLen(s)  charAt(s,i)  substring(s,lo,hi)
indexOf(s,sub)  lastIndexOf  contains  startsWith  endsWith  count  equals  compare
toLower  toUpper  trim  trimLeft  trimRight  repeat  replace  reverse
isDigit(c)  isAlpha  isAlnum  isSpace  isUpper  isLower
toLowerChar(c)  toUpperChar  charCode(c)  charFromCode(n)
```

### std.math

```
floor  ceil  round  trunc  sqrt  cbrt  pow  powInt  hypot
log  log2  log10  sin  cos  tan  asin  acos  atan  atan2
abs  absInt  absLong  min  max  minInt  maxInt  minLong  maxLong
clamp  clampInt  gcd  lcm  isEven  isOdd  isPrime
toRadians  toDegrees
Constants: PI  E  TAU
```

### std.collections

Four classes, all GC-managed, type-erased via `var` (cast with `as`):

#### List

```
List items = new List();
items.add("hello");
items.add("world");
string s = items.get(0) as string;
items.remove(0);
int n = items.length();
bool b = items.isEmpty();
items.insert(0, "first");
List slice = items.slice(1, 3);
List cat   = items.concat(other);
var last   = items.pop();
```

#### Map  (string → any)

```
Map m = new Map();
m.set("key", someObject);
var val = m.get("key");
bool has = m.has("key");
m.remove("key");
int n = m.count();
List keys = m.keys();
List vals = m.values();
var v = m.getOrDefault("k", fallback);
```

#### Set  (string set)

```
Set seen = new Set();
seen.add("apple");
bool has = seen.has("apple");
seen.remove("apple");
int n = seen.count();
List keys = seen.keys();
Set u = seen.union(other);
```

#### StringBuilder

```
StringBuilder sb = new StringBuilder();
sb.append("Hello").appendChar(',').append(" World");
sb.appendInt(42).appendBool(true);
sb.appendLine("next line");
string result = sb.toString();
int len = sb.length();
sb.clear();
```

---

## Garbage collector

Conservative mark-and-sweep. Every `new` allocates via `ec_new` which
prepends a hidden header. Collections trigger automatically at 512 KB,
doubling the threshold after each fruitless collection up to 256 MB.

Stack scanning reads the true top of the stack segment from
`/proc/self/maps` at startup, then scans every pointer-aligned word from
the current frame to that address at each collection. `setjmp` flushes
callee-saved registers before the scan.

---

## File layout

```
include/          C++ headers (AST, Lexer, Parser, TypeChecker, CodeGen, Compiler)
src/              C++ source + runtime.c
stdlib/           ExtendedC standard library
  std.io          I/O, file, GC, system
  std.string      String and char operations
  std.math        Math functions and constants
  std.collections List, Map, Set, StringBuilder classes
examples/         hello.lang, mathlib.lang
Makefile
README.md
```

---

## Self-hosting roadmap

The compiler is being ported to ExtendedC itself. Completed prerequisites:

- [x] `instanceof` / `as` cast
- [x] Polymorphic upcast (`Animal a = new Dog(...)`)
- [x] File I/O (`readFile`, `writeFile`, `fileExists`)
- [x] `List`, `Map`, `Set`, `StringBuilder` collections
- [x] Full string and char operation library
- [x] `bool`/`char` → `int` widening in type checker

Remaining porting order: **Lexer → Parser → TypeChecker → CodeGen →
Compiler → Driver → Bootstrap**.
