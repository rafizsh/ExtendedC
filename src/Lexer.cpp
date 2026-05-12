//===----------------------------------------------------------------------===//
//  Lexer.cpp  –  Implementation of the lexer for the custom language
//===----------------------------------------------------------------------===//
#include "Lexer.h"

#include <cassert>
#include <cctype>
#include <stdexcept>
#include <sstream>

// ─────────────────────────────────────────────────────────────────────────────
//  Keyword table
// ─────────────────────────────────────────────────────────────────────────────
const std::unordered_map<std::string, TokenKind> Lexer::keywords_ = {
    // primitive types
    {"bool",      TokenKind::KW_bool},
    {"char",      TokenKind::KW_char},
    {"int",       TokenKind::KW_int},
    {"uint",      TokenKind::KW_uint},
    {"long",      TokenKind::KW_long},
    {"ulong",     TokenKind::KW_ulong},
    {"float",     TokenKind::KW_float},
    {"ufloat",    TokenKind::KW_ufloat},
    {"double",    TokenKind::KW_double},
    {"string",    TokenKind::KW_string},
    {"void",      TokenKind::KW_void},

    // derived/structural
    {"enum",      TokenKind::KW_enum},
    {"struct",    TokenKind::KW_struct},
    {"union",     TokenKind::KW_union},
    {"class",     TokenKind::KW_class},

    // function keyword
    {"fun",       TokenKind::KW_fun},

    // control flow
    {"if",        TokenKind::KW_if},
    {"else",      TokenKind::KW_else},
    {"for",       TokenKind::KW_for},
    {"while",     TokenKind::KW_while},
    {"do",        TokenKind::KW_do},
    {"switch",    TokenKind::KW_switch},
    {"case",      TokenKind::KW_case},
    {"default",   TokenKind::KW_default},
    {"break",     TokenKind::KW_break},
    {"continue",  TokenKind::KW_continue},
    {"return",    TokenKind::KW_return},

    // exception handling
    {"try",       TokenKind::KW_try},
    {"catch",     TokenKind::KW_catch},
    {"throw",     TokenKind::KW_throw},
    {"throws",    TokenKind::KW_throws},

    // OOP / modifiers
    {"new",       TokenKind::KW_new},
    {"delete",    TokenKind::KW_delete},
    {"this",      TokenKind::KW_this},
    {"super",     TokenKind::KW_super},
    {"extends",   TokenKind::KW_extends},
    {"final",     TokenKind::KW_final},
    {"static",    TokenKind::KW_static},
    {"const",     TokenKind::KW_const},
    {"var",       TokenKind::KW_var},

    // access modifiers
    {"public",    TokenKind::KW_public},
    {"private",   TokenKind::KW_private},
    {"protected", TokenKind::KW_protected},

    // async
    {"async",     TokenKind::KW_async},
    {"await",     TokenKind::KW_await},

    // modules
    {"import",    TokenKind::KW_import},
    {"export",    TokenKind::KW_export},
    {"using",     TokenKind::KW_using},
    {"with",      TokenKind::KW_with},

    // misc
    {"typeof",    TokenKind::KW_typeof},
    {"instanceof",TokenKind::KW_instanceof},
    {"as",        TokenKind::KW_as},

    // boolean / null literals (handled as keywords for clean AST later)
    {"true",      TokenKind::LiteralTrue},
    {"false",     TokenKind::LiteralFalse},
    {"null",      TokenKind::LiteralNull},
};

