#pragma once
//=============================================================================
//  Compiler.h  –  Multi-file compilation pipeline
//
//  Responsibilities:
//    1. Resolve import paths (search path list, cycle detection)
//    2. Parse each file into its own Program (AST)
//    3. Type-check each module in dependency order, propagating exported
//       symbols into importers' scopes
//    4. Merge all module ASTs into a single Program for codegen
//    5. Invoke CodeGen to produce a single .ll file
//
//  Built-in stdlib modules (std.io, std.string, std.math) are still
//  handled by TypeChecker::registerLibrary() — they map to C runtime
//  functions in runtime.c.  User-defined .lang files are parsed normally.
//=============================================================================
#ifndef COMPILER_H
#define COMPILER_H

#include "AST.h"
#include "Lexer.h"
#include "Parser.h"
#include "TypeChecker.h"
#include "CodeGen.h"

#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <filesystem>
#include <optional>
#include <iostream>

namespace fs = std::filesystem;

// ─────────────────────────────────────────────────────────────────────────────
//  CompileOptions
// ─────────────────────────────────────────────────────────────────────────────
struct CompileOptions {
    std::string              outputFile  = "output.ll";
    std::vector<std::string> searchPaths;   // directories to search for imports
    bool                     printAST    = false;
    bool                     printIR     = false;
    bool                     verbose     = false;
    bool                     stopOnError = true;
};

// ─────────────────────────────────────────────────────────────────────────────
//  ModuleInfo  –  one compiled .lang file
// ─────────────────────────────────────────────────────────────────────────────
struct ModuleInfo {
    std::string           canonicalPath;  // absolute path (or module name for builtins)
    std::string           moduleName;     // e.g. "std.io" or "utils/math"
    std::unique_ptr<Program> program;     // parsed AST
    std::vector<TypeError>   errors;
    std::vector<TypeError>   warnings;
    bool                  isBuiltin = false; // handled by registerLibrary()
    bool                  checked   = false;
};

// ─────────────────────────────────────────────────────────────────────────────
//  Compiler
// ─────────────────────────────────────────────────────────────────────────────
class Compiler {
public:
    explicit Compiler(CompileOptions opts = {});

    // Compile one or more entry files → single .ll output.
    // Returns true if compilation succeeded (0 errors).
    bool compile(const std::vector<std::string>& inputFiles);

    // Compile a string of source code directly (for REPL / testing).
    bool compileString(const std::string& source,
                       const std::string& filename = "<input>");

    // Access results after compile()
    const std::string&              generatedIR()  const { return ir_; }
    std::size_t                     totalErrors()  const { return totalErrors_; }
    std::size_t                     totalWarnings()const { return totalWarnings_; }
    const std::vector<std::string>& loadOrder()    const { return loadOrder_; }

private:
    // ── Module loading ───────────────────────────────────────────────────────

    // Resolve an import path string to a canonical file path.
    // Returns empty string if not found (builtin or missing).
    std::string resolveImport(const std::string& importPath,
                              const std::string& fromFile);

    // Load, lex, and parse a file.  Returns module index in modules_.
    // If already loaded returns existing index.  Detects cycles.
    int loadModule(const std::string& canonicalPath,
                   const std::string& moduleName);

    // Type-check a module (after all its dependencies are loaded).
    void checkModule(int idx);

    // Recursively load all imports of a module, depth-first.
    // Fills loadOrder_ with post-order (dependencies before dependents).
    void loadImports(int moduleIdx,
                     std::unordered_set<std::string>& visiting);

    // ── Merging and codegen ──────────────────────────────────────────────────

    // Merge all loaded (non-builtin) modules' ASTs into one Program.
    Program mergePrograms();

    // Run the full pipeline on already-loaded modules.
    bool runPipeline();

    // ── Helpers ──────────────────────────────────────────────────────────────
    bool isBuiltinModule(const std::string& name) const;
    std::string readFile(const std::string& path);
    void printSummary();

    // ── State ────────────────────────────────────────────────────────────────
    CompileOptions opts_;

    // All loaded modules, indexed by canonical path
    std::vector<ModuleInfo>              modules_;
    std::unordered_map<std::string, int> moduleIndex_; // canonical path → index

    // Post-order load sequence (dependencies before dependents)
    std::vector<std::string>             loadOrder_;   // canonical paths

    // Cycle detection
    std::unordered_set<std::string>      inProgress_;

    // Shared type checker state — all modules share one symbol table
    // so symbols from imported modules are visible in importers.
    // We run the type checker once over the merged program.

    std::string  ir_;
    std::size_t  totalErrors_   = 0;
    std::size_t  totalWarnings_ = 0;

    static const std::unordered_set<std::string> kBuiltinModules;
};

#endif // COMPILER_H
