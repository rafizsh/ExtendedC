//=============================================================================
//  Compiler.cpp  –  Multi-file compilation pipeline
//=============================================================================
#include "Compiler.h"
#include "ASTPrinter.h"

#include <fstream>
#include <sstream>
#include <algorithm>
#include <cassert>

// ─────────────────────────────────────────────────────────────────────────────
//  Built-in module names (resolved by TypeChecker::registerLibrary, not parsed)
// ─────────────────────────────────────────────────────────────────────────────
const std::unordered_set<std::string> Compiler::kBuiltinModules = {
    "std.io", "std.string", "std.math", "std.gc",
    "std.collections"  // class stubs emitted by CodeGen::emitCollectionStubs()
};

// ─────────────────────────────────────────────────────────────────────────────
//  Constructor
// ─────────────────────────────────────────────────────────────────────────────
Compiler::Compiler(CompileOptions opts) : opts_(std::move(opts)) {
    // Always include current directory in search path
    if (opts_.searchPaths.empty())
        opts_.searchPaths.push_back(".");
}

// ─────────────────────────────────────────────────────────────────────────────
//  Public entry points
// ─────────────────────────────────────────────────────────────────────────────
bool Compiler::compile(const std::vector<std::string>& inputFiles) {
    if (inputFiles.empty()) {
        std::cerr << "error: no input files\n";
        return false;
    }

    // Add the directory of each input file to the search path
    for (const auto& f : inputFiles) {
        fs::path p(f);
        std::string dir = p.parent_path().empty() ? "." : p.parent_path().string();
        if (std::find(opts_.searchPaths.begin(), opts_.searchPaths.end(), dir)
                == opts_.searchPaths.end())
            opts_.searchPaths.push_back(dir);
    }

    // Load each input file as a module
    for (const auto& f : inputFiles) {
        std::string canonical;
        try {
            canonical = fs::canonical(f).string();
        } catch (...) {
            canonical = f; // file might not exist yet if testing
        }
        std::unordered_set<std::string> visiting;
        int idx = loadModule(canonical, fs::path(f).stem().string());
        if (idx < 0) {
            std::cerr << "error: cannot load '" << f << "'\n";
            ++totalErrors_;
            return false;
        }
        loadImports(idx, visiting);
    }

    return runPipeline();
}

bool Compiler::compileString(const std::string& source,
                              const std::string& filename) {
    // Parse the string directly
    Lexer  lexer(source, filename);
    auto   tokens = lexer.tokenize();

    for (const auto& t : tokens)
        if (t.is(TokenKind::Error)) {
            std::cerr << "lex: " << t.lexeme << "\n";
            ++totalErrors_;
        }

    Parser parser(std::move(tokens));
    auto   program = parser.parse();

    for (const auto& e : parser.errors()) {
        std::cerr << e.format() << "\n";
        ++totalErrors_;
    }

    ModuleInfo mod;
    mod.canonicalPath = filename;
    mod.moduleName    = filename;
    mod.program       = std::make_unique<Program>(std::move(program));
    mod.isBuiltin     = false;

    int idx = (int)modules_.size();
    moduleIndex_[filename] = idx;
    modules_.push_back(std::move(mod));
    loadOrder_.push_back(filename);

    // Load transitive imports
    std::unordered_set<std::string> visiting;
    visiting.insert(filename);
    loadImports(idx, visiting);

    return runPipeline();
}

// ─────────────────────────────────────────────────────────────────────────────
//  File I/O
// ─────────────────────────────────────────────────────────────────────────────
std::string Compiler::readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return "";
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Import resolution
// ─────────────────────────────────────────────────────────────────────────────
bool Compiler::isBuiltinModule(const std::string& name) const {
    return kBuiltinModules.count(name) > 0;
}

