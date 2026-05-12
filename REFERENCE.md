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

## 3. Types

### Primitive types

| Type | LLVM IR | Width | Notes |
|---|---|---|---|
| `bool` | `i1` | 1 bit | `true` or `false` |
| `char` | `i8` | 8 bits | ASCII character (0–127) |
| `int` | `i32` | 32 bits | Signed integer |
| `uint` | `i32` | 32 bits | Unsigned integer |
| `long` | `i64` | 64 bits | Signed 64-bit integer |
| `ulong` | `i64` | 64 bits | Unsigned 64-bit integer |
| `float` | `float` | 32 bits | IEEE 754 single-precision |
| `ufloat` | `float` | 32 bits | Unsigned float (identical to `float`) |
| `double` | `double` | 64 bits | IEEE 754 double-precision |
| `string` | `i8*` | pointer | GC-managed, null-terminated UTF-8 |
| `void` | `void` | — | No value; function return type only |

### Special types

| Type | Description |
|---|---|
| `var` | Type inference — resolved from initializer at compile time |
| `null` | Assignable to any class, `string`, or `var` |
| `T[]` | GC-managed array of elements of type `T` |
| `T*` | Raw pointer to `T` (not GC-tracked) |

### Numeric widening hierarchy

The type checker permits implicit widening without a cast:

```
bool, char  ──►  int, uint, long, ulong, float, ufloat, double
char ──► int ──► uint ──► long ──► ulong ──► float ──► ufloat ──► double
int  ◄──►  uint     (same rank, freely interchangeable)
long ◄──►  ulong    (same rank, freely interchangeable)
```

Narrowing always requires an explicit cast:

```ec
double d = 3.99;
int    n = d as int;    // truncates to 3
```

---

## 4. Literals

### Integer literals

```ec
int  dec  = 1_000_000;      // decimal; underscores are ignored
int  hex  = 0xFF_A0;        // hexadecimal  (0x or 0X prefix)
int  bin  = 0b1010_1100;    // binary       (0b or 0B prefix)
int  oct  = 0o755;          // octal        (0o or 0O prefix)
```

**Suffixes** (case-insensitive, combinable):

| Suffix | Resulting type |
|---|---|
| `u` | `uint` |
| `l` | `long` |
| `ul` / `lu` | `ulong` |
| `f` | `float` (on float literals only) |

```ec
long  big  = 9_000_000_000l;
uint  mask = 0xDEAD_BEEFu;
ulong ul   = 0xFFFF_FFFF_FFFFul;
```

### Float literals

```ec
double pi  = 3.141_592_653;
double sci = 1.5e-10;
double big = 2.998E+8;
float  f   = 1.0f;           // 'f' suffix forces 32-bit float
```

### Character literals

```ec
char a   = 'A';
char nl  = '\n';    // newline        (0x0A)
char tab = '\t';    // horizontal tab (0x09)
char cr  = '\r';    // carriage return(0x0D)
char nul = '\0';    // null byte      (0x00)
char bs  = '\\';   // backslash      (0x5C)
char sq  = '\'';   // single quote   (0x27)
char esc = '\x1b'; // ESC byte       (0x1B)
char bel = '\a';   // bell           (0x07)
```

### String literals

```ec
string s   = "Hello, world!";
string ml  = "line1\nline2\ttabbed";
string esc = "\x1b[0m";         // ESC byte — ANSI reset sequence
string uni = "caf\u00E9";       // UTF-8 encoded Unicode (U+00E9 = é)
string raw = "path\\to\\file";  // literal backslash
```

**All supported escape sequences in strings and char literals:**

| Sequence | Byte value | Description |
|---|---|---|
| `\n` | 0x0A | Newline (LF) |
| `\t` | 0x09 | Horizontal tab |
| `\r` | 0x0D | Carriage return |
| `\0` | 0x00 | Null byte |
| `\\` | 0x5C | Backslash |
| `\"` | 0x22 | Double quote |
| `\'` | 0x27 | Single quote |
| `\a` | 0x07 | Bell |
| `\b` | 0x08 | Backspace |
| `\f` | 0x0C | Form feed |
| `\v` | 0x0B | Vertical tab |
| `\xHH` | 0xHH | Hex byte (1–2 hex digits) |
| `\uXXXX` | UTF-8 | Unicode BMP code point (4 hex digits), encoded as UTF-8 |

### Boolean literals

```ec
bool yes = true;
bool no  = false;
```

### Array literals

```ec
int[]    nums  = {1, 2, 3, 4, 5};
string[] days  = {"Mon", "Tue", "Wed"};
double[] curve = {0.0, 0.5, 1.0};
```

### Null literal

```ec
string   s   = null;
MyClass  obj = null;
var      x   = null;
```

---

## 5. Variables and Declarations

### Typed declaration

```ec
int    x    = 10;
string name = "Alice";
double pi   = 3.14159;
bool   flag;          // zero-initialised (false)
```

### Type inference with `var`

```ec
var result = compute();     // type = return type of compute()
var items  = new List();    // type = List*
var count  = 0;             // type = int
```

`var` requires an initializer. The inferred type is fixed at the point of declaration.

### Modifiers

| Modifier | Scope | Meaning |
|---|---|---|
| `const` | Any | Immutable after initialization |
| `static` | Top-level | One shared instance across all translation units |
| `export` | Top-level | Publicly visible to importing modules |

```ec
const int MAX_ROWS   = 500000;
static int instances = 0;
export int API_VERSION = 2;
```

### Scope rules

Variables are block-scoped. Inner blocks may shadow outer variables. Top-level variables are global and GC-registered.

---

## 6. Operators

### Precedence table (highest to lowest)

| Level | Operators | Associativity |
|---|---|---|
| 1 | `()` `[]` `.` `->` postfix `++` `--` | Left |
| 2 | `!` unary `-` `~` prefix `++` `--` `typeof` `delete` `new` `await` | Right |
| 3 | `*` `/` `%` | Left |
| 4 | `+` `-` | Left |
| 5 | `<<` `>>` | Left |
| 6 | `<` `<=` `>` `>=` `instanceof` `as` | Left |
| 7 | `==` `!=` | Left |
| 8 | `&` | Left |
| 9 | `^` | Left |
| 10 | `\|` | Left |
| 11 | `&&` | Left (short-circuit) |
| 12 | `\|\|` | Left (short-circuit) |
| 13 | `? :` | Right |
| 14 | `=` `+=` `-=` `*=` `/=` `%=` `&=` `\|=` `^=` `<<=` `>>=` | Right |

### Arithmetic

```ec
int a = 10 + 3;   // 13
int b = 10 - 3;   // 7
int c = 10 * 3;   // 30
int d = 10 / 3;   // 3  (integer division — truncates toward zero)
int e = 10 % 3;   // 1  (remainder)
```

