// ============================================================================
//  Parser.lang  --  ExtendedC port of the C++ recursive-descent Parser
//
//  AST nodes are ExtendedC classes with integer 'kind' fields.
//  All node pointers are stored as 'var' (i8*) in List containers.
//
//  Node kind integer constants mirror the C++ ExprKind/StmtKind/DeclKind
//  enums (by ordinal).
// ============================================================================

import "std.io";
import "std.string";
import "std.collections";
import "Lexer";

// ── Node kind constants ───────────────────────────────────────────────────────

// ExprKind (0-based ordinals matching C++ enum)
int EK_IntLit     = 0;
int EK_FloatLit   = 1;
int EK_BoolLit    = 2;
int EK_CharLit    = 3;
int EK_StringLit  = 4;
int EK_NullLit    = 5;
int EK_Identifier = 6;
int EK_Unary      = 7;
int EK_Binary     = 8;
int EK_Ternary    = 9;
int EK_Assign     = 10;
int EK_Call       = 11;
int EK_Member     = 12;
int EK_Index      = 13;
int EK_New        = 14;
int EK_Delete     = 15;
int EK_TypeOf     = 16;
int EK_Cast       = 17;
int EK_Lambda     = 18;
int EK_InstanceOf = 19;
int EK_ArrayLit   = 20;

// StmtKind
int SK_Block      = 0;
int SK_Expr       = 1;
int SK_VarDecl    = 2;
int SK_If         = 3;
int SK_While      = 4;
int SK_DoWhile    = 5;
int SK_For        = 6;
int SK_Switch     = 7;
int SK_Return     = 8;
int SK_Break      = 9;
int SK_Continue   = 10;
int SK_Throw      = 11;
int SK_Try        = 12;

// DeclKind
int DK_Import   = 0;
int DK_Enum     = 1;
int DK_Struct   = 2;
int DK_Union    = 3;
int DK_Class    = 4;
int DK_Function = 5;
int DK_Variable = 6;

// AccessMod
int AM_None      = 0;
int AM_Public    = 1;
int AM_Private   = 2;
int AM_Protected = 3;

// ── TypeRef ───────────────────────────────────────────────────────────────────

class TypeRef {
    public string name;
    public bool   isArray;
    public bool   isPointer;
    public int    line;
    public int    col;

    public fun void init() {
        this.name      = "";
        this.isArray   = false;
        this.isPointer = false;
        this.line      = 1;
        this.col       = 1;
    }
}

// ── Parameter ─────────────────────────────────────────────────────────────────

class Param {
    public TypeRef ptype;
    public string  name;
    public int     line;
    public int     col;

    public fun void init() {
        this.ptype = new TypeRef();
        this.ptype.init();
        this.name = "";
        this.line = 1;
        this.col  = 1;
    }
}

// ── Base AST node ─────────────────────────────────────────────────────────────
// All Expr, Stmt, Decl nodes share: kind, line, col, filename

class ASTNode {
    public int    kind;
    public int    line;
    public int    col;
    public string filename;

    public fun void initNode(int k, int ln, int co, string fn) {
        this.kind     = k;
        this.line     = ln;
        this.col      = co;
        this.filename = fn;
    }
}

// ── Expr nodes ────────────────────────────────────────────────────────────────

class IntLitNode extends ASTNode {
    public string raw;
    public int    value;   // parsed value (fits i32 for most uses)
    public fun void init() {}
}

class FloatLitNode extends ASTNode {
    public string raw;
    public fun void init() {}
}

class BoolLitNode extends ASTNode {
    public bool value;
    public fun void init() {}
}

class CharLitNode extends ASTNode {
    public string raw;   // includes surrounding quotes e.g. 'a'
    public fun void init() {}
}

class StringLitNode extends ASTNode {
    public string value; // unquoted, escape sequences kept as-is
    public fun void init() {}
}

class NullLitNode extends ASTNode {
    public fun void init() {}
}

class IdentNode extends ASTNode {
    public string name;
    public fun void init() {}
}

class UnaryNode extends ASTNode {
    public int  op;       // TokenKind int
    public bool postfix;
    public var  operand;  // ASTNode (Expr)
    public fun void init() {}
}

class BinaryNode extends ASTNode {
    public int op;
    public var left;
    public var right;
    public fun void init() {}
}

class TernaryNode extends ASTNode {
    public var cond;
    public var thenExpr;
    public var elseExpr;
    public fun void init() {}
}

class AssignNode extends ASTNode {
    public int op;
    public var target;
    public var value;
    public fun void init() {}
}

class CallNode extends ASTNode {
    public var  callee;
    public List args;    // List of ASTNode (Expr)
    public fun void init() {}
}

class MemberNode extends ASTNode {
    public var    object;
    public string member;
    public bool   arrow;
    public fun void init() {}
}

class IndexNode extends ASTNode {
    public var array;
    public var index;
    public fun void init() {}
}

class NewNode extends ASTNode {
    public TypeRef ntype;
    public bool    isArray;
    public var     arraySize;  // null if not array
    public List    args;       // for object construction
    public fun void init() {}
}

class DeleteNode extends ASTNode {
    public var operand;
    public fun void init() {}
}

class TypeOfNode extends ASTNode {
    public var operand;
    public fun void init() {}
}

class CastNode extends ASTNode {
    public TypeRef ctype;
    public var     operand;
    public fun void init() {}
}

class InstanceOfNode extends ASTNode {
    public var     operand;
    public TypeRef itype;
    public fun void init() {}
}

class ArrayLitNode extends ASTNode {
    public List elements;  // List of Expr
    public fun void init() {}
}

class LambdaNode extends ASTNode {
    public List params;    // List of Param
    public var  body;      // Expr body (or null)
    public var  blockBody; // BlockStmt body (or null)
    public fun void init() {}
}

// ── Stmt nodes ────────────────────────────────────────────────────────────────

class BlockNode extends ASTNode {
    public List stmts;  // List of Stmt ASTNode
    public fun void init() {}
}

class ExprStmtNode extends ASTNode {
    public var expr;
    public fun void init() {}
}

class VarDeclStmtNode extends ASTNode {
    public TypeRef vtype;
    public string  name;
    public var     init_;   // initializer expr or null
    public bool    isConst;
    public bool    isStatic;
    public fun void init() {}
}

class IfNode extends ASTNode {
    public var cond;
    public var thenBranch;
    public var elseBranch;  // null if no else
    public fun void init() {}
}

class WhileNode extends ASTNode {
    public var cond;
    public var body;
    public fun void init() {}
}

class DoWhileNode extends ASTNode {
    public var body;
    public var cond;
    public fun void init() {}
}

class ForNode extends ASTNode {
    public var init_;   // VarDeclStmt or ExprStmt or null
    public var cond;    // Expr or null
    public var incr;    // Expr or null
    public var body;
    public fun void init() {}
}

class CaseClause {
    public var  value;  // Expr or null for default
    public List stmts;
    public fun void init() {
        this.value = null;
        this.stmts = new List();
    }
}

class SwitchNode extends ASTNode {
    public var  subject;
    public List cases;  // List of CaseClause
    public fun void init() {}
}

class ReturnNode extends ASTNode {
    public var value;  // Expr or null
    public fun void init() {}
}

class ThrowNode extends ASTNode {
    public var value;
    public fun void init() {}
}

