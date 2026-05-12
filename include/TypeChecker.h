#pragma once
//===----------------------------------------------------------------------===//
//  TypeChecker.h  –  Two-pass AST type checker
//===----------------------------------------------------------------------===//
#ifndef TYPE_CHECKER_H
#define TYPE_CHECKER_H

#include "AST.h"
#include "SymbolTable.h"

#include <string>
#include <vector>
#include <memory>
#include <unordered_set>

// ─────────────────────────────────────────────────────────────────────────────
//  TypeError  –  a single type-check diagnostic
// ─────────────────────────────────────────────────────────────────────────────
struct TypeError {
    enum class Severity { Error, Warning };

    std::string message;
    SourceLocation loc;
    Severity severity = Severity::Error;

    std::string format() const {
        std::string s = loc.filename + ":" + std::to_string(loc.line) + ":"
                      + std::to_string(loc.column) + ": ";
        s += (severity == Severity::Error) ? "error: " : "warning: ";
        s += message;
        return s;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
//  TypeChecker
// ─────────────────────────────────────────────────────────────────────────────
class TypeChecker {
public:
    explicit TypeChecker(std::string filename = "<input>");

    /// Tell the type checker that this import path was resolved from a real
    /// source file and merged into the program — so don't warn about it.
    void registerResolvedImport(const std::string& importPath) {
        resolvedImports_.insert(importPath);
    }

    /// Run both passes over the program.
    void check(Program& program);

    const std::vector<TypeError>& errors()   const { return errors_; }
    bool hadError() const {
        for (const auto& e : errors_)
            if (e.severity == TypeError::Severity::Error) return true;
        return false;
    }

private:
    // ── Error emission ───────────────────────────────────────────────────────
    void error(const std::string& msg, SourceLocation loc);
    void warn (const std::string& msg, SourceLocation loc);

    // ── Type resolution ──────────────────────────────────────────────────────
    TypeDescPtr resolveTypeRef(const TypeRef& ref);
    TypeDescPtr builtinType(const std::string& name);

    // ── Type compatibility ───────────────────────────────────────────────────
    bool isAssignable(const TypeDescPtr& target, const TypeDescPtr& value);
    bool isComparable(const TypeDescPtr& a, const TypeDescPtr& b);
    bool isNumericCompatible(const TypeDescPtr& a, const TypeDescPtr& b);
    TypeDescPtr widenNumeric(const TypeDescPtr& a, const TypeDescPtr& b);
    std::string typeMismatch(const std::string& ctx,
                             const TypeDescPtr& expected,
                             const TypeDescPtr& got);

    // ── Library import resolver ──────────────────────────────────────────────
    void registerLibrary(const std::string& path, SourceLocation loc);
    void registerFn(const std::string& name, TypeDescPtr ret,
                    std::vector<TypeDescPtr> params = {});

    // ── Pass 1: Hoist declarations ───────────────────────────────────────────
    void hoistDecl(const Decl& decl);
    void hoistClass(const ClassDecl& decl);
    void hoistFunction(const FunctionDecl& decl, const std::string& ownerClass = "");

    // ── Pass 2: Check declarations ───────────────────────────────────────────
    void checkDecl(Decl& decl);
    void checkImport(ImportDecl& decl);
    void checkEnum(EnumDecl& decl);
    void checkStruct(StructDecl& decl);
    void checkUnion(UnionDecl& decl);
    void checkClass(ClassDecl& decl);
    void checkFunction(FunctionDecl& decl);
    void checkTopLevelVar(VarDecl& decl);

    // ── Statements ───────────────────────────────────────────────────────────
    void checkStmt(Stmt& stmt);
    void checkBlock(BlockStmt& stmt);
    void checkVarDeclStmt(VarDeclStmt& stmt);
    void checkIf(IfStmt& stmt);
    void checkWhile(WhileStmt& stmt);
    void checkDoWhile(DoWhileStmt& stmt);
    void checkFor(ForStmt& stmt);
    void checkSwitch(SwitchStmt& stmt);
    void checkReturn(ReturnStmt& stmt);
    void checkThrow(ThrowStmt& stmt);
    void checkTry(TryStmt& stmt);
    void checkWith(WithStmt& stmt);
    void checkBreak(BreakStmt& stmt);
    void checkContinue(ContinueStmt& stmt);
    void checkExprStmt(ExprStmt& stmt);

    // ── Expressions (return resolved type) ───────────────────────────────────
    TypeDescPtr checkExpr(Expr& expr);
    TypeDescPtr checkIntLit(IntLitExpr& e);
    TypeDescPtr checkFloatLit(FloatLitExpr& e);
    TypeDescPtr checkBoolLit(BoolLitExpr& e);
    TypeDescPtr checkCharLit(CharLitExpr& e);
    TypeDescPtr checkStringLit(StringLitExpr& e);
    TypeDescPtr checkNullLit(NullLitExpr& e);
    TypeDescPtr checkIdent(IdentExpr& e);
    TypeDescPtr checkUnary(UnaryExpr& e);
    TypeDescPtr checkBinary(BinaryExpr& e);
    TypeDescPtr checkTernary(TernaryExpr& e);
    TypeDescPtr checkAssign(AssignExpr& e);
    TypeDescPtr checkCall(CallExpr& e);
    TypeDescPtr checkMember(MemberExpr& e);
    TypeDescPtr checkIndex(IndexExpr& e);
    TypeDescPtr checkNew(NewExpr& e);
    TypeDescPtr checkDelete(DeleteExpr& e);
    TypeDescPtr checkArrayLit(ArrayLitExpr& e);
    TypeDescPtr checkTypeOf(TypeOfExpr& e);
    TypeDescPtr checkInstanceOf(InstanceOfExpr& e);
    TypeDescPtr checkCast(CastExpr& e);
    TypeDescPtr checkLambda(LambdaExpr& e);

    // ── Member lookup helpers ────────────────────────────────────────────────
    TypeDescPtr lookupMember(const TypeDescPtr& objType,
                             const std::string& memberName,
                             SourceLocation loc);
    TypeDescPtr lookupScopedName(const std::string& scope,
                                 const std::string& member,
                                 SourceLocation loc);

    // ── State ────────────────────────────────────────────────────────────────
    SymbolTable            symTable_;
    std::vector<TypeError> errors_;
    std::string            filename_;
    // Import paths resolved from source files (no warning needed for these)
    std::unordered_set<std::string> resolvedImports_;

    // Class context for method checking
    const ClassDecl*  currentClass_ = nullptr;
};

#endif // TYPE_CHECKER_H
