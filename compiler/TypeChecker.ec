// ============================================================================
//  TypeChecker.lang  --  ExtendedC port of the two-pass type checker
//
//  Pass 1: hoist all top-level declarations into the symbol table
//  Pass 2: check every declaration body
//
//  TypeDesc is a class with integer kind + name + elementType etc.
//  SymbolTable is a scoped stack of Map objects.
//  Errors/warnings are collected into a List of TypeError objects.
// ============================================================================

import "std.io";
import "std.string";
import "std.collections";
import "Lexer";
import "Parser";

// ── TypeKind constants ────────────────────────────────────────────────────────
// Mirror C++ enum class TypeKind exactly

int TYK_Void    = 0;
int TYK_Bool    = 1;
int TYK_Char    = 2;
int TYK_Int     = 3;
int TYK_UInt    = 4;
int TYK_Long    = 5;
int TYK_ULong   = 6;
int TYK_Float   = 7;
int TYK_UFloat  = 8;
int TYK_Double  = 9;
int TYK_String  = 10;
int TYK_Array   = 11;
int TYK_Pointer = 12;
int TYK_Enum    = 13;
int TYK_Struct  = 14;
int TYK_Union   = 15;
int TYK_Class   = 16;
int TYK_Function= 17;
int TYK_Null    = 18;
int TYK_Unknown = 19;
int TYK_Var     = 20;

// ── TypeDesc ──────────────────────────────────────────────────────────────────

class TypeDesc {
    public int    kind;
    public string name;
    public bool   isArray_;
    public bool   isPointer_;
    // For Array/Pointer: element type (TypeDesc or null)
    public var    elementType;
    // For Function: return type + param type list
    public var    returnType;   // TypeDesc or null
    public List   paramTypes;   // List of TypeDesc

    public fun void init() {
        this.kind        = 19;  // Unknown
        this.name        = "<unknown>";
        this.isArray_    = false;
        this.isPointer_  = false;
        this.elementType = null;
        this.returnType  = null;
        this.paramTypes  = new List();
    }

    // Factories
    public fun bool isVoid()    { return this.kind == 0;  }
    public fun bool isBool()    { return this.kind == 1;  }
    public fun bool isChar()    { return this.kind == 2;  }
    public fun bool isString()  { return this.kind == 10; }
    public fun bool isNull()    { return this.kind == 18; }
    public fun bool isUnknown() { return this.kind == 19 || this.kind == 20; }
    public fun bool isVar()     { return this.kind == 20; }

    public fun bool isNumeric() {
        int k = this.kind;
        return k == 2 || k == 3 || k == 4 || k == 5 ||
               k == 6 || k == 7 || k == 8 || k == 9;
    }

    public fun bool isInteger() {
        int k = this.kind;
        return k == 2 || k == 3 || k == 4 || k == 5 || k == 6;
    }

    public fun string typeStr() {
        return this.name;
    }
}

fun TypeDesc makeTypeDesc(int kind, string name) {
    TypeDesc t = new TypeDesc();
    t.init();
    t.kind = kind;
    t.name = name;
    return t;
}

fun TypeDesc makeVoidType()    { return makeTypeDesc(0,  "void");    }
fun TypeDesc makeBoolType()    { return makeTypeDesc(1,  "bool");    }
fun TypeDesc makeCharType()    { return makeTypeDesc(2,  "char");    }
fun TypeDesc makeIntType()     { return makeTypeDesc(3,  "int");     }
fun TypeDesc makeUIntType()    { return makeTypeDesc(4,  "uint");    }
fun TypeDesc makeLongType()    { return makeTypeDesc(5,  "long");    }
fun TypeDesc makeULongType()   { return makeTypeDesc(6,  "ulong");   }
fun TypeDesc makeFloatType()   { return makeTypeDesc(7,  "float");   }
fun TypeDesc makeDoubleType()  { return makeTypeDesc(9,  "double");  }
fun TypeDesc makeStringType()  { return makeTypeDesc(10, "string");  }
fun TypeDesc makeNullType()    { return makeTypeDesc(18, "null");    }
fun TypeDesc makeUnknownType() { return makeTypeDesc(19, "<unknown>"); }

fun TypeDesc makeArrayType(TypeDesc elem) {
    TypeDesc t = new TypeDesc();
    t.init();
    t.kind        = 11;  // Array
    t.name        = elem.name + "[]";
    t.isArray_    = true;
    t.elementType = elem;
    return t;
}

fun TypeDesc makeNamedType(int kind, string name) {
    return makeTypeDesc(kind, name);
}

fun TypeDesc makeFunctionType(string fname, TypeDesc ret) {
    TypeDesc t = new TypeDesc();
    t.init();
    t.kind       = 17;  // Function
    t.name       = "fun " + fname;
    t.returnType = ret;
    return t;
}

// ── SymbolKind ────────────────────────────────────────────────────────────────

int SK_Variable   = 0;
int SK_Parameter  = 1;
int SK_Function   = 2;
int SK_Class      = 3;
int SK_Struct     = 4;
int SK_Union      = 5;
int SK_Enum       = 6;
int SK_EnumMember = 7;

// ── Symbol ────────────────────────────────────────────────────────────────────

class Symbol {
    public string   name;
    public int      skind;     // SK_*
    public TypeDesc stype;
    public bool     throws_;
    // AST node backpointers (used for member lookup and hierarchy walking)
    public var      classNode;  // ClassDeclNode or null
    public var      structNode;
    public var      unionNode;
    public var      enumNode;
    // Parameter types (for function symbols)
    public List     paramTypes;  // List of TypeDesc

    public fun void init() {
        this.name       = "";
        this.skind      = 0;
        this.stype      = makeUnknownType();
        this.throws_    = false;
        this.classNode  = null;
        this.structNode = null;
        this.unionNode  = null;
        this.enumNode   = null;
        this.paramTypes = new List();
    }
}

// ── Scope kind constants ──────────────────────────────────────────────────────

int SCOPE_Global   = 0;
int SCOPE_Function = 1;
int SCOPE_Class    = 2;
int SCOPE_Block    = 3;
int SCOPE_Loop     = 4;
int SCOPE_Switch   = 5;

// ── ScopeFrame ────────────────────────────────────────────────────────────────
// One entry in the scope stack

class ScopeFrame {
    public int    kind;      // SCOPE_*
    public Map    symbols;   // name → boxed Symbol
    public var    retType;   // TypeDesc or null (for function scopes)
    public bool   throws_;
    public bool   inTry;

    public fun void init() {
        this.kind    = 0;
        this.symbols = new Map();
        this.retType = null;
        this.throws_ = false;
        this.inTry   = false;
    }

    public fun void initKind(int k) {
        this.init();
        this.kind = k;
    }
}

// ── SymbolTable ───────────────────────────────────────────────────────────────

class SymbolTable {
    private List scopes;  // List of ScopeFrame (index 0 = global/outermost)

    public fun void init() {
        this.scopes = new List();
        // Push global scope
        ScopeFrame global = new ScopeFrame();
        global.initKind(0);  // SCOPE_Global
        this.scopes.add(global);
    }

    public fun void push(int kind) {
        ScopeFrame frame = new ScopeFrame();
        frame.initKind(kind);
        this.scopes.add(frame);
    }

    public fun void pop() {
        int n = this.scopes.length();
        if (n > 1) {
            this.scopes.remove(n - 1);
        }
    }

    // Define in current (innermost) scope. Returns false if already defined.
    public fun bool define(Symbol sym) {
        int n = this.scopes.length();
        ScopeFrame cur = this.scopes.get(n - 1) as ScopeFrame;
        if (cur.symbols.has(sym.name)) {
            return false;
        }
        cur.symbols.set(sym.name, sym);
        return true;
    }

    // Look up walking from innermost to outermost scope
    public fun Symbol lookup(string name) {
        int i = this.scopes.length() - 1;
        while (i >= 0) {
            ScopeFrame frame = this.scopes.get(i) as ScopeFrame;
            if (frame.symbols.has(name)) {
                return frame.symbols.get(name) as Symbol;
            }
            i = i - 1;
        }
        return null;
    }

