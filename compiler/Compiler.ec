// ============================================================================
//  Compiler.lang  --  ExtendedC port of the multi-file compilation pipeline
//
//  Handles:
//    - Loading and caching source files
//    - Import resolution (dot-notation paths → .lang files)
//    - Cycle detection via an in-progress Set
//    - Topological load ordering (dependencies before dependents)
//    - Merging multiple ASTs into one Program
//    - Running TypeChecker + CodeGen on the merged program
//    - Writing the .ll output file
// ============================================================================

import "std.io";
import "std.string";
import "std.collections";
import "Lexer";
import "Parser";
import "TypeChecker";
import "CodeGen";

// ── CompileOptions ────────────────────────────────────────────────────────────

class CompileOptions {
    public string outputFile;
    public List   searchPaths;  // List of boxed string
    public bool   printAST;
    public bool   printIR;
    public bool   verbose;
    public bool   stopOnError;

    public fun void init() {
        this.outputFile  = "output.ll";
        this.searchPaths = new List();
        this.printAST    = false;
        this.printIR     = false;
        this.verbose     = false;
        this.stopOnError = true;
    }
}

// ── ModuleInfo ────────────────────────────────────────────────────────────────

class ModuleInfo {
    public string  canonicalPath;
    public string  moduleName;
    public var     program;       // Program or null
    public bool    isBuiltin;
    public bool    checked;

    public fun void init() {
        this.canonicalPath = "";
        this.moduleName    = "";
        this.program       = null;
        this.isBuiltin     = false;
        this.checked       = false;
    }
}

// ── Compiler ─────────────────────────────────────────────────────────────────

class Compiler {
    private CompileOptions opts;
    private List           modules;      // List of ModuleInfo
    private Map            moduleIndex;  // canonicalPath → boxed int index
    private List           loadOrder;    // List of boxed string (canonical paths, post-order)
    private Set            inProgress;   // canonical paths currently being loaded
    private int            totalErrors;
    private int            totalWarnings;
    private string         ir;           // generated IR text

    public fun void init(CompileOptions options) {
        this.opts          = options;
        this.modules       = new List();
        this.moduleIndex   = new Map();
        this.loadOrder     = new List();
        this.inProgress    = new Set();
        this.totalErrors   = 0;
        this.totalWarnings = 0;
        this.ir            = "";

        if (this.opts.searchPaths.length() == 0) {
            this.opts.searchPaths.add(_boxStr("."));
        }
    }

    public fun string generatedIR() { return this.ir; }

    // ── Public entry points ───────────────────────────────────────────────────

    public fun bool compile(List inputFiles) {
        if (inputFiles.length() == 0) {
            println("error: no input files");
            return false;
        }

        // Add directory of each input file to search path
        int i = 0;
        while (i < inputFiles.length()) {
            string f = _unboxStr(inputFiles.get(i));
            string dir = this.pathDir(f);
            if (stringLen(dir) == 0) { dir = "."; }
            if (!this.listHas(this.opts.searchPaths, dir)) {
                this.opts.searchPaths.add(_boxStr(dir));
            }
            i = i + 1;
        }

        // Load each input file
        i = 0;
        while (i < inputFiles.length()) {
            string f = _unboxStr(inputFiles.get(i));
            // canonical = absolute path (just use the path as-is for now)
            string canonical = f;
            Set visiting = new Set();
            int idx = this.loadModule(canonical, this.pathStem(f));
            if (idx < 0) {
                println("error: cannot load '" + f + "'");
                this.totalErrors = this.totalErrors + 1;
                return false;
            }
            this.loadImports(idx, visiting);
            i = i + 1;
        }

        return this.runPipeline();
    }

