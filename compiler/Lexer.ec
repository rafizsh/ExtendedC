// ============================================================================
//  Lexer.lang  --  ExtendedC port of the C++ Lexer
//
//  Produces a List of Token objects from a source string.
//  Token kinds are integer constants (mirrors C++ enum class TokenKind).
// ============================================================================

import "std.io";
import "std.string";
import "std.collections";

// ── TokenKind constants ───────────────────────────────────────────────────────
// These mirror the C++ enum class TokenKind exactly (by ordinal value).

class TokenKind {
    // End / error
    public int Eof            = 0;
    public int Unknown        = 1;
    public int Error          = 2;
    // Literals
    public int LiteralInteger = 3;
    public int LiteralFloat   = 4;
    public int LiteralChar    = 5;
    public int LiteralString  = 6;
    public int LiteralTrue    = 7;
    public int LiteralFalse   = 8;
    public int LiteralNull    = 9;
    // Identifier
    public int Identifier     = 10;
    // Keyword -- types
    public int KW_bool        = 11;
    public int KW_char        = 12;
    public int KW_int         = 13;
    public int KW_uint        = 14;
    public int KW_long        = 15;
    public int KW_ulong       = 16;
    public int KW_float       = 17;
    public int KW_ufloat      = 18;
    public int KW_double      = 19;
    public int KW_string      = 20;
    public int KW_void        = 21;
    // Keyword -- structures
    public int KW_enum        = 22;
    public int KW_struct      = 23;
    public int KW_union       = 24;
    public int KW_class       = 25;
    public int KW_fun         = 26;
    // Keyword -- control flow
    public int KW_if          = 27;
    public int KW_else        = 28;
    public int KW_for         = 29;
    public int KW_while       = 30;
    public int KW_do          = 31;
    public int KW_switch      = 32;
    public int KW_case        = 33;
    public int KW_default     = 34;
    public int KW_break       = 35;
    public int KW_continue    = 36;
    public int KW_return      = 37;
    // Keyword -- exceptions
    public int KW_try         = 38;
    public int KW_catch       = 39;
    public int KW_throw       = 40;
    public int KW_throws      = 41;
    // Keyword -- OOP
    public int KW_new         = 42;
    public int KW_delete      = 43;
    public int KW_this        = 44;
    public int KW_super       = 45;
    public int KW_extends     = 46;
    public int KW_final       = 47;
    public int KW_static      = 48;
    public int KW_const       = 49;
    public int KW_var         = 50;
    // Keyword -- access modifiers
    public int KW_public      = 51;
    public int KW_private     = 52;
    public int KW_protected   = 53;
    // Keyword -- async
    public int KW_async       = 54;
    public int KW_await       = 55;
    // Keyword -- modules
    public int KW_import      = 56;
    public int KW_export      = 57;
    public int KW_using       = 58;
    public int KW_with        = 59;
    // Keyword -- misc
    public int KW_typeof      = 60;
    public int KW_instanceof  = 61;
    public int KW_as          = 62;
    // Punctuation -- delimiters
    public int LParen         = 63;
    public int RParen         = 64;
    public int LBrace         = 65;
    public int RBrace         = 66;
    public int LBracket       = 67;
    public int RBracket       = 68;
    public int Semi           = 69;
    public int Colon          = 70;
    public int DoubleColon    = 71;
    public int Comma          = 72;
    public int Dot            = 73;
    public int Ellipsis       = 74;
    public int Arrow          = 75;
    public int FatArrow       = 76;
    public int Question       = 77;
    // Assignment operators
    public int Equal          = 78;
    public int PlusEq         = 79;
    public int MinusEq        = 80;
    public int StarEq         = 81;
    public int SlashEq        = 82;
    public int PercentEq      = 83;
    public int AmpEq          = 84;
    public int PipeEq         = 85;
    public int CaretEq        = 86;
    public int LShiftEq       = 87;
    public int RShiftEq       = 88;
    // Arithmetic
    public int Plus           = 89;
    public int Minus          = 90;
    public int Star           = 91;
    public int Slash          = 92;
    public int Percent        = 93;
    public int PlusPlus       = 94;
    public int MinusMinus     = 95;
    // Relational
    public int EqualEqual     = 96;
    public int BangEqual      = 97;
    public int Less           = 98;
    public int LessEqual      = 99;
    public int Greater        = 100;
    public int GreaterEqual   = 101;
    // Logical
    public int AmpAmp         = 102;
    public int PipePipe       = 103;
    public int Bang           = 104;
    // Bitwise
    public int Amp            = 105;
    public int Pipe           = 106;
    public int Caret          = 107;
    public int Tilde          = 108;
    public int LShift         = 109;
    public int RShift         = 110;
    // Misc operators
    public int At             = 111;
    public int Hash           = 112;