std::string Compiler::resolveImport(const std::string& importPath,
                                    const std::string& fromFile) {
    // Builtin modules don't map to files
    if (isBuiltinModule(importPath)) return "";

    // Try search paths in order
    // importPath may be:
    //   "mymodule"        → look for mymodule.lang
    //   "utils/helper"    → look for utils/helper.lang
    //   "std.io"          → builtin (already handled above)

    // Convert dot-notation to path separator: "utils.math" → "utils/math"
    std::string filePath = importPath;
    for (char& c : filePath)
        if (c == '.') c = '/';
    filePath += ".ec";

    for (const auto& dir : opts_.searchPaths) {
        // Try dot-as-slash + .ec  (normal user modules: "utils.math" → "utils/math.ec")
        fs::path candidate = fs::path(dir) / filePath;
        if (fs::exists(candidate)) {
            try { return fs::canonical(candidate).string(); }
            catch (...) { return candidate.string(); }
        }
        // Try bare dotted name as filename  ("std.algorithm" → "std.algorithm")
        // Used by stdlib files whose filenames keep the dot notation.
        fs::path candidate2 = fs::path(dir) / importPath;
        if (fs::exists(candidate2)) {
            try { return fs::canonical(candidate2).string(); }
            catch (...) { return candidate2.string(); }
        }
    }

    // Also try relative to the importing file's directory
    if (!fromFile.empty()) {
        fs::path fromDir = fs::path(fromFile).parent_path();
        fs::path candidate = fromDir / filePath;
        if (fs::exists(candidate)) {
            try {
                return fs::canonical(candidate).string();
            } catch (...) {
                return candidate.string();
            }
        }
    }

    return ""; // not found
}

// ─────────────────────────────────────────────────────────────────────────────
//  Module loading
// ─────────────────────────────────────────────────────────────────────────────
int Compiler::loadModule(const std::string& canonicalPath,
                         const std::string& moduleName) {
    // Already loaded?
    auto it = moduleIndex_.find(canonicalPath);
    if (it != moduleIndex_.end()) return it->second;

    // Cycle check
    if (inProgress_.count(canonicalPath)) {
        std::cerr << "error: import cycle detected involving '"
                  << canonicalPath << "'\n";
        ++totalErrors_;
        return -1;
    }

    // Read source
    std::string source = readFile(canonicalPath);
    if (source.empty()) {
        std::cerr << "error: cannot read '" << canonicalPath << "'\n";
        ++totalErrors_;
        return -1;
    }

    if (opts_.verbose)
        std::cerr << "  loading: " << canonicalPath << "\n";

    // Lex
    Lexer lexer(source, canonicalPath);
    auto  tokens = lexer.tokenize();
    std::size_t lexErrs = 0;
    for (const auto& t : tokens)
        if (t.is(TokenKind::Error)) {
            std::cerr << "lex error in '" << canonicalPath << "': "
                      << t.lexeme << "\n";
            ++lexErrs;
        }
    totalErrors_ += lexErrs;

    // Parse
    Parser parser(std::move(tokens));
    auto   program = parser.parse();
    for (const auto& e : parser.errors()) {
        std::cerr << e.format() << "\n";
        ++totalErrors_;
    }

    // Register module
    ModuleInfo mod;
    mod.canonicalPath = canonicalPath;
    mod.moduleName    = moduleName;
    mod.program       = std::make_unique<Program>(std::move(program));
    mod.isBuiltin     = false;
    mod.checked       = false;

    int idx = (int)modules_.size();
    moduleIndex_[canonicalPath] = idx;
    modules_.push_back(std::move(mod));

    return idx;
}

