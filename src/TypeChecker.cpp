//===----------------------------------------------------------------------===//
//  TypeChecker.cpp  –  Two-pass type checker implementation
//===----------------------------------------------------------------------===//
#include "TypeChecker.h"

#include <sstream>
#include <cassert>
#include <algorithm>

// ─────────────────────────────────────────────────────────────────────────────
//  Constructor
// ─────────────────────────────────────────────────────────────────────────────
TypeChecker::TypeChecker(std::string filename)
    : filename_(std::move(filename)) {}

// ─────────────────────────────────────────────────────────────────────────────
//  Error / warning emission
// ─────────────────────────────────────────────────────────────────────────────
void TypeChecker::error(const std::string& msg, SourceLocation loc) {
    if (loc.filename.empty()) loc.filename = filename_;
    errors_.push_back({msg, loc, TypeError::Severity::Error});
}

void TypeChecker::warn(const std::string& msg, SourceLocation loc) {
    if (loc.filename.empty()) loc.filename = filename_;
    errors_.push_back({msg, loc, TypeError::Severity::Warning});
}

// ─────────────────────────────────────────────────────────────────────────────
//  Built-in type name → TypeDesc
// ─────────────────────────────────────────────────────────────────────────────
TypeDescPtr TypeChecker::builtinType(const std::string& name) {
    if (name == "bool")   return TypeDesc::makeBool();
    if (name == "char")   return TypeDesc::makeChar();
    if (name == "int")    return TypeDesc::makeInt();
    if (name == "uint")   return TypeDesc::makeUInt();
    if (name == "long")   return TypeDesc::makeLong();
    if (name == "ulong")  return TypeDesc::makeULong();
    if (name == "float")  return TypeDesc::makeFloat();
    if (name == "ufloat") return TypeDesc::makeUFloat();
    if (name == "double") return TypeDesc::makeDouble();
    if (name == "string") return TypeDesc::makeString();
    if (name == "void")   return TypeDesc::makeVoid();
    return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Resolve a TypeRef (from the parser) into a TypeDesc
// ─────────────────────────────────────────────────────────────────────────────
TypeDescPtr TypeChecker::resolveTypeRef(const TypeRef& ref) {
    TypeDescPtr base;

    if (ref.name == "var") {
        // inferred – will be replaced when the initializer is known
        base = TypeDesc::makeUnknown();
        base->kind = TypeKind::Var;
        base->name = "var";
    } else {
        base = builtinType(ref.name);
        if (!base) {
            // Look up user-defined type
            Symbol* sym = symTable_.lookup(ref.name);
            if (!sym) {
                // Return a placeholder — error emitted at use-site
                base = TypeDesc::makeNamed(TypeKind::Unknown, ref.name);
            } else {
                base = sym->type;
            }
        }
    }

    if (ref.isArray)   return TypeDesc::makeArray(base);
    if (ref.isPointer) return TypeDesc::makePointer(base);
    return base;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Type compatibility rules
// ─────────────────────────────────────────────────────────────────────────────
bool TypeChecker::isAssignable(const TypeDescPtr& target, const TypeDescPtr& value) {
    if (!target || !value) return true; // error recovery
    if (target->isUnknown() || value->isUnknown()) return true;

    // null is assignable to any class, pointer, or string
    if (value->isNull()) {
        return target->kind == TypeKind::Class   ||
               target->kind == TypeKind::Pointer ||
               target->kind == TypeKind::String  ||
               target->kind == TypeKind::Var     ||  // var = null is always valid
               target->kind == TypeKind::Unknown;
    }

    // exact match
    if (target->kind == target->kind && target->name == value->name) return true;
    if (target->name == value->name) return true;

    // Polymorphic upcast: Dog is assignable to Animal if Dog extends Animal
    // Walk the value's class hierarchy looking for target
    if (target->kind == TypeKind::Class && value->kind == TypeKind::Class) {
        std::string cur = value->name;
        int depth = 0;
        while (!cur.empty() && depth++ < 32) {
            if (cur == target->name) return true;
            // Look up superclass
            Symbol* sym = symTable_.lookup(cur);
            if (!sym || !sym->classDecl || !sym->classDecl->superClass) break;
            cur = *sym->classDecl->superClass;
        }
    }

    // numeric widening  char < int < uint < long < ulong < float < ufloat < double
    // Also allow same-width signed/unsigned interchange (int ↔ uint, long ↔ ulong)
    // bool and char are both assignable to int (they coerce to i32 in codegen)
    if (target->kind == TypeKind::Int || target->kind == TypeKind::UInt ||
        target->kind == TypeKind::Long || target->kind == TypeKind::ULong ||
        target->kind == TypeKind::Float || target->kind == TypeKind::Double) {
        if (value->kind == TypeKind::Bool || value->kind == TypeKind::Char)
            return true;
    }
    if (target->isNumeric() && value->isNumeric()) {
        auto rank = [](TypeKind k) -> int {
            switch (k) {
            case TypeKind::Char:   return 0;
            case TypeKind::Int:    return 1;
            case TypeKind::UInt:   return 1; // same rank as int
            case TypeKind::Long:   return 3;
            case TypeKind::ULong:  return 3; // same rank as long
            case TypeKind::Float:  return 5;
            case TypeKind::UFloat: return 6;
            case TypeKind::Double: return 7;
            default:               return -1;
            }
        };
        return rank(target->kind) >= rank(value->kind);
    }

    // var target: always ok (resolved later)
    if (target->kind == TypeKind::Var) return true;
    // null-typed target: var can always replace null (null is unresolved type)
    if (target->kind == TypeKind::Null) return true;

    return false;
}

bool TypeChecker::isComparable(const TypeDescPtr& a, const TypeDescPtr& b) {
    if (!a || !b) return true;
    if (a->isUnknown() || b->isUnknown()) return true;
    if (a->name == b->name) return true;
    if (a->isNumeric() && b->isNumeric()) return true;
    if (a->isBool() && b->isBool()) return true;
    // null can be compared to any class, string, or var
    if (a->isNull() || b->isNull()) return true;
    // var can be compared to anything
    if (a->kind == TypeKind::Var || b->kind == TypeKind::Var) return true;
    return false;
}

TypeDescPtr TypeChecker::widenNumeric(const TypeDescPtr& a, const TypeDescPtr& b) {
    if (!a || !b) return TypeDesc::makeUnknown();
    auto rank = [](TypeKind k) -> int {
        switch (k) {
        case TypeKind::Char:   return 0;
        case TypeKind::Int:    return 1;
        case TypeKind::UInt:   return 2;
        case TypeKind::Long:   return 3;
        case TypeKind::ULong:  return 4;
        case TypeKind::Float:  return 5;
        case TypeKind::UFloat: return 6;
        case TypeKind::Double: return 7;
        default:               return -1;
        }
    };
    return rank(a->kind) >= rank(b->kind) ? a : b;
}

std::string TypeChecker::typeMismatch(const std::string& ctx,
                                      const TypeDescPtr& expected,
                                      const TypeDescPtr& got) {
    return ctx + ": expected '" + (expected ? expected->toString() : "?") +
           "' but got '" + (got ? got->toString() : "?") + "'";
}

// ─────────────────────────────────────────────────────────────────────────────
//  Entry point
// ─────────────────────────────────────────────────────────────────────────────
void TypeChecker::check(Program& program) {
    // ── Resolve imports → register their exported symbols ─────────────────────
    // Built-in stdlib imports (std.io, std.string, std.math) are registered via
    // registerLibrary(). User module imports that were resolved from .lang files
    // and merged into this program are skipped — their symbols are already
    // present as hoisted top-level declarations.
    for (const auto& d : program.declarations) {
        if (d->kind != DeclKind::Import) continue;
        const auto& imp = static_cast<const ImportDecl&>(*d);
        if (resolvedImports_.count(imp.path)) continue;  // already merged
        registerLibrary(imp.path, imp.loc);
    }

    // Pass 1: register all top-level names so bodies can reference them
    for (const auto& d : program.declarations)
        hoistDecl(*d);

    // Pass 2: check every declaration body
    for (auto& d : program.declarations)
        checkDecl(*d);
}

// ═════════════════════════════════════════════════════════════════════════════
//  Pass 1 – Hoist
// ═════════════════════════════════════════════════════════════════════════════
void TypeChecker::hoistDecl(const Decl& decl) {
    switch (decl.kind) {
    case DeclKind::Enum: {
        const auto& e = static_cast<const EnumDecl&>(decl);
        Symbol sym;
        sym.name       = e.name;
        sym.kind       = SymbolKind::Enum;
        sym.type       = TypeDesc::makeNamed(TypeKind::Enum, e.name);
        sym.definedAt  = e.loc;
        sym.enumDecl   = &e;
        if (!symTable_.define(sym))
            error("redefinition of '" + e.name + "'", e.loc);
        // Register each member as  EnumName::Member  and  Member
        for (const auto& m : e.members) {
            Symbol ms;
            ms.name      = e.name + "::" + m;
            ms.kind      = SymbolKind::EnumMember;
            ms.type      = sym.type;
            ms.definedAt = e.loc;
            symTable_.define(ms);
            // Also plain name for unqualified use
            Symbol ms2 = ms;
            ms2.name = m;
            symTable_.define(ms2);
        }
        break;
    }
    case DeclKind::Struct: {
        const auto& s = static_cast<const StructDecl&>(decl);
        Symbol sym;
        sym.name       = s.name;
        sym.kind       = SymbolKind::Struct;
        sym.type       = TypeDesc::makeNamed(TypeKind::Struct, s.name);
        sym.definedAt  = s.loc;
        sym.structDecl = &s;
        if (!symTable_.define(sym))
            error("redefinition of '" + s.name + "'", s.loc);
        break;
    }
    case DeclKind::Union: {
        const auto& u = static_cast<const UnionDecl&>(decl);
        Symbol sym;
        sym.name       = u.name;
        sym.kind       = SymbolKind::Union;
        sym.type       = TypeDesc::makeNamed(TypeKind::Union, u.name);
        sym.definedAt  = u.loc;
        sym.unionDecl  = &u;
        if (!symTable_.define(sym))
            error("redefinition of '" + u.name + "'", u.loc);
        break;
    }
    case DeclKind::Class:
        hoistClass(static_cast<const ClassDecl&>(decl));
        break;
    case DeclKind::Function:
        hoistFunction(static_cast<const FunctionDecl&>(decl));
        break;
    case DeclKind::Variable: {
        const auto& v = static_cast<const VarDecl&>(decl);
        Symbol sym;
        sym.name      = v.name;
        sym.kind      = SymbolKind::Variable;
        sym.type      = resolveTypeRef(v.type);
        sym.definedAt = v.loc;
        if (!symTable_.define(sym))
            error("redefinition of '" + v.name + "'", v.loc);
        break;
    }
    case DeclKind::Using: {
        // Register the alias as a type synonym
        const auto& u = static_cast<const UsingDecl&>(decl);
        Symbol sym;
        sym.name      = u.alias;
        sym.kind      = SymbolKind::Struct; // treat like a named type
        sym.type      = resolveTypeRef(u.target);
        sym.definedAt = u.loc;
        if (!symTable_.define(sym))
            error("redefinition of type alias '" + u.alias + "'", u.loc);
        break;
    }
    case DeclKind::Import:
        break; // nothing to hoist
    }
}

void TypeChecker::hoistClass(const ClassDecl& decl) {
    Symbol sym;
    sym.name      = decl.name;
    sym.kind      = SymbolKind::Class;
    sym.type      = TypeDesc::makeNamed(TypeKind::Class, decl.name);
    sym.definedAt = decl.loc;
    sym.classDecl = &decl;
    if (!symTable_.define(sym))
        error("redefinition of class '" + decl.name + "'", decl.loc);

    // Hoist methods so calls within the class body resolve correctly
    for (const auto& m : decl.methods)
        hoistFunction(*m, decl.name);
}

void TypeChecker::hoistFunction(const FunctionDecl& decl,
                                const std::string& ownerClass) {
    Symbol sym;
    sym.name      = ownerClass.empty() ? decl.name : ownerClass + "::" + decl.name;
    sym.kind      = SymbolKind::Function;
    sym.throws    = decl.throws;
    sym.definedAt = decl.loc;

    // Build a function-type descriptor
    // async functions return a Future (i8* GC object) at the call site
    auto fnType         = std::make_shared<TypeDesc>();
    fnType->kind        = TypeKind::Function;
    fnType->name        = "fun " + decl.name;
    if (decl.isAsync) {
        // Future is treated as a class type named "Future"
        fnType->returnType = TypeDesc::makeNamed(TypeKind::Var, "Future");
    } else {
        fnType->returnType  = resolveTypeRef(decl.returnType);
    }

    for (const auto& p : decl.params) {
        auto pt = resolveTypeRef(p.type);
        fnType->paramTypes.push_back(pt);
        sym.paramTypes.push_back(pt);
    }
    sym.type = fnType;

    // For methods, also register under just the method name so member-access works
    symTable_.define(sym);
    if (!ownerClass.empty()) {
        Symbol plain = sym;
        plain.name = decl.name;
        symTable_.define(plain);
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  Pass 2 – Check declarations
// ═════════════════════════════════════════════════════════════════════════════
void TypeChecker::checkDecl(Decl& decl) {
    switch (decl.kind) {
    case DeclKind::Import:   checkImport(static_cast<ImportDecl&>(decl));   break;
    case DeclKind::Enum:     checkEnum(static_cast<EnumDecl&>(decl));       break;
    case DeclKind::Struct:   checkStruct(static_cast<StructDecl&>(decl));   break;
    case DeclKind::Union:    checkUnion(static_cast<UnionDecl&>(decl));     break;
    case DeclKind::Class:    checkClass(static_cast<ClassDecl&>(decl));     break;
    case DeclKind::Function: checkFunction(static_cast<FunctionDecl&>(decl)); break;
    case DeclKind::Variable: checkTopLevelVar(static_cast<VarDecl&>(decl)); break;
    case DeclKind::Using:    break; // alias already hoisted; nothing to check in body
    }
}

void TypeChecker::registerFn(const std::string& name, TypeDescPtr ret,
                             std::vector<TypeDescPtr> params) {
    auto fnType        = std::make_shared<TypeDesc>();
    fnType->kind       = TypeKind::Function;
    fnType->name       = "fun " + name;
    fnType->returnType = ret;
    fnType->paramTypes = params;

    Symbol sym;
    sym.name       = name;
    sym.kind       = SymbolKind::Function;
    sym.type       = fnType;
    sym.paramTypes = params;
    sym.definedAt  = {1, 1, "<stdlib>"};
    symTable_.define(sym); // silent if already defined
}

void TypeChecker::registerLibrary(const std::string& path, SourceLocation loc) {
    // ── std.io ────────────────────────────────────────────────────────────────
    if (path == "std.io") {
        registerFn("getChar",       TypeDesc::makeChar());
        registerFn("putChar",       TypeDesc::makeVoid(),   {TypeDesc::makeChar()});
        registerFn("print",         TypeDesc::makeVoid(),   {TypeDesc::makeString()});
        registerFn("println",       TypeDesc::makeVoid(),   {TypeDesc::makeString()});
        registerFn("printNewline",  TypeDesc::makeVoid());
        registerFn("readLine",      TypeDesc::makeString());
        registerFn("readWord",      TypeDesc::makeString());
        registerFn("waitForKey",    TypeDesc::makeBool(),   {TypeDesc::makeChar()});
        registerFn("flushInputLine",TypeDesc::makeVoid());
        registerFn("readInt",       TypeDesc::makeInt());
        registerFn("readFloat",     TypeDesc::makeFloat());
        registerFn("printInt",      TypeDesc::makeVoid(),   {TypeDesc::makeInt()});
        registerFn("printFloat",    TypeDesc::makeVoid(),   {TypeDesc::makeFloat()});
        registerFn("printBool",     TypeDesc::makeVoid(),   {TypeDesc::makeBool()});
        registerFn("prompt",        TypeDesc::makeString(), {TypeDesc::makeString()});
        // GC — always available (runtime is always linked)
        registerFn("gcCollect",     TypeDesc::makeVoid());
        registerFn("gcStats",       TypeDesc::makeString());
        registerFn("gcLiveBytes",   TypeDesc::makeLong());
        registerFn("gcLiveObjects", TypeDesc::makeLong());
        registerFn("gcPin",         TypeDesc::makeVoid(),   {TypeDesc::makeString()});
        registerFn("gcUnpin",       TypeDesc::makeVoid(),   {TypeDesc::makeString()});
        // File I/O
        registerFn("readFile",      TypeDesc::makeString(), {TypeDesc::makeString()});
        registerFn("writeFile",     TypeDesc::makeInt(),    {TypeDesc::makeString(), TypeDesc::makeString()});
        registerFn("fileExists",    TypeDesc::makeInt(),    {TypeDesc::makeString()});
        // Threading
        auto varT = std::make_shared<TypeDesc>(); varT->kind=TypeKind::Var; varT->name="var";
        registerFn("mutexNew",      varT);
        registerFn("mutexLock",     TypeDesc::makeVoid(),  {varT});
        registerFn("mutexUnlock",   TypeDesc::makeVoid(),  {varT});
        registerFn("mutexTrylock",  TypeDesc::makeBool(),  {varT});
        registerFn("mutexDestroy",  TypeDesc::makeVoid(),  {varT});
        registerFn("futureAwait",   varT,                  {varT});
        registerFn("futureDone",    TypeDesc::makeBool(),  {varT});
        registerFn("threadSleep",   TypeDesc::makeVoid(),  {TypeDesc::makeInt()});
        registerFn("threadYield",   TypeDesc::makeVoid());
        // Terminal primitives (for ecvim.ec)
        registerFn("termRaw",        TypeDesc::makeVoid());
        registerFn("termRestore",    TypeDesc::makeVoid());
        registerFn("getTermRows",       TypeDesc::makeInt());
        registerFn("getTermCols",       TypeDesc::makeInt());
        registerFn("readKey",        TypeDesc::makeInt());
        registerFn("writeRaw",       TypeDesc::makeVoid(),  {TypeDesc::makeString()});
        registerFn("flushOutput",    TypeDesc::makeVoid());
        registerFn("runSystem",      TypeDesc::makeInt(),   {TypeDesc::makeString()});
        registerFn("atexitRestore",  TypeDesc::makeVoid());
        registerFn("registerWinch",  TypeDesc::makeVoid());
        registerFn("winchPending",   TypeDesc::makeBool());
        // System
        registerFn("exit",           TypeDesc::makeVoid(),  {TypeDesc::makeInt()});
        registerFn("timeMs",         TypeDesc::makeLong());
        // std.time
        registerFn("wallClockMs",    TypeDesc::makeLong());
        registerFn("timeYear",       TypeDesc::makeInt(),   {TypeDesc::makeLong()});
        registerFn("timeMonth",      TypeDesc::makeInt(),   {TypeDesc::makeLong()});
        registerFn("timeDay",        TypeDesc::makeInt(),   {TypeDesc::makeLong()});
        registerFn("timeHour",       TypeDesc::makeInt(),   {TypeDesc::makeLong()});
        registerFn("timeMinute",     TypeDesc::makeInt(),   {TypeDesc::makeLong()});
        registerFn("timeSecond",     TypeDesc::makeInt(),   {TypeDesc::makeLong()});
        registerFn("timeWeekday",    TypeDesc::makeInt(),   {TypeDesc::makeLong()});
        registerFn("timeYearday",    TypeDesc::makeInt(),   {TypeDesc::makeLong()});
        registerFn("timeFormat",     TypeDesc::makeString(),{TypeDesc::makeLong(), TypeDesc::makeString()});
        registerFn("timeYearLocal",  TypeDesc::makeInt(),   {TypeDesc::makeLong()});
        registerFn("timeMonthLocal", TypeDesc::makeInt(),   {TypeDesc::makeLong()});
        registerFn("timeDayLocal",   TypeDesc::makeInt(),   {TypeDesc::makeLong()});
        registerFn("timeHourLocal",  TypeDesc::makeInt(),   {TypeDesc::makeLong()});
        registerFn("timeMinuteLocal",TypeDesc::makeInt(),   {TypeDesc::makeLong()});
        registerFn("timeSecondLocal",TypeDesc::makeInt(),   {TypeDesc::makeLong()});
        registerFn("timeFormatLocal",TypeDesc::makeString(),{TypeDesc::makeLong(), TypeDesc::makeString()});
        // std.algorithm (native helpers)
        auto varT2 = std::make_shared<TypeDesc>(); varT2->kind=TypeKind::Var; varT2->name="var";
        registerFn("listSortFn",     TypeDesc::makeVoid(),  {varT2, varT2, varT2});
        registerFn("listReverseFn",  TypeDesc::makeVoid(),  {varT2});
        registerFn("listBinarySearch",TypeDesc::makeLong(), {varT2, varT2, varT2, varT2});
        registerFn("listShuffle",    TypeDesc::makeVoid(),  {varT2});
        registerFn("listSumFn",      TypeDesc::makeLong(),  {varT2, varT2, varT2});
        registerFn("randomSeed",     TypeDesc::makeVoid(),  {TypeDesc::makeInt()});
        registerFn("randomInt",      TypeDesc::makeInt(),   {TypeDesc::makeInt(), TypeDesc::makeInt()});
        registerFn("randomDouble",   TypeDesc::makeDouble());
        return;
    }

    // ── std.string ────────────────────────────────────────────────────────────
    if (path == "std.string") {
        // String → String
        registerFn("toString",      TypeDesc::makeString(), {TypeDesc::makeInt()});
        registerFn("toString",      TypeDesc::makeString(), {TypeDesc::makeBool()});
        registerFn("toString",      TypeDesc::makeString(), {TypeDesc::makeChar()});
        registerFn("toString",      TypeDesc::makeString(), {TypeDesc::makeLong()});
        registerFn("longToString",  TypeDesc::makeString(), {TypeDesc::makeLong()});
        registerFn("floatToString", TypeDesc::makeString(), {TypeDesc::makeFloat()});
        registerFn("doubleToString",TypeDesc::makeString(), {TypeDesc::makeDouble()});
        registerFn("charToString",  TypeDesc::makeString(), {TypeDesc::makeChar()});
        registerFn("boolToString",  TypeDesc::makeString(), {TypeDesc::makeBool()});
        // String → Number
        registerFn("parseInt",      TypeDesc::makeInt(),    {TypeDesc::makeString()});
        registerFn("parseLong",     TypeDesc::makeLong(),   {TypeDesc::makeString()});
        registerFn("parseFloat",    TypeDesc::makeFloat(),  {TypeDesc::makeString()});
        registerFn("parseDouble",   TypeDesc::makeDouble(), {TypeDesc::makeString()});
        // String properties
        registerFn("stringLen",     TypeDesc::makeInt(),    {TypeDesc::makeString()});
        registerFn("stringEmpty",   TypeDesc::makeBool(),   {TypeDesc::makeString()});
        registerFn("charAt",        TypeDesc::makeChar(),   {TypeDesc::makeString(), TypeDesc::makeInt()});
        // String search
        registerFn("indexOf",       TypeDesc::makeInt(),    {TypeDesc::makeString(), TypeDesc::makeString()});
        registerFn("lastIndexOf",   TypeDesc::makeInt(),    {TypeDesc::makeString(), TypeDesc::makeString()});
        registerFn("contains",      TypeDesc::makeBool(),   {TypeDesc::makeString(), TypeDesc::makeString()});
        registerFn("startsWith",    TypeDesc::makeBool(),   {TypeDesc::makeString(), TypeDesc::makeString()});
        registerFn("endsWith",      TypeDesc::makeBool(),   {TypeDesc::makeString(), TypeDesc::makeString()});
        registerFn("count",         TypeDesc::makeInt(),    {TypeDesc::makeString(), TypeDesc::makeString()});
        registerFn("equals",        TypeDesc::makeBool(),   {TypeDesc::makeString(), TypeDesc::makeString()});
        registerFn("compare",       TypeDesc::makeInt(),    {TypeDesc::makeString(), TypeDesc::makeString()});
        // String manipulation
        registerFn("substring",     TypeDesc::makeString(), {TypeDesc::makeString(), TypeDesc::makeInt(), TypeDesc::makeInt()});
        registerFn("toLower",       TypeDesc::makeString(), {TypeDesc::makeString()});
        registerFn("toUpper",       TypeDesc::makeString(), {TypeDesc::makeString()});
        registerFn("trim",          TypeDesc::makeString(), {TypeDesc::makeString()});
        registerFn("trimLeft",      TypeDesc::makeString(), {TypeDesc::makeString()});
        registerFn("trimRight",     TypeDesc::makeString(), {TypeDesc::makeString()});
        registerFn("repeat",        TypeDesc::makeString(), {TypeDesc::makeString(), TypeDesc::makeInt()});
        registerFn("replace",       TypeDesc::makeString(), {TypeDesc::makeString(), TypeDesc::makeString(), TypeDesc::makeString()});
        registerFn("reverse",       TypeDesc::makeString(), {TypeDesc::makeString()});
        // Char operations
        registerFn("isDigit",       TypeDesc::makeBool(),   {TypeDesc::makeChar()});
        registerFn("isAlpha",       TypeDesc::makeBool(),   {TypeDesc::makeChar()});
        registerFn("isAlnum",       TypeDesc::makeBool(),   {TypeDesc::makeChar()});
        registerFn("isSpace",       TypeDesc::makeBool(),   {TypeDesc::makeChar()});
        registerFn("isUpper",       TypeDesc::makeBool(),   {TypeDesc::makeChar()});
        registerFn("isLower",       TypeDesc::makeBool(),   {TypeDesc::makeChar()});
        registerFn("isPunct",       TypeDesc::makeBool(),   {TypeDesc::makeChar()});
        registerFn("toLowerChar",   TypeDesc::makeChar(),   {TypeDesc::makeChar()});
        registerFn("toUpperChar",   TypeDesc::makeChar(),   {TypeDesc::makeChar()});
        registerFn("charCode",      TypeDesc::makeInt(),    {TypeDesc::makeChar()});
        registerFn("charFromCode",  TypeDesc::makeChar(),   {TypeDesc::makeInt()});
        return;
    }

    // ── std.math ──────────────────────────────────────────────────────────────
    if (path == "std.math") {
        registerFn("floor",      TypeDesc::makeDouble(), {TypeDesc::makeDouble()});
        registerFn("ceil",       TypeDesc::makeDouble(), {TypeDesc::makeDouble()});
        registerFn("round",      TypeDesc::makeDouble(), {TypeDesc::makeDouble()});
        registerFn("trunc",      TypeDesc::makeDouble(), {TypeDesc::makeDouble()});
        registerFn("abs",        TypeDesc::makeDouble(), {TypeDesc::makeDouble()});
        registerFn("absInt",     TypeDesc::makeInt(),    {TypeDesc::makeInt()});
        registerFn("sqrt",       TypeDesc::makeDouble(), {TypeDesc::makeDouble()});
        registerFn("pow",        TypeDesc::makeDouble(), {TypeDesc::makeDouble(), TypeDesc::makeDouble()});
        registerFn("cbrt",       TypeDesc::makeDouble(), {TypeDesc::makeDouble()});
        registerFn("powInt",     TypeDesc::makeInt(),    {TypeDesc::makeInt(), TypeDesc::makeInt()});
        registerFn("log",        TypeDesc::makeDouble(), {TypeDesc::makeDouble()});
        registerFn("log2",       TypeDesc::makeDouble(), {TypeDesc::makeDouble()});
        registerFn("log10",      TypeDesc::makeDouble(), {TypeDesc::makeDouble()});
        registerFn("sin",        TypeDesc::makeDouble(), {TypeDesc::makeDouble()});
        registerFn("cos",        TypeDesc::makeDouble(), {TypeDesc::makeDouble()});
        registerFn("tan",        TypeDesc::makeDouble(), {TypeDesc::makeDouble()});
        registerFn("asin",       TypeDesc::makeDouble(), {TypeDesc::makeDouble()});
        registerFn("acos",       TypeDesc::makeDouble(), {TypeDesc::makeDouble()});
        registerFn("atan",       TypeDesc::makeDouble(), {TypeDesc::makeDouble()});
        registerFn("atan2",      TypeDesc::makeDouble(), {TypeDesc::makeDouble(), TypeDesc::makeDouble()});
        registerFn("toRadians",  TypeDesc::makeDouble(), {TypeDesc::makeDouble()});
        registerFn("toDegrees",  TypeDesc::makeDouble(), {TypeDesc::makeDouble()});
        registerFn("min",        TypeDesc::makeDouble(), {TypeDesc::makeDouble(), TypeDesc::makeDouble()});
        registerFn("max",        TypeDesc::makeDouble(), {TypeDesc::makeDouble(), TypeDesc::makeDouble()});
        registerFn("minInt",     TypeDesc::makeInt(),    {TypeDesc::makeInt(), TypeDesc::makeInt()});
        registerFn("maxInt",     TypeDesc::makeInt(),    {TypeDesc::makeInt(), TypeDesc::makeInt()});
        registerFn("clamp",      TypeDesc::makeDouble(), {TypeDesc::makeDouble(), TypeDesc::makeDouble(), TypeDesc::makeDouble()});
        registerFn("clampInt",   TypeDesc::makeInt(),    {TypeDesc::makeInt(), TypeDesc::makeInt(), TypeDesc::makeInt()});
        registerFn("gcd",        TypeDesc::makeInt(),    {TypeDesc::makeInt(), TypeDesc::makeInt()});
        registerFn("lcm",        TypeDesc::makeInt(),    {TypeDesc::makeInt(), TypeDesc::makeInt()});
        registerFn("isEven",     TypeDesc::makeBool(),   {TypeDesc::makeInt()});
        registerFn("isOdd",      TypeDesc::makeBool(),   {TypeDesc::makeInt()});
        registerFn("isPrime",    TypeDesc::makeBool(),   {TypeDesc::makeInt()});
        registerFn("absLong",    TypeDesc::makeLong(),   {TypeDesc::makeLong()});
        registerFn("minLong",    TypeDesc::makeLong(),   {TypeDesc::makeLong(), TypeDesc::makeLong()});
        registerFn("maxLong",    TypeDesc::makeLong(),   {TypeDesc::makeLong(), TypeDesc::makeLong()});
        registerFn("hypot",      TypeDesc::makeDouble(), {TypeDesc::makeDouble(), TypeDesc::makeDouble()});
        return;
    }

    // ── std.collections ───────────────────────────────────────────────────────
    // List, Map, Set, StringBuilder are defined as classes in the .lang file.
    // We only need to suppress the "unknown import" warning here.
    if (path == "std.collections") {
        return;
    }
    // New stdlib modules — implemented as .ec files in stdlib/
    // The type checker will see their declarations through normal file loading.
    if (path == "std.algorithm")  { return; }
    if (path == "std.time")       { return; }
    if (path == "std.iterator")   { return; }
    if (path == "std.vector")     { return; }
    if (path == "std.errors")     { return; }
    if (path == "std.exceptions") { return; }
    if (path == "std.validation") { return; }

    warn("unknown import '" + path + "' — no type information available", loc);
}

void TypeChecker::checkImport(ImportDecl&) {
    // Symbols already registered in registerLibrary() during check() prologue.
}

void TypeChecker::checkEnum(EnumDecl& decl) {
    if (decl.members.empty())
        warn("enum '" + decl.name + "' has no members", decl.loc);
}

void TypeChecker::checkStruct(StructDecl& decl) {
    std::unordered_map<std::string, bool> seen;
    for (const auto& f : decl.fields) {
        if (seen.count(f.name))
            error("duplicate field '" + f.name + "' in struct '" + decl.name + "'", f.loc);
        seen[f.name] = true;
        auto t = resolveTypeRef(f.type);
        if (t->isVoid())
            error("field '" + f.name + "' cannot have type 'void'", f.loc);
    }
}

void TypeChecker::checkUnion(UnionDecl& decl) {
    std::unordered_map<std::string, bool> seen;
    for (const auto& f : decl.fields) {
        if (seen.count(f.name))
            error("duplicate field '" + f.name + "' in union '" + decl.name + "'", f.loc);
        seen[f.name] = true;
        auto t = resolveTypeRef(f.type);
        if (t->isVoid())
            error("field '" + f.name + "' cannot have type 'void'", f.loc);
    }
}

void TypeChecker::checkClass(ClassDecl& decl) {
    // Verify superclass exists
    if (decl.superClass) {
        Symbol* sup = symTable_.lookup(*decl.superClass);
        if (!sup || sup->kind != SymbolKind::Class)
            error("unknown base class '" + *decl.superClass + "'", decl.loc);
        else if (sup->classDecl && sup->classDecl->isFinal)
            error("cannot extend final class '" + *decl.superClass + "'", decl.loc);
    }

    currentClass_ = &decl;
    symTable_.push(Scope::Kind::Class);

    // Register fields in class scope
    for (const auto& f : decl.fields) {
        auto t = resolveTypeRef(f.type);
        if (t->isVoid())
            error("field '" + f.name + "' cannot have type 'void'", f.loc);
        Symbol sym;
        sym.name      = f.name;
        sym.kind      = SymbolKind::Variable;
        sym.type      = t;
        sym.definedAt = f.loc;
        if (!symTable_.define(sym))
            error("duplicate field '" + f.name + "' in class '" + decl.name + "'", f.loc);

        if (f.init) {
            auto initType = checkExpr(**f.init);
            if (!isAssignable(t, initType))
                error(typeMismatch("field '" + f.name + "' initializer", t, initType), f.loc);
        }
    }

    for (auto& m : decl.methods)
        checkFunction(*m);

    symTable_.pop();
    currentClass_ = nullptr;
}

void TypeChecker::checkFunction(FunctionDecl& decl) {
    symTable_.push(Scope::Kind::Function);

    // Register parameters
    for (const auto& p : decl.params) {
        auto t = resolveTypeRef(p.type);
        if (t->isVoid())
            error("parameter '" + p.name + "' cannot have type 'void'", p.loc);
        Symbol sym;
        sym.name      = p.name;
        sym.kind      = SymbolKind::Parameter;
        sym.type      = t;
        sym.definedAt = p.loc;
        if (!symTable_.define(sym))
            error("duplicate parameter '" + p.name + "'", p.loc);
    }

    auto retType = resolveTypeRef(decl.returnType);
    symTable_.setReturnType(retType);
    symTable_.setInThrows(decl.throws);

    if (decl.body)
        checkBlock(*decl.body);

    symTable_.setInThrows(false);
    symTable_.pop();
}

void TypeChecker::checkTopLevelVar(VarDecl& decl) {
    auto t = resolveTypeRef(decl.type);
    if (t->isVoid())
        error("variable '" + decl.name + "' cannot have type 'void'", decl.loc);

    if (decl.init) {
        auto initType = checkExpr(**decl.init);
        if (t->kind != TypeKind::Var && !isAssignable(t, initType))
            error(typeMismatch("variable '" + decl.name + "' initializer", t, initType),
                  decl.loc);
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  Statement checking
// ═════════════════════════════════════════════════════════════════════════════
void TypeChecker::checkStmt(Stmt& stmt) {
    switch (stmt.kind) {
    case StmtKind::Block:      checkBlock(static_cast<BlockStmt&>(stmt));       break;
    case StmtKind::Expression: checkExprStmt(static_cast<ExprStmt&>(stmt));    break;
    case StmtKind::VarDecl:    checkVarDeclStmt(static_cast<VarDeclStmt&>(stmt)); break;
    case StmtKind::If:         checkIf(static_cast<IfStmt&>(stmt));             break;
    case StmtKind::While:      checkWhile(static_cast<WhileStmt&>(stmt));       break;
    case StmtKind::DoWhile:    checkDoWhile(static_cast<DoWhileStmt&>(stmt));   break;
    case StmtKind::For:        checkFor(static_cast<ForStmt&>(stmt));           break;
    case StmtKind::Switch:     checkSwitch(static_cast<SwitchStmt&>(stmt));     break;
    case StmtKind::Return:     checkReturn(static_cast<ReturnStmt&>(stmt));     break;
    case StmtKind::Throw:      checkThrow(static_cast<ThrowStmt&>(stmt));       break;
    case StmtKind::Try:        checkTry(static_cast<TryStmt&>(stmt));           break;
    case StmtKind::With:       checkWith(static_cast<WithStmt&>(stmt));           break;
    case StmtKind::Break:      checkBreak(static_cast<BreakStmt&>(stmt));       break;
    case StmtKind::Continue:   checkContinue(static_cast<ContinueStmt&>(stmt)); break;
    }
}

void TypeChecker::checkBlock(BlockStmt& stmt) {
    symTable_.push(Scope::Kind::Block);
    for (auto& s : stmt.stmts) checkStmt(*s);
    symTable_.pop();
}

void TypeChecker::checkVarDeclStmt(VarDeclStmt& stmt) {
    // Check for redefinition in current block scope
    if (symTable_.lookupLocal(stmt.name))
        error("redefinition of '" + stmt.name + "'", stmt.loc);

    TypeDescPtr declaredType = resolveTypeRef(stmt.type);
    TypeDescPtr actualType   = declaredType;

    if (declaredType->isVoid())
        error("variable '" + stmt.name + "' cannot have type 'void'", stmt.loc);

    if (stmt.init) {
        auto initType = checkExpr(**stmt.init);

        if (declaredType->kind == TypeKind::Var) {
            // type inference: adopt the initializer's type
            actualType = initType;
        } else {
            if (!isAssignable(declaredType, initType))
                error(typeMismatch("variable '" + stmt.name + "'", declaredType, initType),
                      stmt.loc);
        }
    } else if (declaredType->kind == TypeKind::Var) {
        error("'var' declaration '" + stmt.name + "' requires an initializer", stmt.loc);
    }

    Symbol sym;
    sym.name      = stmt.name;
    sym.kind      = SymbolKind::Variable;
    sym.type      = actualType;
    sym.definedAt = stmt.loc;
    symTable_.define(sym);
}

void TypeChecker::checkIf(IfStmt& stmt) {
    auto condType = checkExpr(*stmt.cond);
    if (!condType->isUnknown() && !condType->isBool() && !condType->isNumeric())
        error("if condition must be boolean or numeric, got '" +
              condType->toString() + "'", stmt.cond->loc);

    checkStmt(*stmt.thenBranch);
    if (stmt.elseBranch) checkStmt(**stmt.elseBranch);
}

void TypeChecker::checkWhile(WhileStmt& stmt) {
    auto condType = checkExpr(*stmt.cond);
    if (!condType->isUnknown() && !condType->isBool() && !condType->isNumeric())
        error("while condition must be boolean or numeric", stmt.cond->loc);

    symTable_.push(Scope::Kind::Loop);
    checkStmt(*stmt.body);
    symTable_.pop();
}

void TypeChecker::checkDoWhile(DoWhileStmt& stmt) {
    symTable_.push(Scope::Kind::Loop);
    checkStmt(*stmt.body);
    symTable_.pop();

    auto condType = checkExpr(*stmt.cond);
    if (!condType->isUnknown() && !condType->isBool() && !condType->isNumeric())
        error("do-while condition must be boolean or numeric", stmt.cond->loc);
}

void TypeChecker::checkFor(ForStmt& stmt) {
    symTable_.push(Scope::Kind::Loop);

    if (stmt.init) checkStmt(**stmt.init);
    if (stmt.cond) {
        auto ct = checkExpr(**stmt.cond);
        if (!ct->isUnknown() && !ct->isBool() && !ct->isNumeric())
            error("for condition must be boolean or numeric", (*stmt.cond)->loc);
    }
    if (stmt.incr) checkExpr(**stmt.incr);
    checkStmt(*stmt.body);

    symTable_.pop();
}

void TypeChecker::checkSwitch(SwitchStmt& stmt) {
    auto subjType = checkExpr(*stmt.subject);
    bool integralSubject = subjType->isInteger() || subjType->isUnknown() ||
                           subjType->kind == TypeKind::Enum;
    if (!integralSubject)
        error("switch subject must be integer or enum type, got '" +
              subjType->toString() + "'", stmt.subject->loc);

    symTable_.push(Scope::Kind::Switch);
    bool hasDefault = false;

    for (auto& clause : stmt.cases) {
        if (!clause.value) {
            if (hasDefault)
                error("duplicate 'default' in switch", stmt.loc);
            hasDefault = true;
        } else {
            auto caseType = checkExpr(**clause.value);
            if (!isComparable(subjType, caseType))
                error(typeMismatch("case expression", subjType, caseType),
                      (*clause.value)->loc);
        }
        symTable_.push(Scope::Kind::Block);
        for (auto& s : clause.stmts) checkStmt(*s);
        symTable_.pop();
    }
    symTable_.pop();
}

void TypeChecker::checkReturn(ReturnStmt& stmt) {
    auto retType = symTable_.currentReturnType();
    if (!retType) {
        error("'return' outside of a function", stmt.loc);
        return;
    }

    if (stmt.value) {
        auto valType = checkExpr(**stmt.value);
        if (retType->isVoid())
            error("void function must not return a value", stmt.loc);
        else if (!isAssignable(retType, valType))
            error(typeMismatch("return", retType, valType), stmt.loc);
    } else {
        if (!retType->isVoid())
            warn("non-void function '" +
                 (retType ? retType->toString() : "?") +
                 "' missing return value", stmt.loc);
    }
}

void TypeChecker::checkThrow(ThrowStmt& stmt) {
    checkExpr(*stmt.value);
    if (!symTable_.inThrowsFunction() && !symTable_.inTryBlock())
        error("'throw' used outside a 'throws' function or 'try' block", stmt.loc);
}

void TypeChecker::checkTry(TryStmt& stmt) {
    symTable_.enterTry();
    checkBlock(static_cast<BlockStmt&>(*stmt.tryBody));
    symTable_.exitTry();

    for (auto& cc : stmt.catches) {
        symTable_.push(Scope::Kind::Block);
        if (!cc.exName.empty()) {
            Symbol sym;
            sym.name      = cc.exName;
            sym.kind      = SymbolKind::Variable;
            sym.type      = resolveTypeRef(cc.exType);
            sym.definedAt = cc.body->loc;
            symTable_.define(sym);
        }
        checkBlock(static_cast<BlockStmt&>(*cc.body));
        symTable_.pop();
    }

    if (stmt.finallyBody)
        checkBlock(static_cast<BlockStmt&>(**stmt.finallyBody));
}

void TypeChecker::checkWith(WithStmt& stmt) {
    // Check initializer
    auto initType = checkExpr(*stmt.init);
    // Register the resource variable in a new scope
    symTable_.push(Scope::Kind::Block);
    Symbol sym;
    sym.name      = stmt.resName;
    sym.kind      = SymbolKind::Variable;
    sym.type      = resolveTypeRef(stmt.resType);
    sym.definedAt = stmt.loc;
    symTable_.define(sym);
    // Check body
    checkBlock(static_cast<BlockStmt&>(*stmt.body));
    symTable_.pop();
}

void TypeChecker::checkBreak(BreakStmt& stmt) {
    if (!symTable_.inLoop() && !symTable_.inSwitch())
        error("'break' outside of a loop or switch", stmt.loc);
}

void TypeChecker::checkContinue(ContinueStmt& stmt) {
    if (!symTable_.inLoop())
        error("'continue' outside of a loop", stmt.loc);
}

void TypeChecker::checkExprStmt(ExprStmt& stmt) {
    checkExpr(*stmt.expr);
}

// ═════════════════════════════════════════════════════════════════════════════
//  Expression checking  (returns the resolved type)
// ═════════════════════════════════════════════════════════════════════════════
TypeDescPtr TypeChecker::checkExpr(Expr& expr) {
    TypeDescPtr t;
    switch (expr.kind) {
    case ExprKind::IntLit:      t = checkIntLit(static_cast<IntLitExpr&>(expr));         break;
    case ExprKind::FloatLit:    t = checkFloatLit(static_cast<FloatLitExpr&>(expr));     break;
    case ExprKind::BoolLit:     t = checkBoolLit(static_cast<BoolLitExpr&>(expr));       break;
    case ExprKind::CharLit:     t = checkCharLit(static_cast<CharLitExpr&>(expr));       break;
    case ExprKind::StringLit:   t = checkStringLit(static_cast<StringLitExpr&>(expr));   break;
    case ExprKind::NullLit:     t = checkNullLit(static_cast<NullLitExpr&>(expr));       break;
    case ExprKind::Identifier:  t = checkIdent(static_cast<IdentExpr&>(expr));           break;
    case ExprKind::Unary:       t = checkUnary(static_cast<UnaryExpr&>(expr));           break;
    case ExprKind::Binary:      t = checkBinary(static_cast<BinaryExpr&>(expr));         break;
    case ExprKind::Ternary:     t = checkTernary(static_cast<TernaryExpr&>(expr));       break;
    case ExprKind::Assign:      t = checkAssign(static_cast<AssignExpr&>(expr));         break;
    case ExprKind::Call:        t = checkCall(static_cast<CallExpr&>(expr));             break;
    case ExprKind::Member:      t = checkMember(static_cast<MemberExpr&>(expr));         break;
    case ExprKind::Index:       t = checkIndex(static_cast<IndexExpr&>(expr));           break;
    case ExprKind::New:         t = checkNew(static_cast<NewExpr&>(expr));               break;
    case ExprKind::Delete:      t = checkDelete(static_cast<DeleteExpr&>(expr));         break;
    case ExprKind::TypeOf:      t = checkTypeOf(static_cast<TypeOfExpr&>(expr));         break;
    case ExprKind::InstanceOf:  t = checkInstanceOf(static_cast<InstanceOfExpr&>(expr)); break;
    case ExprKind::Cast:        t = checkCast(static_cast<CastExpr&>(expr));             break;
    case ExprKind::Lambda:      t = checkLambda(static_cast<LambdaExpr&>(expr));         break;
    case ExprKind::ArrayLit:    t = checkArrayLit(static_cast<ArrayLitExpr&>(expr));     break;
    default:                    t = TypeDesc::makeUnknown();
    }
    if (!t) t = TypeDesc::makeUnknown();
    // Cache the resolved type back into the AST node for the codegen pass
    expr.resolvedType.name = t->name;
    return t;
}

// ── Literals ──────────────────────────────────────────────────────────────────
TypeDescPtr TypeChecker::checkIntLit(IntLitExpr&)     { return TypeDesc::makeInt();    }
TypeDescPtr TypeChecker::checkFloatLit(FloatLitExpr& e) {
    const std::string& r = e.raw;
    if (!r.empty() && (r.back() == 'f' || r.back() == 'F'))
        return TypeDesc::makeFloat();
    return TypeDesc::makeDouble();
}
TypeDescPtr TypeChecker::checkBoolLit(BoolLitExpr&)   { return TypeDesc::makeBool();   }
TypeDescPtr TypeChecker::checkCharLit(CharLitExpr&)   { return TypeDesc::makeChar();   }
TypeDescPtr TypeChecker::checkStringLit(StringLitExpr&){ return TypeDesc::makeString();}
TypeDescPtr TypeChecker::checkNullLit(NullLitExpr&)   { return TypeDesc::makeNull();   }

// ── Identifier ────────────────────────────────────────────────────────────────
TypeDescPtr TypeChecker::checkIdent(IdentExpr& e) {
    if (e.name == "this") {
        if (!currentClass_) {
            error("'this' used outside of a class", e.loc);
            return TypeDesc::makeUnknown();
        }
        return TypeDesc::makeNamed(TypeKind::Class, currentClass_->name);
    }
    if (e.name == "super") {
        if (!currentClass_) {
            error("'super' used outside of a class", e.loc);
            return TypeDesc::makeUnknown();
        }
        if (!currentClass_->superClass) {
            error("'super' used in class '" + currentClass_->name +
                  "' which has no superclass", e.loc);
            return TypeDesc::makeUnknown();
        }
        return TypeDesc::makeNamed(TypeKind::Class, *currentClass_->superClass);
    }

    Symbol* sym = symTable_.lookup(e.name);
    if (!sym) {
        error("use of undeclared identifier '" + e.name + "'", e.loc);
        return TypeDesc::makeUnknown();
    }
    return sym->type;
}

// ── Unary ─────────────────────────────────────────────────────────────────────
TypeDescPtr TypeChecker::checkUnary(UnaryExpr& e) {
    auto operandType = checkExpr(*e.operand);

    switch (e.op) {
    case TokenKind::Bang:
        if (!operandType->isBool() && !operandType->isNumeric() && !operandType->isUnknown())
            error("'!' operator requires boolean or numeric operand", e.loc);
        return TypeDesc::makeBool();

    case TokenKind::Minus:
        if (!operandType->isNumeric() && !operandType->isUnknown())
            error("unary '-' requires numeric operand", e.loc);
        return operandType;

    case TokenKind::Tilde:
        if (!operandType->isInteger() && !operandType->isUnknown())
            error("'~' requires integer operand", e.loc);
        return operandType;

    case TokenKind::PlusPlus:
    case TokenKind::MinusMinus:
        if (!operandType->isNumeric() && !operandType->isUnknown())
            error("'++'/'-­-' requires numeric operand", e.loc);
        return operandType;

    case TokenKind::KW_await:
        // await just passes through the type for now
        return operandType;

    default:
        return operandType;
    }
}

// ── Binary ────────────────────────────────────────────────────────────────────
TypeDescPtr TypeChecker::checkBinary(BinaryExpr& e) {
    auto lhsType = checkExpr(*e.left);
    auto rhsType = checkExpr(*e.right);

    switch (e.op) {
    // arithmetic
    case TokenKind::Plus:
        // string + string = string
        if (lhsType->kind == TypeKind::String && rhsType->kind == TypeKind::String)
            return TypeDesc::makeString();
        // string + anything = string (concatenation)
        if (lhsType->kind == TypeKind::String || rhsType->kind == TypeKind::String)
            return TypeDesc::makeString();
        [[fallthrough]];
    case TokenKind::Minus:
    case TokenKind::Star:
    case TokenKind::Slash:
    case TokenKind::Percent:
        if (!lhsType->isNumeric() && !lhsType->isUnknown())
            error("arithmetic operator requires numeric operands, got '" +
                  lhsType->toString() + "'", e.loc);
        if (!rhsType->isNumeric() && !rhsType->isUnknown())
            error("arithmetic operator requires numeric operands, got '" +
                  rhsType->toString() + "'", e.loc);
        return widenNumeric(lhsType, rhsType);

    // bitwise
    case TokenKind::Amp:
    case TokenKind::Pipe:
    case TokenKind::Caret:
    case TokenKind::LShift:
    case TokenKind::RShift:
        if (!lhsType->isInteger() && !lhsType->isUnknown())
            error("bitwise operator requires integer operands", e.loc);
        if (!rhsType->isInteger() && !rhsType->isUnknown())
            error("bitwise operator requires integer operands", e.loc);
        return widenNumeric(lhsType, rhsType);

    // logical
    case TokenKind::AmpAmp:
    case TokenKind::PipePipe:
        if (!lhsType->isBool() && !lhsType->isNumeric() && !lhsType->isUnknown())
            error("logical operator requires boolean operands", e.loc);
        if (!rhsType->isBool() && !rhsType->isNumeric() && !rhsType->isUnknown())
            error("logical operator requires boolean operands", e.loc);
        return TypeDesc::makeBool();

    // equality
    case TokenKind::EqualEqual:
    case TokenKind::BangEqual:
        if (!isComparable(lhsType, rhsType))
            error(typeMismatch("equality comparison", lhsType, rhsType), e.loc);
        return TypeDesc::makeBool();

    // relational
    case TokenKind::Less:
    case TokenKind::LessEqual:
    case TokenKind::Greater:
    case TokenKind::GreaterEqual:
        if (!lhsType->isNumeric() && !lhsType->isUnknown())
            error("relational operator requires numeric operands", e.loc);
        if (!rhsType->isNumeric() && !rhsType->isUnknown())
            error("relational operator requires numeric operands", e.loc);
        return TypeDesc::makeBool();

    default:
        return TypeDesc::makeUnknown();
    }
}

// ── Ternary ───────────────────────────────────────────────────────────────────
TypeDescPtr TypeChecker::checkTernary(TernaryExpr& e) {
    auto condType = checkExpr(*e.cond);
    if (!condType->isBool() && !condType->isNumeric() && !condType->isUnknown())
        error("ternary condition must be boolean or numeric", e.cond->loc);

    auto thenType = checkExpr(*e.thenExpr);
    auto elseType = checkExpr(*e.elseExpr);

    if (!isAssignable(thenType, elseType) && !isAssignable(elseType, thenType))
        warn(typeMismatch("ternary branches have incompatible types", thenType, elseType),
             e.loc);

    return widenNumeric(thenType, elseType)->isUnknown() ? thenType
         : widenNumeric(thenType, elseType);
}

// ── Assignment ────────────────────────────────────────────────────────────────
TypeDescPtr TypeChecker::checkAssign(AssignExpr& e) {
    auto targetType = checkExpr(*e.target);
    auto valueType  = checkExpr(*e.value);

    // For compound assignments, verify the base operator applies
    switch (e.op) {
    case TokenKind::PlusEq:
    case TokenKind::MinusEq:
    case TokenKind::StarEq:
    case TokenKind::SlashEq:
    case TokenKind::PercentEq:
        if (!targetType->isNumeric() && !targetType->isUnknown() &&
            !(e.op == TokenKind::PlusEq && targetType->kind == TypeKind::String))
            error("compound assignment requires numeric or string target", e.loc);
        break;
    case TokenKind::AmpEq:
    case TokenKind::PipeEq:
    case TokenKind::CaretEq:
    case TokenKind::LShiftEq:
    case TokenKind::RShiftEq:
        if (!targetType->isInteger() && !targetType->isUnknown())
            error("bitwise assignment requires integer target", e.loc);
        break;
    default:
        break;
    }

    if (e.op == TokenKind::Equal && !isAssignable(targetType, valueType))
        error(typeMismatch("assignment", targetType, valueType), e.loc);

    return targetType;
}

// ── Call ──────────────────────────────────────────────────────────────────────
TypeDescPtr TypeChecker::checkCall(CallExpr& e) {
    // Resolve callee type
    TypeDescPtr calleeType = checkExpr(*e.callee);

    // Check arguments regardless
    std::vector<TypeDescPtr> argTypes;
    for (auto& a : e.args)
        argTypes.push_back(checkExpr(*a));

    // var-typed locals are closures — callable with any args, return var
    if (calleeType->kind == TypeKind::Var) {
        return TypeDesc::makeNamed(TypeKind::Var, "var");  // closure result is var
    }
    if (calleeType->kind != TypeKind::Function && !calleeType->isUnknown()) {
        error("expression is not callable (type '" + calleeType->toString() + "')", e.loc);
        return TypeDesc::makeUnknown();
    }
    if (calleeType->isUnknown()) return TypeDesc::makeUnknown();

    // Arity check
    if (argTypes.size() != calleeType->paramTypes.size()) {
        error("function called with " + std::to_string(argTypes.size()) +
              " argument(s) but expects " +
              std::to_string(calleeType->paramTypes.size()), e.loc);
    } else {
        // Type-check each argument
        for (std::size_t i = 0; i < argTypes.size(); ++i) {
            if (!isAssignable(calleeType->paramTypes[i], argTypes[i]))
                error(typeMismatch("argument " + std::to_string(i+1),
                                   calleeType->paramTypes[i], argTypes[i]), e.loc);
        }
    }

    return calleeType->returnType ? calleeType->returnType : TypeDesc::makeVoid();
}

// ── Member access ─────────────────────────────────────────────────────────────
TypeDescPtr TypeChecker::checkMember(MemberExpr& e) {
    // Handle scope resolution  Direction::NORTH  (stored as member "::NORTH")
    if (e.member.size() > 2 && e.member[0] == ':' && e.member[1] == ':') {
        auto objType = checkExpr(*e.object);
        std::string memberName = e.member.substr(2);
        return lookupScopedName(objType->name, memberName, e.loc);
    }

    auto objType = checkExpr(*e.object);

    // String pseudo-fields: .length  .size
    if (objType->kind == TypeKind::String) {
        if (e.member == "length" || e.member == "size")
            return TypeDesc::makeInt();
    }

    // Array pseudo-fields: .length  .size
    if (objType->isArray) {
        if (e.member == "length" || e.member == "size")
            return TypeDesc::makeInt();
    }

    return lookupMember(objType, e.member, e.loc);
}

TypeDescPtr TypeChecker::lookupMember(const TypeDescPtr& objType,
                                      const std::string& memberName,
                                      SourceLocation loc) {
    if (objType->isUnknown()) return TypeDesc::makeUnknown();

    // Look up the class/struct/union declaration
    Symbol* typeSym = symTable_.lookup(objType->name);
    if (!typeSym) {
        error("type '" + objType->toString() + "' is not defined", loc);
        return TypeDesc::makeUnknown();
    }

    // Class member lookup
    if (typeSym->classDecl) {
        const ClassDecl* cls = typeSym->classDecl;
        // Check fields
        for (const auto& f : cls->fields)
            if (f.name == memberName) return resolveTypeRef(f.type);
        // Check methods — return the full function type so calls type-check correctly
        for (const auto& m : cls->methods) {
            if (m->name == memberName) {
                auto fnType        = std::make_shared<TypeDesc>();
                fnType->kind       = TypeKind::Function;
                fnType->name       = "fun " + m->name;
                fnType->returnType = resolveTypeRef(m->returnType);
                for (const auto& p : m->params)
                    fnType->paramTypes.push_back(resolveTypeRef(p.type));
                return fnType;
            }
        }
        // Try superclass
        if (cls->superClass) {
            auto superType = TypeDesc::makeNamed(TypeKind::Class, *cls->superClass);
            return lookupMember(superType, memberName, loc);
        }
        error("'" + objType->name + "' has no member '" + memberName + "'", loc);
        return TypeDesc::makeUnknown();
    }

    // Struct member lookup
    if (typeSym->structDecl) {
        for (const auto& f : typeSym->structDecl->fields)
            if (f.name == memberName) return resolveTypeRef(f.type);
        error("struct '" + objType->name + "' has no field '" + memberName + "'", loc);
        return TypeDesc::makeUnknown();
    }

    // Union member lookup
    if (typeSym->unionDecl) {
        for (const auto& f : typeSym->unionDecl->fields)
            if (f.name == memberName) return resolveTypeRef(f.type);
        error("union '" + objType->name + "' has no field '" + memberName + "'", loc);
        return TypeDesc::makeUnknown();
    }

    error("cannot access member '" + memberName +
          "' on non-composite type '" + objType->toString() + "'", loc);
    return TypeDesc::makeUnknown();
}

TypeDescPtr TypeChecker::lookupScopedName(const std::string& scope,
                                          const std::string& member,
                                          SourceLocation loc) {
    // Look up "EnumName::MemberName"
    Symbol* sym = symTable_.lookup(scope + "::" + member);
    if (sym) return sym->type;

    // Fall back to plain member
    sym = symTable_.lookup(member);
    if (sym) return sym->type;

    error("'" + scope + "' has no member '" + member + "'", loc);
    return TypeDesc::makeUnknown();
}

// ── Index ─────────────────────────────────────────────────────────────────────
TypeDescPtr TypeChecker::checkIndex(IndexExpr& e) {
    auto arrType = checkExpr(*e.array);
    auto idxType = checkExpr(*e.index);

    if (!idxType->isInteger() && !idxType->isUnknown())
        error("array index must be an integer type, got '" +
              idxType->toString() + "'", e.loc);

    if (arrType->kind == TypeKind::Array && arrType->elementType)
        return arrType->elementType;

    if (arrType->kind == TypeKind::String) return TypeDesc::makeChar();

    if (!arrType->isUnknown())
        error("subscript operator applied to non-array type '" +
              arrType->toString() + "'", e.loc);

    return TypeDesc::makeUnknown();
}

// ── New ───────────────────────────────────────────────────────────────────────
TypeDescPtr TypeChecker::checkNew(NewExpr& e) {
    // Array allocation:  new int[n]
    if (e.isArray) {
        if (e.arraySize) {
            auto szType = checkExpr(*e.arraySize);
            if (!szType->isInteger() && !szType->isUnknown())
                error("array size must be an integer, got '" +
                      szType->toString() + "'", e.loc);
        }
        // Return an array type with the element type
        auto elemType = resolveTypeRef(e.type);
        auto arrType  = std::make_shared<TypeDesc>();
        arrType->kind        = TypeKind::Array;
        arrType->name        = e.type.name + "[]";
        arrType->isArray     = true;
        arrType->elementType = elemType;
        return arrType;
    }

    auto t = resolveTypeRef(e.type);

    // Verify the type is a known class
    Symbol* sym = symTable_.lookup(e.type.name);
    if (!sym || sym->kind != SymbolKind::Class) {
        if (!t->isUnknown())
            error("'new' used with non-class type '" + e.type.name + "'", e.loc);
    } else {
        // Find the constructor (init method) and check args
        const ClassDecl* cls = sym->classDecl;
        if (cls) {
            for (const auto& m : cls->methods) {
                if (m->name == "init") {
                    if (e.args.size() != m->params.size()) {
                        error("constructor for '" + e.type.name + "' expects " +
                              std::to_string(m->params.size()) + " argument(s), got " +
                              std::to_string(e.args.size()), e.loc);
                    } else {
                        for (std::size_t i = 0; i < e.args.size(); ++i) {
                            auto argType = checkExpr(*e.args[i]);
                            auto paramType = resolveTypeRef(m->params[i].type);
                            if (!isAssignable(paramType, argType))
                                error(typeMismatch("constructor argument " +
                                      std::to_string(i+1), paramType, argType), e.loc);
                        }
                    }
                    return t;
                }
            }
        }
        // No init found — check args are empty
        if (!e.args.empty()) {
            for (auto& a : e.args) checkExpr(*a);
        }
    }
    return t;
}

// ── Delete ────────────────────────────────────────────────────────────────────
TypeDescPtr TypeChecker::checkDelete(DeleteExpr& e) {
    auto t = checkExpr(*e.operand);
    if (t->kind != TypeKind::Class && t->kind != TypeKind::Pointer &&
        !t->isUnknown())
        error("'delete' applied to non-pointer/non-class type '" +
              t->toString() + "'", e.loc);
    return TypeDesc::makeVoid();
}

// ── TypeOf ────────────────────────────────────────────────────────────────────
TypeDescPtr TypeChecker::checkTypeOf(TypeOfExpr& e) {
    checkExpr(*e.operand); // check operand but ignore its type
    return TypeDesc::makeString(); // typeof returns a string type name
}

// ── InstanceOf ────────────────────────────────────────────────────────────────
TypeDescPtr TypeChecker::checkInstanceOf(InstanceOfExpr& e) {
    checkExpr(*e.operand);
    auto t = resolveTypeRef(e.type);
    if (t->isUnknown())
        error("unknown type in 'instanceof'", e.loc);
    return TypeDesc::makeBool();
}

// ── Cast  expr as Type ────────────────────────────────────────────────────────
TypeDescPtr TypeChecker::checkCast(CastExpr& e) {
    checkExpr(*e.operand);
    auto targetType = resolveTypeRef(e.type);
    // Any cast between class types is allowed (checked at runtime via instanceof)
    // Numeric casts are also allowed.
    return targetType;
}

// ── Lambda ────────────────────────────────────────────────────────────────────
TypeDescPtr TypeChecker::checkLambda(LambdaExpr& e) {
    symTable_.push(Scope::Kind::Function);

    for (const auto& p : e.params) {
        auto t = resolveTypeRef(p.type);
        Symbol sym;
        sym.name      = p.name;
        sym.kind      = SymbolKind::Parameter;
        sym.type      = t;
        sym.definedAt = p.loc;
        symTable_.define(sym);
    }

    TypeDescPtr retType;
    if (e.body)      retType = checkExpr(*e.body);
    if (e.blockBody) {
        // Push an unknown return type so the lambda body's `return` statements
        // are validated in isolation from the enclosing function's return type.
        auto unknownRet = TypeDesc::makeUnknown();
        symTable_.setReturnType(unknownRet);
        checkStmt(*e.blockBody);
    }

    symTable_.pop();

    // Build a function type descriptor for the lambda
    auto fnType = std::make_shared<TypeDesc>();
    fnType->kind       = TypeKind::Function;
    fnType->name       = "lambda";
    fnType->returnType = retType ? retType : TypeDesc::makeVoid();
    for (const auto& p : e.params)
        fnType->paramTypes.push_back(resolveTypeRef(p.type));

    return fnType;
}

// ── Array literal  { e, e, ... } ─────────────────────────────────────────────
TypeDescPtr TypeChecker::checkArrayLit(ArrayLitExpr& e) {
    if (e.elements.empty()) {
        // Empty literal — return unknown array type
        auto arrType = std::make_shared<TypeDesc>();
        arrType->kind    = TypeKind::Array;
        arrType->name    = "<unknown>[]";
        arrType->isArray = true;
        arrType->elementType = TypeDesc::makeUnknown();
        return arrType;
    }

    // Check all elements; element type is the type of the first element
    TypeDescPtr elemType = checkExpr(*e.elements[0]);
    for (std::size_t i = 1; i < e.elements.size(); ++i) {
        auto et = checkExpr(*e.elements[i]);
        if (!isAssignable(elemType, et) && !et->isUnknown())
            warn("array literal element " + std::to_string(i) +
                 " type '" + et->toString() + "' differs from first element type '" +
                 elemType->toString() + "'", e.loc);
    }

    auto arrType = std::make_shared<TypeDesc>();
    arrType->kind        = TypeKind::Array;
    arrType->name        = elemType->name + "[]";
    arrType->isArray     = true;
    arrType->elementType = elemType;
    return arrType;
}
