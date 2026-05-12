// ============================================================================
//  std.io  –  Standard I/O library
//  Import with:  import "std.io";
// ============================================================================

// ── Low-level character I/O ──────────────────────────────────────────────────

// Read a single character from stdin.
// Returns the character read, or '\0' on end-of-file.
fun char getChar() {
    char c = _nativeGetChar();
    return c;
}

// Write a single character to stdout.
fun void putChar(char c) {
    _nativePutChar(c);
}

// ── String output ─────────────────────────────────────────────────────────────

// Print a string to stdout (no trailing newline).
fun void print(string s) {
    _nativePrint(s);
}

// Print a string followed by a newline.
fun void println(string s) {
    _nativePrint(s);
    _nativePutChar('\n');
}

// Print a blank line.
fun void printNewline() {
    _nativePutChar('\n');
}

// ── String input ──────────────────────────────────────────────────────────────

// Read characters until newline or EOF; returns the line without the newline.
fun string readLine() {
    string line = "";
    char c = getChar();
    while (c != '\n') {
        line = line + c;
        c = getChar();
    }
    return line;
}

// Read a single non-whitespace word (stops at space, tab, newline, or EOF).
fun string readWord() {
    string word = "";
    // skip leading whitespace
    char c = getChar();
    while (c == ' ' || c == '\t' || c == '\n') {
        c = getChar();
    }
    // collect until whitespace
    while (c != ' ' && c != '\t' && c != '\n') {
        word = word + c;
        c = getChar();
    }
    return word;
}

// ── Keyboard ──────────────────────────────────────────────────────────────────

// Read one character; return true if it equals the expected key.
fun bool waitForKey(char expected) {
    char c = getChar();
    return c == expected;
}

// Flush any buffered input up to and including the next newline.
fun void flushInputLine() {
    char c = getChar();
    while (c != '\n') {
        c = getChar();
    }
}

// ── Integer I/O ───────────────────────────────────────────────────────────────

// Read a line and parse it as an integer.
// Throws if the input is empty or not a valid integer.
fun int readInt() throws {
    string raw = readLine();
    if (raw == "") {
        throw "readInt: empty input";
    }
    int result = parseInt(raw);
    return result;
}

// Read a line and parse it as a float.
// Throws if the input is empty or not a valid float.
fun float readFloat() throws {
    string raw = readLine();
    if (raw == "") {
        throw "readFloat: empty input";
    }
    float result = parseFloat(raw);
    return result;
}

// ── Formatted output ──────────────────────────────────────────────────────────

// Print an integer followed by a newline.
fun void printInt(int n) {
    println(toString(n));
}

// Print a float followed by a newline.
fun void printFloat(float f) {
    println(floatToString(f));
}

// Print a bool as "true" or "false" followed by a newline.
fun void printBool(bool b) {
    if (b) {
        println("true");
    } else {
        println("false");
    }
}

// ── Threading ─────────────────────────────────────────────────────────────────
//
//  Mutex — mutual exclusion lock
//  var m = mutexNew();
//  mutexLock(m);   ... critical section ...   mutexUnlock(m);
//
//  Future — result of an async function call
//  var f = myAsyncFn(args);   // returns immediately
//  var r = await f;           // blocks until done, returns result as var
//  bool done = futureDone(f); // non-blocking check

fun var   mutexNew()              { return _native_mutexNew(); }
fun void  mutexLock(var m)        { _native_mutexLock(m); }
fun void  mutexUnlock(var m)      { _native_mutexUnlock(m); }
fun bool  mutexTrylock(var m)     { return _native_mutexTrylock(m); }
fun void  mutexDestroy(var m)     { _native_mutexDestroy(m); }

fun var   futureAwait(var f)      { return _native_futureAwait(f); }
fun bool  futureDone(var f)       { return _native_futureDone(f); }

fun void  threadSleep(int ms)     { _native_threadSleep(ms); }
fun void  threadYield()           { _native_threadYield(); }