    public fun void init() {}

    // Helpers -- mirror C++ Token member functions
    public fun bool isKeyword(int kind) {
        return kind >= 11 && kind <= 62;  // KW_bool..KW_as
    }
    public fun bool isLiteral(int kind) {
        return kind >= 3 && kind <= 9;    // LiteralInteger..LiteralNull
    }
    public fun bool isEof(int kind) {
        return kind == 0;
    }
}

// ── Token ─────────────────────────────────────────────────────────────────────
// Mirrors C++ struct Token.

class Token {
    public int    kind;
    public string lexeme;
    public int    line;
    public int    col;
    public string filename;

    public fun void init() {
        this.kind     = 1;  // Unknown
        this.lexeme   = "";
        this.line     = 1;
        this.col      = 1;
        this.filename = "";
    }

    public fun void initFull(int k, string lex, int ln, int co, string fn) {
        this.kind     = k;
        this.lexeme   = lex;
        this.line     = ln;
        this.col      = co;
        this.filename = fn;
    }

    public fun bool isKind(int k)  { return this.kind == k; }
    public fun bool isEof()        { return this.kind == 0; }
    public fun bool isError()      { return this.kind == 2; }
}

// ── Keyword table ─────────────────────────────────────────────────────────────
// Built once on Lexer construction via buildKeywordMap().

fun Map buildKeywordMap() {
    Map kw = new Map();
    // primitive types
    kw.set("bool",       _box(11));
    kw.set("char",       _box(12));
    kw.set("int",        _box(13));
    kw.set("uint",       _box(14));
    kw.set("long",       _box(15));
    kw.set("ulong",      _box(16));
    kw.set("float",      _box(17));
    kw.set("ufloat",     _box(18));
    kw.set("double",     _box(19));
    kw.set("string",     _box(20));
    kw.set("void",       _box(21));
    // structures
    kw.set("enum",       _box(22));
    kw.set("struct",     _box(23));
    kw.set("union",      _box(24));
    kw.set("class",      _box(25));
    kw.set("fun",        _box(26));
    // control flow
    kw.set("if",         _box(27));
    kw.set("else",       _box(28));
    kw.set("for",        _box(29));
    kw.set("while",      _box(30));
    kw.set("do",         _box(31));
    kw.set("switch",     _box(32));
    kw.set("case",       _box(33));
    kw.set("default",    _box(34));
    kw.set("break",      _box(35));
    kw.set("continue",   _box(36));
    kw.set("return",     _box(37));
    // exceptions
    kw.set("try",        _box(38));
    kw.set("catch",      _box(39));
    kw.set("throw",      _box(40));
    kw.set("throws",     _box(41));
    // OOP
    kw.set("new",        _box(42));
    kw.set("delete",     _box(43));
    kw.set("this",       _box(44));
    kw.set("super",      _box(45));
    kw.set("extends",    _box(46));
    kw.set("final",      _box(47));
    kw.set("static",     _box(48));
    kw.set("const",      _box(49));
    kw.set("var",        _box(50));
    // access modifiers
    kw.set("public",     _box(51));
    kw.set("private",    _box(52));
    kw.set("protected",  _box(53));
    // async
    kw.set("async",      _box(54));
    kw.set("await",      _box(55));
    // modules
    kw.set("import",     _box(56));
    kw.set("export",     _box(57));
    kw.set("using",      _box(58));
    kw.set("with",       _box(59));
    // misc
    kw.set("typeof",     _box(60));
    kw.set("instanceof", _box(61));
    kw.set("as",         _box(62));
    // bool / null literals
    kw.set("true",       _box(7));
    kw.set("false",      _box(8));
    kw.set("null",       _box(9));
    return kw;
}

// Helper: box an int into a GC object so it can be stored in Map (which
// uses i8* values).  We allocate a 4-byte block and store the int in it.
// Retrieve with _unbox(ptr).
fun var _box(int n) {
    int[] arr = new int[1];
    arr[0] = n;
    return arr;
}

fun int _unbox(var v) {
    int[] arr = v as int[];
    return arr[0];
}

// ── Lexer ─────────────────────────────────────────────────────────────────────

class Lexer {
    private string   src;
    private int      pos;
    private int      line;
    private int      col;
    private string   filename;
    private Map      keywords;