class CatchClause {
    public TypeRef exType;
    public string  exName;
    public var     body;   // BlockNode
    public fun void init() {
        this.exType = new TypeRef();
        this.exType.init();
        this.exName = "";
        this.body   = null;
    }
}

class TryNode extends ASTNode {
    public var  tryBody;
    public List catches;   // List of CatchClause
    public var  finallyBody; // null
    public fun void init() {}
}

class BreakNode extends ASTNode {
    public fun void init() {}
}

class ContinueNode extends ASTNode {
    public fun void init() {}
}

// ── Decl nodes ────────────────────────────────────────────────────────────────

class FieldDecl {
    public TypeRef ftype;
    public string  name;
    public int     line;
    public int     col;
    public fun void init() {
        this.ftype = new TypeRef();
        this.ftype.init();
        this.name = "";
        this.line = 1;
        this.col  = 1;
    }
}

class ImportDeclNode extends ASTNode {
    public string path;
    public fun void init() {}
}

class EnumDeclNode extends ASTNode {
    public string name;
    public List   members;  // List of string (boxed)
    public fun void init() {}
}

class StructDeclNode extends ASTNode {
    public string name;
    public List   fields;   // List of FieldDecl
    public fun void init() {}
}

class UnionDeclNode extends ASTNode {
    public string name;
    public List   fields;
    public fun void init() {}
}

class MemberVarDecl {
    public int     access;  // AM_*
    public bool    isStatic;
    public bool    isConst;
    public TypeRef mtype;
    public string  name;
    public var     init_;   // Expr or null
    public int     line;
    public int     col;
    public fun void init() {
        this.access   = 0;
        this.isStatic = false;
        this.isConst  = false;
        this.mtype    = new TypeRef();
        this.mtype.init();
        this.name  = "";
        this.init_ = null;
        this.line  = 1;
        this.col   = 1;
    }
}

class FunctionDeclNode extends ASTNode {
    public int     access;    // AM_*
    public bool    isAsync;
    public bool    isStatic;
    public bool    isFinal;
    public bool    throws_;
    public TypeRef returnType;
    public string  name;
    public List    params;    // List of Param
    public var     body;      // BlockNode
    public fun void init() {}
}

class ClassDeclNode extends ASTNode {
    public string name;
    public bool   isFinal;
    public string superClass;   // "" if none
    public bool   hasSuperClass;
    public List   fields;       // List of MemberVarDecl
    public List   methods;      // List of FunctionDeclNode
    public fun void init() {}
}

class VarDeclNode extends ASTNode {
    public bool    isConst;
    public bool    isStatic;
    public TypeRef vtype;
    public string  name;
    public var     init_;  // Expr or null
    public fun void init() {}
}

// ── Program ───────────────────────────────────────────────────────────────────

class Program {
    public List declarations;  // List of Decl ASTNode

    public fun void init() {
        this.declarations = new List();
    }
}

// ── ParseError ────────────────────────────────────────────────────────────────

class ParseError {
    public string msg;
    public int    line;
    public int    col;
    public string filename;

    public fun void init() {
        this.msg      = "";
        this.line     = 1;
        this.col      = 1;
        this.filename = "";
    }

    public fun void set(string m, int ln, int co, string fn) {
        this.msg      = m;
        this.line     = ln;
        this.col      = co;
        this.filename = fn;
    }
}

// ── Parser ────────────────────────────────────────────────────────────────────

class Parser {
    private List   tokens;   // List of Token
    private int    pos;
    private Token  eofToken;
    private List   errors;   // List of ParseError
    private string filename;

    public fun void init(List toks, string fname) {
        this.tokens   = toks;
        this.pos      = 0;
        this.errors   = new List();
        this.filename = fname;

        this.eofToken = new Token();
        this.eofToken.init();
        this.eofToken.kind   = 0;  // Eof
        this.eofToken.lexeme = "<eof>";

        if (toks.length() > 0) {
            Token last = toks.last() as Token;
            this.eofToken.line     = last.line;
            this.eofToken.col      = last.col;
            this.eofToken.filename = last.filename;
        }
    }

    // ── Token stream ──────────────────────────────────────────────────────────

    private fun Token peekAt(int offset) {
        int idx = this.pos + offset;
        if (idx < 0 || idx >= this.tokens.length()) {
            return this.eofToken;
        }
        return this.tokens.get(idx) as Token;
    }

    private fun Token peekTok() {
        return this.peekAt(0);
    }

    private fun Token advanceTok() {
        if (!this.isAtEnd()) {
            this.pos = this.pos + 1;
        }
        return this.tokens.get(this.pos - 1) as Token;
    }

    private fun bool checkKind(int k) {
        return this.peekTok().kind == k;
    }

    private fun bool matchKind(int k) {
        if (this.checkKind(k)) {
            this.advanceTok();
            return true;
        }
        return false;
    }

    private fun Token expectKind(int k, string msg) {
        if (this.checkKind(k)) {
            return this.advanceTok();
        }
        this.recordError(msg);
        return this.peekTok();
    }

    private fun bool isAtEnd() {
        if (this.pos >= this.tokens.length()) { return true; }
        return this.peekTok().kind == 0;  // Eof
    }

    private fun bool isTypeName() {
        int k = this.peekTok().kind;
        // builtin types: KW_bool(11)..KW_void(21), var(50), Identifier(10)
        bool isBuiltin = (k >= 11 && k <= 21);
        return isBuiltin || k == 10 || k == 50;  // Identifier or var
    }

    private fun bool isAssignOp(int k) {
        // Equal(78) PlusEq(79) MinusEq(80) StarEq(81) SlashEq(82)
        // PercentEq(83) AmpEq(84) PipeEq(85) CaretEq(86)
        // LShiftEq(87) RShiftEq(88)
        return k >= 78 && k <= 88;
    }

    // ── Error handling ────────────────────────────────────────────────────────

    private fun void recordError(string msg) {
        Token t = this.peekTok();
        ParseError e = new ParseError();
        e.set(msg, t.line, t.col, t.filename);
        this.errors.add(e);
    }

    private fun void synchronize() {
        while (!this.isAtEnd()) {
            int k = this.peekTok().kind;
            // after semicolon we're back in sync
            if (k == 69) { this.advanceTok(); return; }  // Semi
            // statement starters
            if (k == 26 || k == 25 || k == 23 || k == 24 ||  // fun class struct union
                k == 22 || k == 56 || k == 37 || k == 27 ||  // enum import return if
                k == 30 || k == 29 || k == 31 || k == 32 ||  // while for do switch
                k == 38 || k == 40 || k == 35 || k == 36 ||  // try throw break continue
                k == 66) {                                      // RBrace
                return;
            }
            this.advanceTok();
        }
    }

    private fun void synchronizeDecl() {
        while (!this.isAtEnd()) {
            int k = this.peekTok().kind;
            if (k == 26 || k == 25 || k == 23 || k == 24 || k == 22 ||  // fun class struct union enum
                k == 56 || k == 51 || k == 52 || k == 53 ||              // import public private protected
                k == 48 || k == 49 || k == 54 || k == 47 ||              // static const async final
                k == 66) {                                                  // RBrace
                return;
            }
            if (k == 69) { this.advanceTok(); return; }  // Semi
            this.advanceTok();
        }
    }