    // Look up only in the current scope
    public fun Symbol lookupLocal(string name) {
        int n = this.scopes.length();
        ScopeFrame cur = this.scopes.get(n - 1) as ScopeFrame;
        if (cur.symbols.has(name)) {
            return cur.symbols.get(name) as Symbol;
        }
        return null;
    }

    public fun void setReturnType(TypeDesc t) {
        int n = this.scopes.length();
        int i = n - 1;
        while (i >= 0) {
            ScopeFrame frame = this.scopes.get(i) as ScopeFrame;
            if (frame.kind == 1) {  // Function scope
                frame.retType = t;
                return;
            }
            i = i - 1;
        }
    }

    public fun void setInThrows(bool v) {
        int n = this.scopes.length();
        int i = n - 1;
        while (i >= 0) {
            ScopeFrame frame = this.scopes.get(i) as ScopeFrame;
            if (frame.kind == 1) {
                frame.throws_ = v;
                return;
            }
            i = i - 1;
        }
    }

    public fun TypeDesc currentReturnType() {
        int i = this.scopes.length() - 1;
        while (i >= 0) {
            ScopeFrame frame = this.scopes.get(i) as ScopeFrame;
            if (frame.kind == 1 && frame.retType != null) {
                return frame.retType as TypeDesc;
            }
            i = i - 1;
        }
        return null;
    }

    public fun bool inThrowsFunction() {
        int i = this.scopes.length() - 1;
        while (i >= 0) {
            ScopeFrame frame = this.scopes.get(i) as ScopeFrame;
            if (frame.kind == 1) { return frame.throws_; }
            i = i - 1;
        }
        return false;
    }

    public fun bool inLoop() {
        int i = this.scopes.length() - 1;
        while (i >= 0) {
            ScopeFrame frame = this.scopes.get(i) as ScopeFrame;
            if (frame.kind == 4) { return true; }   // Loop
            if (frame.kind == 1) { return false; }  // Stop at function boundary
            i = i - 1;
        }
        return false;
    }

    public fun bool inSwitch() {
        int i = this.scopes.length() - 1;
        while (i >= 0) {
            ScopeFrame frame = this.scopes.get(i) as ScopeFrame;
            if (frame.kind == 5) { return true; }   // Switch
            if (frame.kind == 1) { return false; }
            i = i - 1;
        }
        return false;
    }

    public fun void enterTry() {
        int n = this.scopes.length();
        ScopeFrame cur = this.scopes.get(n - 1) as ScopeFrame;
        cur.inTry = true;
    }

    public fun void exitTry() {
        int n = this.scopes.length();
        ScopeFrame cur = this.scopes.get(n - 1) as ScopeFrame;
        cur.inTry = false;
    }

    public fun bool inTryBlock() {
        int i = this.scopes.length() - 1;
        while (i >= 0) {
            ScopeFrame frame = this.scopes.get(i) as ScopeFrame;
            if (frame.inTry) { return true; }
            if (frame.kind == 1) { return false; }
            i = i - 1;
        }
        return false;
    }
}

// ── TypeError ─────────────────────────────────────────────────────────────────

class TypeError {
    public string msg;
    public int    line;
    public int    col;
    public string filename;
    public bool   isWarning;

    public fun void init() {
        this.msg       = "";
        this.line      = 1;
        this.col       = 1;
        this.filename  = "";
        this.isWarning = false;
    }

    public fun string format() {
        StringBuilder sb = new StringBuilder();
        sb.append(this.filename);
        sb.append(":");
        sb.appendInt(this.line);
        sb.append(":");
        sb.appendInt(this.col);
        sb.append(": ");
        sb.append(this.isWarning ? "warning" : "error");
        sb.append(": ");
        sb.append(this.msg);
        return sb.toString();
    }
}

// ── TypeChecker ───────────────────────────────────────────────────────────────

class TypeChecker {
    private SymbolTable symTable;
    private List        errors;      // List of TypeError
    private string      filename;
    private var         currentClass;  // ClassDeclNode or null
    private Set         resolvedImports;  // import paths already merged

    public fun void init(string fname) {
        this.symTable        = new SymbolTable();
        this.symTable.init();
        this.errors          = new List();
        this.filename        = fname;
        this.currentClass    = null;
        this.resolvedImports = new Set();
    }

    public fun List getErrors()    { return this.errors; }
    public fun bool hadError() {
        int i = 0;
        while (i < this.errors.length()) {
            TypeError e = this.errors.get(i) as TypeError;
            if (!e.isWarning) { return true; }
            i = i + 1;
        }
        return false;
    }

    public fun void registerResolvedImport(string path) {
        this.resolvedImports.add(path);
    }

    // ── Error emission ────────────────────────────────────────────────────────

    private fun void emitError(string msg, int line, int col, string fname) {
        TypeError e = new TypeError();
        e.init();
        e.msg       = msg;
        e.line      = line;
        e.col       = col;
        e.filename  = fname;
        e.isWarning = false;
        this.errors.add(e);
    }

    private fun void emitWarn(string msg, int line, int col, string fname) {
        TypeError e = new TypeError();
        e.init();
        e.msg       = msg;
        e.line      = line;
        e.col       = col;
        e.filename  = fname;
        e.isWarning = true;
        this.errors.add(e);
    }

    private fun void err(string msg, ASTNode n) {
        this.emitError(msg, n.line, n.col, n.filename);
    }

    private fun void warn(string msg, ASTNode n) {
        this.emitWarn(msg, n.line, n.col, n.filename);
    }

    // ── Type resolution ───────────────────────────────────────────────────────

    // Resolve a TypeRef from the Parser into a TypeDesc
    private fun TypeDesc resolveTypeRef(TypeRef tr) {
        TypeDesc base;

        string n = tr.name;
        if (n == "var") {
            base = makeTypeDesc(20, "var");
        } else if (n == "void")   { base = makeVoidType();   }
        else if (n == "bool")     { base = makeBoolType();   }
        else if (n == "char")     { base = makeCharType();   }
        else if (n == "int")      { base = makeIntType();    }
        else if (n == "uint")     { base = makeUIntType();   }
        else if (n == "long")     { base = makeLongType();   }
        else if (n == "ulong")    { base = makeULongType();  }
        else if (n == "float")    { base = makeFloatType();  }
        else if (n == "ufloat")   { base = makeTypeDesc(8, "ufloat"); }
        else if (n == "double")   { base = makeDoubleType(); }
        else if (n == "string")   { base = makeStringType(); }
        else {
            // User-defined type
            Symbol sym = this.symTable.lookup(n);
            if (sym == null) {
                base = makeNamedType(19, n);  // Unknown placeholder
            } else {
                base = sym.stype;
            }
        }

        if (tr.isArray)   { return makeArrayType(base); }
        if (tr.isPointer) {
            TypeDesc t = new TypeDesc();
            t.init();
            t.kind        = 12;  // Pointer
            t.name        = base.name + "*";
            t.isPointer_  = true;
            t.elementType = base;
            return t;
        }
        return base;
    }

    // ── Type compatibility ────────────────────────────────────────────────────