    public fun bool compileStr(string source, string fname) {
        Lexer lexer = new Lexer("", "");
        lexer.init(source, fname);
        List tokens = lexer.tokenize();

        // Check lex errors
        int i = 0;
        while (i < tokens.length()) {
            Token t = tokens.get(i) as Token;
            if (t.isError()) {
                println("lex: " + t.lexeme);
                this.totalErrors = this.totalErrors + 1;
            }
            i = i + 1;
        }

        Parser parser = new Parser(tokens, "");
        parser.init(tokens, fname);
        Program prog = parser.parse();

        List perrs = parser.getErrors();
        i = 0;
        while (i < perrs.length()) {
            ParseError e = perrs.get(i) as ParseError;
            println(e.filename + ":" + toString(e.line) + ":" + toString(e.col) + ": error: " + e.msg);
            this.totalErrors = this.totalErrors + 1;
            i = i + 1;
        }

        ModuleInfo mod = new ModuleInfo();
        mod.init();
        mod.canonicalPath = fname;
        mod.moduleName    = fname;
        mod.program       = prog;
        mod.isBuiltin     = false;

        int idx = this.modules.length();
        this.moduleIndex.set(fname, _box(idx));
        this.modules.add(mod);
        this.loadOrder.add(_boxStr(fname));

        // Load transitive imports
        Set visiting = new Set();
        visiting.add(fname);
        this.loadImports(idx, visiting);

        return this.runPipeline();
    }

    // ── File I/O ─────────────────────────────────────────────────────────────

    private fun string readSourceFile(string path) {
        return readFile(path);  // from std.io
    }

    // ── Import resolution ─────────────────────────────────────────────────────

    private fun bool isBuiltinModule(string name) {
        return name == "std.io" || name == "std.string" ||
               name == "std.math" || name == "std.gc" ||
               name == "std.collections";
    }

    private fun string resolveImport(string importPath, string fromFile) {
        if (this.isBuiltinModule(importPath)) { return ""; }

        // Convert dot-notation to path separator: "utils.math" → "utils/math"
        StringBuilder filePath = new StringBuilder();
        int i = 0;
        while (i < stringLen(importPath)) {
            char c = importPath[i];
            if (c == '.') { filePath.appendChar('/'); }
            else          { filePath.appendChar(c); }
            i = i + 1;
        }
        filePath.append(".ec");
        string relPath = filePath.toString();

        // Try each search path
        i = 0;
        while (i < this.opts.searchPaths.length()) {
            string dir = _unboxStr(this.opts.searchPaths.get(i));
            string candidate = this.joinPath(dir, relPath);
            if (fileExists(candidate)) { return candidate; }
            i = i + 1;
        }

        // Try relative to the importing file's directory
        if (stringLen(fromFile) > 0) {
            string fromDir = this.pathDir(fromFile);
            if (stringLen(fromDir) > 0) {
                string candidate = this.joinPath(fromDir, relPath);
                if (fileExists(candidate)) { return candidate; }
            }
        }

        return "";
    }

    // ── Module loading ────────────────────────────────────────────────────────

    private fun int loadModule(string canonicalPath, string moduleName) {
        // Already loaded?
        if (this.moduleIndex.has(canonicalPath)) {
            return _unbox(this.moduleIndex.get(canonicalPath));
        }

        // Cycle check
        if (this.inProgress.has(canonicalPath)) {
            println("error: import cycle detected involving '" + canonicalPath + "'");
            this.totalErrors = this.totalErrors + 1;
            return -1;
        }

        // Read source
        string source = this.readSourceFile(canonicalPath);
        if (stringLen(source) == 0) {
            println("error: cannot read '" + canonicalPath + "'");
            this.totalErrors = this.totalErrors + 1;
            return -1;
        }

        if (this.opts.verbose) {
            println("  loading: " + canonicalPath);
        }

        // Lex
        Lexer lexer = new Lexer("", "");
        lexer.init(source, canonicalPath);
        List tokens = lexer.tokenize();

        int i = 0;
        while (i < tokens.length()) {
            Token t = tokens.get(i) as Token;
            if (t.isError()) {
                println("lex error in '" + canonicalPath + "': " + t.lexeme);
                this.totalErrors = this.totalErrors + 1;
            }
            i = i + 1;
        }

        // Parse
        Parser parser = new Parser(tokens, "");
        parser.init(tokens, canonicalPath);
        Program prog = parser.parse();

        List perrs = parser.getErrors();
        i = 0;
        while (i < perrs.length()) {
            ParseError e = perrs.get(i) as ParseError;
            println(e.filename + ":" + toString(e.line) + ":" + toString(e.col) + ": error: " + e.msg);
            this.totalErrors = this.totalErrors + 1;
            i = i + 1;
        }

        // Register module
        ModuleInfo mod = new ModuleInfo();
        mod.init();
        mod.canonicalPath = canonicalPath;
        mod.moduleName    = moduleName;
        mod.program       = prog;
        mod.isBuiltin     = false;

        int idx = this.modules.length();
        this.moduleIndex.set(canonicalPath, _box(idx));
        this.modules.add(mod);

        return idx;
    }