    public fun void init(string source, string fname) {
        this.src      = source;
        this.pos      = 0;
        this.line     = 1;
        this.col      = 1;
        this.filename = fname;
        this.keywords = buildKeywordMap();
    }

    // ── Public API ────────────────────────────────────────────────────────────

    public fun List tokenize() {
        List tokens = new List();
        while (true) {
            Token t = this.nextToken();
            tokens.add(t);
            if (t.isEof() || t.isError()) {
                break;
            }
        }
        return tokens;
    }

    public fun Token nextToken() {
        this.skipWhitespaceAndComments();

        if (this.atEnd()) {
            return this.makeToken(0, "<eof>");  // Eof
        }

        char c = this.peekChar(0);

        if (isAlpha(c) || c == '_') {
            return this.lexIdentifierOrKeyword();
        }

        if (isDigit(c)) {
            return this.lexNumber();
        }

        if (c == '\'') {
            return this.lexChar();
        }

        if (c == '"') {
            return this.lexString();
        }

        return this.lexOperatorOrPunct();
    }

    public fun bool atEnd() {
        return this.pos >= this.src.length;
    }

    // ── Internal helpers ──────────────────────────────────────────────────────

    private fun char peekChar(int offset) {
        int idx = this.pos + offset;
        if (idx >= this.src.length) {
            return '\0';
        }
        return this.src[idx];
    }

    private fun char advanceChar() {
        char c = this.src[this.pos];
        this.pos = this.pos + 1;
        if (c == '\n') {
            this.line = this.line + 1;
            this.col  = 1;
        } else {
            this.col = this.col + 1;
        }
        return c;
    }

    private fun bool matchChar(char expected) {
        if (this.atEnd()) {
            return false;
        }
        if (this.src[this.pos] != expected) {
            return false;
        }
        this.advanceChar();
        return true;
    }

    private fun Token makeToken(int kind, string lexeme) {
        Token t = new Token();
        t.initFull(kind, lexeme, this.line, this.col, this.filename);
        return t;
    }

    private fun Token makeTokenAtLine(int kind, string lexeme, int ln, int co) {
        Token t = new Token();
        t.initFull(kind, lexeme, ln, co, this.filename);
        return t;
    }

    private fun Token errorToken(string msg) {
        Token t = new Token();
        t.initFull(2, msg, this.line, this.col, this.filename);  // Error = 2
        return t;
    }

    // ── Skip whitespace and comments ──────────────────────────────────────────

    private fun void skipWhitespaceAndComments() {
        while (!this.atEnd()) {
            char c = this.peekChar(0);

            // whitespace
            if (isSpace(c)) {
                this.advanceChar();
                continue;
            }

            // single-line comment //
            if (c == '/' && this.peekChar(1) == '/') {
                while (!this.atEnd() && this.peekChar(0) != '\n') {
                    this.advanceChar();
                }
                continue;
            }

            // block comment /* ... */
            if (c == '/' && this.peekChar(1) == '*') {
                this.advanceChar();  // /
                this.advanceChar();  // *
                while (!this.atEnd()) {
                    if (this.peekChar(0) == '*' && this.peekChar(1) == '/') {
                        this.advanceChar();  // *
                        this.advanceChar();  // /
                        break;
                    }
                    this.advanceChar();
                }
                continue;
            }

            break;
        }
    }

    // ── Lex identifier or keyword ─────────────────────────────────────────────

    private fun Token lexIdentifierOrKeyword() {
        int startLine = this.line;
        int startCol  = this.col;
        StringBuilder sb = new StringBuilder();

        while (!this.atEnd()) {
            char c = this.peekChar(0);
            if (isAlnum(c) || c == '_') {
                sb.appendChar(this.advanceChar());
            } else {
                break;
            }
        }

        string lex = sb.toString();
        int kind = 10;  // Identifier default

        // keyword lookup
        if (this.keywords.has(lex)) {
            var v = this.keywords.get(lex);
            kind = _unbox(v);
        }

        return this.makeTokenAtLine(kind, lex, startLine, startCol);
    }

    // ── Lex number literal ────────────────────────────────────────────────────
    // Supports: decimal, hex (0x), binary (0b), octal (0o),
    //           float with fractional/exponent parts, suffixes u/l/f