    private fun bool isAssignable(TypeDesc target, TypeDesc value) {
        if (target == null || value == null) { return true; }
        if (target.isUnknown() || value.isUnknown()) { return true; }

        // null → class, pointer, string, var
        if (value.isNull()) {
            int tk = target.kind;
            return tk == 16 || tk == 12 || tk == 10 || tk == 20 || tk == 19;
        }

        // exact match
        if (target.name == value.name) { return true; }

        // Polymorphic upcast: walk value's class hierarchy
        if (target.kind == 16 && value.kind == 16) {
            string cur = value.name;
            int depth = 0;
            while (stringLen(cur) > 0 && depth < 32) {
                if (cur == target.name) { return true; }
                Symbol sym = this.symTable.lookup(cur);
                if (sym == null || sym.classNode == null) { break; }
                ClassDeclNode cls = sym.classNode as ClassDeclNode;
                if (!cls.hasSuperClass) { break; }
                cur = cls.superClass;
                depth = depth + 1;
            }
        }

        // bool/char widen to int
        int tkind = target.kind;
        if (tkind == 3 || tkind == 4 || tkind == 5 || tkind == 6 ||
            tkind == 7 || tkind == 9) {
            int vkind = value.kind;
            if (vkind == 1 || vkind == 2) { return true; }  // bool or char
        }

        // Numeric widening
        if (target.isNumeric() && value.isNumeric()) {
            return this.numericRank(target.kind) >= this.numericRank(value.kind);
        }

        // var target: always ok
        if (target.kind == 20) { return true; }
        // null-typed target: anything can replace it
        if (target.kind == 18) { return true; }

        return false;
    }

    private fun int numericRank(int kind) {
        if (kind == 2) { return 0; }   // char
        if (kind == 3) { return 1; }   // int
        if (kind == 4) { return 1; }   // uint
        if (kind == 5) { return 3; }   // long
        if (kind == 6) { return 3; }   // ulong
        if (kind == 7) { return 5; }   // float
        if (kind == 8) { return 6; }   // ufloat
        if (kind == 9) { return 7; }   // double
        return -1;
    }

    private fun bool isComparable(TypeDesc a, TypeDesc b) {
        if (a == null || b == null) { return true; }
        if (a.isUnknown() || b.isUnknown()) { return true; }
        if (a.name == b.name) { return true; }
        if (a.isNumeric() && b.isNumeric()) { return true; }
        if (a.isBool() && b.isBool()) { return true; }
        if (a.isNull() || b.isNull()) { return true; }
        if (a.kind == 20 || b.kind == 20) { return true; }  // var
        return false;
    }

    private fun TypeDesc widenNumeric(TypeDesc a, TypeDesc b) {
        if (a == null || b == null) { return makeUnknownType(); }
        return this.numericRank(a.kind) >= this.numericRank(b.kind) ? a : b;
    }

    private fun string typeMismatch(string ctx, TypeDesc expected, TypeDesc got) {
        string e = expected != null ? expected.typeStr() : "?";
        string g = got      != null ? got.typeStr()      : "?";
        return ctx + ": expected '" + e + "' but got '" + g + "'";
    }

    // ── Entry point ───────────────────────────────────────────────────────────

    public fun void check(Program prog) {
        // Handle imports
        int i = 0;
        while (i < prog.declarations.length()) {
            ASTNode decl = prog.declarations.get(i) as ASTNode;
            if (decl.kind == 0) {  // DK_Import
                ImportDeclNode imp = decl as ImportDeclNode;
                if (!this.resolvedImports.has(imp.path)) {
                    this.registerLibrary(imp.path, decl);
                }
            }
            i = i + 1;
        }

        // Pass 1: hoist all top-level names
        i = 0;
        while (i < prog.declarations.length()) {
            ASTNode decl = prog.declarations.get(i) as ASTNode;
            this.hoistDecl(decl);
            i = i + 1;
        }

        // Pass 2: check bodies
        i = 0;
        while (i < prog.declarations.length()) {
            ASTNode decl = prog.declarations.get(i) as ASTNode;
            this.checkDecl(decl);
            i = i + 1;
        }
    }

    // ── Standard library registration ─────────────────────────────────────────

    private fun void registerFn(string name, TypeDesc ret, List paramTys) {
        TypeDesc fnType = makeFunctionType(name, ret);
        int i = 0;
        while (i < paramTys.length()) {
            TypeDesc pt = paramTys.get(i) as TypeDesc;
            fnType.paramTypes.add(pt);
            i = i + 1;
        }

        Symbol sym = new Symbol();
        sym.init();
        sym.name       = name;
        sym.skind      = 2;  // SK_Function
        sym.stype      = fnType;
        sym.paramTypes = fnType.paramTypes;
        this.symTable.define(sym);  // silent if already defined
    }

    private fun void registerFn0(string name, TypeDesc ret) {
        this.registerFn(name, ret, new List());
    }

    private fun void registerFn1(string name, TypeDesc ret, TypeDesc p1) {
        List ps = new List(); ps.add(p1);
        this.registerFn(name, ret, ps);
    }

    private fun void registerFn2(string name, TypeDesc ret, TypeDesc p1, TypeDesc p2) {
        List ps = new List(); ps.add(p1); ps.add(p2);
        this.registerFn(name, ret, ps);
    }

    private fun void registerFn3(string name, TypeDesc ret, TypeDesc p1, TypeDesc p2, TypeDesc p3) {
        List ps = new List(); ps.add(p1); ps.add(p2); ps.add(p3);
        this.registerFn(name, ret, ps);
    }

