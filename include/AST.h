#pragma once
//===----------------------------------------------------------------------===//
//  AST.h  –  Abstract Syntax Tree node definitions
//===----------------------------------------------------------------------===//
#ifndef AST_H
#define AST_H

#include "Lexer.h"

#include <memory>
#include <string>
#include <vector>
#include <optional>

// ─────────────────────────────────────────────────────────────────────────────
//  Forward declarations
// ─────────────────────────────────────────────────────────────────────────────
struct Expr;
struct Stmt;
struct Decl;

using ExprPtr = std::unique_ptr<Expr>;
using StmtPtr = std::unique_ptr<Stmt>;
using DeclPtr = std::unique_ptr<Decl>;

// ─────────────────────────────────────────────────────────────────────────────
//  TypeRef  –  a parsed type annotation  (int, string, bool[], MyClass, void)
// ─────────────────────────────────────────────────────────────────────────────
struct TypeRef {
    std::string    name;        // base name  e.g. "int", "string", "Dog"
    bool           isArray = false;
    bool           isPointer = false;
    SourceLocation loc;
};

// ─────────────────────────────────────────────────────────────────────────────
//  Parameter  –  single function parameter
// ─────────────────────────────────────────────────────────────────────────────
struct Parameter {
    TypeRef        type;
    std::string    name;
    SourceLocation loc;
};

// ═════════════════════════════════════════════════════════════════════════════
//  Expression nodes
// ═════════════════════════════════════════════════════════════════════════════
enum class ExprKind {
    // literals
    IntLit, FloatLit, BoolLit, CharLit, StringLit, NullLit,

    // name
    Identifier,

    // unary / binary / ternary
    Unary, Binary, Ternary,

    // assignment  (=  +=  -=  …)
    Assign,

    // call  f(args)
    Call,

    // member access  obj.field  obj->field
    Member,

    // subscript  arr[idx]
    Index,

    // object creation  new Type(args)
    New,

    // delete expr
    Delete,

    // typeof expr
    TypeOf,

    // cast  (Type) expr   or  expr as Type
    Cast,

    // lambda / fat-arrow   (params) => expr
    Lambda,

    // instanceof
    InstanceOf,

    // array literal  { expr, expr, ... }
    ArrayLit,
};

struct Expr {
    ExprKind       kind;
    SourceLocation loc;
    TypeRef        resolvedType; // filled by type-checker later

    virtual ~Expr() = default;

protected:
    explicit Expr(ExprKind k, SourceLocation l) : kind(k), loc(l) {}
};

// ── Literal ──────────────────────────────────────────────────────────────────
struct IntLitExpr : Expr {
    std::string raw;  // keep raw text for hex/bin/oct
    int64_t     value = 0;
    IntLitExpr(std::string r, int64_t v, SourceLocation l)
        : Expr(ExprKind::IntLit, l), raw(std::move(r)), value(v) {}
};

struct FloatLitExpr : Expr {
    std::string raw;
    double      value = 0.0;
    FloatLitExpr(std::string r, double v, SourceLocation l)
        : Expr(ExprKind::FloatLit, l), raw(std::move(r)), value(v) {}
};

struct BoolLitExpr : Expr {
    bool value;
    BoolLitExpr(bool v, SourceLocation l)
        : Expr(ExprKind::BoolLit, l), value(v) {}
};

struct CharLitExpr : Expr {
    std::string raw;  // e.g. "'A'"
    CharLitExpr(std::string r, SourceLocation l)
        : Expr(ExprKind::CharLit, l), raw(std::move(r)) {}
};

struct StringLitExpr : Expr {
    std::string value; // content without quotes, escapes resolved
    StringLitExpr(std::string v, SourceLocation l)
        : Expr(ExprKind::StringLit, l), value(std::move(v)) {}
};

struct NullLitExpr : Expr {
    explicit NullLitExpr(SourceLocation l) : Expr(ExprKind::NullLit, l) {}
};

// ── Identifier ───────────────────────────────────────────────────────────────
struct IdentExpr : Expr {
    std::string name;
    IdentExpr(std::string n, SourceLocation l)
        : Expr(ExprKind::Identifier, l), name(std::move(n)) {}
};

