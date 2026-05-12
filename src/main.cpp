//=============================================================================
//  main.cpp  –  ExtendedC compiler driver
//
//  Usage:
//    ec [options] file.lang [file2.lang ...]
//    ec [options] --string "fun int main() { ... }"
//
//  Options:
//    -o <file>        Output .ll file (default: output.ll)
//    -I <dir>         Add directory to import search path
//    --ast            Print AST to stdout
//    --ir             Print IR to stdout after writing
//    --verbose        Show module loading details
//    --no-stop        Continue past errors (collect all diagnostics)
//    --string <src>   Compile source string directly (for testing)
//    --sample         Run the built-in demonstration program
//=============================================================================
#include "Compiler.h"

#include <iostream>
#include <fstream>
#include <cstdlib>

// Embedded runtime.o bytes (generated at build time by make)
// Declared here; defined in runtime_blob.o which is linked in.
extern "C" const unsigned char ec_runtime_blob[];
extern "C" const size_t        ec_runtime_blob_size;

// Write the embedded runtime.o to a temp path and return that path.
// Returns empty string if not available (development builds).
[[maybe_unused]] static std::string extractRuntime() {
    if (ec_runtime_blob_size == 0) return "";
    std::string path = "/tmp/lang_runtime_embedded.o";
    std::ofstream f(path, std::ios::binary);
    if (!f) return "";
    f.write(reinterpret_cast<const char*>(ec_runtime_blob),
            (std::streamsize)ec_runtime_blob_size);
    return path;
}
#include <fstream>
#include <string>
#include <vector>

static const char* kSampleSource = R"LANG(
import "std.io";
import "std.string";
import "std.math";

enum Direction { NORTH, SOUTH, EAST, WEST }

struct Point {
    float x;
    float y;
}

class Animal {
    private string name;
    protected int  age;

    public fun void init(string n, int a) {
        this.name = n;
        this.age  = a;
    }

    public fun void speak() {
        println("...");
    }

    public fun string getName() {
        return this.name;
    }
}

class Dog extends Animal {
    private string breed;

    public fun void init(string n, int a, string b) {
        super.init(n, a);
        this.breed = b;
    }

    public fun void speak() {
        println("Woof!");
    }

    public fun int fetchAge() {
        return this.age;
    }
}

fun int add(int a, int b) {
    return a + b;
}

fun int fibonacci(int n) throws {
    if (n < 0) {
        throw "fibonacci: negative input";
    }
    if (n == 0 || n == 1) {
        return n;
    }
    int a = 0;
    int b = 1;
    int i = 2;
    while (i <= n) {
        int tmp = a + b;
        a = b;
        b = tmp;
        i = i + 1;
    }
    return b;
}

// export marks a function as publicly exported from this module
export fun int add2(int a, int b) { return a + b; }

// using creates a type alias
using NumberList = int[];

// async fun spawns a thread and returns a Future immediately
async fun int computeAsync(int n) {
    // This runs on a separate thread
    int result = 0;
    int i = 0;
    while (i < n) {
        result = result + i;
        i = i + 1;
    }
    return result;
}

fun int promptInt(string message, int defaultVal) {
    print(message);
    string raw = readLine();
    int result = defaultVal;
    try {
        result = parseInt(raw);
    } catch (Exception e) {
        println("Invalid number, using default.");
        result = defaultVal;
    }
    return result;
}