    private fun void registerLibrary(string path, ASTNode loc) {
        if (path == "std.io") {
            TypeDesc vd  = makeVoidType();
            TypeDesc bd  = makeBoolType();
            TypeDesc cd  = makeCharType();
            TypeDesc id  = makeIntType();
            TypeDesc ld  = makeLongType();
            TypeDesc sd  = makeStringType();

            this.registerFn0("gcCollect",     vd);
            this.registerFn0("gcStats",       sd);
            this.registerFn0("gcLiveBytes",   ld);
            this.registerFn0("gcLiveObjects", ld);
            this.registerFn0("getChar",       cd);
            this.registerFn1("putChar",       vd, cd);
            this.registerFn1("print",         vd, sd);
            this.registerFn1("println",       vd, sd);
            this.registerFn0("printNewline",  vd);
            this.registerFn0("readLine",      sd);
            this.registerFn0("readInt",       id);
            this.registerFn0("readFloat",     makeFloatType());
            this.registerFn1("printInt",      vd, id);
            this.registerFn1("printFloat",    vd, makeFloatType());
            this.registerFn1("printBool",     vd, bd);
            this.registerFn1("prompt",        sd, sd);
            this.registerFn1("readFile",      sd, sd);
            this.registerFn2("writeFile",     id, sd, sd);
            this.registerFn1("fileExists",    id, sd);
            this.registerFn1("exit",          vd, id);
            this.registerFn0("timeMs",        ld);
            return;
        }

        if (path == "std.string") {
            TypeDesc vd = makeVoidType();
            TypeDesc bd = makeBoolType();
            TypeDesc cd = makeCharType();
            TypeDesc id = makeIntType();
            TypeDesc sd = makeStringType();
            TypeDesc fd = makeFloatType();
            TypeDesc dd = makeDoubleType();
            TypeDesc ld = makeLongType();

            this.registerFn1("toString",      sd, id);
            this.registerFn1("toString",      sd, bd);
            this.registerFn1("toString",      sd, cd);
            this.registerFn1("toString",      sd, ld);
            this.registerFn1("longToString",  sd, ld);
            this.registerFn1("floatToString", sd, fd);
            this.registerFn1("doubleToString",sd, dd);
            this.registerFn1("charToString",  sd, cd);
            this.registerFn1("boolToString",  sd, bd);
            this.registerFn1("parseInt",      id, sd);
            this.registerFn1("parseLong",     ld, sd);
            this.registerFn1("parseFloat",    fd, sd);
            this.registerFn1("parseDouble",   dd, sd);
            this.registerFn1("stringLen",     id, sd);
            this.registerFn1("stringEmpty",   bd, sd);
            this.registerFn2("charAt",        cd, sd, id);
            this.registerFn2("indexOf",       id, sd, sd);
            this.registerFn2("lastIndexOf",   id, sd, sd);
            this.registerFn2("contains",      bd, sd, sd);
            this.registerFn2("startsWith",    bd, sd, sd);
            this.registerFn2("endsWith",      bd, sd, sd);
            this.registerFn2("count",         id, sd, sd);
            this.registerFn2("equals",        bd, sd, sd);
            this.registerFn2("compare",       id, sd, sd);
            this.registerFn3("substring",     sd, sd, id, id);
            this.registerFn1("toLower",       sd, sd);
            this.registerFn1("toUpper",       sd, sd);
            this.registerFn1("trim",          sd, sd);
            this.registerFn1("trimLeft",      sd, sd);
            this.registerFn1("trimRight",     sd, sd);
            this.registerFn2("repeat",        sd, sd, id);
            this.registerFn3("replace",       sd, sd, sd, sd);
            this.registerFn1("reverse",       sd, sd);
            this.registerFn1("isDigit",       bd, cd);
            this.registerFn1("isAlpha",       bd, cd);
            this.registerFn1("isAlnum",       bd, cd);
            this.registerFn1("isSpace",       bd, cd);
            this.registerFn1("isUpper",       bd, cd);
            this.registerFn1("isLower",       bd, cd);
            this.registerFn1("isPunct",       bd, cd);
            this.registerFn1("toLowerChar",   cd, cd);
            this.registerFn1("toUpperChar",   cd, cd);
            this.registerFn1("charCode",      id, cd);
            this.registerFn1("charFromCode",  cd, id);
            return;
        }

        if (path == "std.math") {
            TypeDesc id = makeIntType();
            TypeDesc ld = makeLongType();
            TypeDesc dd = makeDoubleType();
            TypeDesc bd = makeBoolType();

            this.registerFn1("floor",     dd, dd);
            this.registerFn1("ceil",      dd, dd);
            this.registerFn1("round",     dd, dd);
            this.registerFn1("trunc",     dd, dd);
            this.registerFn1("abs",       dd, dd);
            this.registerFn1("absInt",    id, id);
            this.registerFn1("absLong",   ld, ld);
            this.registerFn1("sqrt",      dd, dd);
            this.registerFn2("pow",       dd, dd, dd);
            this.registerFn2("powInt",    id, id, id);
            this.registerFn1("cbrt",      dd, dd);
            this.registerFn2("hypot",     dd, dd, dd);
            this.registerFn1("log",       dd, dd);
            this.registerFn1("log2",      dd, dd);
            this.registerFn1("log10",     dd, dd);
            this.registerFn1("sin",       dd, dd);
            this.registerFn1("cos",       dd, dd);
            this.registerFn1("tan",       dd, dd);
            this.registerFn1("asin",      dd, dd);
            this.registerFn1("acos",      dd, dd);
            this.registerFn1("atan",      dd, dd);
            this.registerFn2("atan2",     dd, dd, dd);
            this.registerFn1("toRadians", dd, dd);
            this.registerFn1("toDegrees", dd, dd);
            this.registerFn2("min",       dd, dd, dd);
            this.registerFn2("max",       dd, dd, dd);
            this.registerFn2("minInt",    id, id, id);
            this.registerFn2("maxInt",    id, id, id);
            this.registerFn2("minLong",   ld, ld, ld);
            this.registerFn2("maxLong",   ld, ld, ld);
            this.registerFn3("clamp",     dd, dd, dd, dd);
            this.registerFn3("clampInt",  id, id, id, id);
            this.registerFn2("gcd",       id, id, id);
            this.registerFn2("lcm",       id, id, id);
            this.registerFn1("isEven",    bd, id);
            this.registerFn1("isOdd",     bd, id);
            this.registerFn1("isPrime",   bd, id);
            return;
        }

        if (path == "std.collections") {
            // Classes are parsed from the .lang file — just suppress warning
            return;
        }

        this.emitWarn("unknown import '" + path + "' — no type information available",
                      loc.line, loc.col, loc.filename);
    }

    // ── Pass 1: Hoist ─────────────────────────────────────────────────────────

    private fun void hoistDecl(ASTNode decl) {
        int k = decl.kind;
        if (k == 0) { return; }  // Import — nothing to hoist
        if (k == 1) { this.hoistEnum(decl);     return; }
        if (k == 2) { this.hoistStruct(decl);   return; }
        if (k == 3) { this.hoistUnion(decl);    return; }
        if (k == 4) { this.hoistClass(decl);    return; }
        if (k == 5) { this.hoistFunction(decl, ""); return; }
        if (k == 6) { this.hoistTopVar(decl);   return; }
    }

    private fun void hoistEnum(ASTNode n) {
        EnumDeclNode ed = n as EnumDeclNode;
        TypeDesc t = makeNamedType(13, ed.name);  // Enum

        Symbol sym = new Symbol();
        sym.init();
        sym.name     = ed.name;
        sym.skind    = 6;  // SK_Enum
        sym.stype    = t;
        sym.enumNode = ed;
        if (!this.symTable.define(sym)) {
            this.err("redefinition of '" + ed.name + "'", n);
        }

        // Register each member
        int i = 0;
        while (i < ed.members.length()) {
            string mname = _unboxStr(ed.members.get(i));
            Symbol ms = new Symbol();
            ms.init();
            ms.name  = ed.name + "::" + mname;
            ms.skind = 7;  // SK_EnumMember
            ms.stype = t;
            this.symTable.define(ms);

            Symbol ms2 = new Symbol();
            ms2.init();
            ms2.name  = mname;
            ms2.skind = 7;
            ms2.stype = t;
            this.symTable.define(ms2);
            i = i + 1;
        }
    }

    private fun void hoistStruct(ASTNode n) {
        StructDeclNode sd = n as StructDeclNode;
        TypeDesc t = makeNamedType(14, sd.name);  // Struct
        Symbol sym = new Symbol();
        sym.init();
        sym.name       = sd.name;
        sym.skind      = 4;  // SK_Struct
        sym.stype      = t;
        sym.structNode = sd;
        if (!this.symTable.define(sym)) {
            this.err("redefinition of '" + sd.name + "'", n);
        }
    }

    private fun void hoistUnion(ASTNode n) {
        UnionDeclNode ud = n as UnionDeclNode;
        TypeDesc t = makeNamedType(15, ud.name);  // Union
        Symbol sym = new Symbol();
        sym.init();
        sym.name      = ud.name;
        sym.skind     = 5;  // SK_Union
        sym.stype     = t;
        sym.unionNode = ud;
        if (!this.symTable.define(sym)) {
            this.err("redefinition of '" + ud.name + "'", n);
        }
    }

    private fun void hoistClass(ASTNode n) {
        ClassDeclNode cd = n as ClassDeclNode;
        TypeDesc t = makeNamedType(16, cd.name);  // Class
        Symbol sym = new Symbol();
        sym.init();
        sym.name      = cd.name;
        sym.skind     = 3;  // SK_Class
        sym.stype     = t;
        sym.classNode = cd;
        if (!this.symTable.define(sym)) {
            this.err("redefinition of class '" + cd.name + "'", n);
        }

        // Hoist methods
        int i = 0;
        while (i < cd.methods.length()) {
            ASTNode m = cd.methods.get(i) as ASTNode;
            this.hoistFunction(m, cd.name);
            i = i + 1;
        }
    }

    private fun void hoistFunction(ASTNode n, string ownerClass) {
        FunctionDeclNode fd = n as FunctionDeclNode;
        TypeDesc retType = this.resolveTypeRef(fd.returnType);

        TypeDesc fnType = makeFunctionType(fd.name, retType);
        int i = 0;
        while (i < fd.params.length()) {
            Param p = fd.params.get(i) as Param;
            TypeDesc pt = this.resolveTypeRef(p.ptype);
            fnType.paramTypes.add(pt);
            i = i + 1;
        }

        string sname = stringLen(ownerClass) > 0 ?
                       ownerClass + "::" + fd.name : fd.name;

        Symbol sym = new Symbol();
        sym.init();
        sym.name       = sname;
        sym.skind      = 2;  // SK_Function
        sym.stype      = fnType;
        sym.throws_    = fd.throws_;
        sym.paramTypes = fnType.paramTypes;
        this.symTable.define(sym);

        // Also register under plain name for member access
        if (stringLen(ownerClass) > 0) {
            Symbol plain = new Symbol();
            plain.init();
            plain.name       = fd.name;
            plain.skind      = 2;
            plain.stype      = fnType;
            plain.throws_    = fd.throws_;
            plain.paramTypes = fnType.paramTypes;
            this.symTable.define(plain);
        }
    }