// ── Unary ────────────────────────────────────────────────────────────────────
struct UnaryExpr : Expr {
    TokenKind op;
    bool      postfix; // true → expr++  false → ++expr
    ExprPtr   operand;
    UnaryExpr(TokenKind op, bool post, ExprPtr operand, SourceLocation l)
        : Expr(ExprKind::Unary, l), op(op), postfix(post),
          operand(std::move(operand)) {}
};

// ── Binary ───────────────────────────────────────────────────────────────────
struct BinaryExpr : Expr {
    TokenKind op;
    ExprPtr   left;
    ExprPtr   right;
    BinaryExpr(TokenKind op, ExprPtr l, ExprPtr r, SourceLocation loc)
        : Expr(ExprKind::Binary, loc), op(op),
          left(std::move(l)), right(std::move(r)) {}
};

// ── Ternary  cond ? then : else ───────────────────────────────────────────────
struct TernaryExpr : Expr {
    ExprPtr cond;
    ExprPtr thenExpr;
    ExprPtr elseExpr;
    TernaryExpr(ExprPtr c, ExprPtr t, ExprPtr e, SourceLocation l)
        : Expr(ExprKind::Ternary, l),
          cond(std::move(c)), thenExpr(std::move(t)), elseExpr(std::move(e)) {}
};

// ── Assignment ───────────────────────────────────────────────────────────────
struct AssignExpr : Expr {
    TokenKind op;   // Equal / PlusEq / MinusEq …
    ExprPtr   target;
    ExprPtr   value;
    AssignExpr(TokenKind op, ExprPtr tgt, ExprPtr val, SourceLocation l)
        : Expr(ExprKind::Assign, l), op(op),
          target(std::move(tgt)), value(std::move(val)) {}
};

// ── Call  f(args) ─────────────────────────────────────────────────────────────
struct CallExpr : Expr {
    ExprPtr              callee;
    std::vector<ExprPtr> args;
    CallExpr(ExprPtr callee, std::vector<ExprPtr> args, SourceLocation l)
        : Expr(ExprKind::Call, l),
          callee(std::move(callee)), args(std::move(args)) {}
};

// ── Member access  obj.field  obj->field ─────────────────────────────────────
struct MemberExpr : Expr {
    ExprPtr    object;
    std::string member;
    bool        arrow; // true → ->  false → .
    MemberExpr(ExprPtr obj, std::string m, bool arrow, SourceLocation l)
        : Expr(ExprKind::Member, l),
          object(std::move(obj)), member(std::move(m)), arrow(arrow) {}
};

// ── Index  arr[idx] ───────────────────────────────────────────────────────────
struct IndexExpr : Expr {
    ExprPtr array;
    ExprPtr index;
    IndexExpr(ExprPtr arr, ExprPtr idx, SourceLocation l)
        : Expr(ExprKind::Index, l),
          array(std::move(arr)), index(std::move(idx)) {}
};

// ── New  new Type(args) ───────────────────────────────────────────────────────
struct NewExpr : Expr {
    TypeRef              type;
    std::vector<ExprPtr> args;
    ExprPtr              arraySize;  // non-null for  new int[n]
    bool                 isArray = false;
    NewExpr(TypeRef t, std::vector<ExprPtr> a, SourceLocation l)
        : Expr(ExprKind::New, l), type(std::move(t)), args(std::move(a)) {}
    NewExpr(TypeRef t, ExprPtr sz, SourceLocation l)
        : Expr(ExprKind::New, l), type(std::move(t)),
          arraySize(std::move(sz)), isArray(true) {}
};

// ── Array literal  { expr, expr, ... } ────────────────────────────────────────
struct ArrayLitExpr : Expr {
    std::vector<ExprPtr> elements;
    explicit ArrayLitExpr(std::vector<ExprPtr> elems, SourceLocation l)
        : Expr(ExprKind::ArrayLit, l), elements(std::move(elems)) {}
};

// ── Delete ────────────────────────────────────────────────────────────────────
struct DeleteExpr : Expr {
    ExprPtr operand;
    DeleteExpr(ExprPtr op, SourceLocation l)
        : Expr(ExprKind::Delete, l), operand(std::move(op)) {}
};