    private fun Token lexNumber() {
        int startLine = this.line;
        int startCol  = this.col;
        StringBuilder sb = new StringBuilder();
        bool isFloat = false;

        if (this.peekChar(0) == '0') {
            sb.appendChar(this.advanceChar());  // '0'
            char next = this.peekChar(0);

            if (next == 'x' || next == 'X') {
                // hex
                sb.appendChar(this.advanceChar());
                if (!this.isHexDigit(this.peekChar(0))) {
                    return this.errorToken("invalid hex literal");
                }
                while (!this.atEnd() && (this.isHexDigit(this.peekChar(0)) || this.peekChar(0) == '_')) {
                    sb.appendChar(this.advanceChar());
                }
            } else if (next == 'b' || next == 'B') {
                // binary
                sb.appendChar(this.advanceChar());
                char bn = this.peekChar(0);
                if (bn != '0' && bn != '1') {
                    return this.errorToken("invalid binary literal");
                }
                while (!this.atEnd()) {
                    char bc = this.peekChar(0);
                    if (bc == '0' || bc == '1' || bc == '_') {
                        sb.appendChar(this.advanceChar());
                    } else {
                        break;
                    }
                }
            } else if (next == 'o' || next == 'O') {
                // octal
                sb.appendChar(this.advanceChar());
                while (!this.atEnd()) {
                    char oc = this.peekChar(0);
                    int  ov = charCode(oc);
                    if ((ov >= 48 && ov <= 55) || oc == '_') {  // '0'..'7'
                        sb.appendChar(this.advanceChar());
                    } else {
                        break;
                    }
                }
            } else {
                // plain decimal starting with 0
                while (!this.atEnd() && (isDigit(this.peekChar(0)) || this.peekChar(0) == '_')) {
                    sb.appendChar(this.advanceChar());
                }
            }
        } else {
            // decimal
            while (!this.atEnd() && (isDigit(this.peekChar(0)) || this.peekChar(0) == '_')) {
                sb.appendChar(this.advanceChar());
            }
        }

        // fractional part
        if (this.peekChar(0) == '.' && isDigit(this.peekChar(1))) {
            isFloat = true;
            sb.appendChar(this.advanceChar());  // '.'
            while (!this.atEnd() && (isDigit(this.peekChar(0)) || this.peekChar(0) == '_')) {
                sb.appendChar(this.advanceChar());
            }
        }

        // exponent
        char exp = this.peekChar(0);
        if (exp == 'e' || exp == 'E') {
            isFloat = true;
            sb.appendChar(this.advanceChar());
            char sign = this.peekChar(0);
            if (sign == '+' || sign == '-') {
                sb.appendChar(this.advanceChar());
            }
            if (!isDigit(this.peekChar(0))) {
                return this.errorToken("invalid exponent in numeric literal");
            }
            while (!this.atEnd() && isDigit(this.peekChar(0))) {
                sb.appendChar(this.advanceChar());
            }
        }

        // suffixes: u l f (case-insensitive)
        while (!this.atEnd()) {
            char s = toLowerChar(this.peekChar(0));
            if (s == 'u' || s == 'l') {
                sb.appendChar(this.advanceChar());
            } else if (s == 'f') {
                sb.appendChar(this.advanceChar());
                isFloat = true;
            } else {
                break;
            }
        }

        int kind = 3;  // LiteralInteger
        if (isFloat) {
            kind = 4;  // LiteralFloat
        }

        return this.makeTokenAtLine(kind, sb.toString(), startLine, startCol);
    }

    private fun bool isHexDigit(char c) {
        if (isDigit(c)) { return true; }
        int code = charCode(c);
        // a-f: 97-102, A-F: 65-70
        return (code >= 97 && code <= 102) || (code >= 65 && code <= 70);
    }

    // ── Lex char literal  'x'  '\n' ──────────────────────────────────────────

    private fun Token lexChar() {
        int startLine = this.line;
        int startCol  = this.col;
        StringBuilder sb = new StringBuilder();

        sb.appendChar(this.advanceChar());  // opening '

        if (this.atEnd() || this.peekChar(0) == '\n') {
            return this.errorToken("unterminated char literal");
        }

        if (this.peekChar(0) == '\\') {
            sb.appendChar(this.advanceChar());  // backslash
            if (this.atEnd()) {
                return this.errorToken("unterminated escape in char literal");
            }
            sb.appendChar(this.advanceChar());  // escape character
        } else {
            sb.appendChar(this.advanceChar());  // the character
        }

        if (!this.matchChar('\'')) {
            return this.errorToken("unterminated char literal (expected closing ')");
        }
        sb.appendChar('\'');

        return this.makeTokenAtLine(5, sb.toString(), startLine, startCol);  // LiteralChar
    }

