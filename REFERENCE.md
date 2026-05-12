# Enhanced REFERENCE.md for ExtendedC Language

This document is improved with expanded explanations for the standard libraries and the inclusion of diagrams for illustration purposes.

## Enhancements Overview

### Standard Libraries Expansion

#### `std.io`
The `std.io` module includes utilities for file operations, input and output streams, and terminal interaction. Practical usage examples are included to demonstrate its capabilities.

##### Examples:
```ec
import "std.io";

fun void readFile() {
    var handle = openFile("data.ec");
    string content = handle.read();
    println(content);
    handle.close();
}

fun void writeToFile() {
    var handle = openFile("output.ec", "w");
    handle.write("ExtendedC Reference Documentation");
    handle.close();
}
```

#### `std.math`
The `std.math` module provides mathematical constants and functions, such as trigonometric operations, logarithmic calculations, and rounding methods.

##### Examples:
```ec
import "std.math";

fun void calculate() {
    double pi = 3.14159;
    double area = std.math.pow(radius, 2) * pi;
    println("Circle area: " + toString(area));
}
```

#### `std.string`
The `std.string` module simplifies string manipulation, enabling efficient concatenation, searching, and manipulation.

##### Examples:
```ec
import "std.string";

fun void manipulateString() {
    string input = "hello, world!";
    println(toUpperCase(input));  // Output: "HELLO, WORLD!"
    println(reverse(input));     // Output: "!dlrow ,olleh"
}
```

#### `std.collections`
The `std.collections` module introduces data structures like `List`, `Map`, and `Set`, facilitating collection-based operations.

##### Examples:
```ec
import "std.collections";

fun void demoCollections() {
    var items = new List<int>();
    items.add(10);
    items.add(20);
    println(items.size());        // Output: 2
}
```

### Diagrams for Features
Diagrams have been introduced to enhance comprehension of complex concepts.

#### Compilation Stages

```text
source.ec ──► Lexer ──► Parser ──► TypeChecker ──► CodeGen ──► source.ll
source.ll + /exec/runtime.o ──► clang -O2 ──► native binary
```

To visually represent:

```
+-----------+    +----------+   +--------------+   +----------+
|  Source   | -> |  Lexer   | -> |  Parser      | -> |   IR     |
|   (.ec)   |    |          |    |              |    |          |
+-----------+    +----------+   +--------------+   +----------+
```

#### Memory Layout for Strings
```text

```
```

## Continuation below:]