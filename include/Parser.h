#pragma once
//===----------------------------------------------------------------------===//
//  Parser.h  –  Recursive descent parser for the custom language
//===----------------------------------------------------------------------===//
#ifndef PARSER_H
#define PARSER_H

#include "Lexer.h"
#include "AST.h"

#include <string>
#include <vector>
#include <memory>
#include <functional>

// ─────────────────────────────────────────────────────────────────────────────
//  ParseError  –  a single diagnostic collected during parsing
// ─────────────────────────────────────────────────────────────────────────────
struct ParseError {
    std::string    message;
    SourceLocation loc;

    std::string format() const {
        return loc.filename + ":" + std::to_string(loc.line) + ":" +
               std::to_string(loc.column) + ": error: " + message;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
//  Parser
// ─────────────────────────────────────────────────────────────────────────────
class Parser {
public:
    explicit Parser(std::vector<Token> tokens);

    /// Parse the entire program and return the root AST node.
    /// Errors are collected internally; call errors() to retrieve them.
    Program parse();

    const std::vector<ParseError>& errors() const { return errors_; }
    bool hadError() const { return !errors_.empty(); }

private:
    // ── Token stream management ──────────────────────────────────────────────
    const Token& peek(int offset = 0) const;
    const Token& advance();
    bool check(TokenKind k) const;
    bool match(TokenKind k);
    bool matchAny(std::initializer_list<TokenKind> kinds);
    const Token& expect(TokenKind k, const std::string& msg);
    bool isAtEnd() const;

    // ── Error handling & recovery ────────────────────────────────────────────
    void error(const std::string& msg, SourceLocation loc);
    void error(const std::string& msg);
    /// Synchronise: discard tokens until a statement/decl boundary.
    void synchronize();
    void synchronizeDecl();

    // ── Type parsing ─────────────────────────────────────────────────────────
    TypeRef parseType();
    bool    isTypeName() const;

    // ── Declaration parsers ──────────────────────────────────────────────────
    DeclPtr parseDeclaration();
    DeclPtr parseImport();
    DeclPtr parseUsing();
    DeclPtr parseEnum();
    DeclPtr parseStruct();
    DeclPtr parseUnion();
    DeclPtr parseClass();
    std::unique_ptr<FunctionDecl> parseFunctionDecl(AccessMod access,
                                                     bool isAsync,
                                                     bool isStatic,
                                                     bool isFinal);
    std::unique_ptr<VarDecl>      parseTopLevelVar(bool isConst, bool isStatic);

    // ── Statement parsers ────────────────────────────────────────────────────
    StmtPtr parseStatement();
    std::unique_ptr<BlockStmt> parseBlock();
    StmtPtr parseIf();
    StmtPtr parseWhile();
    StmtPtr parseDoWhile();
    StmtPtr parseFor();
    StmtPtr parseSwitch();
    StmtPtr parseReturn();
    StmtPtr parseThrow();
    StmtPtr parseTry();
    StmtPtr parseWith();
    StmtPtr parseBreak();
    StmtPtr parseContinue();
    StmtPtr parseVarDeclStmt(bool isConst, bool isStatic);
    StmtPtr parseExprStmt();

    // ── Expression parsers (precedence climbing) ─────────────────────────────
    ExprPtr parseExpr();
    ExprPtr parseAssignment();
    ExprPtr parseTernary();
    ExprPtr parseOr();
    ExprPtr parseAnd();
    ExprPtr parseBitOr();
    ExprPtr parseBitXor();
    ExprPtr parseBitAnd();
    ExprPtr parseEquality();
    ExprPtr parseRelational();
    ExprPtr parseShift();
    ExprPtr parseAdditive();
    ExprPtr parseMultiplicative();
    ExprPtr parseUnary();
    ExprPtr parsePostfix();
    ExprPtr parsePrimary();

    // ── Helper: parse comma-separated argument list ───────────────────────────
    std::vector<ExprPtr> parseArgList();
    std::vector<Parameter> parseParamList();

    // ── Utilities ────────────────────────────────────────────────────────────
    AccessMod parseAccessMod();
    bool isAssignOp(TokenKind k) const;

    // ── State ────────────────────────────────────────────────────────────────
    std::vector<Token>      tokens_;
    std::size_t             pos_ = 0;
    std::vector<ParseError> errors_;

    // Sentinel EOF token returned when past end
    Token eof_;
};

#endif // PARSER_H