    // ── Lex string literal  "..."  with escape sequences ─────────────────────

    private fun Token lexString() {
        int startLine = this.line;
        int startCol  = this.col;
        StringBuilder sb = new StringBuilder();

        sb.appendChar(this.advanceChar());  // opening "

        while (!this.atEnd() && this.peekChar(0) != '"') {
            if (this.peekChar(0) == '\n') {
                return this.errorToken("unterminated string literal (newline in string)");
            }

            if (this.peekChar(0) == '\\') {
                sb.appendChar(this.advanceChar());  // backslash
                if (this.atEnd()) {
                    return this.errorToken("unterminated escape sequence");
                }
                char esc = this.advanceChar();
                sb.appendChar(esc);
                // handle \uXXXX
                if (esc == 'u') {
                    int ui = 0;
                    while (ui < 4 && !this.atEnd()) {
                        sb.appendChar(this.advanceChar());
                        ui = ui + 1;
                    }
                }
            } else {
                sb.appendChar(this.advanceChar());
            }
        }

        if (this.atEnd()) {
            return this.errorToken("unterminated string literal");
        }

        sb.appendChar(this.advanceChar());  // closing "

        return this.makeTokenAtLine(6, sb.toString(), startLine, startCol);  // LiteralString
    }

    // ── Lex operators and punctuation ─────────────────────────────────────────

