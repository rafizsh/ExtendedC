#pragma once
//===----------------------------------------------------------------------===//
//  Lexer.h  –  Lexer for the custom language (LLVM/Clang style)
//===----------------------------------------------------------------------===//
#ifndef LEXER_H
#define LEXER_H

#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <cstdint>

// ─────────────────────────────────────────────────────────────────────────────
//  Source location
// ─────────────────────────────────────────────────────────────────────────────
struct SourceLocation {
    uint32_t line   = 1;
    uint32_t column = 1;
    std::string filename;
};

// ─────────────────────────────────────────────────────────────────────────────
//  Token kinds
// ─────────────────────────────────────────────────────────────────────────────
enum class TokenKind : uint16_t {

    // ── End / error ──────────────────────────────────────────────────────────
    Eof = 0,
    Unknown,
    Error,

    // ── Literals ─────────────────────────────────────────────────────────────
    LiteralInteger,      // 42   0xFF   0b1010   0o77
    LiteralFloat,        // 3.14  1.0e-5
    LiteralChar,         // 'a'
    LiteralString,       // "hello"
    LiteralTrue,         // true
    LiteralFalse,        // false
    LiteralNull,         // null

    // ── Identifier ───────────────────────────────────────────────────────────
    Identifier,

    // ══ Keywords ═════════════════════════════════════════════════════════════

    // primitive types
    KW_bool,
    KW_char,
    KW_int,
    KW_uint,
    KW_long,
    KW_ulong,
    KW_float,
    KW_ufloat,
    KW_double,
    KW_string,
    KW_void,

    // derived / structural types
    KW_enum,
    KW_struct,      // structure
    KW_union,
    KW_class,

    // function declaration keyword
    KW_fun,

    // control flow
    KW_if,
    KW_else,
    KW_for,
    KW_while,
    KW_do,
    KW_switch,
    KW_case,
    KW_default,
    KW_break,
    KW_continue,
    KW_return,

    // exception handling
    KW_try,
    KW_catch,
    KW_throw,
    KW_throws,

    // OOP / modifiers
    KW_new,
    KW_delete,
    KW_this,
    KW_super,
    KW_extends,
    KW_final,
    KW_static,
    KW_const,
    KW_var,

    // access modifiers
    KW_public,
    KW_private,
    KW_protected,

    // async
    KW_async,
    KW_await,

    // modules
    KW_import,
    KW_export,
    KW_using,
    KW_with,

    // misc
    KW_typeof,
    KW_instanceof,
    KW_as,          // cast:  expr as Type

    // ══ Punctuation & operators ═══════════════════════════════════════════════

    // delimiters
    LParen,         // (
    RParen,         // )
    LBrace,         // {
    RBrace,         // }
    LBracket,       // [
    RBracket,       // ]
    Semi,           // ;
    Colon,          // :
    DoubleColon,    // ::
    Comma,          // ,
    Dot,            // .
    Ellipsis,       // ...
    Arrow,          // ->
    FatArrow,       // =>
    Question,       // ?

    // assignment
    Equal,          // =
    PlusEq,         // +=
    MinusEq,        // -=
    StarEq,         // *=
    SlashEq,        // /=
    PercentEq,      // %=
    AmpEq,          // &=
    PipeEq,         // |=
    CaretEq,        // ^=
    LShiftEq,       // <<=
    RShiftEq,       // >>=

    // arithmetic
    Plus,           // +
    Minus,          // -
    Star,           // *
    Slash,          // /
    Percent,        // %

    // increment / decrement
    PlusPlus,       // ++
    MinusMinus,     // --

    // relational
    EqualEqual,     // ==
    BangEqual,      // !=
    Less,           // <
    LessEqual,      // <=
    Greater,        // >
    GreaterEqual,   // >=

    // logical
    AmpAmp,         // &&
    PipePipe,       // ||
    Bang,           // !

    // bitwise
    Amp,            // &
    Pipe,           // |
    Caret,          // ^
    Tilde,          // ~
    LShift,         // <<
    RShift,         // >>

    // misc operators
    At,             // @  (annotation / attribute marker)
    Hash,           // #  (preprocessor / macro)
};

// ─────────────────────────────────────────────────────────────────────────────
//  Token
// ─────────────────────────────────────────────────────────────────────────────
struct Token {
    TokenKind      kind   = TokenKind::Unknown;
    std::string    lexeme;          // raw text
    SourceLocation loc;

    // helpers
    bool is(TokenKind k)        const { return kind == k; }
    bool isKeyword()            const {
        return kind >= TokenKind::KW_bool && kind <= TokenKind::KW_as;
    }
    bool isLiteral()            const {
        return kind >= TokenKind::LiteralInteger && kind <= TokenKind::LiteralNull;
    }
    bool isEof()                const { return kind == TokenKind::Eof; }

    std::string kindName() const;   // implemented in Lexer.cpp
};

// ─────────────────────────────────────────────────────────────────────────────
//  Lexer
// ─────────────────────────────────────────────────────────────────────────────
class Lexer {
public:
    /// Construct from source text (the string must outlive the Lexer).
    explicit Lexer(std::string_view source,
                   std::string      filename = "<stdin>");

    /// Lex the entire source and return a flat token stream ending with Eof.
    std::vector<Token> tokenize();

    /// Lex and return the next single token (call repeatedly).
    Token nextToken();

    /// Peek without advancing.
    Token peekToken();

    bool atEnd() const { return pos_ >= src_.size(); }

private:
    // ── internal helpers ─────────────────────────────────────────────────────
    char peek(std::size_t offset = 0) const;
    char advance();
    bool match(char expected);
    bool matchStr(std::string_view s);

    void skipWhitespaceAndComments();

    Token makeToken(TokenKind kind, std::string lexeme = "");
    Token errorToken(const std::string& msg);

    Token lexIdentifierOrKeyword();
    Token lexNumber();
    Token lexChar();
    Token lexString();
    Token lexOperatorOrPunct();

    SourceLocation currentLoc() const;
    void newline();

    // ── state ────────────────────────────────────────────────────────────────
    std::string_view src_;
    std::size_t      pos_  = 0;
    uint32_t         line_ = 1;
    uint32_t         col_  = 1;
    std::string      filename_;

    // keyword lookup table (built once in constructor)
    static const std::unordered_map<std::string, TokenKind> keywords_;
};

#endif // LEXER_H