// ── TypeOf ────────────────────────────────────────────────────────────────────
struct TypeOfExpr : Expr {
    ExprPtr operand;
    TypeOfExpr(ExprPtr op, SourceLocation l)
        : Expr(ExprKind::TypeOf, l), operand(std::move(op)) {}
};

// ── InstanceOf ────────────────────────────────────────────────────────────────
struct InstanceOfExpr : Expr {
    ExprPtr operand;
    TypeRef type;
    InstanceOfExpr(ExprPtr op, TypeRef t, SourceLocation l)
        : Expr(ExprKind::InstanceOf, l),
          operand(std::move(op)), type(std::move(t)) {}
};

// ── Cast  (TargetType) expr ───────────────────────────────────────────────────
struct CastExpr : Expr {
    TypeRef type;
    ExprPtr operand;
    CastExpr(TypeRef t, ExprPtr op, SourceLocation l)
        : Expr(ExprKind::Cast, l), type(std::move(t)), operand(std::move(op)) {}
};

// ── Lambda  (int x, int y) => expr ───────────────────────────────────────────
struct LambdaExpr : Expr {
    std::vector<Parameter>   params;
    ExprPtr                  body;       // expression body (or null)
    StmtPtr                  blockBody;  // block body (or null)
    // Capture analysis result (filled in by CodeGen)
    std::vector<std::string> captures;   // names of captured outer variables
    LambdaExpr(std::vector<Parameter> p, ExprPtr b, SourceLocation l)
        : Expr(ExprKind::Lambda, l),
          params(std::move(p)), body(std::move(b)) {}
    LambdaExpr(std::vector<Parameter> p, StmtPtr b, SourceLocation l)
        : Expr(ExprKind::Lambda, l),
          params(std::move(p)), blockBody(std::move(b)) {}
};

// ═════════════════════════════════════════════════════════════════════════════
//  Statement nodes
// ═════════════════════════════════════════════════════════════════════════════
enum class StmtKind {
    Block, Expression, VarDecl,
    If, While, DoWhile, For,
    Switch, Return, Break, Continue,
    Throw, Try,
    With,    // with (Type name = expr) { body }
};

struct Stmt {
    StmtKind       kind;
    SourceLocation loc;
    virtual ~Stmt() = default;
protected:
    Stmt(StmtKind k, SourceLocation l) : kind(k), loc(l) {}
};

// ── Block  { stmts… } ─────────────────────────────────────────────────────────
struct BlockStmt : Stmt {
    std::vector<StmtPtr> stmts;
    BlockStmt(std::vector<StmtPtr> s, SourceLocation l)
        : Stmt(StmtKind::Block, l), stmts(std::move(s)) {}
};

// ── Expression statement ──────────────────────────────────────────────────────
struct ExprStmt : Stmt {
    ExprPtr expr;
    ExprStmt(ExprPtr e, SourceLocation l)
        : Stmt(StmtKind::Expression, l), expr(std::move(e)) {}
};

// ── Variable declaration statement  type name = init; ────────────────────────
struct VarDeclStmt : Stmt {
    TypeRef                type;
    std::string            name;
    std::optional<ExprPtr> init;
    bool                   isConst  = false;
    bool                   isStatic = false;
    VarDeclStmt(TypeRef t, std::string n, std::optional<ExprPtr> i,
                bool c, bool s, SourceLocation l)
        : Stmt(StmtKind::VarDecl, l),
          type(std::move(t)), name(std::move(n)), init(std::move(i)),
          isConst(c), isStatic(s) {}
};

// ── If ────────────────────────────────────────────────────────────────────────
struct IfStmt : Stmt {
    ExprPtr              cond;
    StmtPtr              thenBranch;
    std::optional<StmtPtr> elseBranch;
    IfStmt(ExprPtr c, StmtPtr t, std::optional<StmtPtr> e, SourceLocation l)
        : Stmt(StmtKind::If, l),
          cond(std::move(c)), thenBranch(std::move(t)),
          elseBranch(std::move(e)) {}
};