### String concatenation

The `+` operator concatenates when the left operand is `string`. Non-string right-hand values are auto-converted:

```ec
string s = "Value: " + 42;             // "Value: 42"
string t = "pi = " + 3.14;            // "pi = 3.14"
string u = "flag = " + true;          // "flag = true"
string v = "Hello, " + name + "!";
```

### Bitwise operators

```ec
int and  = 0b1100 & 0b1010;    // 0b1000
int or   = 0b1100 | 0b1010;    // 0b1110
int xor  = 0b1100 ^ 0b1010;    // 0b0110
int not  = ~0b1100;             // bitwise complement
int lsh  = 1 << 4;             // 16
int rsh  = 256 >> 3;           // 32 (arithmetic right shift)
```

### Compound assignment

```ec
x += 5;   x -= 3;   x *= 2;   x /= 4;   x %= 7;
x &= 0xF; x |= 0x10; x ^= 0xFF;
x <<= 2;  x >>= 1;
```

### Increment / decrement

```ec
int x = 5;
x++;    // postfix: returns 5, then x becomes 6
++x;    // prefix:  x becomes 7, returns 7
x--;
--x;
```

### Ternary

```ec
int  max   = (a > b) ? a : b;
string lbl = (count == 1) ? "item" : "items";
```

Nested ternaries are fully supported — the compiler correctly tracks phi node predecessors across all nesting depths.

### `instanceof` and `as`

```ec
Animal a = new Dog("Rex");

if (a instanceof Dog) {
    Dog d = a as Dog;     // safe downcast after instanceof
    d.fetch();
}
```

`instanceof` compares vtable pointers at runtime. `as` is an unchecked bitcast. Numeric `as` truncates or extends:

```ec
double pi = 3.99;
int    n  = pi as int;    // 3 (truncation)
long   l  = n  as long;  // 3 (sign extension)
```

### `typeof`

```ec
string t = typeof expr;   // returns the LLVM type name as a string at runtime
```

### `delete`

```ec
delete obj;   // hint to GC that obj can be freed now
```

---

## 7. Control Flow

### `if` / `else if` / `else`

```ec
if (x > 0) {
    println("positive");
} else if (x < 0) {
    println("negative");
} else {
    println("zero");
}
```

### `while`

```ec
int i = 0;
while (i < 10) {
    i = i + 1;
}
```

### `do` / `while`

```ec
int x = 0;
do {
    x = x + 1;
} while (x < 5);
```

### `for`

All three parts of the `for` header are optional:

```ec
for (int i = 0; i < 10; i = i + 1) {
    println(toString(i));
}

for (;;) { if (done) break; }    // infinite loop
```

### `switch` / `case` / `default`

Subject must be integer or enum. Cases fall through if `break` is omitted.

```ec
switch (key) {
    case 0:   println("zero");  break;
    case 1:   println("one");   break;
    default:  println("other"); break;
}
```

### `break` and `continue`

```ec
while (true) {
    if (x < 0) break;      // exit innermost loop
    if (x == 0) continue;  // next iteration
    process(x);
}
```

### `with` — resource blocks

