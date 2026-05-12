#pragma once
//===----------------------------------------------------------------------===//
//  SymbolTable.h  –  Scope stack, type descriptors, symbol records
//===----------------------------------------------------------------------===//
#ifndef SYMBOL_TABLE_H
#define SYMBOL_TABLE_H

#include "AST.h"

#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <optional>
#include <cassert>

// ─────────────────────────────────────────────────────────────────────────────
//  TypeKind  –  canonical kinds of types the checker understands
// ─────────────────────────────────────────────────────────────────────────────
enum class TypeKind {
    Void,
    Bool,
    Char,
    Int,       // signed 32-bit
    UInt,
    Long,      // signed 64-bit
    ULong,
    Float,
    UFloat,
    Double,
    String,
    Array,     // element type stored in elementType
    Pointer,   // pointee type stored in elementType
    Enum,
    Struct,
    Union,
    Class,
    Function,
    Null,      // type of the null literal
    Unknown,   // used for error recovery
    Var,       // type-inferred (resolved during checking)
};

// ─────────────────────────────────────────────────────────────────────────────
//  TypeDesc  –  fully resolved type descriptor
// ─────────────────────────────────────────────────────────────────────────────
struct TypeDesc {
    TypeKind    kind      = TypeKind::Unknown;
    std::string name;                          // user-defined type name
    bool        isArray   = false;             // convenience: kind == Array
    bool        isPointer = false;             // convenience: kind == Pointer
    std::shared_ptr<TypeDesc> elementType;     // for Array / Pointer

    // For function types
    std::shared_ptr<TypeDesc>              returnType;
    std::vector<std::shared_ptr<TypeDesc>> paramTypes;

    // ── Factories ────────────────────────────────────────────────────────────
    static std::shared_ptr<TypeDesc> makeVoid()    { return make(TypeKind::Void,   "void"); }
    static std::shared_ptr<TypeDesc> makeBool()    { return make(TypeKind::Bool,   "bool"); }
    static std::shared_ptr<TypeDesc> makeChar()    { return make(TypeKind::Char,   "char"); }
    static std::shared_ptr<TypeDesc> makeInt()     { return make(TypeKind::Int,    "int"); }
    static std::shared_ptr<TypeDesc> makeUInt()    { return make(TypeKind::UInt,   "uint"); }
    static std::shared_ptr<TypeDesc> makeLong()    { return make(TypeKind::Long,   "long"); }
    static std::shared_ptr<TypeDesc> makeULong()   { return make(TypeKind::ULong,  "ulong"); }
    static std::shared_ptr<TypeDesc> makeFloat()   { return make(TypeKind::Float,  "float"); }
    static std::shared_ptr<TypeDesc> makeUFloat()  { return make(TypeKind::UFloat, "ufloat"); }
    static std::shared_ptr<TypeDesc> makeDouble()  { return make(TypeKind::Double, "double"); }
    static std::shared_ptr<TypeDesc> makeString()  { return make(TypeKind::String, "string"); }
    static std::shared_ptr<TypeDesc> makeNull()    { return make(TypeKind::Null,   "null"); }
    static std::shared_ptr<TypeDesc> makeUnknown() { return make(TypeKind::Unknown,"<unknown>"); }

    static std::shared_ptr<TypeDesc> makeArray(std::shared_ptr<TypeDesc> elem) {
        auto t = std::make_shared<TypeDesc>();
        t->kind        = TypeKind::Array;
        t->isArray     = true;
        t->name        = elem->name + "[]";
        t->elementType = std::move(elem);
        return t;
    }
    static std::shared_ptr<TypeDesc> makePointer(std::shared_ptr<TypeDesc> elem) {
        auto t = std::make_shared<TypeDesc>();
        t->kind        = TypeKind::Pointer;
        t->isPointer   = true;
        t->name        = elem->name + "*";
        t->elementType = std::move(elem);
        return t;
    }
    static std::shared_ptr<TypeDesc> makeNamed(TypeKind k, std::string name) {
        return make(k, std::move(name));
    }

    // ── Predicates ───────────────────────────────────────────────────────────
    bool isVoid()      const { return kind == TypeKind::Void; }
    bool isBool()      const { return kind == TypeKind::Bool; }
    bool isNumeric()   const {
        return kind == TypeKind::Int   || kind == TypeKind::UInt  ||
               kind == TypeKind::Long  || kind == TypeKind::ULong ||
               kind == TypeKind::Float || kind == TypeKind::UFloat||
               kind == TypeKind::Double|| kind == TypeKind::Char;
    }
    bool isInteger()   const {
        return kind == TypeKind::Int  || kind == TypeKind::UInt ||
               kind == TypeKind::Long || kind == TypeKind::ULong||
               kind == TypeKind::Char;
    }
    bool isFloat()     const {
        return kind == TypeKind::Float || kind == TypeKind::UFloat ||
               kind == TypeKind::Double;
    }
    bool isUserDefined() const {
        return kind == TypeKind::Class || kind == TypeKind::Struct ||
               kind == TypeKind::Union || kind == TypeKind::Enum;
    }
    bool isUnknown()   const { return kind == TypeKind::Unknown; }
    bool isNull()      const { return kind == TypeKind::Null; }