    private fun void hoistTopVar(ASTNode n) {
        VarDeclNode vd = n as VarDeclNode;
        TypeDesc t = this.resolveTypeRef(vd.vtype);
        Symbol sym = new Symbol();
        sym.init();
        sym.name  = vd.name;
        sym.skind = 0;  // SK_Variable
        sym.stype = t;
        if (!this.symTable.define(sym)) {
            this.err("redefinition of '" + vd.name + "'", n);
        }
    }

    // ── Pass 2: Check ─────────────────────────────────────────────────────────

    private fun void checkDecl(ASTNode decl) {
        int k = decl.kind;
        if (k == 0) { return; }  // Import — nothing to check
        if (k == 1) { this.checkEnum(decl);     return; }
        if (k == 2) { this.checkStruct(decl);   return; }
        if (k == 3) { this.checkUnion(decl);    return; }
        if (k == 4) { this.checkClass(decl);    return; }
        if (k == 5) { this.checkFunction(decl); return; }
        if (k == 6) { this.checkTopVar(decl);   return; }
    }

    private fun void checkEnum(ASTNode n) {
        EnumDeclNode ed = n as EnumDeclNode;
        if (ed.members.length() == 0) {
            this.warn("enum '" + ed.name + "' has no members", n);
        }
    }

    private fun void checkStruct(ASTNode n) {
        StructDeclNode sd = n as StructDeclNode;
        Set seen = new Set();
        int i = 0;
        while (i < sd.fields.length()) {
            FieldDecl fd = sd.fields.get(i) as FieldDecl;
            if (seen.has(fd.name)) {
                this.emitError("duplicate field '" + fd.name + "' in struct '" + sd.name + "'",
                               fd.line, fd.col, n.filename);
            }
            seen.add(fd.name);
            TypeDesc t = this.resolveTypeRef(fd.ftype);
            if (t.isVoid()) {
                this.emitError("field '" + fd.name + "' cannot have type 'void'",
                               fd.line, fd.col, n.filename);
            }
            i = i + 1;
        }
    }

    private fun void checkUnion(ASTNode n) {
        UnionDeclNode ud = n as UnionDeclNode;
        Set seen = new Set();
        int i = 0;
        while (i < ud.fields.length()) {
            FieldDecl fd = ud.fields.get(i) as FieldDecl;
            if (seen.has(fd.name)) {
                this.emitError("duplicate field '" + fd.name + "' in union '" + ud.name + "'",
                               fd.line, fd.col, n.filename);
            }
            seen.add(fd.name);
            i = i + 1;
        }
    }

    private fun void checkClass(ASTNode n) {
        ClassDeclNode cd = n as ClassDeclNode;

        // Verify superclass exists
        if (cd.hasSuperClass) {
            Symbol sup = this.symTable.lookup(cd.superClass);
            if (sup == null || sup.skind != 3) {
                this.err("unknown base class '" + cd.superClass + "'", n);
            } else if (sup.classNode != null) {
                ClassDeclNode supCls = sup.classNode as ClassDeclNode;
                if (supCls.isFinal) {
                    this.err("cannot extend final class '" + cd.superClass + "'", n);
                }
            }
        }

        var savedClass = this.currentClass;
        this.currentClass = cd;
        this.symTable.push(2);  // SCOPE_Class

        // Register fields
        int i = 0;
        while (i < cd.fields.length()) {
            MemberVarDecl mv = cd.fields.get(i) as MemberVarDecl;
            TypeDesc t = this.resolveTypeRef(mv.mtype);
            if (t.isVoid()) {
                this.emitError("field '" + mv.name + "' cannot have type 'void'",
                               mv.line, mv.col, n.filename);
            }
            Symbol sym = new Symbol();
            sym.init();
            sym.name  = mv.name;
            sym.skind = 0;
            sym.stype = t;
            if (!this.symTable.define(sym)) {
                this.emitError("duplicate field '" + mv.name + "' in class '" + cd.name + "'",
                               mv.line, mv.col, n.filename);
            }
            if (mv.init_ != null) {
                ASTNode initNode = mv.init_ as ASTNode;
                TypeDesc initType = this.checkExpr(initNode);
                if (!this.isAssignable(t, initType)) {
                    this.emitError(this.typeMismatch("field '" + mv.name + "' initializer", t, initType),
                                   mv.line, mv.col, n.filename);
                }
            }
            i = i + 1;
        }

        // Check methods
        i = 0;
        while (i < cd.methods.length()) {
            ASTNode m = cd.methods.get(i) as ASTNode;
            this.checkFunction(m);
            i = i + 1;
        }

        this.symTable.pop();
        this.currentClass = savedClass;
    }

    private fun void checkFunction(ASTNode n) {
        FunctionDeclNode fd = n as FunctionDeclNode;
        this.symTable.push(1);  // SCOPE_Function

        // Register parameters
        int i = 0;
        while (i < fd.params.length()) {
            Param p = fd.params.get(i) as Param;
            TypeDesc t = this.resolveTypeRef(p.ptype);
            if (t.isVoid()) {
                this.emitError("parameter '" + p.name + "' cannot have type 'void'",
                               p.line, p.col, fd.filename);
            }
            Symbol sym = new Symbol();
            sym.init();
            sym.name  = p.name;
            sym.skind = 1;  // SK_Parameter
            sym.stype = t;
            if (!this.symTable.define(sym)) {
                this.emitError("duplicate parameter '" + p.name + "'",
                               p.line, p.col, fd.filename);
            }
            i = i + 1;
        }

        TypeDesc retType = this.resolveTypeRef(fd.returnType);
        this.symTable.setReturnType(retType);
        this.symTable.setInThrows(fd.throws_);

        if (fd.body != null) {
            this.checkBlock(fd.body as ASTNode);
        }

        this.symTable.setInThrows(false);
        this.symTable.pop();
    }

    private fun void checkTopVar(ASTNode n) {
        VarDeclNode vd = n as VarDeclNode;
        TypeDesc t = this.resolveTypeRef(vd.vtype);
        if (t.isVoid()) {
            this.err("variable '" + vd.name + "' cannot have type 'void'", n);
        }
        if (vd.init_ != null) {
            ASTNode initNode = vd.init_ as ASTNode;
            TypeDesc initType = this.checkExpr(initNode);
            if (t.kind != 20 && !this.isAssignable(t, initType)) {
                this.err(this.typeMismatch("variable '" + vd.name + "' initializer", t, initType), n);
            }
        }
    }

    // ── Statement checking ────────────────────────────────────────────────────

    private fun void checkStmt(ASTNode stmt) {
        int k = stmt.kind;
        if (k == 0)  { this.checkBlock(stmt);       return; }
        if (k == 1)  { this.checkExprStmt(stmt);    return; }
        if (k == 2)  { this.checkVarDeclStmt(stmt); return; }
        if (k == 3)  { this.checkIf(stmt);          return; }
        if (k == 4)  { this.checkWhile(stmt);        return; }
        if (k == 5)  { this.checkDoWhile(stmt);      return; }
        if (k == 6)  { this.checkFor(stmt);          return; }
        if (k == 7)  { this.checkSwitch(stmt);       return; }
        if (k == 8)  { this.checkReturn(stmt);       return; }
        if (k == 9)  { this.checkBreak(stmt);        return; }
        if (k == 10) { this.checkContinue(stmt);     return; }
        if (k == 11) { this.checkThrow(stmt);        return; }
        if (k == 12) { this.checkTry(stmt);          return; }
    }

    private fun void checkBlock(ASTNode n) {
        BlockNode bn = n as BlockNode;
        this.symTable.push(3);  // SCOPE_Block
        int i = 0;
        while (i < bn.stmts.length()) {
            ASTNode s = bn.stmts.get(i) as ASTNode;
            this.checkStmt(s);
            i = i + 1;
        }
        this.symTable.pop();
    }