See [§18](#18-resource-blocks--with) for the full specification. In brief:

```ec
with (FileHandle f = openFile(path)) {
    process(f.read());
}
// f.close() called automatically — even if an exception was thrown
```

---

## 8. Functions

### Declaration

```ec
fun ReturnType name(Type param1, Type param2) {
    return value;
}
```

### Modifiers

| Modifier | Meaning |
|---|---|
| `async` | Spawns a thread; returns a Future immediately (see §20) |
| `static` | No implicit `this`; called on the class name |
| `export` | Publicly visible to importing modules (see §19) |
| `throws` | Declares the function may propagate exceptions |
| `final` | Advisory; may not be overridden (currently unenforced by codegen) |

```ec
export async fun int worker(int n) throws {
    if (n < 0) throw "negative input";
    return n * n;
}
```

### Void functions

```ec
fun void greet(string name) {
    println("Hello, " + name + "!");
}
```

### Recursion

```ec
fun int factorial(int n) {
    if (n <= 1) return 1;
    return n * factorial(n - 1);
}
```

### Static functions

```ec
class Math {
    public static fun double tau() { return 6.283185307179586; }
}

double t = Math.tau();
```

---

## 9. Classes

### Definition

```ec
class Animal {
    private string name;
    protected int  age;

    public fun void init(string n, int a) {
        this.name = n;
        this.age  = a;
    }

    public fun void speak() {
        println(this.name + " says nothing.");
    }

    public fun string getName() { return this.name; }
    public fun int    getAge()  { return this.age;  }
}
```

### Instantiation

```ec
Animal a = new Animal("Cat", 3);
a.speak();
```

### Access modifiers

| Modifier | Visibility |
|---|---|
| `public` | Accessible from anywhere |
| `protected` | Accessible in this class and all subclasses |
| `private` | Accessible only within this class |
| *(none)* | Same as `public` |

### Inheritance

```ec
class Dog extends Animal {
    private string breed;

    public fun void init(string n, int a, string b) {
        super.init(n, a);     // call parent constructor
        this.breed = b;
    }

    public fun void speak() { // overrides Animal.speak
        println(this.getName() + " says: Woof!");
    }

    public fun string getBreed() { return this.breed; }
}
```

- Single inheritance only.
- `super.method(args)` calls the parent's implementation directly (static call, not virtual).
- `super.init(...)` is the standard constructor-chaining pattern.

### Virtual dispatch

A method is virtual (dispatched through the vtable) if and only if it is:
- `public` or `protected`
- Non-static
- Not named `init`
- Has a body

```ec
Animal a = new Dog("Rex", 3, "Lab");   // polymorphic upcast
a.speak();                              // → Dog.speak via vtable
```

Every class object stores a vtable pointer as its first field. The vtable is a global `[N x i8*]` array of function pointers. Subclass overrides occupy the same slot index as in the superclass.

### `init` — constructor

`init` is called automatically by `new` after memory allocation and vtable-pointer initialisation. It is never virtual.

### `final` classes

```ec
final class Singleton {
    public fun void init() { }
}
```

### `instanceof` and `as`

```ec
Animal a = new Dog("Rex", 3, "Lab");
if (a instanceof Dog) {
    Dog d = a as Dog;
    println(d.getBreed());
}
```

---

## 10. Enums

```ec
enum Direction { NORTH, SOUTH, EAST, WEST }
enum Status    { OK, ERROR, PENDING, TIMEOUT }
```

Members are `i32` constants starting at 0. Qualified and unqualified forms are equivalent:

```ec
Direction d = NORTH;
Direction d = Direction::NORTH;

switch (d) {
    case Direction::NORTH: println("north"); break;
    default: println("other"); break;
}

int n = NORTH as int;    // 0
```

---

## 11. Structs and Unions

### Structs

Named product types. All fields are public. Allocated with `new`.

```ec
struct Point { float x; float y; }
struct Rect  { Point origin; float width; float height; }

Point p = new Point();
p.x = 3.0; p.y = 4.0;
```

Structs do not support inheritance or virtual methods.

### Unions

Variant storage — semantics identical to C unions. The LLVM IR uses a byte array sized to the largest member.

```ec
union Number { int asInt; float asFloat; long asLong; }

Number n = new Number();
n.asInt = 42;
int x = n.asInt;   // 42
```

Accessing a field that was not the last one written is undefined behavior.

---

## 12. Arrays

### Allocation

```ec
int[]    nums  = new int[10];      // zero-initialised; size may be a runtime value
string[] words = new string[5];
double[] data  = new double[n];
```

### Literals

```ec
int[]    lit  = {1, 2, 3, 4, 5};
string[] days = {"Mon", "Tue", "Wed"};
```

### Access

```ec
int first = nums[0];
nums[9]   = 42;
int len   = nums.length;    // or nums.size
```

No bounds checking is performed — out-of-bounds access is undefined behavior.

### Memory layout

```
GC header       (16 bytes, hidden ObjHeader)
[0..7]  i64     length — number of elements
[8..]   T[]     data   — length × sizeof(T) bytes
```

### Element sizes

| Element type | Bytes |
|---|---|
| `bool`, `char` | 1 |
| `int`, `uint`, `float` | 4 |
| `long`, `ulong`, `double` | 8 |
| any class / `string` / `var` / `T[]` | 8 (pointer) |

---

## 13. Strings

`string` is an alias for `i8*` — a GC-managed, null-terminated UTF-8 string.

### Indexing and length

```ec
string s = "Hello";
char   c = s[0];        // 'H'
int    n = s.length;    // 5
```

### Concatenation

```ec
string greeting = "Hello, " + name + "!";
string info     = "count = " + toString(count);
```

### Comparison

Use `equals` or `compare` — never `==` for content equality (which compares pointer addresses):

```ec
bool same = equals(s1, s2);    // true if identical content
int  cmp  = compare(s1, s2);  // <0 / 0 / >0
```

---

## 14. Closures and Lambdas

### Syntax

```ec
// Expression body
var square  = (int x) => x * x;
var add     = (int a, int b) => a + b;
var zero    = () => 0;

// Block body
var safeDiv = (int a, int b) => {
    if (b == 0) return 0;
    return a / b;
};
```

### Calling

```ec
int r = square(5);     // 25
int s = add(3, 4);     // 7
```

### Capture (by value)

Variables from the enclosing scope are captured **by value** at closure creation time:

```ec
int base = 10;
var addBase = (int x) => x + base;   // captures base = 10

base = 99;           // does NOT affect addBase
addBase(5);          // 15, not 104
```

### Passing closures as arguments

Use `var` for closure-typed parameters:

```ec
fun int apply(var fn, int x) {
    return fn(x) as int;
}

fun void forEach(int[] arr, var fn) {
    int i = 0;
    while (i < arr.length) { fn(arr[i]); i = i + 1; }
}

forEach({1, 2, 3}, (int x) => println(toString(x)));
```

### Memory layout

```
GC header    (16 bytes)
[0..7]  i8*  fn_ptr   — pointer to lambda's LLVM function
[8..15] i8*  env_ptr  — pointer to captured-variable env struct
```

The env struct contains one field per captured variable.

---

## 15. Exceptions

### `throw`

```ec
throw "something went wrong";
throw "error code: " + toString(code);
```

`throw` takes any `string`. The message is stored in a thread-local jmpbuf record.

### `throws` modifier

Functions that may propagate exceptions must declare `throws`:

```ec
fun int divide(int a, int b) throws {
    if (b == 0) throw "division by zero";
    return a / b;
}
```

### `try` / `catch`

```ec
try {
    int result = divide(10, 0);
} catch (Exception e) {
    println("Caught: " + e);
}
```

The exception variable holds the thrown string. The type annotation in `catch` is syntactically required; only `Exception` is currently meaningful.

### Implementation

Exceptions use `setjmp`/`longjmp` with a caller-owned jmpbuf:
- `ec_try_push(buf)` — register a jmpbuf
- `ec_throw(msg)` — `longjmp` into nearest registered frame
- `ec_catch_msg()` — pop frame, return exception string
- `ec_try_pop()` — pop on normal (non-throwing) exit

---

## 16. Imports and Modules

```ec
import "std.io";
import "std.string";
import "std.math";
import "std.collections";
import "mymodule";         // resolves to mymodule.ec via search path
import "utils/helper";     // resolves to utils/helper.ec
import "pkg.subpkg";       // dot-to-slash: pkg/subpkg.ec
```

### Built-in modules

| Module | Contents |
|---|---|
| `std.io` | I/O, file, time, threading, GC, terminal |
| `std.string` | String and character functions |
| `std.math` | Math functions and constants |
| `std.collections` | `List`, `Map`, `Set`, `StringBuilder` |

Built-in modules do not correspond to files — their symbols are registered directly by the type checker.

### Import semantics

All declarations from all imported files are merged into a single program. There is no separate namespace or symbol isolation between files.

### Cycle detection

Circular imports are detected during DFS loading and reported as compile errors.

---

## 17. Type Aliases — using

```ec
using Name = ExistingType;
```

Compile-time name substitution. Zero runtime cost.

```ec
using IntArray  = int[];
using Score     = double;
using Callback  = var;
using Timestamp = long;

IntArray scores = new int[10];
Score    grade  = 98.5;
```

`export using` makes the alias visible to importing modules:

```ec
export using Timestamp = long;
export using Handler   = var;
```

---

## 18. Resource Blocks — with

```ec
with (Type name = initializer) {
    // body
}
// name.close() called automatically on both normal and exceptional exit
```

### Desugar

```
Type name = initializer;
try {
    // body
    ec_try_pop();
} catch (Exception e) {
    name.close();
    rethrow(e);
}
name.close();
```

### Requirements

The resource type must have `public fun void close()`. If not present, cleanup is a no-op but the block still compiles.

### Example

```ec
class Connection {
    private string host;

    public fun void init(string h) {
        this.host = h;
        println("Connected to: " + h);
    }

    public fun string query(string sql) { return "result"; }

    public fun void close() {
        println("Disconnected from: " + this.host);
    }
}

with (Connection db = new Connection("localhost")) {
    string r = db.query("SELECT 1");
    println(r);
}
// "Disconnected from: localhost" printed automatically
```

---

## 19. Export

`export` marks a top-level declaration as intended for cross-module use. Works on functions, classes, variables, and type aliases.

```ec
export fun int add(int a, int b) { return a + b; }
export class Point { ... }
export int VERSION = 2;
export using Coord = double;
export async fun int worker(int n) { ... }
export final class Singleton { ... }
export static int counter = 0;
```

In the LLVM IR, `export` adds a `; export` comment before the function definition. The flag is stored in the AST and available to tooling.

---

## 20. Async / Await and Multithreading

### Declaring an async function

```ec
async fun ReturnType name(params) {
    // runs on a separate pthread
    return value;
}
```

### Spawning and awaiting

```ec
var future = heavyCompute(1000);    // returns immediately; thread spawned
doOtherWork();
var result = await future;          // blocks until thread finishes
int r = result as int;
```

### Non-blocking check

```ec
if (futureDone(future)) {
    var r = futureAwait(future);    // instant — already done
}
```

### Parallel execution pattern

```ec
var f1 = taskA(100);
var f2 = taskB(200);
var f3 = fetchData("url");
// All three running concurrently
var r1 = await f1;
var r2 = await f2;
var r3 = await f3;
```

### Internal code generation

The compiler emits two LLVM functions per `async fun`:

1. `@name_async_body(i8* env_raw) → i8*` — actual body; parameters unpacked from a GC env struct
2. `@name(params) → i8*` — spawner; packs params, calls `ec_future_new(@body, env)` which calls `pthread_create` + `pthread_detach`, returns the Future immediately

**Future memory layout (GC-managed, 32 bytes):**

```
[0..7]   i8*       result    — null until task completes
[8..11]  i32       done      — 0 = running, 1 = complete
[12..15] i32       padding
[16..23] pthread_t tid
[24..31] i32       reserved
```

`ec_future_await` busy-spins with `sched_yield()` until `done` is set.

### Mutexes

```ec
var m = mutexNew();

mutexLock(m);
sharedCounter = sharedCounter + 1;
mutexUnlock(m);

if (mutexTrylock(m)) { doWork(); mutexUnlock(m); }

mutexDestroy(m);
```

### Threading function reference

| Function | Signature | Description |
|---|---|---|
| `mutexNew()` | `→ var` | Allocate and initialise a mutex |
| `mutexLock(var)` | `→ void` | Block until mutex acquired |
| `mutexUnlock(var)` | `→ void` | Release mutex |
| `mutexTrylock(var)` | `→ bool` | Try to acquire without blocking |
| `mutexDestroy(var)` | `→ void` | Release OS mutex resources |
| `futureAwait(var)` | `→ var` | Block until Future completes; return result |
| `futureDone(var)` | `→ bool` | Non-blocking completion check |
| `threadSleep(int)` | `→ void` | Sleep calling thread N milliseconds |
| `threadYield()` | `→ void` | Yield calling thread to scheduler |

### GC thread safety note

The GC scans only the main thread's stack. Objects on worker thread stacks are tracked in the GC heap but not scanned. Pin long-lived worker objects with `gcPin`/`gcUnpin` or return them through the Future result.

---

## 21. Garbage Collector

ExtendedC uses a conservative mark-and-sweep collector. Every `new` calls `ec_new(bytes)` which prepends a hidden 16-byte `ObjHeader` and links the object into a global intrusive list.

### Parameters

| Parameter | Value |
|---|---|
| Initial threshold | 512 KB |
| Growth factor | ×2 after a fruitless collection |
| Maximum threshold | 256 MB |
| Max registered globals | 1024 |
| Max GC roots | 4096 |

### Stack scanning

`ec_gc_init()` reads `/proc/self/maps` at startup to find the true top of the main thread's stack. During collection, `setjmp` flushes registers to the stack, then every aligned word from the current SP to the stack top is scanned for pointers that look like GC allocations. A `pthread_getattr_np` fallback handles environments where `/proc` is unavailable.

### GC API

| Function | Signature | Description |
|---|---|---|
| `gcCollect()` | `→ void` | Force a collection |
| `gcStats()` | `→ string` | Human-readable summary |
| `gcLiveBytes()` | `→ long` | Live heap bytes |
| `gcLiveObjects()` | `→ long` | Live object count |
| `gcPin(var)` | `→ void` | Prevent an object from being freed |
| `gcUnpin(var)` | `→ void` | Allow a pinned object to be freed |

---

## 22. Standard Library — std.io

```ec
import "std.io";
```

### Console output

| Function | Signature | Description |
|---|---|---|
| `print(string)` | `→ void` | Print without newline |
| `println(string)` | `→ void` | Print with newline |
| `printNewline()` | `→ void` | Print only a newline |
| `printInt(int)` | `→ void` | Print integer |
| `printFloat(float)` | `→ void` | Print float |
| `printBool(bool)` | `→ void` | Print `true` or `false` |
| `putChar(char)` | `→ void` | Write one character to stdout |

### Console input

| Function | Signature | Description |
|---|---|---|
| `readLine()` | `→ string` | Read a line (strips `\n`) |
| `readWord()` | `→ string` | Read whitespace-delimited token |
| `readInt()` | `→ int` | Read line and parse integer |
| `readFloat()` | `→ float` | Read line and parse float |
| `getChar()` | `→ char` | Read one character |
| `prompt(string)` | `→ string` | Print message then read a line |
| `waitForKey(char)` | `→ bool` | Read one char; true if it matches |
| `flushInputLine()` | `→ void` | Discard rest of current input line |

### File I/O

| Function | Signature | Description |
|---|---|---|
| `readFile(string)` | `→ string` | Read entire file as a string |
| `writeFile(string, string)` | `→ int` | Write string to file; non-zero on success |
| `fileExists(string)` | `→ bool` | Check if a path exists |

### System

| Function | Signature | Description |
|---|---|---|
| `exit(int)` | `→ void` | Terminate process |
| `timeMs()` | `→ long` | Monotonic milliseconds since startup |

---

## 23. Standard Library — std.string

```ec
import "std.string";
```

### Type conversion

| Function | Signature | Description |
|---|---|---|
| `toString(int)` | `→ string` | Integer to decimal string |
| `toString(long)` | `→ string` | Long to decimal string |
| `toString(bool)` | `→ string` | `"true"` or `"false"` |
| `toString(char)` | `→ string` | Single-character string |
| `longToString(long)` | `→ string` | Long to string |
| `floatToString(float)` | `→ string` | Float to string |
| `doubleToString(double)` | `→ string` | Double to string |
| `boolToString(bool)` | `→ string` | Bool to string |
| `charToString(char)` | `→ string` | Char to string |
| `parseInt(string)` | `→ int` | Parse decimal integer |
| `parseLong(string)` | `→ long` | Parse long |
| `parseFloat(string)` | `→ float` | Parse float |
| `parseDouble(string)` | `→ double` | Parse double |

### String inspection

| Function | Signature | Description |
|---|---|---|
| `stringLen(string)` | `→ int` | Byte length |
| `stringEmpty(string)` | `→ bool` | True if length is zero |
| `charAt(string, int)` | `→ char` | Character at index |
| `indexOf(string, string)` | `→ int` | First occurrence index (–1 if absent) |
| `lastIndexOf(string, string)` | `→ int` | Last occurrence index (–1 if absent) |
| `contains(string, string)` | `→ bool` | Contains substring |
| `startsWith(string, string)` | `→ bool` | Has prefix |
| `endsWith(string, string)` | `→ bool` | Has suffix |
| `equals(string, string)` | `→ bool` | Content equality |
| `compare(string, string)` | `→ int` | Lexicographic: <0, 0, >0 |
| `count(string, string)` | `→ int` | Non-overlapping occurrence count |

### String transformation

| Function | Signature | Description |
|---|---|---|
| `substring(string, int, int)` | `→ string` | Substring `[lo, hi)` |
| `toLower(string)` | `→ string` | Lowercase copy |
| `toUpper(string)` | `→ string` | Uppercase copy |
| `trim(string)` | `→ string` | Strip leading and trailing whitespace |
| `trimLeft(string)` | `→ string` | Strip leading whitespace |
| `trimRight(string)` | `→ string` | Strip trailing whitespace |
| `repeat(string, int)` | `→ string` | Repeat N times |
| `replace(string, string, string)` | `→ string` | Replace all occurrences |
| `reverse(string)` | `→ string` | Reverse |

### Character functions

| Function | Signature | Description |
|---|---|---|
| `isDigit(char)` | `→ bool` | ASCII digit 0–9 |
| `isAlpha(char)` | `→ bool` | ASCII letter |
| `isAlnum(char)` | `→ bool` | ASCII alphanumeric |
| `isSpace(char)` | `→ bool` | Whitespace |
| `isUpper(char)` | `→ bool` | Uppercase letter |
| `isLower(char)` | `→ bool` | Lowercase letter |
| `isPunct(char)` | `→ bool` | Punctuation |
| `toUpperChar(char)` | `→ char` | Uppercase version |
| `toLowerChar(char)` | `→ char` | Lowercase version |
| `charCode(char)` | `→ int` | ASCII code point |
| `charFromCode(int)` | `→ char` | Character from code point |
| `charToString(char)` | `→ string` | Single-character string |

---

## 24. Standard Library — std.math

```ec
import "std.math";
```

### Constants

| Name | Value |
|---|---|
| `PI` | 3.141592653589793 |
| `E` | 2.718281828459045 |
| `TAU` | 6.283185307179586 |

### Rounding — all `double → double`

`floor` `ceil` `round` `trunc`

### Absolute value

`abs(double) → double`  `absInt(int) → int`  `absLong(long) → long`

### Powers and roots

| Function | Signature | Description |
|---|---|---|
| `sqrt(double)` | `→ double` | Square root |
| `cbrt(double)` | `→ double` | Cube root |
| `pow(double, double)` | `→ double` | xʸ |
| `powInt(int, int)` | `→ int` | Integer xʸ |
| `hypot(double, double)` | `→ double` | √(x²+y²) |

### Logarithms

`log(double)` `log2(double)` `log10(double)` — all `→ double`

### Trigonometry — all `double → double`

`sin` `cos` `tan` `asin` `acos` `atan`  
`atan2(double y, double x) → double`  
`toRadians(double) → double`  `toDegrees(double) → double`

### Min / Max / Clamp

`min(double,double)` `max(double,double)`  
`minInt(int,int)` `maxInt(int,int)`  
`minLong(long,long)` `maxLong(long,long)`  
`clamp(double,lo,hi)` `clampInt(int,lo,hi)`

### Integer math

`gcd(int,int) → int`  `lcm(int,int) → int`  
`isEven(int) → bool`  `isOdd(int) → bool`  `isPrime(int) → bool`

---

## 25. Standard Library — std.collections

```ec
import "std.collections";
```

All collections use type erasure (`var` / `i8*`) internally. Cast elements to their actual types using `as`.

### List — growable array

```ec
List items = new List();
items.add("hello");
items.add("world");
string s  = items.get(0) as string;
int    n  = items.length();
bool   e  = items.isEmpty();
items.remove(0);
items.insert(0, "first");
bool  has = items.contains("world");
int   idx = items.indexOf("world");
var   top = items.last();
var   pop = items.pop();
items.prepend("zero");
List  sub = items.slice(1, 3);
List  cat = items.concat(other);
items.clear();
```

| Method | Return | Description |
|---|---|---|
| `init()` | void | Constructor |
| `length()` / `size()` | int | Number of elements |
| `isEmpty()` | bool | True if empty |
| `add(var)` | void | Append |
| `get(int)` | var | Element at index |
| `set(int, var)` | void | Set element at index |
| `remove(int)` | void | Remove element at index |
| `insert(int, var)` | void | Insert before index |
| `indexOf(var)` | int | First match index (–1 if absent) |
| `contains(var)` | bool | Linear search |
| `first()` | var | First element |
| `last()` | var | Last element |
| `pop()` | var | Remove and return last element |
| `prepend(var)` | void | Insert at front |
| `slice(int, int)` | List | Sub-list `[start, end)` |
| `concat(List)` | List | Concatenated copy |
| `clear()` | void | Remove all elements |

### Map — string-keyed hash map

```ec
Map m = new Map();
m.set("key", value);
var  v   = m.get("key");
bool has = m.has("key");
m.remove("key");
int  cnt = m.count();
List ks  = m.keys();
List vs  = m.values();
var  def = m.getOrDefault("key", fallback);
```

| Method | Return | Description |
|---|---|---|
| `init()` | void | Constructor |
| `set(string, var)` | void | Insert or update |
| `get(string)` | var | Value (null if absent) |
| `has(string)` | bool | Key exists |
| `remove(string)` | void | Delete key |
| `count()` | int | Number of entries |
| `isEmpty()` | bool | True if empty |
| `keys()` | List | All keys |
| `values()` | List | All values |
| `getOrDefault(string, var)` | var | Get or return default |

### Set — string members

```ec
Set s = new Set();
s.add("apple");
bool has = s.has("apple");
s.remove("apple");
int  cnt = s.count();
List ks  = s.keys();
Set  u   = s.union(other);
```

| Method | Return | Description |
|---|---|---|
| `init()` | void | Constructor |
| `add(string)` | void | Add member |
| `has(string)` | bool | Member exists |
| `remove(string)` | void | Remove member |
| `count()` | int | Member count |
| `isEmpty()` | bool | True if empty |
| `keys()` | List | All members |
| `union(Set)` | Set | Union — new Set |

### StringBuilder — efficient string accumulation

```ec
StringBuilder sb = new StringBuilder();
sb.append("Hello")
  .appendChar(',')
  .append(" World");
sb.appendInt(42);
sb.appendLong(9000000000l);
sb.appendBool(true);
sb.appendLine("next");       // append + '\n'
string result = sb.toString();
int    len    = sb.length();
sb.clear();
```

| Method | Return | Description |
|---|---|---|
| `init()` | void | Constructor |
| `append(string)` | StringBuilder | Append string (chainable) |
| `appendChar(char)` | StringBuilder | Append character |
| `appendInt(int)` | StringBuilder | Append integer |
| `appendLong(long)` | StringBuilder | Append long |
| `appendBool(bool)` | StringBuilder | Append `"true"` or `"false"` |
| `appendLine(string)` | StringBuilder | Append string + `\n` |
| `toString()` | string | Get accumulated result |
| `length()` | int | Current byte length |
| `isEmpty()` | bool | True if empty |
| `clear()` | StringBuilder | Reset to empty |

---

## 26. Terminal Primitives

Available after `import "std.io"`. These functions expose raw terminal I/O for building TUI applications.

| Function | Signature | Description |
|---|---|---|
| `termRaw()` | `→ void` | Put stdin in raw mode (no echo, no canonical processing) |
| `termRestore()` | `→ void` | Restore original terminal settings |
| `getTermRows()` | `→ int` | Terminal height in rows |
| `getTermCols()` | `→ int` | Terminal width in columns |
| `readKey()` | `→ int` | Read one keypress; escape sequences decoded to values ≥ 1000 |
| `writeRaw(string)` | `→ void` | Write bytes to stdout without newline translation or buffering |
| `flushOutput()` | `→ void` | Flush stdout |
| `runSystem(string)` | `→ int` | Run shell command; return exit code |
| `atexitRestore()` | `→ void` | Register `termRestore` as atexit handler |
| `registerWinch()` | `→ void` | Register SIGWINCH handler |
| `winchPending()` | `→ bool` | Returns true once after terminal was resized (auto-clears) |

### `readKey()` return values

| Value | Key |
|---|---|
| 1–126 | ASCII character |
| 27 | ESC |
| 1000 | Arrow Up |
| 1001 | Arrow Down |
| 1002 | Arrow Left |
| 1003 | Arrow Right |
| 1004 | Delete |
| 1005 | Home |
| 1006 | End |
| 1007 | Page Up |
| 1008 | Page Down |
| 1009 | F1 |
| 1010 | F2 |
| 1013 | F5 |

### ANSI escape sequences

After the escape-sequence fix, `\x1b` in string literals correctly produces the ESC byte (0x1B):

```ec
writeRaw("\x1b[2J\x1b[H");                       // clear screen, cursor home
writeRaw("\x1b[38;5;196mRed text\x1b[0m");       // 256-colour foreground
writeRaw("\x1b[1;33mBold yellow\x1b[0m");
writeRaw("\x1b[" + toString(row) + ";" + toString(col) + "H");  // cursor position
writeRaw("\x1b[?25l");    // hide cursor
writeRaw("\x1b[?25h");    // show cursor
writeRaw("\x1b[K");       // erase to end of line
```

---

## 27. Compiler Reference

### Installed layout

```
/exec/ec            Compiler binary (runtime.o embedded via xxd blob)
/exec/libec.a       Static library of compiler components
/exec/runtime.o     Precompiled C runtime for manual clang linking
/exec/stdlib/       Standard library source files
  std.io
  std.string
  std.math
  std.collections
/exec/include/      C++ headers (Lexer.h Parser.h TypeChecker.h CodeGen.h ...)
/exec/compiler/     Self-hosted compiler ports in ExtendedC
  Lexer.ec  Parser.ec  TypeChecker.ec  CodeGen.ec  Compiler.ec  Driver.ec
```

### Workflow

```bash
# Single file
/exec/ec source.ec -o source.ll -I /exec/stdlib
clang -O2 source.ll /exec/runtime.o -lm -o program
./program

# Multi-file project
/exec/ec main.ec mod1.ec mod2.ec -o app.ll -I /exec/stdlib -I ./src
clang -O2 app.ll /exec/runtime.o -lm -o app
```

### Makefile targets

| Target | Description |
|---|---|
| `make` / `make release` | Build compiler with optimisations |
| `make debug` | Build with AddressSanitizer + UBSan |
| `make run` | Compile and run the built-in demo |
| `make ir` | Emit LLVM IR for the built-in demo |
| `make install` | Install to `/exec/` |
| `make install-system` | Install + symlink `/usr/local/bin/ec` |
| `make compile-file SRC=f.ec` | Compile a specific `.ec` file |
| `make lex-port` | Build `compiler/Lexer.ec` → binary |
| `make parser-port` | Build `compiler/Parser.ec` → binary |
| `make codegen-port` | Build `compiler/CodeGen.ec` → binary |
| `make self-hosted` | Build the full self-hosted compiler |
| `make ecvim` | Build `ecvim.ec` → editor binary |
| `make clean` | Remove all build artifacts |

### Error format

```
filename.ec:line:col: error: message
filename.ec:line:col: warning: message

── Summary ─────────────────────────────────────────────
  Modules loaded : 1
  Total errors   : 0
  Total warnings : 0
  IR written to  : output.ll
  IR size        : 609343 bytes
```

---

## 28. Memory and Object Model

### Allocation

Every `new T(...)` or `new T[n]` calls `ec_new(size_bytes)` which:
1. Prepends a hidden 16-byte `ObjHeader` (next-pointer + size)
2. Zero-initialises the allocation
3. Links it into the global GC object list
4. Triggers a collection if `live_bytes > threshold`

### Object layouts

**Class object:**
```
[header]   ObjHeader  (16 bytes, hidden)
[0..7]     i8**  vtptr      — pointer to class's vtable global
[8..15]    field0
[16..23]   field1
...
```

**Array:**
```
[header]   ObjHeader  (16 bytes, hidden)
[0..7]     i64   length     — element count
[8..]      T[]   data       — length × sizeof(T) bytes
```

**Closure (fat pointer):**
```
[header]   ObjHeader  (16 bytes, hidden)
[0..7]     i8*   fn_ptr     — lambda LLVM function pointer
[8..15]    i8*   env_ptr    — captured-variable env struct pointer
```

**Future:**
```
[header]   ObjHeader  (16 bytes, hidden)
[0..7]     i8*       result    — null until done
[8..11]    i32       done      — 0 = running, 1 = complete
[12..15]   i32       padding
[16..23]   pthread_t tid
[24..31]   i32       reserved
```

**Collection classes (List / Map / Set / StringBuilder):**
```
[header]   ObjHeader  (16 bytes, hidden)
[0..7]     i8**  vtptr
[8..15]    i8*   data        — opaque GC block managed by ec_* runtime
```

`data` is updated in-place by mutation methods (the runtime may reallocate and return a new pointer; the method writes it back to `this.data`).

### Vtable layout

```llvm
@ClassName_vtable = global [N x i8*] [
    i8* bitcast (retType (%ClassName*, ...)* @ClassName_method0 to i8*),
    i8* bitcast (retType (%ClassName*, ...)* @ClassName_method1 to i8*),
    ...
]
```

Overrides occupy the same slot index as in the superclass. New methods in a subclass occupy additional slots at the end.

---

## 29. LLVM IR ABI

### Target

```llvm
target triple = "x86_64-unknown-linux-gnu"
```

### Name mangling

| ExtendedC | LLVM IR |
|---|---|
| `fun int add(int, int)` | `@add` |
| `class Foo { fun void bar() }` | `@Foo_bar` |
| `class Foo { fun void init() }` | `@Foo_init` |
| `async fun int work(int n)` | `@work` (spawner) + `@work_async_body` (thread thunk) |
| lambda #3 | `@lambda.3` |
| lambda #3 env struct | `%lambda.3_env` |
| async env struct for `work` | `%work_async_env` |

### Method calling convention

Methods receive `this` as the first argument:

```llvm
define void @Animal_speak(%Animal* %self) { ... }
define i32  @Counter_get(%Counter* %self) { ... }
define void @List_add(%List* %self, i8* %item) { ... }
```

### Virtual dispatch sequence

```llvm
; a.speak() — a is %Animal*, speak is in vtable slot 0
%vtptr_slot = getelementptr inbounds %Animal, %Animal* %a, i32 0, i32 0
%vtptr      = load i8**, i8*** %vtptr_slot
%fn_slot    = getelementptr inbounds i8*, i8** %vtptr, i32 0
%fn_raw     = load i8*, i8** %fn_slot
%fn         = bitcast i8* %fn_raw to void (%Animal*)*
call void %fn(%Animal* %a)
```

### Collection struct declarations (emitted in every compilation unit)

```llvm
%List          = type { i8**, i8* }
%Map           = type { i8**, i8* }
%Set           = type { i8**, i8* }
%StringBuilder = type { i8**, i8* }
```

Emitted by `CodeGen::emitPreamble()` so user classes that hold collection fields can reference them.

### String constants

```llvm
@.str.1 = private unnamed_addr constant [14 x i8] c"Hello, world!\00"
```

`\x1b` in EC source → `\1B` in the IR constant (real ESC byte, 0x1B). Before the fix, `\x1b` produced the literal text `\5Cx1b` (backslash + "x1b").

### Link command

```bash
clang -O2 output.ll /exec/runtime.o -lm -o program
```

`-lm` links the C math library. `-lpthread` is not needed — pthreads are linked via `runtime.o`.

---

## 30. Self-Hosting Status

The compiler is being re-implemented in ExtendedC. All six pipeline components are complete:

| Component | File | Lines |
|---|---|---|
| Lexer | `compiler/Lexer.ec` | 863 |
| Parser | `compiler/Parser.ec` | 2019 |
| TypeChecker | `compiler/TypeChecker.ec` | 1819 |
| CodeGen | `compiler/CodeGen.ec` | 2848 |
| Compiler driver | `compiler/Compiler.ec` | 531 |
| Entry point | `compiler/Driver.ec` | 119 |
| **Total** | | **8199** |

Build the self-hosted compiler:

```bash
make self-hosted
./build/self_hosted
```

IR size: ~2.0 MB. All 6 modules compile with zero errors and zero warnings.

---

## 31. ecvim Editor

`ecvim.ec` (~1800 lines) is a fully-featured Vim-like terminal editor written entirely in ExtendedC. It is the primary real-world test program for the language.

### Build

```bash
/exec/ec ecvim.ec -o ecvim.ll -I /exec/stdlib
clang -O2 ecvim.ll /exec/runtime.o -lm -o ecvim

# Or via make:
make ecvim
./build/ecvim myfile.ec
```

IR output: 622 KB, 133 called functions, 0 undefined references, 0 bad phi nodes.

### Opening files

Because ExtendedC does not yet expose `argc`/`argv`, use the launcher script:

```bash
#!/bin/bash
rm -f /tmp/.ecvim_args
for f in "$@"; do echo "$f" >> /tmp/.ecvim_args; done
exec ./ecvim
```

ecvim reads `/tmp/.ecvim_args` on startup.

### Modes

| Mode | Entry keys | Exit |
|---|---|---|
| Normal | Default; `Esc` from any other mode | — |
| Insert | `i` `I` `a` `A` `o` `O` | `Esc` |
| Replace | `R` | `Esc` |
| Visual (char) | `v` | `Esc` |
| Visual (line) | `V` | `Esc` |
| Command | `:` | `Enter` to execute, `Esc` to cancel |
| Search | `/` (forward) `?` (backward) | `Enter` or `Esc` |

### Normal mode — navigation

| Key | Action |
|---|---|
| `h` `j` `k` `l` / arrows | Move left / down / up / right |
| `w` `W` | Forward to next word / WORD start |
| `b` `B` | Backward to word / WORD start |
| `e` `E` | Forward to word / WORD end |
| `0` | Beginning of line |
| `^` | First non-blank character |
| `$` | End of line |
| `gg` | First line |
| `G` | Last line (`{N}G` for line N) |
| `Ctrl-f` / `Ctrl-b` | Page down / up |
| `Ctrl-d` / `Ctrl-u` | Half-page down / up |
| `Ctrl-e` / `Ctrl-y` | Scroll view down / up (cursor stays) |

### Normal mode — editing

| Key | Action |
|---|---|
| `i` / `I` | Insert before cursor / first non-blank |
| `a` / `A` | Insert after cursor / end of line |
| `o` / `O` | New line below / above, enter Insert |
| `r{c}` | Replace character under cursor with `c` |
| `R` | Enter Replace mode |
| `~` | Toggle case of character under cursor |
| `x` / `X` | Delete char under / before cursor |
| `dd` | Delete current line |
| `D` | Delete to end of line |
| `dw` | Delete word |
| `d$` | Delete to end of line |
| `d0` | Delete to beginning of line |
| `yy` / `Y` | Yank (copy) current line |
| `p` / `P` | Paste after / before cursor |
| `cc` | Change entire line |
| `cw` | Change word |
| `C` | Change to end of line |
| `>>` / `<<` | Indent / de-indent current line |
| `u` | Undo (100-level stack) |

### Normal mode — search and marks

| Key | Action |
|---|---|
| `/pattern` | Search forward (live preview as you type) |
| `?pattern` | Search backward |
| `n` / `N` | Next / previous match |
| `*` / `#` | Search word under cursor forward / backward |
| `f{c}` / `F{c}` | Find character on line forward / backward |
| `t{c}` / `T{c}` | To character (stop one before / after) |
| `;` / `,` | Repeat last `f`/`F`/`t`/`T` same / opposite direction |
| `%` | Jump to matching bracket `(){}[]` |
| `m{a-z}` | Set mark |
| `'{a-z}` | Jump to mark |
| `Ctrl-g` | Show file info in status bar |

### Visual mode

Enter with `v` (character selection) or `V` (line selection):

| Key | Action |
|---|---|
| Motion keys | Extend selection |
| `d` / `x` | Delete selection |
| `y` | Yank selection |
| `>` / `<` | Indent / de-indent selection |
| `:` | Enter Command mode |
| `Esc` | Cancel |

### Command mode

| Command | Description |
|---|---|
| `:w [file]` | Write buffer to file |
| `:q` | Quit (fails if unsaved changes) |
| `:q!` | Force quit without saving |
| `:wq` or `:x` | Write and quit |
| `:e <file>` | Open file in new buffer |
| `:bn` / `:bp` | Next / previous buffer |
| `:{N}` | Jump to line N |
| `:compile` | Compile current `.ec` file |
| `:run` | Run compiled binary |
| `:make` | Compile then run (same as F5) |
| `:set nu` | Show line numbers |
| `:set nonu` | Hide line numbers |
| `:set rnu` | Relative line numbers |
| `:set ai` / `:set noai` | Auto-indent on/off |
| `:set list` / `:set nolist` | Show/hide whitespace |
| `:set ts=N` | Tab stop width (1–16) |
| `:syntax on` / `:syntax off` | Toggle syntax highlighting |
| `:help` | Key binding summary in status bar |
| `ZZ` | Save and quit |
| `ZQ` | Quit without saving |

### F5 — compile and run

Saves the current file, runs `:compile` (calls `/exec/ec` then `clang`), then runs the compiled binary. The terminal is restored while programs run; press `Enter` to return to the editor.

### Compile integration

`:compile` runs:
```bash
/exec/ec 'file.ec' -o 'file.ll' -I /exec/stdlib && \
clang -O2 'file.ll' /exec/runtime.o -lm -o 'file'
```

### Syntax highlighting colours

| Element | Colour |
|---|---|
| Keywords (`fun` `class` `if` `while` …) | Salmon red (`\x1b[38;5;204m`) |
| Types (`int` `string` `bool` …) | Sky blue (`\x1b[38;5;81m`) |
| String literals | Pale yellow (`\x1b[38;5;186m`) |
| Char literals | Warm orange (`\x1b[38;5;179m`) |
| Line and block comments | Muted grey (`\x1b[38;5;102m`) |
| Numbers | Lavender (`\x1b[38;5;141m`) |
| Operators | Peach (`\x1b[38;5;215m`) |
| Current line background | Dark (`\x1b[48;5;235m`) |
| Visual selection | Mid-grey (`\x1b[48;5;237m`) |
| Search matches | Yellow-on-green (`\x1b[48;5;58m\x1b[38;5;226m`) |
| Status bar — Normal | Dark blue (`\x1b[48;5;24m`) |
| Status bar — Insert | Dark green (`\x1b[48;5;28m`) |
| Status bar — Visual | Purple (`\x1b[48;5;90m`) |
| Status bar — Command | Brown (`\x1b[48;5;130m`) |
| Status bar — Replace | Dark red (`\x1b[48;5;88m`) |

---

## 32. Bugs Fixed

### String escape sequences not resolved

**Symptom:** `"\x1b[0m"` in EC source printed as literal text `\x1b[0m`. ecvim rendered its entire UI as plain text instead of ANSI-formatted output.

**Cause:** `Parser.cpp` stripped quotes from string literals but passed the raw source text verbatim to CodeGen. `escapeString()` saw the backslash (0x5C) and emitted `\5C` in the IR rather than the actual ESC byte `\1B`.

**Fix:** Added full escape resolution in `Parser::parsePrimary` for `LiteralString`: `\n \t \r \0 \a \b \f \v \\ \" \' \xHH \uXXXX`. Also fixed `CodeGen::emitCharLit` which was only handling `\n \t \r \0`.

### Phi node predecessor mismatch in nested ternaries

**Symptom:** `clang -O2` rejected the IR with `PHI node entries do not match predecessors`.

**Cause:** `CodeGen::emitTernary` captured `thenLbl` and `elseLbl` as the phi predecessors before emitting sub-expressions. Nested ternaries emitted additional basic blocks; the outer `br` to the merge block came from the last inner block (e.g. `tern.end.273`), not from `tern.else.269`. The phi referenced a non-predecessor.

**Fix:** Added `currentBlock_` field to `CodeGen`, updated in `emitLabel`. `emitTernary` captures `currentBlock_` after each `emitExpr` call, giving the correct predecessor regardless of nesting depth.

### Collection method bodies missing

**Symptom:** `use of undefined value '@List_add'`, `'@List_get'`, etc.

**Cause:** `std.collections` is not code-generated — the Compiler's dot-to-slash conversion turns `"std.collections"` into `"std/collections.ec"` (doesn't exist), so `List`, `Map`, `Set`, `StringBuilder` class bodies never appear in `program.declarations` and their method bodies are never emitted.