    // ── Public API ────────────────────────────────────────────────────────────

    public fun List getErrors() { return this.errors; }

    public fun Program parse() {
        Program prog = new Program();
        while (!this.isAtEnd()) {
            var decl = this.parseDeclaration();
            if (decl != null) {
                prog.declarations.add(decl);
            }
        }
        return prog;
    }

    // ── Type parsing ──────────────────────────────────────────────────────────

    private fun TypeRef parseType() {
        TypeRef t = new TypeRef();
        t.init();
        Token tok = this.peekTok();
        t.line = tok.line;
        t.col  = tok.col;

        int k = tok.kind;
        bool isBuiltin = (k >= 11 && k <= 21);
        if (isBuiltin || k == 10) {   // builtin or Identifier
            t.name = this.advanceTok().lexeme;
        } else if (k == 50) {         // KW_var
            t.name = "var";
            this.advanceTok();
        } else {
            this.recordError("expected a type name, got '" + tok.lexeme + "'");
            t.name = "<error>";
            return t;
        }

        // array suffix []
        if (this.checkKind(67) && this.peekAt(1).kind == 68) {  // [ ]
            this.advanceTok();  // [
            this.advanceTok();  // ]
            t.isArray = true;
        }

        // pointer suffix *
        if (this.checkKind(91)) {  // Star
            this.advanceTok();
            t.isPointer = true;
        }

        return t;
    }

    // ── Declaration dispatcher ────────────────────────────────────────────────

    private fun var parseDeclaration() {
        if (this.checkKind(56)) { return this.parseImportDecl(); }  // KW_import
        if (this.checkKind(22)) { return this.parseEnumDecl(); }    // KW_enum
        if (this.checkKind(23)) { return this.parseStructDecl(); }  // KW_struct
        if (this.checkKind(24)) { return this.parseUnionDecl(); }   // KW_union
        if (this.checkKind(25) || this.checkKind(47)) {             // KW_class or KW_final
            return this.parseClassDecl();
        }

        // modifiers before fun or var
        bool isAsync  = false;
        bool isStatic = false;
        bool isFinal  = false;
        bool isConst  = false;

        bool changed = true;
        while (changed) {
            changed = false;
            if (this.matchKind(54)) { isAsync  = true; changed = true; }  // KW_async
            if (this.matchKind(48)) { isStatic = true; changed = true; }  // KW_static
            if (this.matchKind(47)) { isFinal  = true; changed = true; }  // KW_final
            if (this.matchKind(49)) { isConst  = true; changed = true; }  // KW_const
        }

        if (this.checkKind(26)) {  // KW_fun
            return this.parseFunctionDecl(0, isAsync, isStatic, isFinal);  // AM_None
        }

        if (this.isTypeName()) {
            return this.parseTopLevelVarDecl(isConst, isStatic);
        }

        this.recordError("unexpected token '" + this.peekTok().lexeme + "' at top level");
        this.advanceTok();
        return null;
    }

    // ── Import ────────────────────────────────────────────────────────────────

    private fun var parseImportDecl() {
        Token loc = this.peekTok();
        this.expectKind(56, "expected 'import'");  // KW_import

        if (!this.checkKind(6)) {  // LiteralString
            this.recordError("expected a string path after 'import'");
            this.synchronize();
            return null;
        }

        string raw  = this.advanceTok().lexeme;
        string path = substring(raw, 1, stringLen(raw) - 1);  // strip quotes
        this.expectKind(69, "expected ';' after import path");  // Semi

        ImportDeclNode n = new ImportDeclNode();
        n.initNode(0, loc.line, loc.col, loc.filename);  // DK_Import = 0
        n.path = path;
        return n;
    }

    // ── Enum ──────────────────────────────────────────────────────────────────

    private fun var parseEnumDecl() {
        Token loc = this.peekTok();
        this.expectKind(22, "expected 'enum'");

        if (!this.checkKind(10)) {  // Identifier
            this.recordError("expected enum name");
            this.synchronizeDecl();
            return null;
        }
        string name = this.advanceTok().lexeme;
        this.expectKind(65, "expected '{' after enum name");  // LBrace

        List members = new List();
        while (!this.checkKind(66) && !this.isAtEnd()) {  // RBrace
            if (!this.checkKind(10)) {
                this.recordError("expected enum member name");
                this.synchronize();
                break;
            }
            string mname = this.advanceTok().lexeme;
            // box the string so it can go in the List
            members.add(_boxStr(mname));
            if (!this.matchKind(72)) { break; }  // Comma
        }

        this.expectKind(66, "expected '}' to close enum");

        EnumDeclNode n = new EnumDeclNode();
        n.initNode(1, loc.line, loc.col, loc.filename);  // DK_Enum = 1
        n.name    = name;
        n.members = members;
        return n;
    }

    // ── Struct ────────────────────────────────────────────────────────────────

    private fun var parseStructDecl() {
        Token loc = this.peekTok();
        this.expectKind(23, "expected 'struct'");

        if (!this.checkKind(10)) {
            this.recordError("expected struct name");
            this.synchronizeDecl();
            return null;
        }
        string name = this.advanceTok().lexeme;
        this.expectKind(65, "expected '{' after struct name");

        List fields = this.parseFieldList(66);  // until RBrace

        this.expectKind(66, "expected '}' to close struct");

        StructDeclNode n = new StructDeclNode();
        n.initNode(2, loc.line, loc.col, loc.filename);  // DK_Struct = 2
        n.name   = name;
        n.fields = fields;
        return n;
    }

    // ── Union ─────────────────────────────────────────────────────────────────

    private fun var parseUnionDecl() {
        Token loc = this.peekTok();
        this.expectKind(24, "expected 'union'");

        if (!this.checkKind(10)) {
            this.recordError("expected union name");
            this.synchronizeDecl();
            return null;
        }
        string name = this.advanceTok().lexeme;
        this.expectKind(65, "expected '{' after union name");

        List fields = this.parseFieldList(66);

        this.expectKind(66, "expected '}' to close union");

        UnionDeclNode n = new UnionDeclNode();
        n.initNode(3, loc.line, loc.col, loc.filename);  // DK_Union = 3
        n.name   = name;
        n.fields = fields;
        return n;
    }

    private fun List parseFieldList(int stopKind) {
        List fields = new List();
        while (!this.checkKind(stopKind) && !this.isAtEnd()) {
            if (!this.isTypeName()) {
                this.recordError("expected field type");
                this.synchronize();
                continue;
            }
            FieldDecl fd = new FieldDecl();
            fd.init();
            Token floc = this.peekTok();
            fd.line  = floc.line;
            fd.col   = floc.col;
            fd.ftype = this.parseType();
            if (!this.checkKind(10)) {
                this.recordError("expected field name");
                this.synchronize();
                continue;
            }
            fd.name = this.advanceTok().lexeme;
            this.expectKind(69, "expected ';' after field");  // Semi
            fields.add(fd);
        }
        return fields;
    }

    // ── Class ─────────────────────────────────────────────────────────────────