// ─────────────────────────────────────────────────────────────────────────────
//  Token::kindName
// ─────────────────────────────────────────────────────────────────────────────
std::string Token::kindName() const {
    switch (kind) {
#define CASE(k) case TokenKind::k: return #k
        CASE(Eof); CASE(Unknown); CASE(Error);
        CASE(LiteralInteger); CASE(LiteralFloat);
        CASE(LiteralChar); CASE(LiteralString);
        CASE(LiteralTrue); CASE(LiteralFalse); CASE(LiteralNull);
        CASE(Identifier);
        CASE(KW_bool); CASE(KW_char); CASE(KW_int); CASE(KW_uint);
        CASE(KW_long); CASE(KW_ulong); CASE(KW_float); CASE(KW_ufloat);
        CASE(KW_double); CASE(KW_string); CASE(KW_void);
        CASE(KW_enum); CASE(KW_struct); CASE(KW_union); CASE(KW_class);
        CASE(KW_fun);
        CASE(KW_if); CASE(KW_else); CASE(KW_for); CASE(KW_while);
        CASE(KW_do); CASE(KW_switch); CASE(KW_case); CASE(KW_default);
        CASE(KW_break); CASE(KW_continue); CASE(KW_return);
        CASE(KW_try); CASE(KW_catch); CASE(KW_throw); CASE(KW_throws);
        CASE(KW_new); CASE(KW_delete); CASE(KW_this); CASE(KW_super);
        CASE(KW_extends); CASE(KW_final); CASE(KW_static); CASE(KW_const);
        CASE(KW_var);
        CASE(KW_public); CASE(KW_private); CASE(KW_protected);
        CASE(KW_async); CASE(KW_await);
        CASE(KW_import); CASE(KW_export); CASE(KW_using); CASE(KW_with);
        CASE(KW_typeof); CASE(KW_instanceof); CASE(KW_as);
        CASE(LParen); CASE(RParen); CASE(LBrace); CASE(RBrace);
        CASE(LBracket); CASE(RBracket);
        CASE(Semi); CASE(Colon); CASE(DoubleColon); CASE(Comma);
        CASE(Dot); CASE(Ellipsis); CASE(Arrow); CASE(FatArrow); CASE(Question);
        CASE(Equal); CASE(PlusEq); CASE(MinusEq); CASE(StarEq);
        CASE(SlashEq); CASE(PercentEq); CASE(AmpEq); CASE(PipeEq);
        CASE(CaretEq); CASE(LShiftEq); CASE(RShiftEq);
        CASE(Plus); CASE(Minus); CASE(Star); CASE(Slash); CASE(Percent);
        CASE(PlusPlus); CASE(MinusMinus);
        CASE(EqualEqual); CASE(BangEqual); CASE(Less); CASE(LessEqual);
        CASE(Greater); CASE(GreaterEqual);
        CASE(AmpAmp); CASE(PipePipe); CASE(Bang);
        CASE(Amp); CASE(Pipe); CASE(Caret); CASE(Tilde);
        CASE(LShift); CASE(RShift);
        CASE(At); CASE(Hash);
#undef CASE
        default: return "??";
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Lexer – constructor
// ─────────────────────────────────────────────────────────────────────────────
Lexer::Lexer(std::string_view source, std::string filename)
    : src_(source), filename_(std::move(filename)) {}

// ─────────────────────────────────────────────────────────────────────────────
//  Public API
// ─────────────────────────────────────────────────────────────────────────────
std::vector<Token> Lexer::tokenize() {
    std::vector<Token> tokens;
    while (true) {
        Token t = nextToken();
        tokens.push_back(t);
        if (t.isEof() || t.is(TokenKind::Error)) break;
    }
    return tokens;
}

Token Lexer::nextToken() {
    skipWhitespaceAndComments();

    if (atEnd())
        return makeToken(TokenKind::Eof, "<eof>");

    char c = peek();

    if (std::isalpha(static_cast<unsigned char>(c)) || c == '_')
        return lexIdentifierOrKeyword();

    if (std::isdigit(static_cast<unsigned char>(c)))
        return lexNumber();

    if (c == '\'')
        return lexChar();

    if (c == '"')
        return lexString();

    return lexOperatorOrPunct();
}

Token Lexer::peekToken() {
    // Save state
    auto savedPos  = pos_;
    auto savedLine = line_;
    auto savedCol  = col_;

    Token t = nextToken();

    // Restore state
    pos_  = savedPos;
    line_ = savedLine;
    col_  = savedCol;

    return t;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Internal helpers
// ─────────────────────────────────────────────────────────────────────────────
char Lexer::peek(std::size_t offset) const {
    if (pos_ + offset >= src_.size()) return '\0';
    return src_[pos_ + offset];
}

char Lexer::advance() {
    char c = src_[pos_++];
    if (c == '\n') { ++line_; col_ = 1; }
    else            { ++col_; }
    return c;
}

bool Lexer::match(char expected) {
    if (atEnd() || src_[pos_] != expected) return false;
    advance();
    return true;
}

bool Lexer::matchStr(std::string_view s) {
    if (pos_ + s.size() > src_.size()) return false;
    if (src_.substr(pos_, s.size()) != s) return false;
    for (std::size_t i = 0; i < s.size(); ++i) advance();
    return true;
}

SourceLocation Lexer::currentLoc() const {
    return {line_, col_, filename_};
}

Token Lexer::makeToken(TokenKind kind, std::string lexeme) {
    Token t;
    t.kind   = kind;
    t.lexeme = std::move(lexeme);
    t.loc    = currentLoc();
    return t;
}

Token Lexer::errorToken(const std::string& msg) {
    Token t;
    t.kind   = TokenKind::Error;
    t.lexeme = msg;
    t.loc    = currentLoc();
    return t;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Skip whitespace and comments
//  Supports:
//    //  single-line
//    /*  */ block (non-nestable)
//    /** */ doc-comment (non-nestable, stored as token in the future)
// ─────────────────────────────────────────────────────────────────────────────
void Lexer::skipWhitespaceAndComments() {
    while (!atEnd()) {
        char c = peek();

        // whitespace
        if (std::isspace(static_cast<unsigned char>(c))) {
            advance();
            continue;
        }

        // single-line comment
        if (c == '/' && peek(1) == '/') {
            while (!atEnd() && peek() != '\n') advance();
            continue;
        }

        // block comment  /* ... */
        if (c == '/' && peek(1) == '*') {
            advance(); advance(); // consume '/' '*'
            while (!atEnd()) {
                if (peek() == '*' && peek(1) == '/') {
                    advance(); advance(); // consume '*' '/'
                    break;
                }
                advance();
            }
            continue;
        }

        break;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Lex identifier or keyword
// ─────────────────────────────────────────────────────────────────────────────
Token Lexer::lexIdentifierOrKeyword() {
    SourceLocation loc = currentLoc();
    std::string    lex;

    while (!atEnd()) {
        char c = peek();
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_')
            lex += advance();
        else
            break;
    }

    // keyword lookup
    auto it = keywords_.find(lex);
    TokenKind kind = (it != keywords_.end()) ? it->second : TokenKind::Identifier;

    Token t;
    t.kind   = kind;
    t.lexeme = lex;
    t.loc    = loc;
    return t;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Lex number literal
//  Supports:
//    decimal:  123  123_456
//    hex:      0xFF  0XFF
//    binary:   0b1010  0B1010
//    octal:    0o77  0O77
//    float:    3.14  1.0e+5  1.0E-5f
//    suffixes: u  l  ul  f  (case-insensitive)
// ─────────────────────────────────────────────────────────────────────────────
Token Lexer::lexNumber() {
    SourceLocation loc = currentLoc();
    std::string    lex;
    bool           isFloat = false;

    auto isHexDigit = [](char c) {
        return std::isdigit(static_cast<unsigned char>(c))
            || (c >= 'a' && c <= 'f')
            || (c >= 'A' && c <= 'F');
    };

    // prefix detection
    if (peek() == '0') {
        lex += advance(); // '0'
        char next = peek();

        if (next == 'x' || next == 'X') {
            lex += advance();
            if (!isHexDigit(peek()))
                return errorToken("invalid hex literal");
            while (!atEnd() && (isHexDigit(peek()) || peek() == '_'))
                lex += advance();
        } else if (next == 'b' || next == 'B') {
            lex += advance();
            if (peek() != '0' && peek() != '1')
                return errorToken("invalid binary literal");
            while (!atEnd() && (peek() == '0' || peek() == '1' || peek() == '_'))
                lex += advance();
        } else if (next == 'o' || next == 'O') {
            lex += advance();
            while (!atEnd() && ((peek() >= '0' && peek() <= '7') || peek() == '_'))
                lex += advance();
        } else {
            // plain decimal starting with 0, or just 0
            while (!atEnd() && (std::isdigit(static_cast<unsigned char>(peek())) || peek() == '_'))
                lex += advance();
        }
    } else {
        // decimal
        while (!atEnd() && (std::isdigit(static_cast<unsigned char>(peek())) || peek() == '_'))
            lex += advance();
    }

    // fractional part
    if (peek() == '.' && std::isdigit(static_cast<unsigned char>(peek(1)))) {
        isFloat = true;
        lex += advance(); // '.'
        while (!atEnd() && (std::isdigit(static_cast<unsigned char>(peek())) || peek() == '_'))
            lex += advance();
    }

    // exponent
    if (peek() == 'e' || peek() == 'E') {
        isFloat = true;
        lex += advance();
        if (peek() == '+' || peek() == '-') lex += advance();
        if (!std::isdigit(static_cast<unsigned char>(peek())))
            return errorToken("invalid exponent in numeric literal");
        while (!atEnd() && std::isdigit(static_cast<unsigned char>(peek())))
            lex += advance();
    }

    // suffixes (u, l, ul, ull, f)
    while (!atEnd()) {
        char s = static_cast<char>(std::tolower(static_cast<unsigned char>(peek())));
        if (s == 'u' || s == 'l' || s == 'f') { lex += advance(); if (s == 'f') isFloat = true; }
        else break;
    }

    Token t;
    t.kind   = isFloat ? TokenKind::LiteralFloat : TokenKind::LiteralInteger;
    t.lexeme = lex;
    t.loc    = loc;
    return t;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Lex char literal  'x'  '\n'  '\u0041'
// ─────────────────────────────────────────────────────────────────────────────
Token Lexer::lexChar() {
    SourceLocation loc = currentLoc();
    std::string    lex;
    lex += advance(); // opening '

    if (atEnd() || peek() == '\n')
        return errorToken("unterminated char literal");

    if (peek() == '\\') {
        lex += advance();
        if (atEnd()) return errorToken("unterminated escape in char literal");
        lex += advance(); // escape char
    } else {
        lex += advance(); // the character
    }

    if (!match('\''))
        return errorToken("unterminated char literal (expected closing ')");

    lex += '\'';

    Token t;
    t.kind   = TokenKind::LiteralChar;
    t.lexeme = lex;
    t.loc    = loc;
    return t;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Lex string literal  "..."  with escape sequences
// ─────────────────────────────────────────────────────────────────────────────
Token Lexer::lexString() {
    SourceLocation loc = currentLoc();
    std::string    lex;
    lex += advance(); // opening "

    while (!atEnd() && peek() != '"') {
        if (peek() == '\n')
            return errorToken("unterminated string literal (newline in string)");

        if (peek() == '\\') {
            lex += advance(); // backslash
            if (atEnd()) return errorToken("unterminated escape sequence");
            char esc = advance();
            lex += esc;
            // handle \uXXXX
            if (esc == 'u') {
                for (int i = 0; i < 4 && !atEnd(); ++i)
                    lex += advance();
            }
        } else {
            lex += advance();
        }
    }

    if (atEnd())
        return errorToken("unterminated string literal");

    lex += advance(); // closing "

    Token t;
    t.kind   = TokenKind::LiteralString;
    t.lexeme = lex;
    t.loc    = loc;
    return t;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Lex operators and punctuation
// ─────────────────────────────────────────────────────────────────────────────
Token Lexer::lexOperatorOrPunct() {
    SourceLocation loc = currentLoc();
    char c = advance();
    std::string lex(1, c);

#define TOK(k)    do { Token t; t.kind=TokenKind::k; t.lexeme=lex; t.loc=loc; return t; } while(0)
#define TOK2(ch, yes, no) \
    if (match(ch)) { lex += (ch); TOK(yes); } TOK(no)

    switch (c) {
    // simple single-char
    case '(': TOK(LParen);
    case ')': TOK(RParen);
    case '{': TOK(LBrace);
    case '}': TOK(RBrace);
    case '[': TOK(LBracket);
    case ']': TOK(RBracket);
    case ';': TOK(Semi);
    case ',': TOK(Comma);
    case '~': TOK(Tilde);
    case '@': TOK(At);
    case '#': TOK(Hash);
    case '?': TOK(Question);

    // colon / double-colon
    case ':':
        TOK2(':', DoubleColon, Colon);

    // dot / ellipsis
    case '.':
        if (peek() == '.' && peek(1) == '.') {
            lex += advance(); lex += advance();
            TOK(Ellipsis);
        }
        TOK(Dot);

    // =  ==  =>
    case '=':
        if (match('=')) { lex += '='; TOK(EqualEqual); }
        if (match('>')) { lex += '>'; TOK(FatArrow); }
        TOK(Equal);

    // !  !=
    case '!':
        TOK2('=', BangEqual, Bang);

    // <  <=  <<=  <<
    case '<':
        if (match('<')) {
            lex += '<';
            if (match('=')) { lex += '='; TOK(LShiftEq); }
            TOK(LShift);
        }
        TOK2('=', LessEqual, Less);

    // >  >=  >>=  >>
    case '>':
        if (match('>')) {
            lex += '>';
            if (match('=')) { lex += '='; TOK(RShiftEq); }
            TOK(RShift);
        }
        TOK2('=', GreaterEqual, Greater);

    // +  ++  +=
    case '+':
        if (match('+')) { lex += '+'; TOK(PlusPlus); }
        TOK2('=', PlusEq, Plus);

    // -  --  -=  ->
    case '-':
        if (match('-')) { lex += '-'; TOK(MinusMinus); }
        if (match('>')) { lex += '>'; TOK(Arrow); }
        TOK2('=', MinusEq, Minus);

    // *  *=
    case '*':
        TOK2('=', StarEq, Star);

    // /  /=   (comments already consumed above)
    case '/':
        TOK2('=', SlashEq, Slash);

    // %  %=
    case '%':
        TOK2('=', PercentEq, Percent);

    // &  &&  &=
    case '&':
        if (match('&')) { lex += '&'; TOK(AmpAmp); }
        TOK2('=', AmpEq, Amp);

    // |  ||  |=
    case '|':
        if (match('|')) { lex += '|'; TOK(PipePipe); }
        TOK2('=', PipeEq, Pipe);

    // ^  ^=
    case '^':
        TOK2('=', CaretEq, Caret);

    default: {
        std::string msg = "unexpected character '";
        msg += c;
        msg += "'";
        return errorToken(msg);
    }
    }
#undef TOK
#undef TOK2
}