    private fun Token lexOperatorOrPunct() {
        int startLine = this.line;
        int startCol  = this.col;

        char c = this.advanceChar();
        int  code = charCode(c);

        // single-character tokens
        if (c == '(') { return this.makeTokenAtLine(63,  "(", startLine, startCol); }
        if (c == ')') { return this.makeTokenAtLine(64,  ")", startLine, startCol); }
        if (c == '{') { return this.makeTokenAtLine(65,  "{", startLine, startCol); }
        if (c == '}') { return this.makeTokenAtLine(66,  "}", startLine, startCol); }
        if (c == '[') { return this.makeTokenAtLine(67,  "[", startLine, startCol); }
        if (c == ']') { return this.makeTokenAtLine(68,  "]", startLine, startCol); }
        if (c == ';') { return this.makeTokenAtLine(69,  ";", startLine, startCol); }
        if (c == ',') { return this.makeTokenAtLine(72,  ",", startLine, startCol); }
        if (c == '~') { return this.makeTokenAtLine(108, "~", startLine, startCol); }
        if (c == '@') { return this.makeTokenAtLine(111, "@", startLine, startCol); }
        if (c == '#') { return this.makeTokenAtLine(112, "#", startLine, startCol); }
        if (c == '?') { return this.makeTokenAtLine(77,  "?", startLine, startCol); }

        // colon / double-colon
        if (c == ':') {
            if (this.matchChar(':')) {
                return this.makeTokenAtLine(71, "::", startLine, startCol);  // DoubleColon
            }
            return this.makeTokenAtLine(70, ":", startLine, startCol);  // Colon
        }

        // dot / ellipsis
        if (c == '.') {
            if (this.peekChar(0) == '.' && this.peekChar(1) == '.') {
                this.advanceChar();
                this.advanceChar();
                return this.makeTokenAtLine(74, "...", startLine, startCol);  // Ellipsis
            }
            return this.makeTokenAtLine(73, ".", startLine, startCol);  // Dot
        }

        // = == =>
        if (c == '=') {
            if (this.matchChar('=')) {
                return this.makeTokenAtLine(96, "==", startLine, startCol);  // EqualEqual
            }
            if (this.matchChar('>')) {
                return this.makeTokenAtLine(76, "=>", startLine, startCol);  // FatArrow
            }
            return this.makeTokenAtLine(78, "=", startLine, startCol);  // Equal
        }

        // ! !=
        if (c == '!') {
            if (this.matchChar('=')) {
                return this.makeTokenAtLine(97, "!=", startLine, startCol);  // BangEqual
            }
            return this.makeTokenAtLine(104, "!", startLine, startCol);  // Bang
        }

        // < <= << <<=
        if (c == '<') {
            if (this.matchChar('<')) {
                if (this.matchChar('=')) {
                    return this.makeTokenAtLine(87, "<<=", startLine, startCol);  // LShiftEq
                }
                return this.makeTokenAtLine(109, "<<", startLine, startCol);  // LShift
            }
            if (this.matchChar('=')) {
                return this.makeTokenAtLine(99, "<=", startLine, startCol);  // LessEqual
            }
            return this.makeTokenAtLine(98, "<", startLine, startCol);  // Less
        }

        // > >= >> >>=
        if (c == '>') {
            if (this.matchChar('>')) {
                if (this.matchChar('=')) {
                    return this.makeTokenAtLine(88, ">>=", startLine, startCol);  // RShiftEq
                }
                return this.makeTokenAtLine(110, ">>", startLine, startCol);  // RShift
            }
            if (this.matchChar('=')) {
                return this.makeTokenAtLine(101, ">=", startLine, startCol);  // GreaterEqual
            }
            return this.makeTokenAtLine(100, ">", startLine, startCol);  // Greater
        }

        // + ++ +=
        if (c == '+') {
            if (this.matchChar('+')) {
                return this.makeTokenAtLine(94, "++", startLine, startCol);  // PlusPlus
            }
            if (this.matchChar('=')) {
                return this.makeTokenAtLine(79, "+=", startLine, startCol);  // PlusEq
            }
            return this.makeTokenAtLine(89, "+", startLine, startCol);  // Plus
        }

        // - -- -= ->
        if (c == '-') {
            if (this.matchChar('-')) {
                return this.makeTokenAtLine(95, "--", startLine, startCol);  // MinusMinus
            }
            if (this.matchChar('>')) {
                return this.makeTokenAtLine(75, "->", startLine, startCol);  // Arrow
            }
            if (this.matchChar('=')) {
                return this.makeTokenAtLine(80, "-=", startLine, startCol);  // MinusEq
            }
            return this.makeTokenAtLine(90, "-", startLine, startCol);  // Minus
        }

        // * *=
        if (c == '*') {
            if (this.matchChar('=')) {
                return this.makeTokenAtLine(81, "*=", startLine, startCol);  // StarEq
            }
            return this.makeTokenAtLine(91, "*", startLine, startCol);  // Star
        }

        // / /=   (comments already consumed in skipWhitespace)
        if (c == '/') {
            if (this.matchChar('=')) {
                return this.makeTokenAtLine(82, "/=", startLine, startCol);  // SlashEq
            }
            return this.makeTokenAtLine(92, "/", startLine, startCol);  // Slash
        }

        // % %=
        if (c == '%') {
            if (this.matchChar('=')) {
                return this.makeTokenAtLine(83, "%=", startLine, startCol);  // PercentEq
            }
            return this.makeTokenAtLine(93, "%", startLine, startCol);  // Percent
        }

        // & && &=
        if (c == '&') {
            if (this.matchChar('&')) {
                return this.makeTokenAtLine(102, "&&", startLine, startCol);  // AmpAmp
            }
            if (this.matchChar('=')) {
                return this.makeTokenAtLine(84, "&=", startLine, startCol);  // AmpEq
            }
            return this.makeTokenAtLine(105, "&", startLine, startCol);  // Amp
        }

        // | || |=
        if (c == '|') {
            if (this.matchChar('|')) {
                return this.makeTokenAtLine(103, "||", startLine, startCol);  // PipePipe
            }
            if (this.matchChar('=')) {
                return this.makeTokenAtLine(85, "|=", startLine, startCol);  // PipeEq
            }
            return this.makeTokenAtLine(106, "|", startLine, startCol);  // Pipe
        }

        // ^ ^=
        if (c == '^') {
            if (this.matchChar('=')) {
                return this.makeTokenAtLine(86, "^=", startLine, startCol);  // CaretEq
            }
            return this.makeTokenAtLine(107, "^", startLine, startCol);  // Caret
        }

        // unknown character
        string msg = "unexpected character '";
        msg = msg + charToString(c);
        msg = msg + "'";
        return this.errorToken(msg);
    }
}

// ── Test driver ───────────────────────────────────────────────────────────────

fun int main() {
    string source = readFile("test.ec");
    if (source == "") {
        // Use inline test source if no file given
        source = "fun int main() { int x = 42; return x + 1; }";
    }

    Lexer lexer = new Lexer("test", "test");
    lexer.init(source, "test.ec");

    List tokens = lexer.tokenize();

    int i = 0;
    while (i < tokens.length()) {
        Token t = tokens.get(i) as Token;
        print(toString(t.line));
        print(":");
        print(toString(t.col));
        print("  kind=");
        print(toString(t.kind));
        print("  lexeme=");
        println(t.lexeme);
        if (t.isEof() || t.isError()) {
            break;
        }
        i = i + 1;
    }

    print("Total tokens: ");
    println(toString(tokens.length()));
    return 0;
}