    private fun var parseClassDecl() {
        Token loc     = this.peekTok();
        bool  isFinal = false;

        if (this.matchKind(47)) { isFinal = true; }  // KW_final

        this.expectKind(25, "expected 'class'");

        if (!this.checkKind(10)) {
            this.recordError("expected class name");
            this.synchronizeDecl();
            return null;
        }

        ClassDeclNode cls = new ClassDeclNode();
        cls.initNode(4, loc.line, loc.col, loc.filename);  // DK_Class = 4
        cls.name         = this.advanceTok().lexeme;
        cls.isFinal      = isFinal;
        cls.hasSuperClass = false;
        cls.superClass   = "";
        cls.fields       = new List();
        cls.methods      = new List();

        if (this.matchKind(46)) {  // KW_extends
            if (!this.checkKind(10)) {
                this.recordError("expected superclass name after 'extends'");
            } else {
                cls.superClass    = this.advanceTok().lexeme;
                cls.hasSuperClass = true;
            }
        }

        this.expectKind(65, "expected '{' to open class body");

        while (!this.checkKind(66) && !this.isAtEnd()) {  // RBrace
            int  access   = 0;   // AM_None
            bool isStatic = false;
            bool isAsync  = false;
            bool isFinalM = false;
            bool isConst  = false;

            bool ch = true;
            while (ch) {
                ch = false;
                if (this.checkKind(51)) { access = 1; this.advanceTok(); ch = true; }      // public
                else if (this.checkKind(52)) { access = 2; this.advanceTok(); ch = true; }  // private
                else if (this.checkKind(53)) { access = 3; this.advanceTok(); ch = true; }  // protected
                else if (this.matchKind(48)) { isStatic = true; ch = true; }  // static
                else if (this.matchKind(54)) { isAsync  = true; ch = true; }  // async
                else if (this.matchKind(47)) { isFinalM = true; ch = true; }  // final
                else if (this.matchKind(49)) { isConst  = true; ch = true; }  // const
            }

            if (this.checkKind(26)) {  // KW_fun
                var fn = this.parseFunctionDecl(access, isAsync, isStatic, isFinalM);
                if (fn != null) { cls.methods.add(fn); }
            } else if (this.isTypeName()) {
                MemberVarDecl mv = new MemberVarDecl();
                mv.init();
                mv.access   = access;
                mv.isStatic = isStatic;
                mv.isConst  = isConst;
                Token mloc  = this.peekTok();
                mv.line     = mloc.line;
                mv.col      = mloc.col;
                mv.mtype    = this.parseType();
                if (!this.checkKind(10)) {
                    this.recordError("expected field name");
                    this.synchronizeDecl();
                    continue;
                }
                mv.name = this.advanceTok().lexeme;
                if (this.matchKind(78)) {   // Equal
                    mv.init_ = this.parseExpr();
                }
                this.expectKind(69, "expected ';' after field declaration");
                cls.fields.add(mv);
            } else {
                this.recordError("unexpected token '" + this.peekTok().lexeme + "' in class body");
                this.synchronizeDecl();
            }
        }

        this.expectKind(66, "expected '}' to close class body");
        return cls;
    }

    // ── Function declaration ──────────────────────────────────────────────────

    private fun var parseFunctionDecl(int access, bool isAsync, bool isStatic, bool isFinal) {
        Token loc = this.peekTok();
        this.expectKind(26, "expected 'fun'");

        if (!this.isTypeName()) {
            this.recordError("expected return type after 'fun'");
            this.synchronizeDecl();
            return null;
        }
        TypeRef retType = this.parseType();

        if (!this.checkKind(10)) {   // Identifier
            this.recordError("expected function name");
            this.synchronizeDecl();
            return null;
        }
        string fname = this.advanceTok().lexeme;

        this.expectKind(63, "expected '(' after function name");  // LParen
        List params = this.parseParamList();
        this.expectKind(64, "expected ')' to close parameter list");  // RParen

        bool throws_ = false;
        if (this.matchKind(41)) { throws_ = true; }  // KW_throws

        if (!this.checkKind(65)) {  // LBrace
            this.recordError("expected '{' to open function body");
            this.synchronizeDecl();
            return null;
        }
        var body = this.parseBlock();

        FunctionDeclNode fn = new FunctionDeclNode();
        fn.initNode(5, loc.line, loc.col, loc.filename);  // DK_Function = 5
        fn.access     = access;
        fn.isAsync    = isAsync;
        fn.isStatic   = isStatic;
        fn.isFinal    = isFinal;
        fn.throws_    = throws_;
        fn.returnType = retType;
        fn.name       = fname;
        fn.params     = params;
        fn.body       = body;
        return fn;
    }

    // ── Top-level variable declaration ────────────────────────────────────────

    private fun var parseTopLevelVarDecl(bool isConst, bool isStatic) {
        Token  loc  = this.peekTok();
        TypeRef vt  = this.parseType();

        if (!this.checkKind(10)) {
            this.recordError("expected variable name");
            this.synchronize();
            return null;
        }
        string vname = this.advanceTok().lexeme;

        var initExpr = null;
        if (this.matchKind(78)) {  // Equal
            initExpr = this.parseExpr();
        }
        this.expectKind(69, "expected ';' after variable declaration");

        VarDeclNode vd = new VarDeclNode();
        vd.initNode(6, loc.line, loc.col, loc.filename);  // DK_Variable = 6
        vd.isConst  = isConst;
        vd.isStatic = isStatic;
        vd.vtype    = vt;
        vd.name     = vname;
        vd.init_    = initExpr;
        return vd;
    }

    // ── Parameter list ────────────────────────────────────────────────────────

    private fun List parseParamList() {
        List params = new List();
        if (this.checkKind(64)) { return params; }  // RParen = empty

        while (true) {
            if (!this.isTypeName()) {
                this.recordError("expected parameter type");
                break;
            }
            Param p   = new Param();
            p.init();
            Token ploc = this.peekTok();
            p.line  = ploc.line;
            p.col   = ploc.col;
            p.ptype = this.parseType();
            if (!this.checkKind(10)) {
                this.recordError("expected parameter name");
                break;
            }
            p.name = this.advanceTok().lexeme;
            params.add(p);
            if (!this.matchKind(72)) { break; }  // Comma
        }
        return params;
    }

    // ══════════════════════════════════════════════════════════════════════════
    //  Statement parsers
    // ══════════════════════════════════════════════════════════════════════════

    private fun var parseStatement() {
        int k = this.peekTok().kind;

        if (k == 65) { return this.parseBlock(); }         // LBrace
        if (k == 27) { return this.parseIf(); }            // KW_if
        if (k == 30) { return this.parseWhile(); }         // KW_while
        if (k == 31) { return this.parseDoWhile(); }       // KW_do
        if (k == 29) { return this.parseFor(); }           // KW_for
        if (k == 32) { return this.parseSwitch(); }        // KW_switch
        if (k == 37) { return this.parseReturn(); }        // KW_return
        if (k == 40) { return this.parseThrow(); }         // KW_throw
        if (k == 38) { return this.parseTry(); }           // KW_try
        if (k == 35) { return this.parseBreak(); }         // KW_break
        if (k == 36) { return this.parseContinue(); }      // KW_continue

        // const / static prefix
        if (k == 49 || k == 48) {   // KW_const or KW_static
            bool isConst  = false;
            bool isStatic = false;
            bool ch = true;
            while (ch) {
                ch = false;
                if (this.matchKind(49)) { isConst  = true; ch = true; }
                if (this.matchKind(48)) { isStatic = true; ch = true; }
            }
            return this.parseVarDeclStmt(isConst, isStatic);
        }

        // var decl disambiguation
        if (this.isTypeName()) {
            int k1 = this.peekAt(1).kind;
            int k2 = this.peekAt(2).kind;
            int k3 = this.peekAt(3).kind;

            // int[] name  — array type decl
            bool isArrayDecl = (k1 == 67 && k2 == 68 && k3 == 10);
            // Type name  (not followed by ( or . or [  — those are calls/member/index)
            bool isSimpleDecl = (k != 50) && !isArrayDecl &&
                                (k1 == 10) &&
                                (k2 != 63 && k2 != 73 && k2 != 67);
            bool isVarDecl = (k == 50);  // KW_var

            if (isSimpleDecl || isArrayDecl || isVarDecl) {
                return this.parseVarDeclStmt(false, false);
            }
        }

        return this.parseExprStmt();
    }