    private fun void loadImports(int moduleIdx, Set visiting) {
        if (moduleIdx < 0 || moduleIdx >= this.modules.length()) { return; }

        ModuleInfo mod = this.modules.get(moduleIdx) as ModuleInfo;
        string canonPath = mod.canonicalPath;

        // Collect import paths from the AST
        List importPaths = new List();
        if (mod.program != null) {
            Program prog = mod.program as Program;
            int i = 0;
            while (i < prog.declarations.length()) {
                ASTNode decl = prog.declarations.get(i) as ASTNode;
                if (decl.kind == 0) {  // Import
                    ImportDeclNode imp = decl as ImportDeclNode;
                    importPaths.add(_boxStr(imp.path));
                }
                i = i + 1;
            }
        }

        this.inProgress.add(canonPath);
        visiting.add(canonPath);

        int i = 0;
        while (i < importPaths.length()) {
            string importPath = _unboxStr(importPaths.get(i));
            if (this.isBuiltinModule(importPath)) { i = i + 1; continue; }

            string resolved = this.resolveImport(importPath, canonPath);
            if (stringLen(resolved) == 0) { i = i + 1; continue; }
            if (this.moduleIndex.has(resolved)) { i = i + 1; continue; }
            if (visiting.has(resolved)) {
                println("error: import cycle: '" + canonPath +
                        "' imports '" + resolved + "'");
                this.totalErrors = this.totalErrors + 1;
                i = i + 1;
                continue;
            }

            int depIdx = this.loadModule(resolved, importPath);
            if (depIdx >= 0) {
                this.loadImports(depIdx, visiting);
            }
            i = i + 1;
        }

        this.inProgress.remove(canonPath);
        visiting.remove(canonPath);

        // Post-order: add after dependencies
        if (!this.listHasStr(this.loadOrder, canonPath)) {
            this.loadOrder.add(_boxStr(canonPath));
        }
    }

    // ── Merge programs ────────────────────────────────────────────────────────

    private fun Program mergePrograms() {
        Program merged = new Program();
        merged.init();

        int i = 0;
        while (i < this.loadOrder.length()) {
            string canonPath = _unboxStr(this.loadOrder.get(i));
            if (!this.moduleIndex.has(canonPath)) { i = i + 1; continue; }
            int idx = _unbox(this.moduleIndex.get(canonPath));
            ModuleInfo mod = this.modules.get(idx) as ModuleInfo;
            if (mod.program == null) { i = i + 1; continue; }

            Program prog = mod.program as Program;
            int j = 0;
            while (j < prog.declarations.length()) {
                ASTNode decl = prog.declarations.get(j) as ASTNode;
                merged.declarations.add(decl);
                j = j + 1;
            }
            i = i + 1;
        }
        return merged;
    }

    // ── Full pipeline ─────────────────────────────────────────────────────────

