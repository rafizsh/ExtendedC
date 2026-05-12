#pragma once
//===----------------------------------------------------------------------===//
//  ASTPrinter.h  –  Debug pretty-printer for the AST
//===----------------------------------------------------------------------===//
#ifndef AST_PRINTER_H
#define AST_PRINTER_H

#include "AST.h"
#include <ostream>
#include <string>

class ASTPrinter {
public:
    explicit ASTPrinter(std::ostream& out) : out_(out) {}

    void print(const Program& program) {
        out_ << "Program\n";
        for (const auto& d : program.declarations)
            printDecl(*d, 1);
    }

private:
    std::ostream& out_;

    std::string ind(int n) { return std::string(n * 2, ' '); }

    // ── Access modifier string ────────────────────────────────────────────────
    static const char* accessStr(AccessMod a) {
        switch (a) {
        case AccessMod::Public:    return "public";
        case AccessMod::Private:   return "private";
        case AccessMod::Protected: return "protected";
        default:                   return "";
        }
    }

    static std::string typeStr(const TypeRef& t) {
        std::string s = t.name;
        if (t.isArray)   s += "[]";
        if (t.isPointer) s += "*";
        return s;
    }

    // ── Declarations ──────────────────────────────────────────────────────────
    void printDecl(const Decl& d, int depth) {
        switch (d.kind) {
        case DeclKind::Import:   printImport(static_cast<const ImportDecl&>(d), depth); break;
        case DeclKind::Enum:     printEnum(static_cast<const EnumDecl&>(d), depth); break;
        case DeclKind::Struct:   printStruct(static_cast<const StructDecl&>(d), depth); break;
        case DeclKind::Union:    printUnion(static_cast<const UnionDecl&>(d), depth); break;
        case DeclKind::Class:    printClass(static_cast<const ClassDecl&>(d), depth); break;
        case DeclKind::Function: printFunc(static_cast<const FunctionDecl&>(d), depth); break;
        case DeclKind::Variable: printVarDecl(static_cast<const VarDecl&>(d), depth); break;
        case DeclKind::Using: {
            const auto& u = static_cast<const UsingDecl&>(d);
            out_ << ind(depth) << "UsingDecl  " << u.alias << " = " << u.target.name;
            if (u.isExport) out_ << "  [export]";
            out_ << "\n";
            break;
        }
        }
    }

    void printImport(const ImportDecl& n, int d) {
        out_ << ind(d) << "ImportDecl  path=\"" << n.path << "\"\n";
    }

    void printEnum(const EnumDecl& n, int d) {
        out_ << ind(d) << "EnumDecl  name=" << n.name << "\n";
        for (const auto& m : n.members)
            out_ << ind(d+1) << "Member: " << m << "\n";
    }

    void printStruct(const StructDecl& n, int d) {
        out_ << ind(d) << "StructDecl  name=" << n.name << "\n";
        for (const auto& f : n.fields)
            out_ << ind(d+1) << "Field: " << typeStr(f.type) << "  " << f.name << "\n";
    }

    void printUnion(const UnionDecl& n, int d) {
        out_ << ind(d) << "UnionDecl  name=" << n.name << "\n";
        for (const auto& f : n.fields)
            out_ << ind(d+1) << "Field: " << typeStr(f.type) << "  " << f.name << "\n";
    }

    void printClass(const ClassDecl& n, int d) {
        out_ << ind(d) << "ClassDecl  name=" << n.name;
        if (n.isFinal)   out_ << "  [final]";
        if (n.superClass) out_ << "  extends=" << *n.superClass;
        out_ << "\n";
        for (const auto& f : n.fields) {
            out_ << ind(d+1) << "Field  " << accessStr(f.access) << "  "
                 << typeStr(f.type) << "  " << f.name;
            if (f.isStatic) out_ << "  [static]";
            if (f.isConst)  out_ << "  [const]";
            out_ << "\n";
            if (f.init) printExpr(**f.init, d+2);
        }
        for (const auto& m : n.methods)
            printFunc(*m, d+1);
    }

    void printFunc(const FunctionDecl& n, int d) {
        out_ << ind(d) << "FunctionDecl  ";
        if (*accessStr(n.access)) out_ << accessStr(n.access) << "  ";
        if (n.isAsync)  out_ << "async  ";
        if (n.isStatic) out_ << "static  ";
        if (n.isFinal)  out_ << "final  ";
        out_ << typeStr(n.returnType) << "  " << n.name << "(";
        for (std::size_t i = 0; i < n.params.size(); ++i) {
            if (i) out_ << ", ";
            out_ << typeStr(n.params[i].type) << " " << n.params[i].name;
        }
        out_ << ")";
        if (n.throws) out_ << "  throws";
        out_ << "\n";
        if (n.body) printStmt(*n.body, d+1);
    }

    void printVarDecl(const VarDecl& n, int d) {
        out_ << ind(d) << "VarDecl  " << typeStr(n.type) << "  " << n.name;
        if (n.isConst)  out_ << "  [const]";
        if (n.isStatic) out_ << "  [static]";
        out_ << "\n";
        if (n.init) printExpr(**n.init, d+1);
    }