    private fun var parseBlock() {
        Token loc = this.peekTok();
        this.expectKind(65, "expected '{'");

        List stmts = new List();
        while (!this.checkKind(66) && !this.isAtEnd()) {
            var s = this.parseStatement();
            if (s != null) { stmts.add(s); }
        }

        this.expectKind(66, "expected '}' to close block");

        BlockNode n = new BlockNode();
        n.initNode(0, loc.line, loc.col, loc.filename);  // SK_Block = 0
        n.stmts = stmts;
        return n;
    }

    private fun var parseIf() {
        Token loc = this.peekTok();
        this.expectKind(27, "expected 'if'");
        this.expectKind(63, "expected '(' after 'if'");
        var cond = this.parseExpr();
        this.expectKind(64, "expected ')' after if condition");
        var thenBranch = this.parseStatement();

        var elseBranch = null;
        if (this.matchKind(28)) {  // KW_else
            elseBranch = this.parseStatement();
        }

        IfNode n = new IfNode();
        n.initNode(3, loc.line, loc.col, loc.filename);
        n.cond       = cond;
        n.thenBranch = thenBranch;
        n.elseBranch = elseBranch;
        return n;
    }

    private fun var parseWhile() {
        Token loc = this.peekTok();
        this.expectKind(30, "expected 'while'");
        this.expectKind(63, "expected '(' after 'while'");
        var cond = this.parseExpr();
        this.expectKind(64, "expected ')' after while condition");
        var body = this.parseStatement();

        WhileNode n = new WhileNode();
        n.initNode(4, loc.line, loc.col, loc.filename);
        n.cond = cond;
        n.body = body;
        return n;
    }

    private fun var parseDoWhile() {
        Token loc = this.peekTok();
        this.expectKind(31, "expected 'do'");
        var body = this.parseStatement();
        this.expectKind(30, "expected 'while' after do body");
        this.expectKind(63, "expected '(' after 'while'");
        var cond = this.parseExpr();
        this.expectKind(64, "expected ')' after do-while condition");
        this.expectKind(69, "expected ';' after do-while");

        DoWhileNode n = new DoWhileNode();
        n.initNode(5, loc.line, loc.col, loc.filename);
        n.body = body;
        n.cond = cond;
        return n;
    }

    private fun var parseFor() {
        Token loc = this.peekTok();
        this.expectKind(29, "expected 'for'");
        this.expectKind(63, "expected '(' after 'for'");

        // initializer
        var initStmt = null;
        if (!this.checkKind(69)) {  // not Semi
            int k1 = this.peekAt(1).kind;
            int k2 = this.peekAt(2).kind;
            int k3 = this.peekAt(3).kind;
            bool isArrayDecl = this.isTypeName() && k1 == 67 && k2 == 68 && k3 == 10;
            bool isVarDecl   = this.isTypeName() && k1 == 10;
            if (isArrayDecl || isVarDecl) {
                initStmt = this.parseVarDeclStmt(false, false);
            } else {
                initStmt = this.parseExprStmt();
            }
        } else {
            this.advanceTok();  // consume ';'
        }

        var cond = null;
        if (!this.checkKind(69)) {  // not Semi
            cond = this.parseExpr();
        }
        this.expectKind(69, "expected ';' after for condition");

        var incr = null;
        if (!this.checkKind(64)) {  // not RParen
            incr = this.parseExpr();
        }
        this.expectKind(64, "expected ')' after for increment");

        var body = this.parseStatement();

        ForNode n = new ForNode();
        n.initNode(6, loc.line, loc.col, loc.filename);
        n.init_ = initStmt;
        n.cond  = cond;
        n.incr  = incr;
        n.body  = body;
        return n;
    }

    private fun var parseSwitch() {
        Token loc = this.peekTok();
        this.expectKind(32, "expected 'switch'");
        this.expectKind(63, "expected '(' after 'switch'");
        var subject = this.parseExpr();
        this.expectKind(64, "expected ')' after switch subject");
        this.expectKind(65, "expected '{' to open switch body");

        List cases = new List();
        while (!this.checkKind(66) && !this.isAtEnd()) {
            CaseClause cc = new CaseClause();
            cc.init();

            if (this.matchKind(33)) {  // KW_case
                cc.value = this.parseExpr();
                this.expectKind(70, "expected ':' after case value");  // Colon
            } else if (this.matchKind(34)) {  // KW_default
                // cc.value stays null
                this.expectKind(70, "expected ':' after 'default'");
            } else {
                this.recordError("expected 'case' or 'default' in switch body");
                this.synchronize();
                continue;
            }

            while (!this.checkKind(33) && !this.checkKind(34) &&
                   !this.checkKind(66) && !this.isAtEnd()) {
                var s = this.parseStatement();
                if (s != null) { cc.stmts.add(s); }
            }
            cases.add(cc);
        }

        this.expectKind(66, "expected '}' to close switch body");

        SwitchNode n = new SwitchNode();
        n.initNode(7, loc.line, loc.col, loc.filename);
        n.subject = subject;
        n.cases   = cases;
        return n;
    }

    private fun var parseReturn() {
        Token loc = this.peekTok();
        this.expectKind(37, "expected 'return'");

        var value = null;
        if (!this.checkKind(69)) {  // not Semi
            value = this.parseExpr();
        }
        this.expectKind(69, "expected ';' after return");

        ReturnNode n = new ReturnNode();
        n.initNode(8, loc.line, loc.col, loc.filename);
        n.value = value;
        return n;
    }

    private fun var parseThrow() {
        Token loc = this.peekTok();
        this.expectKind(40, "expected 'throw'");
        var value = this.parseExpr();
        this.expectKind(69, "expected ';' after throw");

        ThrowNode n = new ThrowNode();
        n.initNode(11, loc.line, loc.col, loc.filename);
        n.value = value;
        return n;
    }

    private fun var parseTry() {
        Token loc = this.peekTok();
        this.expectKind(38, "expected 'try'");
        var tryBody = this.parseBlock();

        List catches = new List();
        while (this.checkKind(39)) {  // KW_catch
            this.advanceTok();
            this.expectKind(63, "expected '(' after 'catch'");

            CatchClause cc = new CatchClause();
            cc.init();
            cc.exType = this.parseType();
            if (this.checkKind(10)) {
                cc.exName = this.advanceTok().lexeme;
            }
            this.expectKind(64, "expected ')' after catch parameter");
            cc.body = this.parseBlock();
            catches.add(cc);
        }

        TryNode n = new TryNode();
        n.initNode(12, loc.line, loc.col, loc.filename);
        n.tryBody     = tryBody;
        n.catches     = catches;
        n.finallyBody = null;
        return n;
    }