// ── While ─────────────────────────────────────────────────────────────────────
struct WhileStmt : Stmt {
    ExprPtr cond;
    StmtPtr body;
    WhileStmt(ExprPtr c, StmtPtr b, SourceLocation l)
        : Stmt(StmtKind::While, l), cond(std::move(c)), body(std::move(b)) {}
};

// ── Do-While ──────────────────────────────────────────────────────────────────
struct DoWhileStmt : Stmt {
    StmtPtr body;
    ExprPtr cond;
    DoWhileStmt(StmtPtr b, ExprPtr c, SourceLocation l)
        : Stmt(StmtKind::DoWhile, l), body(std::move(b)), cond(std::move(c)) {}
};

// ── For ───────────────────────────────────────────────────────────────────────
struct ForStmt : Stmt {
    std::optional<StmtPtr> init;   // VarDeclStmt or ExprStmt
    std::optional<ExprPtr> cond;
    std::optional<ExprPtr> incr;
    StmtPtr                body;
    ForStmt(std::optional<StmtPtr> i, std::optional<ExprPtr> c,
            std::optional<ExprPtr> inc, StmtPtr b, SourceLocation l)
        : Stmt(StmtKind::For, l),
          init(std::move(i)), cond(std::move(c)),
          incr(std::move(inc)), body(std::move(b)) {}
};

// ── Switch ────────────────────────────────────────────────────────────────────
struct CaseClause {
    std::optional<ExprPtr> value;  // nullopt → default
    std::vector<StmtPtr>   stmts;
};

struct SwitchStmt : Stmt {
    ExprPtr                 subject;
    std::vector<CaseClause> cases;
    SwitchStmt(ExprPtr s, std::vector<CaseClause> c, SourceLocation l)
        : Stmt(StmtKind::Switch, l),
          subject(std::move(s)), cases(std::move(c)) {}
};

// ── Return ────────────────────────────────────────────────────────────────────
struct ReturnStmt : Stmt {
    std::optional<ExprPtr> value;
    ReturnStmt(std::optional<ExprPtr> v, SourceLocation l)
        : Stmt(StmtKind::Return, l), value(std::move(v)) {}
};

// ── Break / Continue ──────────────────────────────────────────────────────────
struct BreakStmt : Stmt {
    explicit BreakStmt(SourceLocation l) : Stmt(StmtKind::Break, l) {}
};
struct ContinueStmt : Stmt {
    explicit ContinueStmt(SourceLocation l) : Stmt(StmtKind::Continue, l) {}
};

// ── Throw ─────────────────────────────────────────────────────────────────────
struct ThrowStmt : Stmt {
    ExprPtr value;
    ThrowStmt(ExprPtr v, SourceLocation l)
        : Stmt(StmtKind::Throw, l), value(std::move(v)) {}
};

// ── Try / Catch ───────────────────────────────────────────────────────────────
struct CatchClause {
    TypeRef     exType;
    std::string exName;
    StmtPtr     body;
};

struct TryStmt : Stmt {
    StmtPtr                  tryBody;
    std::vector<CatchClause> catches;
    std::optional<StmtPtr>   finallyBody;
    TryStmt(StmtPtr tb, std::vector<CatchClause> c,
            std::optional<StmtPtr> f, SourceLocation l)
        : Stmt(StmtKind::Try, l),
          tryBody(std::move(tb)), catches(std::move(c)),
          finallyBody(std::move(f)) {}
};

// ═════════════════════════════════════════════════════════════════════════════
//  Declaration nodes  (top-level and class-member level)
// ═════════════════════════════════════════════════════════════════════════════
enum class DeclKind {
    Import, Enum, Struct, Union, Class, Function, Variable,
    Using,   // using Name = Type
};

struct Decl {
    DeclKind       kind;
    SourceLocation loc;
    virtual ~Decl() = default;
protected:
    Decl(DeclKind k, SourceLocation l) : kind(k), loc(l) {}
};

// ── Import ────────────────────────────────────────────────────────────────────
struct ImportDecl : Decl {
    std::string path;   // the string literal value
    ImportDecl(std::string p, SourceLocation l)
        : Decl(DeclKind::Import, l), path(std::move(p)) {}
};