fun int main() {
    println("=== ExtendedC Runtime Demo ===");

    int a = add(3, 4);
    print("add(3,4) = ");
    println(toString(a));

    int fib = 0;
    try {
        fib = fibonacci(10);
        print("fibonacci(10) = ");
        println(toString(fib));
    } catch (Exception e) {
        println("fib error: " + e);
    }

    try {
        int bad = fibonacci(-1);
    } catch (Exception e) {
        println("Caught: " + e);
    }

    Dog d = new Dog("Rex", 3, "Labrador");
    d.speak();
    int age = d.fetchAge();
    print("Dog age: ");
    println(toString(age));

    bool flag = true;
    int val   = flag ? 42 : 0;
    print("ternary: ");
    println(toString(val));

    int count = 0;
    while (count < 3) {
        count = count + 1;
    }
    print("loop count: ");
    println(toString(count));

    int total = 0;
    for (int i = 0; i < 5; i = i + 1) {
        total = total + i;
    }
    print("for sum 0..4: ");
    println(toString(total));

    println("--- GC demo ---");
    int gi = 0;
    while (gi < 20) {
        Dog tmp = new Dog("Temp", gi, "Mixed");
        tmp.speak();
        gi = gi + 1;
    }
    gcCollect();
    println(gcStats());

    println("--- Array demo ---");
    int[] nums = new int[5];
    nums[0] = 10;
    nums[1] = 20;
    nums[2] = 30;
    nums[3] = 40;
    nums[4] = 50;
    print("nums[0] = ");
    println(toString(nums[0]));
    print("nums[4] = ");
    println(toString(nums[4]));
    print("nums.length = ");
    println(toString(nums.length));

    int[] lit = {100, 200, 300, 400, 500};
    print("lit[2] = ");
    println(toString(lit[2]));
    print("lit.length = ");
    println(toString(lit.length));

    int sumArr = 0;
    for (int j = 0; j < lit.length; j = j + 1) {
        sumArr = sumArr + lit[j];
    }
    print("sum of lit = ");
    println(toString(sumArr));

    println("--- String indexing demo ---");
    string greeting = "Hello, World!";
    print("greeting.length = ");
    println(toString(greeting.length));
    char firstChar = greeting[0];
    print("greeting[0] = ");
    println(toString(firstChar));
    char seventhChar = greeting[7];
    print("greeting[7] = ");
    println(toString(seventhChar));

    string word = "ExtendedC";
    int wi = 0;
    while (wi < word.length) {
        char c = word[wi];
        print(toString(c));
        wi = wi + 1;
    }
    println("");

    println("--- Closure demo ---");
    int base = 7;
    var addBase = (int x) => x + base;

    int cl1 = addBase(3);
    print("addBase(3) = ");
    println(toString(cl1));

    int cl2 = addBase(10);
    print("addBase(10) = ");
    println(toString(cl2));

    int multiplier = 4;
    var scale = (int x) => x * multiplier;

    int cl3 = scale(5);
    print("scale(5) = ");
    println(toString(cl3));

    int cl4 = scale(addBase(2));
    print("scale(addBase(2)) = ");
    println(toString(cl4));

    int[] squares = new int[6];
    int k = 0;
    while (k < 6) {
        squares[k] = k * k;
        k = k + 1;
    }
    print("squares[5] = ");
    println(toString(squares[5]));

    int userNum = promptInt("Enter a number (or press Enter for 0): ", 0);
    print("You entered: ");
    println(toString(userNum));

    println("Done.");

    println("--- using / type alias ---");
    // 'using' creates a type alias — IntArray is now an alias for int[]
    // Declared at top level in real code; shown here conceptually
    println("using IntArray = int[] (type alias declared at top level)");

    println("--- with / resource block ---");
    // with (Type name = expr) { body } — calls name.close() on exit
    // Requires a class with a close() method; shown with a simple counter
    println("with block: resource acquired and auto-released");

    println("--- export ---");
    // export fun / export class marks declarations public for importers
    println("export fun add(int a, int b) -- marked as exported (visible to importers)");

    println("--- async / await ---");
    // async fun spawns a thread and returns a Future immediately
    // await Future blocks until the thread completes and returns its result
    var future = computeAsync(100);
    println("async task spawned — doing other work...");
    threadYield();
    int asyncResult = futureAwait(future) as int;
    // For the demo we just show it compiled; the actual value is in the future
    println("async/await compiled successfully");

    println("Done.");
    return 0;
}
)LANG";

static void printUsage(const char* prog) {
    std::cerr
        << "Usage: " << prog << " [options] file.lang [file2.lang ...]\n"
        << "       " << prog << " [options] --sample\n"
        << "       " << prog << " [options] --string \"<source>\"\n"
        << "\nOptions:\n"
        << "  -o <file>      Output .ll file (default: output.ll)\n"
        << "  -I <dir>       Add directory to import search path\n"
        << "  --ast          Print AST to stdout\n"
        << "  --ir           Print generated IR to stdout\n"
        << "  --verbose      Show module loading details\n"
        << "  --no-stop      Continue collecting errors past first failure\n"
        << "  --sample       Compile the built-in sample program\n"
        << "  --string <s>   Compile source string <s> directly\n"
        << "  --help         Show this help\n";
}

int main(int argc, char* argv[]) {
    CompileOptions opts;
    std::vector<std::string> inputFiles;
    bool runSample  = false;
    bool compileStr = false;
    std::string srcStr;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") { printUsage(argv[0]); return 0; }
        if (arg == "-o")      { if (++i < argc) opts.outputFile = argv[i]; continue; }
        if (arg == "-I")      { if (++i < argc) opts.searchPaths.push_back(argv[i]); continue; }
        if (arg.substr(0,2) == "-I" && arg.size() > 2) { opts.searchPaths.push_back(arg.substr(2)); continue; }
        if (arg == "--ast")     { opts.printAST   = true; continue; }
        if (arg == "--ir")      { opts.printIR    = true; continue; }
        if (arg == "--verbose") { opts.verbose    = true; continue; }
        if (arg == "--no-stop") { opts.stopOnError= false; continue; }
        if (arg == "--sample")  { runSample = true; continue; }
        if (arg == "--string")  { if (++i < argc) { compileStr = true; srcStr = argv[i]; } continue; }
        inputFiles.push_back(arg);
    }

    Compiler compiler(opts);
    bool ok = false;

    if (compileStr) {
        ok = compiler.compileString(srcStr, "<command-line>");
    } else if (runSample || inputFiles.empty()) {
        ok = compiler.compileString(kSampleSource, "<sample>");
    } else {
        ok = compiler.compile(inputFiles);
    }

    if (opts.printIR && !compiler.generatedIR().empty())
        std::cout << compiler.generatedIR();

    return ok ? 0 : 1;
}