    private fun var parseBreak() {
        Token loc = this.peekTok();
        this.expectKind(35, "expected 'break'");
        this.expectKind(69, "expected ';' after 'break'");
        BreakNode n = new BreakNode();
        n.initNode(9, loc.line, loc.col, loc.filename);
        return n;
    }

    private fun var parseContinue() {
        Token loc = this.peekTok();
        this.expectKind(36, "expected 'continue'");
        this.expectKind(69, "expected ';' after 'continue'");
        ContinueNode n = new ContinueNode();
        n.initNode(10, loc.line, loc.col, loc.filename);
        return n;
    }

    private fun var parseVarDeclStmt(bool isConst, bool isStatic) {
        Token  loc   = this.peekTok();
        TypeRef vt   = this.parseType();

        if (!this.checkKind(10)) {
            this.recordError("expected variable name");
            this.synchronize();
            return null;
        }
        string vname = this.advanceTok().lexeme;

        var initExpr = null;
        if (this.matchKind(78)) {  // Equal
            initExpr = this.parseExpr();
        }
        this.expectKind(69, "expected ';' after variable declaration");

        VarDeclStmtNode n = new VarDeclStmtNode();
        n.initNode(2, loc.line, loc.col, loc.filename);
        n.vtype    = vt;
        n.name     = vname;
        n.init_    = initExpr;
        n.isConst  = isConst;
        n.isStatic = isStatic;
        return n;
    }

    private fun var parseExprStmt() {
        Token loc  = this.peekTok();
        var   expr = this.parseExpr();
        this.expectKind(69, "expected ';' after expression");

        ExprStmtNode n = new ExprStmtNode();
        n.initNode(1, loc.line, loc.col, loc.filename);
        n.expr = expr;
        return n;
    }

    // ══════════════════════════════════════════════════════════════════════════
    //  Expression parsers  (precedence ladder)
    // ══════════════════════════════════════════════════════════════════════════

    private fun var parseExpr() {
        return this.parseAssignment();
    }

    private fun var parseAssignment() {
        var left = this.parseTernary();

        int k = this.peekTok().kind;
        if (this.isAssignOp(k)) {
            Token loc = this.peekTok();
            int   op  = this.advanceTok().kind;
            var   rhs = this.parseAssignment();  // right-associative

            AssignNode n = new AssignNode();
            n.initNode(10, loc.line, loc.col, loc.filename);
            n.op     = op;
            n.target = left;
            n.value  = rhs;
            return n;
        }
        return left;
    }

    private fun var parseTernary() {
        var left = this.parseOr();

        if (this.matchKind(77)) {  // Question
            Token loc      = this.peekTok();
            var   thenExpr = this.parseExpr();
            this.expectKind(70, "expected ':' in ternary expression");  // Colon
            var   elseExpr = this.parseTernary();

            TernaryNode n = new TernaryNode();
            n.initNode(9, loc.line, loc.col, loc.filename);
            n.cond     = left;
            n.thenExpr = thenExpr;
            n.elseExpr = elseExpr;
            return n;
        }
        return left;
    }

    private fun var parseOr() {
        var left = this.parseAnd();
        while (this.checkKind(103)) {  // PipePipe
            Token loc = this.peekTok();
            int   op  = this.advanceTok().kind;
            var   rhs = this.parseAnd();
            left = this.makeBinary(op, left, rhs, loc);
        }
        return left;
    }

    private fun var parseAnd() {
        var left = this.parseBitOr();
        while (this.checkKind(102)) {  // AmpAmp
            Token loc = this.peekTok();
            int   op  = this.advanceTok().kind;
            var   rhs = this.parseBitOr();
            left = this.makeBinary(op, left, rhs, loc);
        }
        return left;
    }

    private fun var parseBitOr() {
        var left = this.parseBitXor();
        while (this.checkKind(106)) {  // Pipe
            Token loc = this.peekTok();
            int   op  = this.advanceTok().kind;
            var   rhs = this.parseBitXor();
            left = this.makeBinary(op, left, rhs, loc);
        }
        return left;
    }

    private fun var parseBitXor() {
        var left = this.parseBitAnd();
        while (this.checkKind(107)) {  // Caret
            Token loc = this.peekTok();
            int   op  = this.advanceTok().kind;
            var   rhs = this.parseBitAnd();
            left = this.makeBinary(op, left, rhs, loc);
        }
        return left;
    }

    private fun var parseBitAnd() {
        var left = this.parseEquality();
        while (this.checkKind(105)) {  // Amp
            Token loc = this.peekTok();
            int   op  = this.advanceTok().kind;
            var   rhs = this.parseEquality();
            left = this.makeBinary(op, left, rhs, loc);
        }
        return left;
    }

    private fun var parseEquality() {
        var left = this.parseRelational();
        while (this.checkKind(96) || this.checkKind(97)) {  // EqualEqual BangEqual
            Token loc = this.peekTok();
            int   op  = this.advanceTok().kind;
            var   rhs = this.parseRelational();
            left = this.makeBinary(op, left, rhs, loc);
        }
        return left;
    }

    private fun var parseRelational() {
        var left = this.parseShift();
        while (this.checkKind(98) || this.checkKind(99) ||   // Less LessEqual
               this.checkKind(100) || this.checkKind(101) ||  // Greater GreaterEqual
               this.checkKind(61) || this.checkKind(62)) {    // instanceof as
            Token loc = this.peekTok();

            if (this.checkKind(61)) {   // KW_instanceof
                this.advanceTok();
                TypeRef itype = this.parseType();
                InstanceOfNode n = new InstanceOfNode();
                n.initNode(19, loc.line, loc.col, loc.filename);
                n.operand = left;
                n.itype   = itype;
                left = n;
            } else if (this.checkKind(62)) {   // KW_as
                this.advanceTok();
                TypeRef ctype = this.parseType();
                CastNode n = new CastNode();
                n.initNode(17, loc.line, loc.col, loc.filename);
                n.ctype   = ctype;
                n.operand = left;
                left = n;
            } else {
                int op  = this.advanceTok().kind;
                var rhs = this.parseShift();
                left = this.makeBinary(op, left, rhs, loc);
            }
        }
        return left;
    }

    private fun var parseShift() {
        var left = this.parseAdditive();
        while (this.checkKind(109) || this.checkKind(110)) {  // LShift RShift
            Token loc = this.peekTok();
            int   op  = this.advanceTok().kind;
            var   rhs = this.parseAdditive();
            left = this.makeBinary(op, left, rhs, loc);
        }
        return left;
    }

    private fun var parseAdditive() {
        var left = this.parseMultiplicative();
        while (this.checkKind(89) || this.checkKind(90)) {  // Plus Minus
            Token loc = this.peekTok();
            int   op  = this.advanceTok().kind;
            var   rhs = this.parseMultiplicative();
            left = this.makeBinary(op, left, rhs, loc);
        }
        return left;
    }

    private fun var parseMultiplicative() {
        var left = this.parseUnary();
        while (this.checkKind(91) || this.checkKind(92) || this.checkKind(93)) {  // * / %
            Token loc = this.peekTok();
            int   op  = this.advanceTok().kind;
            var   rhs = this.parseUnary();
            left = this.makeBinary(op, left, rhs, loc);
        }
        return left;
    }

