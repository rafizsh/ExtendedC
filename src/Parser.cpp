//===----------------------------------------------------------------------===//
//  Parser.cpp  –  Recursive descent parser implementation
//===----------------------------------------------------------------------===//
#include "Parser.h"

#include <cassert>
#include <sstream>
#include <stdexcept>
#include <charconv>
#include <cstdlib>

// ─────────────────────────────────────────────────────────────────────────────
//  Constructor
// ─────────────────────────────────────────────────────────────────────────────
Parser::Parser(std::vector<Token> tokens)
    : tokens_(std::move(tokens)) {
    // Build a sentinel EOF token
    if (!tokens_.empty()) {
        eof_.kind   = TokenKind::Eof;
        eof_.lexeme = "<eof>";
        eof_.loc    = tokens_.back().loc;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Token stream management
// ─────────────────────────────────────────────────────────────────────────────
const Token& Parser::peek(int offset) const {
    std::size_t idx = pos_ + static_cast<std::size_t>(offset);
    if (idx >= tokens_.size()) return eof_;
    return tokens_[idx];
}

const Token& Parser::advance() {
    if (!isAtEnd()) ++pos_;
    return tokens_[pos_ - 1];
}

bool Parser::check(TokenKind k) const {
    return peek().kind == k;
}

bool Parser::match(TokenKind k) {
    if (check(k)) { advance(); return true; }
    return false;
}

bool Parser::matchAny(std::initializer_list<TokenKind> kinds) {
    for (auto k : kinds)
        if (check(k)) { advance(); return true; }
    return false;
}

const Token& Parser::expect(TokenKind k, const std::string& msg) {
    if (check(k)) return advance();
    error(msg);
    return peek(); // don't advance — let caller recover
}

bool Parser::isAtEnd() const {
    return pos_ >= tokens_.size() || tokens_[pos_].kind == TokenKind::Eof;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Error handling
// ─────────────────────────────────────────────────────────────────────────────
void Parser::error(const std::string& msg, SourceLocation loc) {
    errors_.push_back({msg, loc});
}

void Parser::error(const std::string& msg) {
    error(msg, peek().loc);
}

/// Panic-mode recovery: skip tokens until we hit something that looks like
/// a statement or declaration boundary.
void Parser::synchronize() {
    while (!isAtEnd()) {
        // After a semicolon we're probably back in sync
        if (peek(-1 + static_cast<int>(pos_ > 0 ? 1 : 0)).kind == TokenKind::Semi &&
            pos_ > 0 && tokens_[pos_-1].kind == TokenKind::Semi)
            return;

        switch (peek().kind) {
        // These tokens reliably start new statements or declarations
        case TokenKind::KW_fun:
        case TokenKind::KW_class:
        case TokenKind::KW_struct:
        case TokenKind::KW_union:
        case TokenKind::KW_enum:
        case TokenKind::KW_import:
        case TokenKind::KW_return:
        case TokenKind::KW_if:
        case TokenKind::KW_while:
        case TokenKind::KW_for:
        case TokenKind::KW_do:
        case TokenKind::KW_switch:
        case TokenKind::KW_try:
        case TokenKind::KW_throw:
        case TokenKind::KW_break:
        case TokenKind::KW_continue:
        case TokenKind::RBrace:
            return;
        case TokenKind::Semi:
            advance(); // consume the semicolon
            return;
        default:
            advance();
        }
    }
}

void Parser::synchronizeDecl() {
    while (!isAtEnd()) {
        switch (peek().kind) {
        case TokenKind::KW_fun:
        case TokenKind::KW_class:
        case TokenKind::KW_struct:
        case TokenKind::KW_union:
        case TokenKind::KW_enum:
        case TokenKind::KW_import:
        case TokenKind::KW_public:
        case TokenKind::KW_private:
        case TokenKind::KW_protected:
        case TokenKind::KW_static:
        case TokenKind::KW_const:
        case TokenKind::KW_async:
        case TokenKind::KW_final:
        case TokenKind::RBrace:
            return;
        case TokenKind::Semi:
            advance();
            return;
        default:
            advance();
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Type parsing
// ─────────────────────────────────────────────────────────────────────────────
static bool isBuiltinType(TokenKind k) {
    switch (k) {
    case TokenKind::KW_bool:   case TokenKind::KW_char:
    case TokenKind::KW_int:    case TokenKind::KW_uint:
    case TokenKind::KW_long:   case TokenKind::KW_ulong:
    case TokenKind::KW_float:  case TokenKind::KW_ufloat:
    case TokenKind::KW_double: case TokenKind::KW_string:
    case TokenKind::KW_void:
        return true;
    default: return false;
    }
}

bool Parser::isTypeName() const {
    return isBuiltinType(peek().kind) || peek().kind == TokenKind::Identifier
        || peek().kind == TokenKind::KW_var;
}

TypeRef Parser::parseType() {
    TypeRef t;
    t.loc = peek().loc;

    if (isBuiltinType(peek().kind)) {
        t.name = advance().lexeme;
    } else if (check(TokenKind::Identifier)) {
        t.name = advance().lexeme;
    } else if (check(TokenKind::KW_var)) {
        t.name = "var";
        advance();
    } else {
        error("expected a type name, got '" + peek().lexeme + "'");
        t.name = "<error>";
        return t;
    }

    // array suffix  []
    if (check(TokenKind::LBracket) && peek(1).kind == TokenKind::RBracket) {
        advance(); advance();
        t.isArray = true;
    }

    // pointer suffix  *
    if (check(TokenKind::Star)) {
        advance();
        t.isPointer = true;
    }

    return t;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Top-level parse
// ─────────────────────────────────────────────────────────────────────────────
Program Parser::parse() {
    Program program;
    while (!isAtEnd()) {
        try {
            auto decl = parseDeclaration();
            if (decl) program.declarations.push_back(std::move(decl));
        } catch (const std::exception& ex) {
            error(std::string("internal: ") + ex.what());
            synchronizeDecl();
        }
    }
    return program;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Declaration dispatcher
// ─────────────────────────────────────────────────────────────────────────────
DeclPtr Parser::parseDeclaration() {
    // import
    if (check(TokenKind::KW_import))  return parseImport();
    if (check(TokenKind::KW_using))   return parseUsing();
    if (check(TokenKind::KW_enum))    return parseEnum();
    if (check(TokenKind::KW_struct))  return parseStruct();
    if (check(TokenKind::KW_union))   return parseUnion();

    // export can appear before class, fun, or var
    // Peek past it so class dispatch still works
    if (check(TokenKind::KW_export)) {
        // Save position, consume export, dispatch, then set isExport on result
        advance(); // consume 'export'
        if (check(TokenKind::KW_class) || check(TokenKind::KW_final)) {
            auto cls = parseClass();
            if (cls) static_cast<ClassDecl*>(cls.get())->isExport = true;
            return cls;
        }
        // For fun/var: fall through to modifier loop below which has isExport
        // but we already consumed 'export', so set a flag
        // We need to reinsert it — simplest: treat as already parsed
        // We'll handle this by injecting a pre-set isExport flag
        // Actually: just parse fun/var directly here
        bool isAsync = false, isStatic = false, isFinal = false, isConst = false;
        while (true) {
            if (match(TokenKind::KW_async))  { isAsync  = true; continue; }
            if (match(TokenKind::KW_static)) { isStatic = true; continue; }
            if (match(TokenKind::KW_final))  { isFinal  = true; continue; }
            if (match(TokenKind::KW_const))  { isConst  = true; continue; }
            break;
        }
        if (check(TokenKind::KW_fun)) {
            auto fn = parseFunctionDecl(AccessMod::None, isAsync, isStatic, isFinal);
            if (fn) static_cast<FunctionDecl*>(fn.get())->isExport = true;
            return fn;
        }
        if (isTypeName()) {
            auto vd = parseTopLevelVar(isConst, isStatic);
            if (vd) static_cast<VarDecl*>(vd.get())->isExport = true;
            return vd;
        }
        error("expected declaration after 'export'");
        return nullptr;
    }

    if (check(TokenKind::KW_class))   return parseClass();

    // modifiers that can precede fun or var at top-level
    bool isAsync  = false;
    bool isStatic = false;
    bool isFinal  = false;
    bool isConst  = false;

    bool isExport = false;
    while (true) {
        if (match(TokenKind::KW_async))  { isAsync  = true; continue; }
        if (match(TokenKind::KW_static)) { isStatic = true; continue; }
        if (match(TokenKind::KW_final))  { isFinal  = true; continue; }
        if (match(TokenKind::KW_const))  { isConst  = true; continue; }
        if (match(TokenKind::KW_export)) { isExport = true; continue; }
        break;
    }

    if (check(TokenKind::KW_fun)) {
        auto fn = parseFunctionDecl(AccessMod::None, isAsync, isStatic, isFinal);
        if (fn && isExport) static_cast<FunctionDecl*>(fn.get())->isExport = true;
        return fn;
    }

    if (isTypeName()) {
        auto vd = parseTopLevelVar(isConst, isStatic);
        if (vd && isExport) static_cast<VarDecl*>(vd.get())->isExport = true;
        return vd;
    }

    // Unknown – record error and skip token
    error("unexpected token '" + peek().lexeme + "' at top level");
    advance();
    return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Import
// ─────────────────────────────────────────────────────────────────────────────
DeclPtr Parser::parseImport() {
    auto loc = peek().loc;
    expect(TokenKind::KW_import, "expected 'import'");

    if (!check(TokenKind::LiteralString)) {
        error("expected a string path after 'import'");
        synchronize();
        return nullptr;
    }

    // Strip surrounding quotes from the lexeme
    std::string raw = advance().lexeme;
    std::string path = raw.substr(1, raw.size() - 2);
    expect(TokenKind::Semi, "expected ';' after import path");

    return std::make_unique<ImportDecl>(std::move(path), loc);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Enum
// ─────────────────────────────────────────────────────────────────────────────
DeclPtr Parser::parseEnum() {
    auto loc = peek().loc;
    expect(TokenKind::KW_enum, "expected 'enum'");

    if (!check(TokenKind::Identifier)) {
        error("expected enum name");
        synchronizeDecl();
        return nullptr;
    }
    std::string name = advance().lexeme;

    expect(TokenKind::LBrace, "expected '{' after enum name");

    std::vector<std::string> members;
    while (!check(TokenKind::RBrace) && !isAtEnd()) {
        if (!check(TokenKind::Identifier)) {
            error("expected enum member name");
            synchronize();
            break;
        }
        members.push_back(advance().lexeme);
        if (!match(TokenKind::Comma)) break;
    }

    expect(TokenKind::RBrace, "expected '}' to close enum");
    return std::make_unique<EnumDecl>(std::move(name), std::move(members), loc);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Struct
// ─────────────────────────────────────────────────────────────────────────────
DeclPtr Parser::parseStruct() {
    auto loc = peek().loc;
    expect(TokenKind::KW_struct, "expected 'struct'");

    if (!check(TokenKind::Identifier)) {
        error("expected struct name");
        synchronizeDecl();
        return nullptr;
    }
    std::string name = advance().lexeme;
    expect(TokenKind::LBrace, "expected '{' after struct name");

    std::vector<FieldDecl> fields;
    while (!check(TokenKind::RBrace) && !isAtEnd()) {
        if (!isTypeName()) {
            error("expected field type in struct");
            synchronize();
            continue;
        }
        FieldDecl fd;
        fd.loc  = peek().loc;
        fd.type = parseType();
        if (!check(TokenKind::Identifier)) {
            error("expected field name in struct");
            synchronize();
            continue;
        }
        fd.name = advance().lexeme;
        expect(TokenKind::Semi, "expected ';' after struct field");
        fields.push_back(std::move(fd));
    }

    expect(TokenKind::RBrace, "expected '}' to close struct");
    return std::make_unique<StructDecl>(std::move(name), std::move(fields), loc);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Union
// ─────────────────────────────────────────────────────────────────────────────
DeclPtr Parser::parseUnion() {
    auto loc = peek().loc;
    expect(TokenKind::KW_union, "expected 'union'");

    if (!check(TokenKind::Identifier)) {
        error("expected union name");
        synchronizeDecl();
        return nullptr;
    }
    std::string name = advance().lexeme;
    expect(TokenKind::LBrace, "expected '{' after union name");

    std::vector<FieldDecl> fields;
    while (!check(TokenKind::RBrace) && !isAtEnd()) {
        if (!isTypeName()) {
            error("expected field type in union");
            synchronize();
            continue;
        }
        FieldDecl fd;
        fd.loc  = peek().loc;
        fd.type = parseType();
        if (!check(TokenKind::Identifier)) {
            error("expected field name in union");
            synchronize();
            continue;
        }
        fd.name = advance().lexeme;
        expect(TokenKind::Semi, "expected ';' after union field");
        fields.push_back(std::move(fd));
    }

    expect(TokenKind::RBrace, "expected '}' to close union");
    return std::make_unique<UnionDecl>(std::move(name), std::move(fields), loc);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Class
// ─────────────────────────────────────────────────────────────────────────────
DeclPtr Parser::parseClass() {
    auto loc = peek().loc;
    bool isFinal  = false;
    bool isExport = false;

    if (match(TokenKind::KW_export)) isExport = true;
    if (match(TokenKind::KW_final)) isFinal = true;
    expect(TokenKind::KW_class, "expected 'class'");

    if (!check(TokenKind::Identifier)) {
        error("expected class name");
        synchronizeDecl();
        return nullptr;
    }

    auto cls = std::make_unique<ClassDecl>(loc);
    cls->name     = advance().lexeme;
    cls->isFinal  = isFinal;
    cls->isExport = isExport;

    if (match(TokenKind::KW_extends)) {
        if (!check(TokenKind::Identifier)) {
            error("expected superclass name after 'extends'");
        } else {
            cls->superClass = advance().lexeme;
        }
    }

    expect(TokenKind::LBrace, "expected '{' to open class body");

    while (!check(TokenKind::RBrace) && !isAtEnd()) {
        // collect modifiers
        AccessMod access   = AccessMod::None;
        bool      isStatic = false;
        bool      isAsync  = false;
        bool      isFinalM = false;
        bool      isConst  = false;

        // parse any leading modifiers
        bool changed = true;
        while (changed) {
            changed = false;
            if (check(TokenKind::KW_public))    { access=AccessMod::Public;    advance(); changed=true; }
            else if (check(TokenKind::KW_private))   { access=AccessMod::Private;   advance(); changed=true; }
            else if (check(TokenKind::KW_protected)) { access=AccessMod::Protected; advance(); changed=true; }
            else if (match(TokenKind::KW_static)) { isStatic=true; changed=true; }
            else if (match(TokenKind::KW_async))  { isAsync =true; changed=true; }
            else if (match(TokenKind::KW_final))  { isFinalM=true; changed=true; }
            else if (match(TokenKind::KW_const))  { isConst =true; changed=true; }
        }

        if (check(TokenKind::KW_export)) { advance(); } // export inside class is no-op

        if (check(TokenKind::KW_fun)) {
            auto fn = parseFunctionDecl(access, isAsync, isStatic, isFinalM);
            if (fn) cls->methods.push_back(std::unique_ptr<FunctionDecl>(
                static_cast<FunctionDecl*>(fn.release())));
        } else if (isTypeName()) {
            // field declaration
            MemberVarDecl mv;
            mv.access   = access;
            mv.isStatic = isStatic;
            mv.isConst  = isConst;
            mv.loc      = peek().loc;
            mv.type     = parseType();
            if (!check(TokenKind::Identifier)) {
                error("expected field name");
                synchronizeDecl();
                continue;
            }
            mv.name = advance().lexeme;
            if (match(TokenKind::Equal)) {
                mv.init = parseExpr();
            }
            expect(TokenKind::Semi, "expected ';' after field declaration");
            cls->fields.push_back(std::move(mv));
        } else {
            error("unexpected token '" + peek().lexeme + "' in class body");
            synchronizeDecl();
        }
    }

    expect(TokenKind::RBrace, "expected '}' to close class body");
    return cls;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Function declaration
//  fun returnType name ( params ) [throws] block
// ─────────────────────────────────────────────────────────────────────────────
std::unique_ptr<FunctionDecl> Parser::parseFunctionDecl(
    AccessMod access, bool isAsync, bool isStatic, bool isFinal)
{
    auto loc = peek().loc;
    expect(TokenKind::KW_fun, "expected 'fun'");

    auto fn       = std::make_unique<FunctionDecl>(loc);
    fn->access    = access;
    fn->isAsync   = isAsync;
    fn->isStatic  = isStatic;
    fn->isFinal   = isFinal;

    // return type
    if (!isTypeName()) {
        error("expected return type after 'fun'");
        synchronizeDecl();
        return nullptr;
    }
    fn->returnType = parseType();

    // function name
    if (!check(TokenKind::Identifier)) {
        error("expected function name");
        synchronizeDecl();
        return nullptr;
    }
    fn->name = advance().lexeme;

    // parameter list
    expect(TokenKind::LParen, "expected '(' after function name");
    fn->params = parseParamList();
    expect(TokenKind::RParen, "expected ')' to close parameter list");

    // optional throws
    if (match(TokenKind::KW_throws)) fn->throws = true;

    // body
    if (!check(TokenKind::LBrace)) {
        error("expected '{' to open function body");
        synchronizeDecl();
        return nullptr;
    }
    fn->body = parseBlock();
    return fn;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Top-level variable declaration
// ─────────────────────────────────────────────────────────────────────────────
std::unique_ptr<VarDecl> Parser::parseTopLevelVar(bool isConst, bool isStatic) {
    auto loc   = peek().loc;
    auto vd    = std::make_unique<VarDecl>(loc);
    vd->isConst  = isConst;
    vd->isStatic = isStatic;
    vd->type     = parseType();

    if (!check(TokenKind::Identifier)) {
        error("expected variable name");
        synchronize();
        return nullptr;
    }
    vd->name = advance().lexeme;

    if (match(TokenKind::Equal)) {
        vd->init = parseExpr();
    }
    expect(TokenKind::Semi, "expected ';' after variable declaration");
    return vd;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Parameter list  (type name, type name, …)
// ─────────────────────────────────────────────────────────────────────────────
std::vector<Parameter> Parser::parseParamList() {
    std::vector<Parameter> params;
    if (check(TokenKind::RParen)) return params; // empty

    do {
        if (!isTypeName()) {
            error("expected parameter type");
            break;
        }
        Parameter p;
        p.loc  = peek().loc;
        p.type = parseType();
        if (!check(TokenKind::Identifier)) {
            error("expected parameter name");
            break;
        }
        p.name = advance().lexeme;
        params.push_back(std::move(p));
    } while (match(TokenKind::Comma));

    return params;
}

// ═════════════════════════════════════════════════════════════════════════════
//  Statement parsers
// ═════════════════════════════════════════════════════════════════════════════
StmtPtr Parser::parseStatement() {
    switch (peek().kind) {
    case TokenKind::LBrace:    return parseBlock();
    case TokenKind::KW_if:     return parseIf();
    case TokenKind::KW_while:  return parseWhile();
    case TokenKind::KW_do:     return parseDoWhile();
    case TokenKind::KW_for:    return parseFor();
    case TokenKind::KW_switch: return parseSwitch();
    case TokenKind::KW_return: return parseReturn();
    case TokenKind::KW_throw:  return parseThrow();
    case TokenKind::KW_try:    return parseTry();
    case TokenKind::KW_with:   return parseWith();
    case TokenKind::KW_break:  return parseBreak();
    case TokenKind::KW_continue: return parseContinue();

    // const / static prefix before a var decl inside a function
    case TokenKind::KW_const:
    case TokenKind::KW_static: {
        bool isConst  = false;
        bool isStatic = false;
        while (true) {
            if (match(TokenKind::KW_const))  { isConst  = true; continue; }
            if (match(TokenKind::KW_static)) { isStatic = true; continue; }
            break;
        }
        return parseVarDeclStmt(isConst, isStatic);
    }

    default:
        // Could be a var decl (type name …) or an expression statement
        if (isTypeName()) {
            // Disambiguate: if followed by identifier, it's a declaration
            // Edge: could be  "foo(…)" which starts with an identifier too.
            // Look ahead: type then identifier → var decl
            // Special case:  int[] name  → type is array, still a var decl
            // peek(0)=type, peek(1)=[, peek(2)=], peek(3)=identifier
            bool isArrayTypeDecl =
                (peek(1).kind == TokenKind::LBracket &&
                 peek(2).kind == TokenKind::RBracket &&
                 peek(3).kind == TokenKind::Identifier);

            if (peek().kind != TokenKind::KW_var &&
                !isArrayTypeDecl &&
                peek(1).kind == TokenKind::Identifier &&
                // make sure it's not a call like  foo(…)
                peek(2).kind != TokenKind::LParen &&
                peek(2).kind != TokenKind::Dot &&
                peek(2).kind != TokenKind::LBracket)
            {
                return parseVarDeclStmt(false, false);
            }
            // int[] name  — array type decl
            if (isArrayTypeDecl)
                return parseVarDeclStmt(false, false);
            // var keyword always means declaration
            if (peek().kind == TokenKind::KW_var)
                return parseVarDeclStmt(false, false);
        }
        return parseExprStmt();
    }
}

std::unique_ptr<BlockStmt> Parser::parseBlock() {
    auto loc = peek().loc;
    expect(TokenKind::LBrace, "expected '{'");

    std::vector<StmtPtr> stmts;
    while (!check(TokenKind::RBrace) && !isAtEnd()) {
        try {
            auto s = parseStatement();
            if (s) stmts.push_back(std::move(s));
        } catch (...) {
            synchronize();
        }
    }

    expect(TokenKind::RBrace, "expected '}' to close block");
    return std::make_unique<BlockStmt>(std::move(stmts), loc);
}

StmtPtr Parser::parseIf() {
    auto loc = peek().loc;
    expect(TokenKind::KW_if, "expected 'if'");
    expect(TokenKind::LParen, "expected '(' after 'if'");
    auto cond = parseExpr();
    expect(TokenKind::RParen, "expected ')' after if condition");
    auto thenBranch = parseStatement();

    std::optional<StmtPtr> elseBranch;
    if (match(TokenKind::KW_else))
        elseBranch = parseStatement();

    return std::make_unique<IfStmt>(
        std::move(cond), std::move(thenBranch), std::move(elseBranch), loc);
}

StmtPtr Parser::parseWhile() {
    auto loc = peek().loc;
    expect(TokenKind::KW_while, "expected 'while'");
    expect(TokenKind::LParen, "expected '(' after 'while'");
    auto cond = parseExpr();
    expect(TokenKind::RParen, "expected ')' after while condition");
    auto body = parseStatement();
    return std::make_unique<WhileStmt>(std::move(cond), std::move(body), loc);
}

StmtPtr Parser::parseDoWhile() {
    auto loc = peek().loc;
    expect(TokenKind::KW_do, "expected 'do'");
    auto body = parseStatement();
    expect(TokenKind::KW_while, "expected 'while' after do body");
    expect(TokenKind::LParen, "expected '(' after 'while'");
    auto cond = parseExpr();
    expect(TokenKind::RParen, "expected ')' after do-while condition");
    expect(TokenKind::Semi, "expected ';' after do-while");
    return std::make_unique<DoWhileStmt>(std::move(body), std::move(cond), loc);
}

StmtPtr Parser::parseFor() {
    auto loc = peek().loc;
    expect(TokenKind::KW_for, "expected 'for'");
    expect(TokenKind::LParen, "expected '(' after 'for'");

    // initializer
    std::optional<StmtPtr> init;
    if (!check(TokenKind::Semi)) {
        bool isArrayInit = isTypeName() &&
                           peek(1).kind == TokenKind::LBracket &&
                           peek(2).kind == TokenKind::RBracket &&
                           peek(3).kind == TokenKind::Identifier;
        if (isArrayInit || (isTypeName() && peek(1).kind == TokenKind::Identifier))
            init = parseVarDeclStmt(false, false);
        else
            init = parseExprStmt();
    } else {
        advance(); // consume ';'
    }

    // condition
    std::optional<ExprPtr> cond;
    if (!check(TokenKind::Semi)) cond = parseExpr();
    expect(TokenKind::Semi, "expected ';' after for condition");

    // increment
    std::optional<ExprPtr> incr;
    if (!check(TokenKind::RParen)) incr = parseExpr();
    expect(TokenKind::RParen, "expected ')' after for increment");

    auto body = parseStatement();
    return std::make_unique<ForStmt>(
        std::move(init), std::move(cond), std::move(incr), std::move(body), loc);
}

StmtPtr Parser::parseSwitch() {
    auto loc = peek().loc;
    expect(TokenKind::KW_switch, "expected 'switch'");
    expect(TokenKind::LParen, "expected '(' after 'switch'");
    auto subject = parseExpr();
    expect(TokenKind::RParen, "expected ')' after switch subject");
    expect(TokenKind::LBrace, "expected '{' to open switch body");

    std::vector<CaseClause> cases;

    while (!check(TokenKind::RBrace) && !isAtEnd()) {
        CaseClause clause;

        if (match(TokenKind::KW_case)) {
            clause.value = parseExpr();
            // Allow  Direction::NORTH  (binary expr with ::)
            expect(TokenKind::Colon, "expected ':' after case value");
        } else if (match(TokenKind::KW_default)) {
            // nullopt = default
            expect(TokenKind::Colon, "expected ':' after 'default'");
        } else {
            error("expected 'case' or 'default' in switch body");
            synchronize();
            continue;
        }

        // collect statements until next case / default / }
        while (!check(TokenKind::KW_case) &&
               !check(TokenKind::KW_default) &&
               !check(TokenKind::RBrace) &&
               !isAtEnd())
        {
            auto s = parseStatement();
            if (s) clause.stmts.push_back(std::move(s));
        }

        cases.push_back(std::move(clause));
    }

    expect(TokenKind::RBrace, "expected '}' to close switch body");
    return std::make_unique<SwitchStmt>(std::move(subject), std::move(cases), loc);
}

StmtPtr Parser::parseReturn() {
    auto loc = peek().loc;
    expect(TokenKind::KW_return, "expected 'return'");

    std::optional<ExprPtr> value;
    if (!check(TokenKind::Semi))
        value = parseExpr();

    expect(TokenKind::Semi, "expected ';' after return");
    return std::make_unique<ReturnStmt>(std::move(value), loc);
}

StmtPtr Parser::parseThrow() {
    auto loc = peek().loc;
    expect(TokenKind::KW_throw, "expected 'throw'");
    auto val = parseExpr();
    expect(TokenKind::Semi, "expected ';' after throw");
    return std::make_unique<ThrowStmt>(std::move(val), loc);
}

StmtPtr Parser::parseTry() {
    auto loc = peek().loc;
    expect(TokenKind::KW_try, "expected 'try'");
    auto tryBody = parseBlock();

    std::vector<CatchClause> catches;
    while (check(TokenKind::KW_catch)) {
        advance();
        expect(TokenKind::LParen, "expected '(' after 'catch'");
        CatchClause cc;
        cc.exType = parseType();
        if (check(TokenKind::Identifier))
            cc.exName = advance().lexeme;
        expect(TokenKind::RParen, "expected ')' after catch parameter");
        cc.body = parseBlock();
        catches.push_back(std::move(cc));
    }

    std::optional<StmtPtr> finallyBody;
    // optional finally (extend the language slightly)
    // if (match(TokenKind::KW_finally)) finallyBody = parseBlock();

    return std::make_unique<TryStmt>(
        std::move(tryBody), std::move(catches), std::move(finallyBody), loc);
}

StmtPtr Parser::parseBreak() {
    auto loc = peek().loc;
    expect(TokenKind::KW_break, "expected 'break'");
    expect(TokenKind::Semi, "expected ';' after 'break'");
    return std::make_unique<BreakStmt>(loc);
}

StmtPtr Parser::parseContinue() {
    auto loc = peek().loc;
    expect(TokenKind::KW_continue, "expected 'continue'");
    expect(TokenKind::Semi, "expected ';' after 'continue'");
    return std::make_unique<ContinueStmt>(loc);
}

StmtPtr Parser::parseVarDeclStmt(bool isConst, bool isStatic) {
    auto loc  = peek().loc;
    auto type = parseType();

    if (!check(TokenKind::Identifier)) {
        error("expected variable name");
        synchronize();
        return nullptr;
    }
    std::string name = advance().lexeme;

    std::optional<ExprPtr> init;
    if (match(TokenKind::Equal))
        init = parseExpr();

    expect(TokenKind::Semi, "expected ';' after variable declaration");
    return std::make_unique<VarDeclStmt>(
        std::move(type), std::move(name), std::move(init),
        isConst, isStatic, loc);
}

StmtPtr Parser::parseExprStmt() {
    auto loc  = peek().loc;
    auto expr = parseExpr();
    expect(TokenKind::Semi, "expected ';' after expression");
    return std::make_unique<ExprStmt>(std::move(expr), loc);
}

// ═════════════════════════════════════════════════════════════════════════════
//  Expression parsers  (precedence ladder)
// ═════════════════════════════════════════════════════════════════════════════
bool Parser::isAssignOp(TokenKind k) const {
    switch (k) {
    case TokenKind::Equal:
    case TokenKind::PlusEq: case TokenKind::MinusEq:
    case TokenKind::StarEq: case TokenKind::SlashEq: case TokenKind::PercentEq:
    case TokenKind::AmpEq:  case TokenKind::PipeEq:  case TokenKind::CaretEq:
    case TokenKind::LShiftEq: case TokenKind::RShiftEq:
        return true;
    default: return false;
    }
}

ExprPtr Parser::parseExpr()          { return parseAssignment(); }

ExprPtr Parser::parseAssignment() {
    auto expr = parseTernary();

    if (isAssignOp(peek().kind)) {
        auto loc = peek().loc;
        auto op  = advance().kind;
        auto rhs = parseAssignment(); // right-associative
        return std::make_unique<AssignExpr>(op, std::move(expr), std::move(rhs), loc);
    }
    return expr;
}

ExprPtr Parser::parseTernary() {
    auto expr = parseOr();
    if (match(TokenKind::Question)) {
        auto loc      = tokens_[pos_-1].loc;
        auto thenExpr = parseExpr();
        expect(TokenKind::Colon, "expected ':' in ternary expression");
        auto elseExpr = parseTernary();
        return std::make_unique<TernaryExpr>(
            std::move(expr), std::move(thenExpr), std::move(elseExpr), loc);
    }
    return expr;
}

ExprPtr Parser::parseOr() {
    auto left = parseAnd();
    while (check(TokenKind::PipePipe)) {
        auto loc = peek().loc;
        auto op  = advance().kind;
        auto rhs = parseAnd();
        left = std::make_unique<BinaryExpr>(op, std::move(left), std::move(rhs), loc);
    }
    return left;
}

ExprPtr Parser::parseAnd() {
    auto left = parseBitOr();
    while (check(TokenKind::AmpAmp)) {
        auto loc = peek().loc;
        auto op  = advance().kind;
        auto rhs = parseBitOr();
        left = std::make_unique<BinaryExpr>(op, std::move(left), std::move(rhs), loc);
    }
    return left;
}

ExprPtr Parser::parseBitOr() {
    auto left = parseBitXor();
    while (check(TokenKind::Pipe)) {
        auto loc = peek().loc;
        auto op  = advance().kind;
        auto rhs = parseBitXor();
        left = std::make_unique<BinaryExpr>(op, std::move(left), std::move(rhs), loc);
    }
    return left;
}

ExprPtr Parser::parseBitXor() {
    auto left = parseBitAnd();
    while (check(TokenKind::Caret)) {
        auto loc = peek().loc;
        auto op  = advance().kind;
        auto rhs = parseBitAnd();
        left = std::make_unique<BinaryExpr>(op, std::move(left), std::move(rhs), loc);
    }
    return left;
}

ExprPtr Parser::parseBitAnd() {
    auto left = parseEquality();
    while (check(TokenKind::Amp)) {
        auto loc = peek().loc;
        auto op  = advance().kind;
        auto rhs = parseEquality();
        left = std::make_unique<BinaryExpr>(op, std::move(left), std::move(rhs), loc);
    }
    return left;
}

ExprPtr Parser::parseEquality() {
    auto left = parseRelational();
    while (check(TokenKind::EqualEqual) || check(TokenKind::BangEqual)) {
        auto loc = peek().loc;
        auto op  = advance().kind;
        auto rhs = parseRelational();
        left = std::make_unique<BinaryExpr>(op, std::move(left), std::move(rhs), loc);
    }
    return left;
}

ExprPtr Parser::parseRelational() {
    auto left = parseShift();
    while (check(TokenKind::Less) || check(TokenKind::LessEqual) ||
           check(TokenKind::Greater) || check(TokenKind::GreaterEqual) ||
           check(TokenKind::KW_instanceof) || check(TokenKind::KW_as))
    {
        auto loc = peek().loc;
        if (check(TokenKind::KW_instanceof)) {
            advance();
            auto type = parseType();
            left = std::make_unique<InstanceOfExpr>(std::move(left), std::move(type), loc);
        } else if (check(TokenKind::KW_as)) {
            advance();
            auto type = parseType();
            left = std::make_unique<CastExpr>(std::move(type), std::move(left), loc);
        } else {
            auto op  = advance().kind;
            auto rhs = parseShift();
            left = std::make_unique<BinaryExpr>(op, std::move(left), std::move(rhs), loc);
        }
    }
    return left;
}

ExprPtr Parser::parseShift() {
    auto left = parseAdditive();
    while (check(TokenKind::LShift) || check(TokenKind::RShift)) {
        auto loc = peek().loc;
        auto op  = advance().kind;
        auto rhs = parseAdditive();
        left = std::make_unique<BinaryExpr>(op, std::move(left), std::move(rhs), loc);
    }
    return left;
}

ExprPtr Parser::parseAdditive() {
    auto left = parseMultiplicative();
    while (check(TokenKind::Plus) || check(TokenKind::Minus)) {
        auto loc = peek().loc;
        auto op  = advance().kind;
        auto rhs = parseMultiplicative();
        left = std::make_unique<BinaryExpr>(op, std::move(left), std::move(rhs), loc);
    }
    return left;
}

ExprPtr Parser::parseMultiplicative() {
    auto left = parseUnary();
    while (check(TokenKind::Star) || check(TokenKind::Slash) ||
           check(TokenKind::Percent))
    {
        auto loc = peek().loc;
        auto op  = advance().kind;
        auto rhs = parseUnary();
        left = std::make_unique<BinaryExpr>(op, std::move(left), std::move(rhs), loc);
    }
    return left;
}

ExprPtr Parser::parseUnary() {
    auto loc = peek().loc;

    // prefix operators
    switch (peek().kind) {
    case TokenKind::Bang:      { advance(); auto op=TokenKind::Bang;      auto e=parseUnary(); return std::make_unique<UnaryExpr>(op,false,std::move(e),loc); }
    case TokenKind::Minus:     { advance(); auto op=TokenKind::Minus;     auto e=parseUnary(); return std::make_unique<UnaryExpr>(op,false,std::move(e),loc); }
    case TokenKind::Tilde:     { advance(); auto op=TokenKind::Tilde;     auto e=parseUnary(); return std::make_unique<UnaryExpr>(op,false,std::move(e),loc); }
    case TokenKind::PlusPlus:  { advance(); auto op=TokenKind::PlusPlus;  auto e=parseUnary(); return std::make_unique<UnaryExpr>(op,false,std::move(e),loc); }
    case TokenKind::MinusMinus:{ advance(); auto op=TokenKind::MinusMinus;auto e=parseUnary(); return std::make_unique<UnaryExpr>(op,false,std::move(e),loc); }

    case TokenKind::KW_typeof: {
        advance();
        auto e = parseUnary();
        return std::make_unique<TypeOfExpr>(std::move(e), loc);
    }
    case TokenKind::KW_await: {
        // await expr  –  treat as a unary prefix operator
        advance();
        auto e = parseUnary();
        return std::make_unique<UnaryExpr>(TokenKind::KW_await, false, std::move(e), loc);
    }
    case TokenKind::KW_delete: {
        advance();
        auto e = parseUnary();
        return std::make_unique<DeleteExpr>(std::move(e), loc);
    }
    case TokenKind::KW_new: {
        advance();
        TypeRef t = parseType();
        // new Type[n]  — array allocation
        if (match(TokenKind::LBracket)) {
            auto sz = parseExpr();
            expect(TokenKind::RBracket, "expected ']' in array allocation");
            return std::make_unique<NewExpr>(std::move(t), std::move(sz), loc);
        }
        // new Type(args)  — object construction
        expect(TokenKind::LParen, "expected '(' after type in 'new' expression");
        auto args = parseArgList();
        expect(TokenKind::RParen, "expected ')' to close 'new' argument list");
        return std::make_unique<NewExpr>(std::move(t), std::move(args), loc);
    }
    default:
        break;
    }

    return parsePostfix();
}

ExprPtr Parser::parsePostfix() {
    auto expr = parsePrimary();

    while (true) {
        auto loc = peek().loc;

        if (check(TokenKind::PlusPlus)) {
            advance();
            expr = std::make_unique<UnaryExpr>(TokenKind::PlusPlus, true, std::move(expr), loc);
        } else if (check(TokenKind::MinusMinus)) {
            advance();
            expr = std::make_unique<UnaryExpr>(TokenKind::MinusMinus, true, std::move(expr), loc);
        } else if (match(TokenKind::Dot)) {
            if (!check(TokenKind::Identifier)) {
                error("expected member name after '.'");
                break;
            }
            std::string member = advance().lexeme;
            expr = std::make_unique<MemberExpr>(std::move(expr), std::move(member), false, loc);
        } else if (match(TokenKind::Arrow)) {
            if (!check(TokenKind::Identifier)) {
                error("expected member name after '->'");
                break;
            }
            std::string member = advance().lexeme;
            expr = std::make_unique<MemberExpr>(std::move(expr), std::move(member), true, loc);
        } else if (match(TokenKind::LParen)) {
            // call
            auto args = parseArgList();
            expect(TokenKind::RParen, "expected ')' to close argument list");
            expr = std::make_unique<CallExpr>(std::move(expr), std::move(args), loc);
        } else if (match(TokenKind::LBracket)) {
            // index
            auto idx = parseExpr();
            expect(TokenKind::RBracket, "expected ']' after index expression");
            expr = std::make_unique<IndexExpr>(std::move(expr), std::move(idx), loc);
        } else if (check(TokenKind::DoubleColon)) {
            // scope resolution: treat as binary op (e.g. Direction::NORTH)
            auto scopeLoc = peek().loc;
            advance();
            if (!check(TokenKind::Identifier)) {
                error("expected identifier after '::'");
                break;
            }
            std::string member = advance().lexeme;
            // Represent as a member expression with arrow=false, member="::NAME"
            // (The type-checker will resolve scope vs member access)
            expr = std::make_unique<MemberExpr>(std::move(expr), "::" + member, false, scopeLoc);
        } else {
            break;
        }
    }

    return expr;
}

ExprPtr Parser::parsePrimary() {
    auto loc = peek().loc;

    switch (peek().kind) {
    // ── literals ─────────────────────────────────────────────────────────────
    case TokenKind::LiteralInteger: {
        auto raw = advance().lexeme;
        // Parse raw value (strip underscores, handle prefixes)
        std::string clean;
        for (char c : raw) if (c != '_') clean += c;
        int64_t val = 0;
        if (clean.size() > 2 && clean[0] == '0' && (clean[1]=='x'||clean[1]=='X'))
            val = std::stoll(clean, nullptr, 16);
        else if (clean.size() > 2 && clean[0] == '0' && (clean[1]=='b'||clean[1]=='B'))
            val = std::stoll(clean.substr(2), nullptr, 2);
        else if (clean.size() > 2 && clean[0] == '0' && (clean[1]=='o'||clean[1]=='O'))
            val = std::stoll(clean.substr(2), nullptr, 8);
        else {
            // strip trailing suffixes
            std::size_t end = clean.size();
            while (end > 0 && (clean[end-1]=='u'||clean[end-1]=='U'||
                               clean[end-1]=='l'||clean[end-1]=='L')) --end;
            val = std::stoll(clean.substr(0, end));
        }
        return std::make_unique<IntLitExpr>(std::move(raw), val, loc);
    }
    case TokenKind::LiteralFloat: {
        auto raw = advance().lexeme;
        std::string clean;
        for (char c : raw) if (c != '_') clean += c;
        // strip trailing suffixes
        while (!clean.empty() && (clean.back()=='f'||clean.back()=='F')) clean.pop_back();
        double val = std::stod(clean);
        return std::make_unique<FloatLitExpr>(raw, val, loc);
    }
    case TokenKind::LiteralTrue:
        advance();
        return std::make_unique<BoolLitExpr>(true, loc);
    case TokenKind::LiteralFalse:
        advance();
        return std::make_unique<BoolLitExpr>(false, loc);
    case TokenKind::LiteralNull:
        advance();
        return std::make_unique<NullLitExpr>(loc);
    case TokenKind::LiteralChar:
        return std::make_unique<CharLitExpr>(advance().lexeme, loc);
    case TokenKind::LiteralString: {
        // Strip surrounding quotes and resolve escape sequences
        auto raw = advance().lexeme;
        std::string inner = raw.substr(1, raw.size() - 2);
        std::string val;
        for (size_t i = 0; i < inner.size(); ++i) {
            if (inner[i] != '\\' || i + 1 >= inner.size()) {
                val += inner[i];
                continue;
            }
            ++i; // consume backslash
            char esc = inner[i];
            switch (esc) {
            case 'n':  val += '\n'; break;
            case 't':  val += '\t'; break;
            case 'r':  val += '\r'; break;
            case '0':  val += '\0'; break;
            case '\\': val += '\\'; break;
            case '"':  val += '"'; break;
            case '\''  : val += '\''; break;
            case 'a':  val += '\a'; break;
            case 'b':  val += '\b'; break;
            case 'f':  val += '\f'; break;
            case 'v':  val += '\v'; break;
            case 'x': {
                // \xHH — hex escape: consume up to 2 hex digits
                unsigned int code = 0;
                int digits = 0;
                while (digits < 2 && i + 1 < inner.size() &&
                       std::isxdigit((unsigned char)inner[i + 1])) {
                    ++i;
                    char h = inner[i];
                    code = code * 16 + (std::isdigit((unsigned char)h)
                                        ? h - '0'
                                        : std::tolower((unsigned char)h) - 'a' + 10);
                    ++digits;
                }
                val += (char)(unsigned char)code;
                break;
            }
            case 'u': {
                // \uXXXX — Unicode code point (BMP only), encoded as UTF-8
                unsigned int code = 0;
                for (int d = 0; d < 4 && i + 1 < inner.size(); ++d) {
                    char h = inner[++i];
                    code = code * 16 + (std::isdigit((unsigned char)h)
                                        ? h - '0'
                                        : std::tolower((unsigned char)h) - 'a' + 10);
                }
                if (code < 0x80) {
                    val += (char)code;
                } else if (code < 0x800) {
                    val += (char)(0xC0 | (code >> 6));
                    val += (char)(0x80 | (code & 0x3F));
                } else {
                    val += (char)(0xE0 | (code >> 12));
                    val += (char)(0x80 | ((code >> 6) & 0x3F));
                    val += (char)(0x80 | (code & 0x3F));
                }
                break;
            }
            default:
                // Unknown escape: keep as-is
                val += '\\';
                val += esc;
                break;
            }
        }
        return std::make_unique<StringLitExpr>(std::move(val), loc);
    }

    // ── identifier ────────────────────────────────────────────────────────────
    case TokenKind::Identifier:
        return std::make_unique<IdentExpr>(advance().lexeme, loc);

    // ── this / super ──────────────────────────────────────────────────────────
    case TokenKind::KW_this:
        advance();
        return std::make_unique<IdentExpr>("this", loc);
    case TokenKind::KW_super:
        advance();
        return std::make_unique<IdentExpr>("super", loc);

    // ── grouped expression / lambda ───────────────────────────────────────────
    case TokenKind::LParen: {
        advance(); // consume '('

        // Lambda detection: look for  (Type name, …) =>
        // Heuristic: if inside we find  Type Identifier  then ')' '=>'
        // We'll try to parse as lambda; backtrack if it fails via saved pos.
        bool isLambda = false;
        {
            // Quick look-ahead: check if pattern is (type id [, type id]* ) =>
            std::size_t lookahead = pos_;
            auto checkParam = [&]() -> bool {
                // skip type — includes 'var' as a valid parameter type
                if (lookahead >= tokens_.size()) return false;
                auto& tk = tokens_[lookahead];
                if (!isBuiltinType(tk.kind) && tk.kind != TokenKind::Identifier
                    && tk.kind != TokenKind::KW_var) return false;
                ++lookahead;
                // optional array
                if (lookahead+1 < tokens_.size() &&
                    tokens_[lookahead].kind == TokenKind::LBracket &&
                    tokens_[lookahead+1].kind == TokenKind::RBracket)
                    lookahead += 2;
                // name
                if (lookahead >= tokens_.size() || tokens_[lookahead].kind != TokenKind::Identifier)
                    return false;
                ++lookahead;
                return true;
            };
            // check first param
            bool ok = checkParam();
            if (ok) {
                while (ok && lookahead < tokens_.size() && tokens_[lookahead].kind == TokenKind::Comma) {
                    ++lookahead; ok = checkParam();
                }
                if (ok && lookahead < tokens_.size() &&
                    tokens_[lookahead].kind == TokenKind::RParen &&
                    lookahead+1 < tokens_.size() &&
                    tokens_[lookahead+1].kind == TokenKind::FatArrow)
                {
                    isLambda = true;
                }
            }
            // also check zero-param lambda  () =>
            if (!isLambda && pos_ < tokens_.size() &&
                tokens_[pos_].kind == TokenKind::RParen &&
                pos_+1 < tokens_.size() &&
                tokens_[pos_+1].kind == TokenKind::FatArrow)
            {
                isLambda = true;
            }
        }

        if (isLambda) {
            auto params = parseParamList();
            expect(TokenKind::RParen, "expected ')' in lambda");
            expect(TokenKind::FatArrow, "expected '=>' in lambda");
            // body: block or single expression
            if (check(TokenKind::LBrace)) {
                auto block = parseBlock();
                return std::make_unique<LambdaExpr>(std::move(params), std::move(block), loc);
            }
            auto body = parseExpr();
            return std::make_unique<LambdaExpr>(std::move(params), std::move(body), loc);
        }

        // regular grouped expression
        auto expr = parseExpr();
        expect(TokenKind::RParen, "expected ')' after grouped expression");
        return expr;
    }

    // ── Array literal  { expr, expr, ... } ───────────────────────────────────
    case TokenKind::LBrace: {
        advance(); // consume '{'
        std::vector<ExprPtr> elems;
        if (!check(TokenKind::RBrace)) {
            elems.push_back(parseExpr());
            while (match(TokenKind::Comma) && !check(TokenKind::RBrace))
                elems.push_back(parseExpr());
        }
        expect(TokenKind::RBrace, "expected '}' to close array literal");
        return std::make_unique<ArrayLitExpr>(std::move(elems), loc);
    }

    default:
        error("unexpected token '" + peek().lexeme + "' in expression");
        advance(); // consume bad token so we don't loop forever
        // Return a placeholder null literal so callers have something valid
        return std::make_unique<NullLitExpr>(loc);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  using  Name = Type;
// ─────────────────────────────────────────────────────────────────────────────
DeclPtr Parser::parseUsing() {
    auto loc = peek().loc;
    bool isExport = false;
    if (match(TokenKind::KW_export)) isExport = true;
    expect(TokenKind::KW_using, "expected 'using'");

    if (!check(TokenKind::Identifier)) {
        error("expected alias name after 'using'");
        synchronize();
        return nullptr;
    }
    std::string alias = advance().lexeme;

    expect(TokenKind::Equal, "expected '=' in using declaration");

    if (!isTypeName()) {
        error("expected a type name in using declaration");
        synchronize();
        return nullptr;
    }
    TypeRef target = parseType();
    expect(TokenKind::Semi, "expected ';' after using declaration");

    auto ud = std::make_unique<UsingDecl>(std::move(alias), std::move(target), loc);
    ud->isExport = isExport;
    return ud;
}

// ─────────────────────────────────────────────────────────────────────────────
//  with (Type name = init) { body }
//  Desugars to:  Type name = init; try { body } finally { name.close(); }
// ─────────────────────────────────────────────────────────────────────────────
StmtPtr Parser::parseWith() {
    auto loc = peek().loc;
    expect(TokenKind::KW_with, "expected 'with'");
    expect(TokenKind::LParen, "expected '(' after 'with'");

    if (!isTypeName()) {
        error("expected resource type in 'with'");
        synchronize();
        return nullptr;
    }
    TypeRef resType = parseType();
    if (!check(TokenKind::Identifier)) {
        error("expected resource name in 'with'");
        synchronize();
        return nullptr;
    }
    std::string resName = advance().lexeme;
    expect(TokenKind::Equal, "expected '=' in 'with' declaration");
    auto init = parseExpr();
    expect(TokenKind::RParen, "expected ')' after with resource");
    auto body = parseBlock();

    return std::make_unique<WithStmt>(
        std::move(resType), std::move(resName),
        std::move(init), std::move(body), loc);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Argument list  expr (, expr)*
// ─────────────────────────────────────────────────────────────────────────────
std::vector<ExprPtr> Parser::parseArgList() {
    std::vector<ExprPtr> args;
    if (check(TokenKind::RParen)) return args;
    do {
        args.push_back(parseExpr());
    } while (match(TokenKind::Comma));
    return args;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Access modifier
// ─────────────────────────────────────────────────────────────────────────────
AccessMod Parser::parseAccessMod() {
    if (match(TokenKind::KW_public))    return AccessMod::Public;
    if (match(TokenKind::KW_private))   return AccessMod::Private;
    if (match(TokenKind::KW_protected)) return AccessMod::Protected;
    return AccessMod::None;
}