**Fix:** Added `CodeGen::emitCollectionStubs()` (pass 2.75 in `generate()`) which emits handwritten LLVM IR bodies for all 35+ collection methods, each loading `this.data` from field 1 and forwarding to the `ec_*` runtime.

### Collection struct types undefined

**Symptom:** `use of undefined type named 'List'` when a user class had a `List` field.

**Cause:** Same root cause as above — struct type declarations were never emitted.

**Fix:** `CodeGen::emitPreamble()` now always emits forward struct declarations for all four collection classes.

### Collection method return types defaulting to i32

**Symptom:** `bitcast i32 → %Row*` — collection getters typed as `i32` instead of `i8*`.

**Cause:** `userFnRetTypes_["List_get"]` had no entry (class never in `program.declarations`), so the call-site fallback defaulted to `"i32"`.

**Fix:** All collection method return types are pre-registered at the start of `CodeGen::generate()` before pass 0.5 runs.

### Terminal function names not mapped

**Symptom:** `use of undefined value '@atexitRestore'` at link time.

**Cause:** Stale `/exec/ec` binary installed before terminal function fnMap entries were added.

**Fix:** `sudo make install` updates the compiler. Functions are mapped: `"atexitRestore" → "ec_atexit_restore"`, `"writeRaw" → "ec_write_raw"`, etc.

---

*ExtendedC compiler: ~12,000 lines of C++ across 6 components. Self-hosted re-implementation: 8,199 lines of ExtendedC. ecvim editor: ~1,800 lines of ExtendedC, compiles to 622 KB LLVM IR, links to a 52 KB native binary.*