    private fun var parseUnary() {
        Token loc = this.peekTok();
        int   k   = loc.kind;

        // prefix !  -  ~  ++  --
        if (k == 104 || k == 90 || k == 108 || k == 94 || k == 95) {
            int op = this.advanceTok().kind;
            var e  = this.parseUnary();
            return this.makeUnary(op, false, e, loc);
        }

        if (k == 60) {  // KW_typeof
            this.advanceTok();
            var e = this.parseUnary();
            TypeOfNode n = new TypeOfNode();
            n.initNode(16, loc.line, loc.col, loc.filename);
            n.operand = e;
            return n;
        }

        if (k == 55) {  // KW_await -- treat as unary prefix
            int op = this.advanceTok().kind;
            var e  = this.parseUnary();
            return this.makeUnary(op, false, e, loc);
        }

        if (k == 43) {  // KW_delete
            this.advanceTok();
            var e = this.parseUnary();
            DeleteNode n = new DeleteNode();
            n.initNode(15, loc.line, loc.col, loc.filename);
            n.operand = e;
            return n;
        }

        if (k == 42) {  // KW_new
            this.advanceTok();
            TypeRef ntype = this.parseType();

            if (this.matchKind(67)) {  // LBracket  — array allocation
                var sz = this.parseExpr();
                this.expectKind(68, "expected ']' in array allocation");

                NewNode n = new NewNode();
                n.initNode(14, loc.line, loc.col, loc.filename);
                n.ntype     = ntype;
                n.isArray   = true;
                n.arraySize = sz;
                n.args      = new List();
                return n;
            }

            // object construction
            this.expectKind(63, "expected '(' after type in 'new'");
            List args = this.parseArgList();
            this.expectKind(64, "expected ')' to close 'new' argument list");

            NewNode n = new NewNode();
            n.initNode(14, loc.line, loc.col, loc.filename);
            n.ntype     = ntype;
            n.isArray   = false;
            n.arraySize = null;
            n.args      = args;
            return n;
        }

        return this.parsePostfix();
    }

    private fun var parsePostfix() {
        var expr = this.parsePrimary();

        while (true) {
            Token loc = this.peekTok();
            int   k   = loc.kind;

            if (k == 94) {   // PlusPlus (postfix)
                this.advanceTok();
                expr = this.makeUnary(94, true, expr, loc);
            } else if (k == 95) {  // MinusMinus (postfix)
                this.advanceTok();
                expr = this.makeUnary(95, true, expr, loc);
            } else if (k == 73) {  // Dot
                this.advanceTok();
                if (!this.checkKind(10)) {
                    this.recordError("expected member name after '.'");
                    break;
                }
                string member = this.advanceTok().lexeme;
                MemberNode n = new MemberNode();
                n.initNode(12, loc.line, loc.col, loc.filename);
                n.object = expr;
                n.member = member;
                n.arrow  = false;
                expr = n;
            } else if (k == 75) {  // Arrow
                this.advanceTok();
                if (!this.checkKind(10)) {
                    this.recordError("expected member name after '->'");
                    break;
                }
                string member = this.advanceTok().lexeme;
                MemberNode n = new MemberNode();
                n.initNode(12, loc.line, loc.col, loc.filename);
                n.object = expr;
                n.member = member;
                n.arrow  = true;
                expr = n;
            } else if (k == 63) {  // LParen — call
                this.advanceTok();
                List args = this.parseArgList();
                this.expectKind(64, "expected ')' to close argument list");
                CallNode n = new CallNode();
                n.initNode(11, loc.line, loc.col, loc.filename);
                n.callee = expr;
                n.args   = args;
                expr = n;
            } else if (k == 67) {  // LBracket — index
                this.advanceTok();
                var idx = this.parseExpr();
                this.expectKind(68, "expected ']' after index expression");
                IndexNode n = new IndexNode();
                n.initNode(13, loc.line, loc.col, loc.filename);
                n.array = expr;
                n.index = idx;
                expr = n;
            } else if (k == 71) {  // DoubleColon — scope resolution
                Token sloc = this.peekTok();
                this.advanceTok();
                if (!this.checkKind(10)) {
                    this.recordError("expected identifier after '::'");
                    break;
                }
                string member = "::" + this.advanceTok().lexeme;
                MemberNode n = new MemberNode();
                n.initNode(12, sloc.line, sloc.col, sloc.filename);
                n.object = expr;
                n.member = member;
                n.arrow  = false;
                expr = n;
            } else {
                break;
            }
        }

        return expr;
    }

    private fun var parsePrimary() {
        Token loc = this.peekTok();
        int   k   = loc.kind;

        // Integer literal
        if (k == 3) {  // LiteralInteger
            string raw = this.advanceTok().lexeme;
            int    val = this.parseIntLiteral(raw);
            IntLitNode n = new IntLitNode();
            n.initNode(0, loc.line, loc.col, loc.filename);
            n.raw   = raw;
            n.value = val;
            return n;
        }

        // Float literal
        if (k == 4) {  // LiteralFloat
            string raw = this.advanceTok().lexeme;
            FloatLitNode n = new FloatLitNode();
            n.initNode(1, loc.line, loc.col, loc.filename);
            n.raw = raw;
            return n;
        }

        // Bool literals
        if (k == 7) {  // LiteralTrue
            this.advanceTok();
            BoolLitNode n = new BoolLitNode();
            n.initNode(2, loc.line, loc.col, loc.filename);
            n.value = true;
            return n;
        }
        if (k == 8) {  // LiteralFalse
            this.advanceTok();
            BoolLitNode n = new BoolLitNode();
            n.initNode(2, loc.line, loc.col, loc.filename);
            n.value = false;
            return n;
        }

        // Null
        if (k == 9) {  // LiteralNull
            this.advanceTok();
            NullLitNode n = new NullLitNode();
            n.initNode(5, loc.line, loc.col, loc.filename);
            return n;
        }

        // Char literal
        if (k == 5) {  // LiteralChar
            string raw = this.advanceTok().lexeme;
            CharLitNode n = new CharLitNode();
            n.initNode(3, loc.line, loc.col, loc.filename);
            n.raw = raw;
            return n;
        }

        // String literal
        if (k == 6) {  // LiteralString
            string raw = this.advanceTok().lexeme;
            string val = substring(raw, 1, stringLen(raw) - 1);
            StringLitNode n = new StringLitNode();
            n.initNode(4, loc.line, loc.col, loc.filename);
            n.value = val;
            return n;
        }

        // Identifier
        if (k == 10) {  // Identifier
            string name = this.advanceTok().lexeme;
            IdentNode n = new IdentNode();
            n.initNode(6, loc.line, loc.col, loc.filename);
            n.name = name;
            return n;
        }

        // this / super → IdentNode
        if (k == 44 || k == 45) {  // KW_this KW_super
            string name = this.advanceTok().lexeme;
            IdentNode n = new IdentNode();
            n.initNode(6, loc.line, loc.col, loc.filename);
            n.name = name;
            return n;
        }

        // Grouped expression or lambda
        if (k == 63) {  // LParen
            this.advanceTok();

            // Lambda detection: check if pattern is (Type name [, Type name]*) =>
            bool isLambda = this.looksLikeLambda();

            if (isLambda) {
                List params = this.parseParamList();
                this.expectKind(64, "expected ')' in lambda");
                this.expectKind(76, "expected '=>' in lambda");  // FatArrow

                LambdaNode n = new LambdaNode();
                n.initNode(18, loc.line, loc.col, loc.filename);
                n.params = params;

                if (this.checkKind(65)) {  // LBrace — block body
                    n.body      = null;
                    n.blockBody = this.parseBlock();
                } else {
                    n.body      = this.parseExpr();
                    n.blockBody = null;
                }
                return n;
            }

            // Regular grouped expression
            var expr = this.parseExpr();
            this.expectKind(64, "expected ')' after grouped expression");
            return expr;
        }

        // Array literal { expr, expr, ... }
        if (k == 65) {  // LBrace
            this.advanceTok();
            List elems = new List();
            if (!this.checkKind(66)) {  // RBrace
                elems.add(this.parseExpr());
                while (this.matchKind(72) && !this.checkKind(66)) {  // Comma
                    elems.add(this.parseExpr());
                }
            }
            this.expectKind(66, "expected '}' to close array literal");

            ArrayLitNode n = new ArrayLitNode();
            n.initNode(20, loc.line, loc.col, loc.filename);
            n.elements = elems;
            return n;
        }

        // Error recovery
        this.recordError("unexpected token '" + this.peekTok().lexeme + "' in expression");
        this.advanceTok();
        NullLitNode err = new NullLitNode();
        err.initNode(5, loc.line, loc.col, loc.filename);
        return err;
    }