    private fun bool runPipeline() {
        if (this.totalErrors > 0 && this.opts.stopOnError) {
            this.printSummary();
            return false;
        }

        Program merged = this.mergePrograms();

        // Type check
        string mainFile = this.loadOrder.length() > 0 ?
            _unboxStr(this.loadOrder.last()) : "<input>";

        TypeChecker checker = new TypeChecker("");
        checker.init(mainFile);

        // Tell checker which imports were resolved from real files
        int i = 0;
        while (i < this.modules.length()) {
            ModuleInfo mod = this.modules.get(i) as ModuleInfo;
            if (!mod.isBuiltin && stringLen(mod.moduleName) > 0) {
                checker.registerResolvedImport(mod.moduleName);
            }
            if (!mod.isBuiltin && stringLen(mod.canonicalPath) > 0) {
                checker.registerResolvedImport(mod.canonicalPath);
            }
            i = i + 1;
        }

        checker.check(merged);

        List cerrs = checker.getErrors();
        i = 0;
        while (i < cerrs.length()) {
            TypeError e = cerrs.get(i) as TypeError;
            println(e.filename + ":" + toString(e.line) + ":" + toString(e.col) + ": error: " + e.msg);
            if (!e.isWarning) {
                this.totalErrors = this.totalErrors + 1;
            } else {
                this.totalWarnings = this.totalWarnings + 1;
            }
            i = i + 1;
        }

        if (this.totalErrors > 0) {
            this.printSummary();
            return false;
        }

        // Code generation
        CodeGen cg = new CodeGen("");
        cg.init(mainFile);
        this.ir = cg.generate(merged);

        // Write output file
        int writeOk = writeFile(this.opts.outputFile, this.ir);
        bool ok = writeOk != 0;
        if (!ok) {
            println("error: cannot write '" + this.opts.outputFile + "'");
            this.totalErrors = this.totalErrors + 1;
            this.printSummary();
            return false;
        }

        this.printSummary();
        return true;
    }

    // ── Summary ───────────────────────────────────────────────────────────────

    private fun void printSummary() {
        println("");
        println("── Summary ----------------------------------------");
        print("  Modules loaded : "); println(toString(this.loadOrder.length()));
        print("  Total errors   : "); println(toString(this.totalErrors));
        print("  Total warnings : "); println(toString(this.totalWarnings));
        if (this.totalErrors == 0) {
            print("  IR written to  : "); println(this.opts.outputFile);
            print("  IR size        : "); println(toString(stringLen(this.ir)) + " bytes");
            println("");
            println("To compile and run:");
            print("  clang -O2 "); print(this.opts.outputFile);
            println(" /exec/runtime.o -lm -o program");
            println("  ./program");
        }
    }

    // ── Path utilities ────────────────────────────────────────────────────────

    private fun string pathDir(string path) {
        // Return directory part of path
        int lastSlash = -1;
        int i = stringLen(path) - 1;
        while (i >= 0) {
            char c = path[i];
            if (c == '/' || c == '\\') { lastSlash = i; break; }
            i = i - 1;
        }
        if (lastSlash < 0) { return "."; }
        if (lastSlash == 0) { return "/"; }
        return substring(path, 0, lastSlash);
    }

    private fun string pathStem(string path) {
        // Return filename without directory and without extension
        int lastSlash = -1;
        int i = stringLen(path) - 1;
        while (i >= 0) {
            char c = path[i];
            if (c == '/' || c == '\\') { lastSlash = i; break; }
            i = i - 1;
        }
        string fname = substring(path, lastSlash + 1, stringLen(path));
        // Strip .lang extension
        if (endsWith(fname, ".ec")) {
            return substring(fname, 0, stringLen(fname) - 5);
        }
        return fname;
    }

    private fun string joinPath(string dir, string file) {
        int dlen = stringLen(dir);
        if (dlen == 0) { return file; }
        char last = dir[dlen - 1];
        if (last == '/' || last == '\\') { return dir + file; }
        return dir + "/" + file;
    }

    private fun bool listHas(List lst, string val) {
        int i = 0;
        while (i < lst.length()) {
            string s = _unboxStr(lst.get(i));
            if (s == val) { return true; }
            i = i + 1;
        }
        return false;
    }

    private fun bool listHasStr(List lst, string val) {
        return this.listHas(lst, val);
    }
}

// ── Test driver ───────────────────────────────────────────────────────────────

fun int main() {
    println("Compiler.lang loaded OK");
    println("Use Driver.lang for a complete command-line interface.");
    return 0;
}