void Compiler::loadImports(int moduleIdx,
                            std::unordered_set<std::string>& visiting) {
    if (moduleIdx < 0 || moduleIdx >= (int)modules_.size()) return;

    // Copy data we need BEFORE any recursive loadModule() calls.
    // loadModule() pushes to modules_, which may reallocate the vector
    // and invalidate any reference into it.
    const std::string canonPath = modules_[moduleIdx].canonicalPath;

    // Collect all import paths up front from the module's AST
    std::vector<std::pair<std::string,std::string>> imports; // (path, resolved)
    if (modules_[moduleIdx].program) {
        for (const auto& decl : modules_[moduleIdx].program->declarations) {
            if (decl->kind != DeclKind::Import) continue;
            const auto& imp = static_cast<const ImportDecl&>(*decl);
            imports.push_back({imp.path, ""});
        }
    }

    // Mark as in-progress for cycle detection
    inProgress_.insert(canonPath);
    visiting.insert(canonPath);

    for (auto& [importPath, resolved] : imports) {
        if (isBuiltinModule(importPath)) continue;

        resolved = resolveImport(importPath, canonPath);
        if (resolved.empty()) {
            if (opts_.verbose)
                std::cerr << "  note: import '" << importPath
                          << "' not found as file (will try builtin)\n";
            continue;
        }
        if (moduleIndex_.count(resolved)) continue;
        if (visiting.count(resolved)) {
            std::cerr << "error: import cycle: '" << canonPath
                      << "' imports '" << resolved << "' which is already being loaded\n";
            ++totalErrors_;
            continue;
        }

        // Load dependency (may push to modules_, but we hold no ref to it)
        int depIdx = loadModule(resolved, importPath);
        if (depIdx >= 0)
            loadImports(depIdx, visiting);
    }

    inProgress_.erase(canonPath);
    visiting.erase(canonPath);

    // Add to post-order (dependencies before this module)
    if (std::find(loadOrder_.begin(), loadOrder_.end(), canonPath)
            == loadOrder_.end())
        loadOrder_.push_back(canonPath);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Type checking
// ─────────────────────────────────────────────────────────────────────────────
void Compiler::checkModule(int idx) {
    // Type checking is done on the merged program in runPipeline().
    // This method is kept for potential future incremental checking.
    (void)idx;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Program merging
// ─────────────────────────────────────────────────────────────────────────────
Program Compiler::mergePrograms() {
    Program merged;

    // Process modules in dependency order
    for (const auto& canonPath : loadOrder_) {
        auto it = moduleIndex_.find(canonPath);
        if (it == moduleIndex_.end()) continue;

        ModuleInfo& mod = modules_[it->second];
        if (!mod.program) continue;

        // Move all declarations into the merged program.
        // Imports from each module are kept so the type checker can
        // call registerLibrary() for built-in stdlib.
        for (auto& decl : mod.program->declarations)
            merged.declarations.push_back(std::move(decl));
    }

    return merged;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Full pipeline
// ─────────────────────────────────────────────────────────────────────────────
bool Compiler::runPipeline() {
    if (totalErrors_ > 0 && opts_.stopOnError) {
        printSummary();
        return false;
    }

    // Merge all module ASTs into one Program
    Program merged = mergePrograms();

    // Print AST if requested
    if (opts_.printAST) {
        ASTPrinter printer(std::cout);
        printer.print(merged);
    }

    // Type check the merged program
    // Use the last loaded file (the entry point) as the "filename" for errors
    std::string mainFile = loadOrder_.empty() ? "<input>" : loadOrder_.back();
    TypeChecker checker(mainFile);

    // Tell the type checker which import paths were resolved from real .lang
    // files — their symbols are already in the merged program as top-level
    // declarations, so registerLibrary() should not warn about them.
    for (const auto& mod : modules_) {
        if (!mod.isBuiltin && !mod.moduleName.empty())
            checker.registerResolvedImport(mod.moduleName);
        // Also register the canonical path in case it's used as the import string
        if (!mod.isBuiltin && !mod.canonicalPath.empty())
            checker.registerResolvedImport(mod.canonicalPath);
    }

    checker.check(merged);

    for (const auto& e : checker.errors()) {
        std::cerr << e.format() << "\n";
        if (e.severity == TypeError::Severity::Error)
            ++totalErrors_;
        else
            ++totalWarnings_;
    }

    if (totalErrors_ > 0) {
        printSummary();
        return false;
    }

    // Code generation
    CodeGen cg(mainFile);
    ir_ = cg.generate(merged);

    // Write .ll file
    std::ofstream out(opts_.outputFile);
    if (!out) {
        std::cerr << "error: cannot write '" << opts_.outputFile << "'\n";
        ++totalErrors_;
        printSummary();
        return false;
    }
    out << ir_;
    out.close();

    printSummary();
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Summary
// ─────────────────────────────────────────────────────────────────────────────
void Compiler::printSummary() {
    std::cerr << "\n── Summary ";
    std::cerr << std::string(40, '-') << "\n";
    std::cerr << "  Modules loaded : " << loadOrder_.size() << "\n";
    std::cerr << "  Total errors   : " << totalErrors_   << "\n";
    std::cerr << "  Total warnings : " << totalWarnings_ << "\n";
    if (totalErrors_ == 0) {
        std::cerr << "  IR written to  : " << opts_.outputFile << "\n";
        std::cerr << "  IR size        : " << ir_.size() << " bytes\n";

        // Find the best runtime.o — prefer the installed one
        std::string rtPath = "/exec/runtime.o";
        {
            std::ifstream probe(rtPath);
            if (!probe) rtPath = "runtime.o";  // fallback to local
        }
        std::cerr << "\nTo compile and run:\n"
                  << "  clang -O2 " << opts_.outputFile
                  << " " << rtPath << " -lm -o program\n"
                  << "  ./program\n";
    }
}