    private fun void checkExprStmt(ASTNode n) {
        ExprStmtNode es = n as ExprStmtNode;
        if (es.expr != null) {
            this.checkExpr(es.expr as ASTNode);
        }
    }

    private fun void checkVarDeclStmt(ASTNode n) {
        VarDeclStmtNode vs = n as VarDeclStmtNode;

        // Check for redefinition in current block scope
        if (this.symTable.lookupLocal(vs.name) != null) {
            this.err("redefinition of '" + vs.name + "'", n);
        }

        TypeDesc declaredType = this.resolveTypeRef(vs.vtype);
        TypeDesc actualType   = declaredType;

        if (declaredType.isVoid()) {
            this.err("variable '" + vs.name + "' cannot have type 'void'", n);
        }

        if (vs.init_ != null) {
            ASTNode initNode = vs.init_ as ASTNode;
            TypeDesc initType = this.checkExpr(initNode);
            if (declaredType.kind == 20) {  // var
                actualType = initType;
            } else {
                if (!this.isAssignable(declaredType, initType)) {
                    this.err(this.typeMismatch("variable '" + vs.name + "'", declaredType, initType), n);
                }
            }
        } else if (declaredType.kind == 20) {
            this.err("'var' declaration '" + vs.name + "' requires an initializer", n);
        }

        Symbol sym = new Symbol();
        sym.init();
        sym.name  = vs.name;
        sym.skind = 0;
        sym.stype = actualType;
        this.symTable.define(sym);
    }

    private fun void checkIf(ASTNode n) {
        IfNode ifn = n as IfNode;
        TypeDesc condType = this.checkExpr(ifn.cond as ASTNode);
        if (!condType.isUnknown() && !condType.isBool() && !condType.isNumeric()) {
            this.err("if condition must be boolean or numeric", n);
        }
        this.checkStmt(ifn.thenBranch as ASTNode);
        if (ifn.elseBranch != null) {
            this.checkStmt(ifn.elseBranch as ASTNode);
        }
    }

    private fun void checkWhile(ASTNode n) {
        WhileNode wn = n as WhileNode;
        TypeDesc condType = this.checkExpr(wn.cond as ASTNode);
        if (!condType.isUnknown() && !condType.isBool() && !condType.isNumeric()) {
            this.err("while condition must be boolean or numeric", n);
        }
        this.symTable.push(4);  // SCOPE_Loop
        this.checkStmt(wn.body as ASTNode);
        this.symTable.pop();
    }

    private fun void checkDoWhile(ASTNode n) {
        DoWhileNode dn = n as DoWhileNode;
        this.symTable.push(4);
        this.checkStmt(dn.body as ASTNode);
        this.symTable.pop();
        TypeDesc condType = this.checkExpr(dn.cond as ASTNode);
        if (!condType.isUnknown() && !condType.isBool() && !condType.isNumeric()) {
            this.err("do-while condition must be boolean or numeric", n);
        }
    }

    private fun void checkFor(ASTNode n) {
        ForNode fn = n as ForNode;
        this.symTable.push(4);
        if (fn.init_ != null) { this.checkStmt(fn.init_ as ASTNode); }
        if (fn.cond  != null) {
            TypeDesc ct = this.checkExpr(fn.cond as ASTNode);
            if (!ct.isUnknown() && !ct.isBool() && !ct.isNumeric()) {
                this.err("for condition must be boolean or numeric", n);
            }
        }
        if (fn.incr != null) { this.checkExpr(fn.incr as ASTNode); }
        this.checkStmt(fn.body as ASTNode);
        this.symTable.pop();
    }

    private fun void checkSwitch(ASTNode n) {
        SwitchNode sn = n as SwitchNode;
        TypeDesc subjType = this.checkExpr(sn.subject as ASTNode);
        bool integralSubj = subjType.isInteger() || subjType.isUnknown() ||
                            subjType.kind == 13;  // Enum

        if (!integralSubj) {
            this.err("switch subject must be integer or enum type", n);
        }

        this.symTable.push(5);  // SCOPE_Switch
        bool hasDefault = false;
        int i = 0;
        while (i < sn.cases.length()) {
            CaseClause cc = sn.cases.get(i) as CaseClause;
            if (cc.value == null) {
                if (hasDefault) { this.err("duplicate 'default' in switch", n); }
                hasDefault = true;
            } else {
                TypeDesc caseType = this.checkExpr(cc.value as ASTNode);
                if (!this.isComparable(subjType, caseType)) {
                    this.err(this.typeMismatch("case expression", subjType, caseType), n);
                }
            }
            this.symTable.push(3);
            int j = 0;
            while (j < cc.stmts.length()) {
                ASTNode s = cc.stmts.get(j) as ASTNode;
                this.checkStmt(s);
                j = j + 1;
            }
            this.symTable.pop();
            i = i + 1;
        }
        this.symTable.pop();
    }

    private fun void checkReturn(ASTNode n) {
        ReturnNode rn = n as ReturnNode;
        TypeDesc retType = this.symTable.currentReturnType();
        if (retType == null) {
            this.err("'return' outside of a function", n);
            return;
        }
        if (rn.value != null) {
            TypeDesc valType = this.checkExpr(rn.value as ASTNode);
            if (retType.isVoid()) {
                this.err("void function must not return a value", n);
            } else if (!this.isAssignable(retType, valType)) {
                this.err(this.typeMismatch("return", retType, valType), n);
            }
        } else {
            if (!retType.isVoid()) {
                this.warn("non-void function missing return value", n);
            }
        }
    }

    private fun void checkThrow(ASTNode n) {
        ThrowNode tn = n as ThrowNode;
        this.checkExpr(tn.value as ASTNode);
        if (!this.symTable.inThrowsFunction() && !this.symTable.inTryBlock()) {
            this.err("'throw' used outside a 'throws' function or 'try' block", n);
        }
    }

    private fun void checkTry(ASTNode n) {
        TryNode tn = n as TryNode;
        this.symTable.enterTry();
        this.checkBlock(tn.tryBody as ASTNode);
        this.symTable.exitTry();

        int i = 0;
        while (i < tn.catches.length()) {
            CatchClause cc = tn.catches.get(i) as CatchClause;
            this.symTable.push(3);
            if (stringLen(cc.exName) > 0) {
                TypeDesc exType = this.resolveTypeRef(cc.exType);
                Symbol sym = new Symbol();
                sym.init();
                sym.name  = cc.exName;
                sym.skind = 0;
                sym.stype = exType;
                this.symTable.define(sym);
            }
            this.checkBlock(cc.body as ASTNode);
            this.symTable.pop();
            i = i + 1;
        }

        if (tn.finallyBody != null) {
            this.checkBlock(tn.finallyBody as ASTNode);
        }
    }

    private fun void checkBreak(ASTNode n) {
        if (!this.symTable.inLoop() && !this.symTable.inSwitch()) {
            this.err("'break' outside of a loop or switch", n);
        }
    }

    private fun void checkContinue(ASTNode n) {
        if (!this.symTable.inLoop()) {
            this.err("'continue' outside of a loop", n);
        }
    }

    // ── Expression checking ───────────────────────────────────────────────────