    std::string toString() const { return name; }

private:
    static std::shared_ptr<TypeDesc> make(TypeKind k, std::string n) {
        auto t  = std::make_shared<TypeDesc>();
        t->kind = k;
        t->name = std::move(n);
        return t;
    }
};

using TypeDescPtr = std::shared_ptr<TypeDesc>;

// ─────────────────────────────────────────────────────────────────────────────
//  SymbolKind
// ─────────────────────────────────────────────────────────────────────────────
enum class SymbolKind {
    Variable,
    Parameter,
    Function,
    Class,
    Struct,
    Union,
    Enum,
    EnumMember,
};

// ─────────────────────────────────────────────────────────────────────────────
//  Symbol  –  one entry in a scope
// ─────────────────────────────────────────────────────────────────────────────
struct Symbol {
    std::string    name;
    SymbolKind     kind;
    TypeDescPtr    type;
    SourceLocation definedAt;

    // Extra info for callables
    std::vector<TypeDescPtr> paramTypes;  // function/method params
    bool                     throws = false;

    // For class symbols: the declaration node (for member lookup)
    const ClassDecl*  classDecl  = nullptr;
    const StructDecl* structDecl = nullptr;
    const UnionDecl*  unionDecl  = nullptr;
    const EnumDecl*   enumDecl   = nullptr;
};

// ─────────────────────────────────────────────────────────────────────────────
//  Scope
// ─────────────────────────────────────────────────────────────────────────────
struct Scope {
    enum class Kind { Global, Class, Function, Block, Loop, Switch };

    Kind                                    kind;
    std::unordered_map<std::string, Symbol> symbols;
    // Back-pointer to the enclosing function's return type (for return checking)
    TypeDescPtr                             returnType;

    explicit Scope(Kind k) : kind(k) {}

    bool has(const std::string& name) const {
        return symbols.count(name) > 0;
    }
    Symbol* get(const std::string& name) {
        auto it = symbols.find(name);
        return it != symbols.end() ? &it->second : nullptr;
    }
    void define(Symbol sym) {
        symbols.emplace(sym.name, std::move(sym));
    }
};

// ─────────────────────────────────────────────────────────────────────────────
//  SymbolTable  –  stack of scopes
// ─────────────────────────────────────────────────────────────────────────────
class SymbolTable {
public:
    SymbolTable() {
        // Push the global scope
        push(Scope::Kind::Global);
    }

    void push(Scope::Kind kind) {
        scopes_.emplace_back(kind);
    }

    void pop() {
        assert(!scopes_.empty());
        scopes_.pop_back();
    }

    // Define in the current (innermost) scope; returns false if already defined
    bool define(Symbol sym) {
        auto& cur = scopes_.back();
        if (cur.has(sym.name)) return false;
        cur.define(std::move(sym));
        return true;
    }

    // Look up walking from innermost to outermost scope
    Symbol* lookup(const std::string& name) {
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
            if (auto* s = it->get(name)) return s;
        }
        return nullptr;
    }

    // Look up only in the current scope
    Symbol* lookupLocal(const std::string& name) {
        return scopes_.back().get(name);
    }

    // Set the return type of the current function scope
    void setReturnType(TypeDescPtr t) {
        // Walk inward to find the function scope
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
            if (it->kind == Scope::Kind::Function) {
                it->returnType = std::move(t);
                return;
            }
        }
    }

    // Get current function's return type
    TypeDescPtr currentReturnType() const {
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it)
            if (it->kind == Scope::Kind::Function && it->returnType)
                return it->returnType;
        return nullptr;
    }

    // Are we currently inside a loop?
    bool inLoop() const {
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
            if (it->kind == Scope::Kind::Loop)   return true;
            if (it->kind == Scope::Kind::Function) break;
        }
        return false;
    }

    // Are we currently inside a switch?
    bool inSwitch() const {
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
            if (it->kind == Scope::Kind::Switch)   return true;
            if (it->kind == Scope::Kind::Function) break;
        }
        return false;
    }

    // Are we inside a throws-declared function?
    bool inThrowsFunction() const { return inThrows_; }
    void setInThrows(bool v)      { inThrows_ = v; }

    // Are we inside a try block?
    bool inTryBlock() const { return tryDepth_ > 0; }
    void enterTry()         { ++tryDepth_; }
    void exitTry()          { if (tryDepth_ > 0) --tryDepth_; }

    Scope& current() { return scopes_.back(); }

private:
    std::vector<Scope> scopes_;
    bool               inThrows_  = false;
    int                tryDepth_  = 0;
};

#endif // SYMBOL_TABLE_H