// ── Enum ──────────────────────────────────────────────────────────────────────
struct EnumDecl : Decl {
    std::string              name;
    std::vector<std::string> members;
    EnumDecl(std::string n, std::vector<std::string> m, SourceLocation l)
        : Decl(DeclKind::Enum, l), name(std::move(n)), members(std::move(m)) {}
};

// ── Struct ────────────────────────────────────────────────────────────────────
struct FieldDecl {
    TypeRef     type;
    std::string name;
    SourceLocation loc;
};

struct StructDecl : Decl {
    std::string            name;
    std::vector<FieldDecl> fields;
    StructDecl(std::string n, std::vector<FieldDecl> f, SourceLocation l)
        : Decl(DeclKind::Struct, l), name(std::move(n)), fields(std::move(f)) {}
};

// ── Union ─────────────────────────────────────────────────────────────────────
struct UnionDecl : Decl {
    std::string            name;
    std::vector<FieldDecl> fields;
    UnionDecl(std::string n, std::vector<FieldDecl> f, SourceLocation l)
        : Decl(DeclKind::Union, l), name(std::move(n)), fields(std::move(f)) {}
};

// ── Function ──────────────────────────────────────────────────────────────────
enum class AccessMod { None, Public, Private, Protected };

struct FunctionDecl : Decl {
    AccessMod              access   = AccessMod::None;
    bool                   isAsync  = false;
    bool                   isStatic = false;
    bool                   isFinal  = false;
    bool                   throws   = false;
    bool                   isExport = false;   // export keyword
    TypeRef                returnType;
    std::string            name;
    std::vector<Parameter> params;
    std::unique_ptr<BlockStmt> body;   // nullopt = abstract / forward decl

    FunctionDecl(SourceLocation l) : Decl(DeclKind::Function, l) {}
};

// ── Class member variable ─────────────────────────────────────────────────────
struct MemberVarDecl {
    AccessMod              access = AccessMod::None;
    bool                   isStatic = false;
    bool                   isConst  = false;
    TypeRef                type;
    std::string            name;
    std::optional<ExprPtr> init;
    SourceLocation         loc;
};

// ── Class ─────────────────────────────────────────────────────────────────────
struct ClassDecl : Decl {
    std::string                      name;
    std::optional<std::string>       superClass;
    bool                             isFinal  = false;
    bool                             isExport = false;
    std::vector<MemberVarDecl>       fields;
    std::vector<std::unique_ptr<FunctionDecl>> methods;

    ClassDecl(SourceLocation l) : Decl(DeclKind::Class, l) {}
};

// ── Top-level variable ────────────────────────────────────────────────────────
struct VarDecl : Decl {
    bool                   isConst  = false;
    bool                   isStatic = false;
    bool                   isExport = false;
    TypeRef                type;
    std::string            name;
    std::optional<ExprPtr> init;
    VarDecl(SourceLocation l) : Decl(DeclKind::Variable, l) {}
};

// ─────────────────────────────────────────────────────────────────────────────
//  Program  –  root of the AST
// ─────────────────────────────────────────────────────────────────────────────
struct Program {
    std::vector<DeclPtr> declarations;
};


// ── Using  (type alias)  ───────────────────────────────────────────────────────
// using Name = ExistingType;
struct UsingDecl : Decl {
    std::string alias;      // new name
    TypeRef     target;     // existing type
    bool        isExport = false;
    UsingDecl(std::string a, TypeRef t, SourceLocation l)
        : Decl(DeclKind::Using, l), alias(std::move(a)), target(std::move(t)) {}
};

// ── With  (scoped resource block)  ────────────────────────────────────────────
// with (Type name = init) { body }
// Desugars to:  Type name = init; try { body } finally { name.close(); }
struct WithStmt : Stmt {
    TypeRef    resType;
    std::string resName;
    ExprPtr    init;
    StmtPtr    body;
    WithStmt(TypeRef t, std::string n, ExprPtr i, StmtPtr b, SourceLocation l)
        : Stmt(StmtKind::With, l),
          resType(std::move(t)), resName(std::move(n)),
          init(std::move(i)), body(std::move(b)) {}
};

#endif // AST_H