    private fun TypeDesc checkExpr(ASTNode expr) {
        if (expr == null) { return makeUnknownType(); }
        int k = expr.kind;

        TypeDesc t = makeUnknownType();

        if (k == 0)  { t = this.checkIntLit(expr);    }
        else if (k == 1)  { t = this.checkFloatLit(expr);  }
        else if (k == 2)  { t = makeBoolType();        }  // BoolLit
        else if (k == 3)  { t = makeCharType();        }  // CharLit
        else if (k == 4)  { t = makeStringType();      }  // StringLit
        else if (k == 5)  { t = makeNullType();        }  // NullLit
        else if (k == 6)  { t = this.checkIdent(expr);     }
        else if (k == 7)  { t = this.checkUnary(expr);     }
        else if (k == 8)  { t = this.checkBinary(expr);    }
        else if (k == 9)  { t = this.checkTernary(expr);   }
        else if (k == 10) { t = this.checkAssign(expr);    }
        else if (k == 11) { t = this.checkCall(expr);      }
        else if (k == 12) { t = this.checkMember(expr);    }
        else if (k == 13) { t = this.checkIndex(expr);     }
        else if (k == 14) { t = this.checkNew(expr);       }
        else if (k == 15) { makeVoidType();             }  // Delete
        else if (k == 16) { t = makeStringType();       }  // TypeOf
        else if (k == 17) { t = this.checkCast(expr);      }
        else if (k == 18) { t = this.checkLambda(expr);    }
        else if (k == 19) { t = makeBoolType();         }  // InstanceOf
        else if (k == 20) { t = this.checkArrayLit(expr);  }

        return t;
    }

    private fun TypeDesc checkIntLit(ASTNode n) {
        return makeIntType();
    }

    private fun TypeDesc checkFloatLit(ASTNode n) {
        FloatLitNode fl = n as FloatLitNode;
        string raw = fl.raw;
        int len = stringLen(raw);
        if (len > 0) {
            char last = raw[len - 1];
            if (last == 'f' || last == 'F') { return makeFloatType(); }
        }
        return makeDoubleType();
    }

    private fun TypeDesc checkIdent(ASTNode n) {
        IdentNode id = n as IdentNode;

        if (id.name == "this") {
            if (this.currentClass == null) {
                this.err("'this' used outside of a class", n);
                return makeUnknownType();
            }
            ClassDeclNode cd = this.currentClass as ClassDeclNode;
            return makeNamedType(16, cd.name);
        }

        if (id.name == "super") {
            if (this.currentClass == null) {
                this.err("'super' used outside of a class", n);
                return makeUnknownType();
            }
            ClassDeclNode cd = this.currentClass as ClassDeclNode;
            if (!cd.hasSuperClass) {
                this.err("'super' used in class '" + cd.name + "' which has no superclass", n);
                return makeUnknownType();
            }
            return makeNamedType(16, cd.superClass);
        }

        Symbol sym = this.symTable.lookup(id.name);
        if (sym == null) {
            this.err("use of undeclared identifier '" + id.name + "'", n);
            return makeUnknownType();
        }
        return sym.stype;
    }

    private fun TypeDesc checkUnary(ASTNode n) {
        UnaryNode un = n as UnaryNode;
        TypeDesc ot = this.checkExpr(un.operand as ASTNode);

        int op = un.op;
        if (op == 104) { return makeBoolType(); }   // Bang → bool
        if (op == 90)  { return ot; }               // Minus → same type
        if (op == 108) { return ot; }               // Tilde → same type
        if (op == 94 || op == 95) { return ot; }    // ++ / -- → same type
        return ot;
    }

    private fun TypeDesc checkBinary(ASTNode n) {
        BinaryNode bn = n as BinaryNode;
        TypeDesc lhs = this.checkExpr(bn.left  as ASTNode);
        TypeDesc rhs = this.checkExpr(bn.right as ASTNode);

        int op = bn.op;

        // Arithmetic: Plus(89) Minus(90) Star(91) Slash(92) Percent(93)
        if (op == 89) {
            if (lhs.kind == 10 || rhs.kind == 10) { return makeStringType(); }  // string concat
            return this.widenNumeric(lhs, rhs);
        }
        if (op == 90 || op == 91 || op == 92 || op == 93) {
            return this.widenNumeric(lhs, rhs);
        }

        // Bitwise: Amp(105) Pipe(106) Caret(107) LShift(109) RShift(110)
        if (op == 105 || op == 106 || op == 107 || op == 109 || op == 110) {
            return this.widenNumeric(lhs, rhs);
        }

        // Logical: AmpAmp(102) PipePipe(103)
        if (op == 102 || op == 103) { return makeBoolType(); }

        // Equality: EqualEqual(96) BangEqual(97)
        if (op == 96 || op == 97) {
            if (!this.isComparable(lhs, rhs)) {
                this.err(this.typeMismatch("equality comparison", lhs, rhs), n);
            }
            return makeBoolType();
        }

        // Relational: Less(98) LessEqual(99) Greater(100) GreaterEqual(101)
        if (op == 98 || op == 99 || op == 100 || op == 101) {
            return makeBoolType();
        }

        return makeUnknownType();
    }

    private fun TypeDesc checkTernary(ASTNode n) {
        TernaryNode tn = n as TernaryNode;
        this.checkExpr(tn.cond as ASTNode);
        TypeDesc thenType = this.checkExpr(tn.thenExpr as ASTNode);
        TypeDesc elseType = this.checkExpr(tn.elseExpr as ASTNode);
        TypeDesc wider = this.widenNumeric(thenType, elseType);
        if (wider.isUnknown()) { return thenType; }
        return wider;
    }

    private fun TypeDesc checkAssign(ASTNode n) {
        AssignNode an = n as AssignNode;
        TypeDesc targetType = this.checkExpr(an.target as ASTNode);
        TypeDesc valueType  = this.checkExpr(an.value  as ASTNode);
        if (an.op == 78 && !this.isAssignable(targetType, valueType)) {  // Equal
            this.err(this.typeMismatch("assignment", targetType, valueType), n);
        }
        return targetType;
    }

    private fun TypeDesc checkCall(ASTNode n) {
        CallNode cn = n as CallNode;
        TypeDesc calleeType = this.checkExpr(cn.callee as ASTNode);

        List argTypes = new List();
        int i = 0;
        while (i < cn.args.length()) {
            ASTNode a = cn.args.get(i) as ASTNode;
            argTypes.add(this.checkExpr(a));
            i = i + 1;
        }

        if (calleeType.kind != 17 && !calleeType.isUnknown()) {  // Not Function
            this.err("expression is not callable (type '" + calleeType.typeStr() + "')", n);
            return makeUnknownType();
        }
        if (calleeType.isUnknown()) { return makeUnknownType(); }

        // Arity check
        if (argTypes.length() != calleeType.paramTypes.length()) {
            this.err("function called with " + toString(argTypes.length()) +
                     " argument(s) but expects " +
                     toString(calleeType.paramTypes.length()), n);
        } else {
            i = 0;
            while (i < argTypes.length()) {
                TypeDesc argType   = argTypes.get(i) as TypeDesc;
                TypeDesc paramType = calleeType.paramTypes.get(i) as TypeDesc;
                if (!this.isAssignable(paramType, argType)) {
                    this.err(this.typeMismatch("argument " + toString(i + 1), paramType, argType), n);
                }
                i = i + 1;
            }
        }

        if (calleeType.returnType != null) {
            return calleeType.returnType as TypeDesc;
        }
        return makeVoidType();
    }

    private fun TypeDesc checkMember(ASTNode n) {
        MemberNode mn = n as MemberNode;
        string member = mn.member;

        // Scope resolution: ::NORTH
        if (stringLen(member) > 2 && member[0] == ':' && member[1] == ':') {
            TypeDesc objType = this.checkExpr(mn.object as ASTNode);
            string memberName = substring(member, 2, stringLen(member));
            return this.lookupScopedName(objType.name, memberName, n);
        }

        TypeDesc objType = this.checkExpr(mn.object as ASTNode);

        // String pseudo-fields
        if (objType.kind == 10 && (member == "length" || member == "size")) {
            return makeIntType();
        }

        // Array pseudo-fields
        if (objType.isArray_ && (member == "length" || member == "size")) {
            return makeIntType();
        }

        return this.lookupMember(objType, member, n);
    }