    // ── Statements ────────────────────────────────────────────────────────────
    void printStmt(const Stmt& s, int d) {
        switch (s.kind) {
        case StmtKind::Block: {
            const auto& b = static_cast<const BlockStmt&>(s);
            out_ << ind(d) << "Block\n";
            for (const auto& st : b.stmts) printStmt(*st, d+1);
            break;
        }
        case StmtKind::Expression: {
            out_ << ind(d) << "ExprStmt\n";
            printExpr(*static_cast<const ExprStmt&>(s).expr, d+1);
            break;
        }
        case StmtKind::VarDecl: {
            const auto& v = static_cast<const VarDeclStmt&>(s);
            out_ << ind(d) << "VarDeclStmt  " << typeStr(v.type) << "  " << v.name;
            if (v.isConst)  out_ << "  [const]";
            if (v.isStatic) out_ << "  [static]";
            out_ << "\n";
            if (v.init) printExpr(**v.init, d+1);
            break;
        }
        case StmtKind::If: {
            const auto& n = static_cast<const IfStmt&>(s);
            out_ << ind(d) << "IfStmt\n";
            out_ << ind(d+1) << "Cond:\n";  printExpr(*n.cond, d+2);
            out_ << ind(d+1) << "Then:\n";  printStmt(*n.thenBranch, d+2);
            if (n.elseBranch) { out_ << ind(d+1) << "Else:\n"; printStmt(**n.elseBranch, d+2); }
            break;
        }
        case StmtKind::While: {
            const auto& n = static_cast<const WhileStmt&>(s);
            out_ << ind(d) << "WhileStmt\n";
            out_ << ind(d+1) << "Cond:\n"; printExpr(*n.cond, d+2);
            out_ << ind(d+1) << "Body:\n"; printStmt(*n.body, d+2);
            break;
        }
        case StmtKind::DoWhile: {
            const auto& n = static_cast<const DoWhileStmt&>(s);
            out_ << ind(d) << "DoWhileStmt\n";
            out_ << ind(d+1) << "Body:\n"; printStmt(*n.body, d+2);
            out_ << ind(d+1) << "Cond:\n"; printExpr(*n.cond, d+2);
            break;
        }
        case StmtKind::For: {
            const auto& n = static_cast<const ForStmt&>(s);
            out_ << ind(d) << "ForStmt\n";
            if (n.init) { out_ << ind(d+1) << "Init:\n"; printStmt(**n.init, d+2); }
            if (n.cond) { out_ << ind(d+1) << "Cond:\n"; printExpr(**n.cond, d+2); }
            if (n.incr) { out_ << ind(d+1) << "Incr:\n"; printExpr(**n.incr, d+2); }
            out_ << ind(d+1) << "Body:\n"; printStmt(*n.body, d+2);
            break;
        }
        case StmtKind::Switch: {
            const auto& n = static_cast<const SwitchStmt&>(s);
            out_ << ind(d) << "SwitchStmt\n";
            out_ << ind(d+1) << "Subject:\n"; printExpr(*n.subject, d+2);
            for (const auto& c : n.cases) {
                if (c.value) { out_ << ind(d+1) << "Case:\n"; printExpr(**c.value, d+2); }
                else          { out_ << ind(d+1) << "Default:\n"; }
                for (const auto& st : c.stmts) printStmt(*st, d+2);
            }
            break;
        }
        case StmtKind::Return: {
            const auto& n = static_cast<const ReturnStmt&>(s);
            out_ << ind(d) << "ReturnStmt\n";
            if (n.value) printExpr(**n.value, d+1);
            break;
        }
        case StmtKind::Break:
            out_ << ind(d) << "BreakStmt\n"; break;
        case StmtKind::Continue:
            out_ << ind(d) << "ContinueStmt\n"; break;
        case StmtKind::Throw: {
            const auto& n = static_cast<const ThrowStmt&>(s);
            out_ << ind(d) << "ThrowStmt\n";
            printExpr(*n.value, d+1);
            break;
        }
        case StmtKind::Try: {
            const auto& n = static_cast<const TryStmt&>(s);
            out_ << ind(d) << "TryStmt\n";
            out_ << ind(d+1) << "Try:\n"; printStmt(*n.tryBody, d+2);
            for (const auto& c : n.catches) {
                out_ << ind(d+1) << "Catch  " << typeStr(c.exType)
                     << "  " << c.exName << "\n";
                printStmt(*c.body, d+2);
            }
            if (n.finallyBody) { out_ << ind(d+1) << "Finally:\n"; printStmt(**n.finallyBody, d+2); }
            break;
        }
        case StmtKind::With: {
            const auto& n = static_cast<const WithStmt&>(s);
            out_ << ind(d) << "WithStmt  " << n.resType.name << " " << n.resName << "\n";
            out_ << ind(d+1) << "Init:\n";    printExpr(*n.init, d+2);
            out_ << ind(d+1) << "Body:\n";    printStmt(*n.body, d+2);
            break;
        }
        }
    }