    // Lambda look-ahead: check if (Type name [, Type name]* ) =>
    private fun bool looksLikeLambda() {
        // zero-param lambda:  ) =>
        if (this.checkKind(64) && this.peekAt(1).kind == 76) {
            return true;
        }

        // save position and try to match the parameter pattern
        int savedPos = this.pos;
        bool ok = this.tryMatchLambdaParams();
        this.pos = savedPos;  // restore
        return ok;
    }

    private fun bool tryMatchLambdaParams() {
        // Try to match  Type name [, Type name]*  ) =>
        bool first = true;
        while (true) {
            // type: builtin or identifier
            int k = this.peekTok().kind;
            bool isBuiltin = (k >= 11 && k <= 21);
            if (!isBuiltin && k != 10) { return false; }
            this.advanceTok();
            // optional []
            if (this.checkKind(67) && this.peekAt(1).kind == 68) {
                this.advanceTok(); this.advanceTok();
            }
            // name must be identifier
            if (!this.checkKind(10)) { return false; }
            this.advanceTok();
            first = false;
            // comma or )
            if (this.checkKind(64)) { break; }  // RParen
            if (!this.matchKind(72)) { return false; }  // not Comma
        }
        // expect ) =>
        if (!this.checkKind(64)) { return false; }
        this.advanceTok();  // )
        return this.checkKind(76);  // FatArrow
    }

    private fun List parseArgList() {
        List args = new List();
        if (this.checkKind(64)) { return args; }  // RParen

        while (true) {
            args.add(this.parseExpr());
            if (!this.matchKind(72)) { break; }  // Comma
        }
        return args;
    }

    // ── Helper factories ──────────────────────────────────────────────────────

    private fun var makeBinary(int op, var left, var right, Token loc) {
        BinaryNode n = new BinaryNode();
        n.initNode(8, loc.line, loc.col, loc.filename);
        n.op    = op;
        n.left  = left;
        n.right = right;
        return n;
    }

    private fun var makeUnary(int op, bool post, var operand, Token loc) {
        UnaryNode n = new UnaryNode();
        n.initNode(7, loc.line, loc.col, loc.filename);
        n.op      = op;
        n.postfix = post;
        n.operand = operand;
        return n;
    }

    // Parse an integer literal lexeme → int value
    private fun int parseIntLiteral(string raw) {
        // strip underscores
        StringBuilder sb = new StringBuilder();
        int i = 0;
        while (i < stringLen(raw)) {
            char c = raw[i];
            if (c != '_') { sb.appendChar(c); }
            i = i + 1;
        }
        string clean = sb.toString();

        if (stringLen(clean) == 0) { return 0; }

        // strip trailing suffixes u/l/U/L
        int end = stringLen(clean);
        while (end > 0) {
            char last = clean[end - 1];
            if (last == 'u' || last == 'U' || last == 'l' || last == 'L') {
                end = end - 1;
            } else {
                break;
            }
        }
        clean = substring(clean, 0, end);

        if (stringLen(clean) >= 2 && clean[0] == '0') {
            char base = clean[1];
            if (base == 'x' || base == 'X') {
                return this.parseHex(substring(clean, 2, stringLen(clean)));
            }
            if (base == 'b' || base == 'B') {
                return this.parseBin(substring(clean, 2, stringLen(clean)));
            }
            if (base == 'o' || base == 'O') {
                return this.parseOct(substring(clean, 2, stringLen(clean)));
            }
        }

        return parseInt(clean);
    }

    private fun int parseHex(string s) {
        int result = 0;
        int i = 0;
        while (i < stringLen(s)) {
            char c = s[i];
            int  d = 0;
            if (c >= '0' && c <= '9') {
                d = charCode(c) - 48;
            } else if (c >= 'a' && c <= 'f') {
                d = charCode(c) - 87;
            } else if (c >= 'A' && c <= 'F') {
                d = charCode(c) - 55;
            }
            result = result * 16 + d;
            i = i + 1;
        }
        return result;
    }

    private fun int parseBin(string s) {
        int result = 0;
        int i = 0;
        while (i < stringLen(s)) {
            result = result * 2 + (charCode(s[i]) - 48);
            i = i + 1;
        }
        return result;
    }

    private fun int parseOct(string s) {
        int result = 0;
        int i = 0;
        while (i < stringLen(s)) {
            result = result * 8 + (charCode(s[i]) - 48);
            i = i + 1;
        }
        return result;
    }
}

// ── String boxing helper ──────────────────────────────────────────────────────
// Store a string in a List (which uses var/i8*) by wrapping in a single-element array

fun var _boxStr(string s) {
    string[] arr = new string[1];
    arr[0] = s;
    return arr;
}

fun string _unboxStr(var v) {
    string[] arr = v as string[];
    return arr[0];
}

// ── Test driver ───────────────────────────────────────────────────────────────

fun int main() {
    string source = "fun int add(int a, int b) throws { return a + b; }\nfun int main() { int x = add(1,2); return x; }";

    Lexer lexer = new Lexer("test", "");
    lexer.init(source, "test.ec");
    List tokens = lexer.tokenize();

    print("Tokens: "); println(toString(tokens.length()));

    Parser parser = new Parser(tokens, "");
    parser.init(tokens, "test.ec");
    Program prog = parser.parse();

    print("Declarations parsed: ");
    println(toString(prog.declarations.length()));

    List errs = parser.getErrors();
    print("Parse errors: ");
    println(toString(errs.length()));

    int i = 0;
    while (i < errs.length()) {
        ParseError e = errs.get(i) as ParseError;
        print(toString(e.line));
        print(":");
        print(toString(e.col));
        print(": ");
        println(e.msg);
        i = i + 1;
    }

    println("Done.");
    return 0;
}