    private fun TypeDesc lookupMember(TypeDesc objType, string memberName, ASTNode loc) {
        if (objType.isUnknown()) { return makeUnknownType(); }

        Symbol typeSym = this.symTable.lookup(objType.name);
        if (typeSym == null) {
            return makeUnknownType();  // type might be from stdlib
        }

        if (typeSym.classNode != null) {
            ClassDeclNode cls = typeSym.classNode as ClassDeclNode;

            // Check fields
            int i = 0;
            while (i < cls.fields.length()) {
                MemberVarDecl mv = cls.fields.get(i) as MemberVarDecl;
                if (mv.name == memberName) {
                    return this.resolveTypeRef(mv.mtype);
                }
                i = i + 1;
            }

            // Check methods
            i = 0;
            while (i < cls.methods.length()) {
                FunctionDeclNode m = cls.methods.get(i) as FunctionDeclNode;
                if (m.name == memberName) {
                    TypeDesc retType = this.resolveTypeRef(m.returnType);
                    TypeDesc fnType  = makeFunctionType(m.name, retType);
                    int j = 0;
                    while (j < m.params.length()) {
                        Param p = m.params.get(j) as Param;
                        fnType.paramTypes.add(this.resolveTypeRef(p.ptype));
                        j = j + 1;
                    }
                    return fnType;
                }
                i = i + 1;
            }

            // Try superclass
            if (cls.hasSuperClass) {
                TypeDesc superType = makeNamedType(16, cls.superClass);
                return this.lookupMember(superType, memberName, loc);
            }

            this.err("'" + objType.name + "' has no member '" + memberName + "'", loc);
            return makeUnknownType();
        }

        if (typeSym.structNode != null) {
            StructDeclNode sd = typeSym.structNode as StructDeclNode;
            int i = 0;
            while (i < sd.fields.length()) {
                FieldDecl fd = sd.fields.get(i) as FieldDecl;
                if (fd.name == memberName) { return this.resolveTypeRef(fd.ftype); }
                i = i + 1;
            }
        }

        return makeUnknownType();
    }

    private fun TypeDesc lookupScopedName(string scope, string member, ASTNode loc) {
        Symbol sym = this.symTable.lookup(scope + "::" + member);
        if (sym != null) { return sym.stype; }
        sym = this.symTable.lookup(member);
        if (sym != null) { return sym.stype; }
        this.err("'" + scope + "' has no member '" + member + "'", loc);
        return makeUnknownType();
    }

    private fun TypeDesc checkIndex(ASTNode n) {
        IndexNode idx = n as IndexNode;
        TypeDesc arrType = this.checkExpr(idx.array as ASTNode);
        this.checkExpr(idx.index as ASTNode);

        if (arrType.kind == 11 && arrType.elementType != null) {  // Array
            return arrType.elementType as TypeDesc;
        }
        if (arrType.kind == 10) { return makeCharType(); }  // String → char
        return makeUnknownType();
    }

    private fun TypeDesc checkNew(ASTNode n) {
        NewNode nn = n as NewNode;

        if (nn.isArray) {
            if (nn.arraySize != null) {
                this.checkExpr(nn.arraySize as ASTNode);
            }
            TypeDesc elem = this.resolveTypeRef(nn.ntype);
            return makeArrayType(elem);
        }

        TypeDesc t = this.resolveTypeRef(nn.ntype);
        Symbol sym = this.symTable.lookup(nn.ntype.name);
        if (sym == null || sym.skind != 3) {
            if (!t.isUnknown()) {
                this.err("'new' used with non-class type '" + nn.ntype.name + "'", n);
            }
        } else if (sym.classNode != null) {
            ClassDeclNode cls = sym.classNode as ClassDeclNode;
            // Find init method and check args
            int i = 0;
            while (i < cls.methods.length()) {
                FunctionDeclNode m = cls.methods.get(i) as FunctionDeclNode;
                if (m.name == "init") {
                    if (nn.args.length() != m.params.length()) {
                        this.err("constructor for '" + nn.ntype.name + "' expects " +
                                 toString(m.params.length()) + " argument(s), got " +
                                 toString(nn.args.length()), n);
                    } else {
                        int j = 0;
                        while (j < nn.args.length()) {
                            ASTNode arg = nn.args.get(j) as ASTNode;
                            TypeDesc argType = this.checkExpr(arg);
                            Param p = m.params.get(j) as Param;
                            TypeDesc paramType = this.resolveTypeRef(p.ptype);
                            if (!this.isAssignable(paramType, argType)) {
                                this.err(this.typeMismatch("constructor argument " +
                                         toString(j + 1), paramType, argType), n);
                            }
                            j = j + 1;
                        }
                    }
                    return t;
                }
                i = i + 1;
            }
            // No init — just check args are empty
            int i2 = 0;
            while (i2 < nn.args.length()) {
                ASTNode arg = nn.args.get(i2) as ASTNode;
                this.checkExpr(arg);
                i2 = i2 + 1;
            }
        }
        return t;
    }

    private fun TypeDesc checkCast(ASTNode n) {
        CastNode cn = n as CastNode;
        this.checkExpr(cn.operand as ASTNode);
        return this.resolveTypeRef(cn.ctype);
    }

    private fun TypeDesc checkLambda(ASTNode n) {
        LambdaNode ln = n as LambdaNode;
        this.symTable.push(1);  // Function scope

        int i = 0;
        while (i < ln.params.length()) {
            Param p = ln.params.get(i) as Param;
            TypeDesc t = this.resolveTypeRef(p.ptype);
            Symbol sym = new Symbol();
            sym.init();
            sym.name  = p.name;
            sym.skind = 1;
            sym.stype = t;
            this.symTable.define(sym);
            i = i + 1;
        }

        TypeDesc retType = makeVoidType();
        if (ln.body != null) {
            retType = this.checkExpr(ln.body as ASTNode);
        }
        if (ln.blockBody != null) {
            this.checkBlock(ln.blockBody as ASTNode);
        }

        this.symTable.pop();

        TypeDesc fnType = makeFunctionType("lambda", retType);
        i = 0;
        while (i < ln.params.length()) {
            Param p = ln.params.get(i) as Param;
            fnType.paramTypes.add(this.resolveTypeRef(p.ptype));
            i = i + 1;
        }
        return fnType;
    }

    private fun TypeDesc checkArrayLit(ASTNode n) {
        ArrayLitNode al = n as ArrayLitNode;
        if (al.elements.length() == 0) {
            return makeArrayType(makeUnknownType());
        }
        ASTNode first = al.elements.get(0) as ASTNode;
        TypeDesc elemType = this.checkExpr(first);
        int i = 1;
        while (i < al.elements.length()) {
            ASTNode e = al.elements.get(i) as ASTNode;
            this.checkExpr(e);
            i = i + 1;
        }
        return makeArrayType(elemType);
    }
}

// ── Test driver ───────────────────────────────────────────────────────────────

fun int main() {
    string source = "import \"std.io\";\nimport \"std.string\";\n" +
                    "fun int add(int a, int b) throws { return a + b; }\n" +
                    "class Point { public int x; public int y;\n" +
                    "  public fun void init(int x, int y) { this.x = x; this.y = y; }\n" +
                    "  public fun int sum() { return this.x + this.y; }\n" +
                    "}\n" +
                    "fun int main() {\n" +
                    "  Point p = new Point(3, 4);\n" +
                    "  int s = p.sum();\n" +
                    "  println(toString(s));\n" +
                    "  return 0;\n" +
                    "}";

    Lexer lexer = new Lexer("", "");
    lexer.init(source, "test.ec");
    List tokens = lexer.tokenize();

    Parser parser = new Parser(tokens, "");
    parser.init(tokens, "test.ec");
    Program prog = parser.parse();

    print("Parse errors: ");
    println(toString(parser.getErrors().length()));

    TypeChecker tc = new TypeChecker("");
    tc.init("test.ec");
    tc.registerResolvedImport("std.io");
    tc.registerResolvedImport("std.string");
    tc.check(prog);

    List errs = tc.getErrors();
    print("Type errors: ");
    println(toString(errs.length()));

    int i = 0;
    while (i < errs.length()) {
        TypeError e = errs.get(i) as TypeError;
        println(e.format());
        i = i + 1;
    }

    if (errs.length() == 0) {
        println("Type check PASSED.");
    }
    return 0;
}