    // ── Expressions ───────────────────────────────────────────────────────────
    void printExpr(const Expr& e, int d) {
        switch (e.kind) {
        case ExprKind::IntLit:
            out_ << ind(d) << "IntLit  " << static_cast<const IntLitExpr&>(e).raw << "\n"; break;
        case ExprKind::FloatLit:
            out_ << ind(d) << "FloatLit  " << static_cast<const FloatLitExpr&>(e).raw << "\n"; break;
        case ExprKind::BoolLit:
            out_ << ind(d) << "BoolLit  " << (static_cast<const BoolLitExpr&>(e).value ? "true" : "false") << "\n"; break;
        case ExprKind::CharLit:
            out_ << ind(d) << "CharLit  " << static_cast<const CharLitExpr&>(e).raw << "\n"; break;
        case ExprKind::StringLit:
            out_ << ind(d) << "StringLit  \"" << static_cast<const StringLitExpr&>(e).value << "\"\n"; break;
        case ExprKind::NullLit:
            out_ << ind(d) << "NullLit\n"; break;
        case ExprKind::Identifier:
            out_ << ind(d) << "Ident  " << static_cast<const IdentExpr&>(e).name << "\n"; break;

        case ExprKind::Unary: {
            const auto& n = static_cast<const UnaryExpr&>(e);
            Token t; t.kind = n.op;
            out_ << ind(d) << "Unary  op=" << t.kindName()
                 << (n.postfix ? "  [postfix]" : "  [prefix]") << "\n";
            printExpr(*n.operand, d+1);
            break;
        }
        case ExprKind::Binary: {
            const auto& n = static_cast<const BinaryExpr&>(e);
            Token t; t.kind = n.op;
            out_ << ind(d) << "Binary  op=" << t.kindName() << "\n";
            printExpr(*n.left, d+1);
            printExpr(*n.right, d+1);
            break;
        }
        case ExprKind::Ternary: {
            const auto& n = static_cast<const TernaryExpr&>(e);
            out_ << ind(d) << "Ternary\n";
            out_ << ind(d+1) << "Cond:\n"; printExpr(*n.cond, d+2);
            out_ << ind(d+1) << "Then:\n"; printExpr(*n.thenExpr, d+2);
            out_ << ind(d+1) << "Else:\n"; printExpr(*n.elseExpr, d+2);
            break;
        }
        case ExprKind::Assign: {
            const auto& n = static_cast<const AssignExpr&>(e);
            Token t; t.kind = n.op;
            out_ << ind(d) << "Assign  op=" << t.kindName() << "\n";
            printExpr(*n.target, d+1);
            printExpr(*n.value, d+1);
            break;
        }
        case ExprKind::Call: {
            const auto& n = static_cast<const CallExpr&>(e);
            out_ << ind(d) << "Call\n";
            out_ << ind(d+1) << "Callee:\n"; printExpr(*n.callee, d+2);
            if (!n.args.empty()) {
                out_ << ind(d+1) << "Args:\n";
                for (const auto& a : n.args) printExpr(*a, d+2);
            }
            break;
        }
        case ExprKind::Member: {
            const auto& n = static_cast<const MemberExpr&>(e);
            out_ << ind(d) << "Member  ." << n.member
                 << (n.arrow ? "  [->]" : "") << "\n";
            printExpr(*n.object, d+1);
            break;
        }
        case ExprKind::Index: {
            const auto& n = static_cast<const IndexExpr&>(e);
            out_ << ind(d) << "Index\n";
            out_ << ind(d+1) << "Array:\n"; printExpr(*n.array, d+2);
            out_ << ind(d+1) << "Index:\n"; printExpr(*n.index, d+2);
            break;
        }
        case ExprKind::New: {
            const auto& n = static_cast<const NewExpr&>(e);
            out_ << ind(d) << "New  type=" << typeStr(n.type) << "\n";
            for (const auto& a : n.args) printExpr(*a, d+1);
            break;
        }
        case ExprKind::Delete: {
            out_ << ind(d) << "Delete\n";
            printExpr(*static_cast<const DeleteExpr&>(e).operand, d+1);
            break;
        }
        case ExprKind::TypeOf: {
            out_ << ind(d) << "TypeOf\n";
            printExpr(*static_cast<const TypeOfExpr&>(e).operand, d+1);
            break;
        }
        case ExprKind::InstanceOf: {
            const auto& n = static_cast<const InstanceOfExpr&>(e);
            out_ << ind(d) << "InstanceOf  type=" << typeStr(n.type) << "\n";
            printExpr(*n.operand, d+1);
            break;
        }
        case ExprKind::Lambda: {
            const auto& n = static_cast<const LambdaExpr&>(e);
            out_ << ind(d) << "Lambda  (";
            for (std::size_t i = 0; i < n.params.size(); ++i) {
                if (i) out_ << ", ";
                out_ << typeStr(n.params[i].type) << " " << n.params[i].name;
            }
            out_ << ")\n";
            if (n.body) printExpr(*n.body, d+1);
            if (n.blockBody) printStmt(*n.blockBody, d+1);
            break;
        }
        default:
            out_ << ind(d) << "<unknown expr>\n";
        }
    }
};

#endif // AST_PRINTER_H
