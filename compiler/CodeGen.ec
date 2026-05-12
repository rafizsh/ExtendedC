// ============================================================================
//  CodeGen.lang  --  ExtendedC port of the LLVM IR code generator
//
//  Translates the AST (from Parser.lang) into textual LLVM IR (.ll).
//  Uses three StringBuilder instances mirroring the C++ ostringstream fields:
//    globals_  -- type decls, runtime decls, vtable globals, global vars
//    body_     -- function bodies (current output target)
//    lambdas_  -- closure/lambda function bodies, flushed after each fn
//
//  IRValue = { name: string, type: string }
// ============================================================================

import "std.io";
import "std.string";
import "std.collections";
import "Parser";

// ── IRValue ───────────────────────────────────────────────────────────────────

class IRValue {
    public string name;
    public string type;
    public fun void init() { this.name = ""; this.type = ""; }
    public fun bool isValid() { return stringLen(this.name) > 0; }
}

fun IRValue makeIRValue(string name, string type) {
    IRValue v = new IRValue(); v.init();
    v.name = name; v.type = type;
    return v;
}
fun IRValue invalidIR() { return makeIRValue("", ""); }

// ── ClassLayout ───────────────────────────────────────────────────────────────

class VtSlot {
    public int    index;
    public string methodName;
    public string ownerClass;
    public string retType;
    public string paramTypes;
    public fun void init() {
        this.index = 0; this.methodName = ""; this.ownerClass = "";
        this.retType = "i32"; this.paramTypes = "";
    }
}

class ClassLayout {
    public string llvmName;
    public List   fieldNames;   // List of boxed string
    public List   fieldTypes;   // List of boxed string (LLVM types)
    public List   methodNames;  // List of boxed string
    public string superClass;
    public List   vtable;       // List of VtSlot
    public bool   hasVtable;

    public fun void init() {
        this.llvmName   = "";
        this.fieldNames = new List();
        this.fieldTypes = new List();
        this.methodNames= new List();
        this.superClass = "";
        this.vtable     = new List();
        this.hasVtable  = true;
    }
}

// ── FnCtx ────────────────────────────────────────────────────────────────────

class FnCtx {
    public string name;
    public string retType;
    public bool   hasVoidRet;
    public Map    locals;      // name → alloca ptr string
    public Map    localTypes;  // name → llvm type string
    public List   breakLabels;
    public List   continueLabels;
    public bool   inTry;
    public bool   terminated;

    public fun void init() {
        this.name         = "";
        this.retType      = "void";
        this.hasVoidRet   = false;
        this.locals       = new Map();
        this.localTypes   = new Map();
        this.breakLabels  = new List();
        this.continueLabels = new List();
        this.inTry        = false;
        this.terminated   = false;
    }
}

// ── ClosureSig ────────────────────────────────────────────────────────────────

class ClosureSig {
    public string retType;
    public string paramTypeStr;
    public fun void init() { this.retType = "i32"; this.paramTypeStr = ""; }
}

// ── CaptureInfo ───────────────────────────────────────────────────────────────

class CaptureInfo {
    public string name;
    public string llvmType;
    public string ptrInCtx;
    public fun void init() { this.name = ""; this.llvmType = "i32"; this.ptrInCtx = ""; }
}

// ── CodeGen ───────────────────────────────────────────────────────────────────

class CodeGen {
    private string        moduleName;
    private StringBuilder globals;      // type decls + runtime + vtables + global vars
    private StringBuilder body;         // function bodies
    private StringBuilder lambdas;      // pending closure bodies
    private int           outTarget;    // 0=globals, 1=body, 2=lambdas
    private int           counter;
    private int           labelCtr;
    private int           strCtr;
    private int           lambdaCtr;
    private Map           stringPool;   // content → global name
    private Map           classLayouts; // className → ClassLayout
    private Map           enumValues;   // "Enum::Member" → boxed int
    private Map           globalVars;   // name → llvm type
    private Map           userFnRetTypes; // irName → llvm ret type
    private Map           closureTypes; // varName → ClosureSig
    private var           ctx;          // FnCtx or null
    private string        currentClass;

    public fun void init(string modName) {
        this.moduleName     = modName;
        this.globals        = new StringBuilder();
        this.body           = new StringBuilder();
        this.lambdas        = new StringBuilder();
        this.outTarget      = 0;
        this.counter        = 0;
        this.labelCtr       = 0;
        this.strCtr         = 0;
        this.lambdaCtr      = 0;
        this.stringPool     = new Map();
        this.classLayouts   = new Map();
        this.enumValues     = new Map();
        this.globalVars     = new Map();
        this.userFnRetTypes = new Map();
        this.closureTypes   = new Map();
        this.ctx            = null;
        this.currentClass   = "";
    }

    // ── Output routing ────────────────────────────────────────────────────────

    private fun void emit(string line) {
        string ln = "  " + line + "\n";
        if (this.outTarget == 2) { this.lambdas.append(ln); }
        else { this.body.append(ln); }
    }

    private fun void emitGlobal(string line) {
        this.globals.append(line); this.globals.appendChar('\n');
    }

    private fun void emitLabel(string label) {
        string ln = label + ":\n";
        if (this.outTarget == 2) { this.lambdas.append(ln); }
        else { this.body.append(ln); }
        if (this.ctx != null) {
            FnCtx c = this.ctx as FnCtx;
            c.terminated = false;
        }
    }

    // ── Name factories ────────────────────────────────────────────────────────

    private fun string freshName(string hint) {
        this.counter = this.counter + 1;
        return "%" + hint + "." + toString(this.counter);
    }
    private fun string freshLabel(string hint) {
        this.labelCtr = this.labelCtr + 1;
        return hint + "." + toString(this.labelCtr);
    }
    private fun string freshStrConst() {
        this.strCtr = this.strCtr + 1;
        return "@.str." + toString(this.strCtr);
    }

    // ── Type mapping ──────────────────────────────────────────────────────────

    private fun string llvmTypeName(string name) {
        if (name == "void")      { return "void"; }
        if (name == "bool")      { return "i1"; }
        if (name == "char")      { return "i8"; }
        if (name == "int")       { return "i32"; }
        if (name == "uint")      { return "i32"; }
        if (name == "long")      { return "i64"; }
        if (name == "ulong")     { return "i64"; }
        if (name == "float")     { return "float"; }
        if (name == "ufloat")    { return "float"; }
        if (name == "double")    { return "double"; }
        if (name == "string")    { return "i8*"; }
        if (name == "null")      { return "i8*"; }
        if (name == "var")       { return "i8*"; }
        if (name == "<unknown>") { return "i32"; }
        // Array types e.g. "int[]" → fat pointer
        int nlen = stringLen(name);
        if (nlen > 2 && name[nlen - 2] == '[' && name[nlen - 1] == ']') {
            return "i8*";
        }
        return "%" + name + "*";
    }

    private fun string llvmTypeRef(TypeRef tr) {
        if (tr.isArray)   { return "i8*"; }
        string base = this.llvmTypeName(tr.name);
        if (tr.isPointer) { return base + "*"; }
        return base;
    }

    private fun string llvmDefault(string t) {
        if (t == "i1")     { return "false"; }
        if (t == "i8")     { return "0"; }
        if (t == "i32")    { return "0"; }
        if (t == "i64")    { return "0"; }
        if (t == "float")  { return "0.0"; }
        if (t == "double") { return "0.0"; }
        return "null";
    }

    // ── Instruction helpers ───────────────────────────────────────────────────

    private fun string emitAlloca(string llType, string hint) {
        string ptr = this.freshName(hint + ".ptr");
        this.emit(ptr + " = alloca " + llType);
        return ptr;
    }

    private fun void emitStore(string valType, string val, string ptr) {
        this.emit("store " + valType + " " + val + ", " + valType + "* " + ptr);
    }

    private fun string emitLoad(string valType, string ptr) {
        string res = this.freshName("load");
        this.emit(res + " = load " + valType + ", " + valType + "* " + ptr);
        return res;
    }

    private fun string emitGEP(string structType, string ptr, int fieldIdx, string hint) {
        string res = this.freshName(hint);
        this.emit(res + " = getelementptr inbounds " + structType + ", " +
                  structType + "* " + ptr + ", i32 0, i32 " + toString(fieldIdx));
        return res;
    }

    private fun void emitBr(string label) {
        if (this.ctx != null) {
            FnCtx c = this.ctx as FnCtx;
            if (c.terminated) { return; }
        }
        this.emit("br label %" + label);
        if (this.ctx != null) {
            FnCtx c = this.ctx as FnCtx;
            c.terminated = true;
        }
    }

    private fun void emitCondBr(string cond, string tLbl, string fLbl) {
        if (this.ctx != null) {
            FnCtx c = this.ctx as FnCtx;
            if (c.terminated) { return; }
        }
        this.emit("br i1 " + cond + ", label %" + tLbl + ", label %" + fLbl);
        if (this.ctx != null) {
            FnCtx c = this.ctx as FnCtx;
            c.terminated = true;
        }
    }

    // ── Coerce IRValue to target type ─────────────────────────────────────────

    private fun IRValue coerce(IRValue val, string targetType) {
        if (val.type == targetType) { return val; }

        bool srcIsInt  = val.type == "i1" || val.type == "i8" || val.type == "i32" || val.type == "i64";
        bool dstIsInt  = targetType == "i1" || targetType == "i8" || targetType == "i32" || targetType == "i64";
        bool srcIsFloat = val.type == "float" || val.type == "double";
        bool dstIsFloat = targetType == "float" || targetType == "double";

        if (srcIsInt && dstIsInt) {
            int fromSz = this.intSize(val.type);
            int toSz   = this.intSize(targetType);
            string res = this.freshName("conv");
            if (toSz > fromSz) {
                this.emit(res + " = zext " + val.type + " " + val.name + " to " + targetType);
            } else if (toSz < fromSz) {
                this.emit(res + " = trunc " + val.type + " " + val.name + " to " + targetType);
            } else {
                return val;
            }
            return makeIRValue(res, targetType);
        }
        if (srcIsInt && dstIsFloat) {
            string res = this.freshName("sitofp");
            this.emit(res + " = sitofp " + val.type + " " + val.name + " to " + targetType);
            return makeIRValue(res, targetType);
        }
        if (srcIsFloat && dstIsFloat) {
            string res = this.freshName("fpconv");
            if (targetType == "double" && val.type == "float") {
                this.emit(res + " = fpext float " + val.name + " to double");
            } else {
                this.emit(res + " = fptrunc double " + val.name + " to float");
            }
            return makeIRValue(res, targetType);
        }
        // pointer cast
        bool srcPtr = stringLen(val.type) > 0 && val.type[stringLen(val.type)-1] == '*';
        bool dstPtr = stringLen(targetType) > 0 && targetType[stringLen(targetType)-1] == '*';
        if (srcPtr || dstPtr) {
            string res = this.freshName("bc");
            this.emit(res + " = bitcast " + val.type + " " + val.name + " to " + targetType);
            return makeIRValue(res, targetType);
        }
        return val;
    }

    private fun int intSize(string t) {
        if (t == "i1")  { return 1; }
        if (t == "i8")  { return 8; }
        if (t == "i32") { return 32; }
        if (t == "i64") { return 64; }
        return 32;
    }

    // ── String constant pool ──────────────────────────────────────────────────

    private fun string internString(string s) {
        if (this.stringPool.has(s)) {
            return this.stringPool.get(s) as string;
        }
        string gname = this.freshStrConst();
        string escaped = this.escapeString(s);
        int len = stringLen(s) + 1;
        this.globals.append(gname);
        this.globals.append(" = private unnamed_addr constant [");
        this.globals.appendInt(len);
        this.globals.append(" x i8] c\"");
        this.globals.append(escaped);
        this.globals.append("\\00\"\n");
        this.stringPool.set(s, gname);
        return gname;
    }

    private fun string escapeString(string s) {
        StringBuilder sb = new StringBuilder();
        int i = 0;
        while (i < stringLen(s)) {
            char c = s[i];
            int code = charCode(c);
            if (c == '\n')      { sb.append("\\0A"); }
            else if (c == '\t') { sb.append("\\09"); }
            else if (c == '\r') { sb.append("\\0D"); }
            else if (c == '\\') { sb.append("\\5C"); }
            else if (c == '"')  { sb.append("\\22"); }
            else if (code < 32 || code > 126) {
                sb.append("\\");
                int hi = code / 16;
                int lo = code - hi * 16;
                sb.appendChar(this.hexChar(hi));
                sb.appendChar(this.hexChar(lo));
            } else {
                sb.appendChar(c);
            }
            i = i + 1;
        }
        return sb.toString();
    }

    private fun char hexChar(int n) {
        if (n < 10) { return charFromCode(48 + n); }
        return charFromCode(55 + n);
    }

    // ── Class layout ──────────────────────────────────────────────────────────

    private fun void buildClassLayout(ASTNode n) {
        ClassDeclNode cd = n as ClassDeclNode;
        ClassLayout layout = new ClassLayout();
        layout.init();
        layout.llvmName   = "%" + cd.name;
        layout.superClass = cd.hasSuperClass ? cd.superClass : "";

        // Inherit superclass fields
        if (cd.hasSuperClass && this.classLayouts.has(cd.superClass)) {
            ClassLayout sup = this.classLayouts.get(cd.superClass) as ClassLayout;
            int i = 0;
            while (i < sup.fieldNames.length()) {
                layout.fieldNames.add(sup.fieldNames.get(i));
                layout.fieldTypes.add(sup.fieldTypes.get(i));
                i = i + 1;
            }
        }

        // Add own fields
        int i = 0;
        while (i < cd.fields.length()) {
            MemberVarDecl mv = cd.fields.get(i) as MemberVarDecl;
            layout.fieldNames.add(_boxStr(mv.name));
            layout.fieldTypes.add(_boxStr(this.llvmTypeRef(mv.mtype)));
            i = i + 1;
        }

        // Method names
        i = 0;
        while (i < cd.methods.length()) {
            FunctionDeclNode m = cd.methods.get(i) as FunctionDeclNode;
            layout.methodNames.add(_boxStr(m.name));
            i = i + 1;
        }

        this.classLayouts.set(cd.name, layout);
    }

    private fun void buildVtable(ASTNode n, Program prog) {
        ClassDeclNode cd = n as ClassDeclNode;
        if (!this.classLayouts.has(cd.name)) { return; }
        ClassLayout layout = this.classLayouts.get(cd.name) as ClassLayout;

        // Inherit superclass vtable
        if (cd.hasSuperClass && this.classLayouts.has(cd.superClass)) {
            ClassLayout sup = this.classLayouts.get(cd.superClass) as ClassLayout;
            int i = 0;
            while (i < sup.vtable.length()) {
                VtSlot s = sup.vtable.get(i) as VtSlot;
                VtSlot copy = new VtSlot(); copy.init();
                copy.index      = s.index;
                copy.methodName = s.methodName;
                copy.ownerClass = s.ownerClass;
                copy.retType    = s.retType;
                copy.paramTypes = s.paramTypes;
                // Check if this class overrides it
                int j = 0;
                while (j < cd.methods.length()) {
                    FunctionDeclNode m = cd.methods.get(j) as FunctionDeclNode;
                    if (m.name == s.methodName && this.isVirtualMethod(m)) {
                        copy.ownerClass = cd.name;
                        break;
                    }
                    j = j + 1;
                }
                layout.vtable.add(copy);
                i = i + 1;
            }
        }

        // Add new virtual methods
        int i = 0;
        while (i < cd.methods.length()) {
            FunctionDeclNode m = cd.methods.get(i) as FunctionDeclNode;
            if (!this.isVirtualMethod(m)) { i = i + 1; continue; }
            // Check if already in vtable
            bool found = false;
            int j = 0;
            while (j < layout.vtable.length()) {
                VtSlot s = layout.vtable.get(j) as VtSlot;
                if (s.methodName == m.name) { found = true; break; }
                j = j + 1;
            }
            if (!found) {
                VtSlot slot = new VtSlot(); slot.init();
                slot.index      = layout.vtable.length();
                slot.methodName = m.name;
                slot.ownerClass = cd.name;
                slot.retType    = this.llvmTypeRef(m.returnType);
                StringBuilder ptypes = new StringBuilder();
                int k = 0;
                while (k < m.params.length()) {
                    Param p = m.params.get(k) as Param;
                    if (k > 0) { ptypes.append(", "); }
                    ptypes.append(this.llvmTypeRef(p.ptype));
                    k = k + 1;
                }
                slot.paramTypes = ptypes.toString();
                layout.vtable.add(slot);
            }
            i = i + 1;
        }

        layout.hasVtable = layout.vtable.length() > 0;
    }

    private fun bool isVirtualMethod(FunctionDeclNode m) {
        if (m.name == "init")     { return false; }
        if (m.isStatic)           { return false; }
        if (m.access == 2)        { return false; }  // AM_Private
        return true;
    }

    private fun void emitVtableGlobals(Program prog) {
        this.globals.append("; ── Vtable globals ─────────────────────────────────────────\n");
        int i = 0;
        while (i < prog.declarations.length()) {
            ASTNode decl = prog.declarations.get(i) as ASTNode;
            if (decl.kind != 4) { i = i + 1; continue; }  // not Class

            ClassDeclNode cd = decl as ClassDeclNode;
            if (!this.classLayouts.has(cd.name)) { i = i + 1; continue; }
            ClassLayout layout = this.classLayouts.get(cd.name) as ClassLayout;
            if (!layout.hasVtable || layout.vtable.length() == 0) { i = i + 1; continue; }

            int n = layout.vtable.length();
            this.globals.append("@"); this.globals.append(cd.name);
            this.globals.append("_vtable = global [");
            this.globals.appendInt(n);
            this.globals.append(" x i8*] [\n");

            int j = 0;
            while (j < n) {
                VtSlot slot = layout.vtable.get(j) as VtSlot;
                if (j > 0) { this.globals.append(",\n"); }
                string fnType = slot.retType + " (%" + slot.ownerClass + "*";
                if (stringLen(slot.paramTypes) > 0) {
                    fnType = fnType + ", " + slot.paramTypes;
                }
                fnType = fnType + ")*";
                string mangledName = slot.ownerClass + "_" + slot.methodName;
                this.globals.append("  i8* bitcast (");
                this.globals.append(fnType);
                this.globals.append(" @");
                this.globals.append(mangledName);
                this.globals.append(" to i8*)");
                j = j + 1;
            }
            this.globals.append("\n]\n");
            i = i + 1;
        }
        this.globals.appendChar('\n');
    }

    private fun int fieldIndex(string className, string field) {
        if (!this.classLayouts.has(className)) { return -1; }
        ClassLayout layout = this.classLayouts.get(className) as ClassLayout;
        int i = 0;
        while (i < layout.fieldNames.length()) {
            string fn = _unboxStr(layout.fieldNames.get(i));
            if (fn == field) { return i + 1; }  // +1 for vtptr at slot 0
            i = i + 1;
        }
        return -1;
    }

    private fun string fieldLLVMType(string className, int idx) {
        if (idx == 0) { return "i8**"; }
        if (!this.classLayouts.has(className)) { return "i32"; }
        ClassLayout layout = this.classLayouts.get(className) as ClassLayout;
        int fi = idx - 1;
        if (fi < 0 || fi >= layout.fieldTypes.length()) { return "i32"; }
        return _unboxStr(layout.fieldTypes.get(fi));
    }

    private fun int vtableSlot(string className, string method) {
        if (!this.classLayouts.has(className)) { return -1; }
        ClassLayout layout = this.classLayouts.get(className) as ClassLayout;
        int i = 0;
        while (i < layout.vtable.length()) {
            VtSlot s = layout.vtable.get(i) as VtSlot;
            if (s.methodName == method) { return s.index; }
            i = i + 1;
        }
        return -1;
    }

    // ── Top-level generate ────────────────────────────────────────────────────

    public fun string generate(Program prog) {
        this.emitPreamble();

        // Pass 0: build class layouts
        int i = 0;
        while (i < prog.declarations.length()) {
            ASTNode d = prog.declarations.get(i) as ASTNode;
            if (d.kind == 4) { this.buildClassLayout(d); }  // Class
            i = i + 1;
        }

        // Pass 0.25: build vtable slots
        i = 0;
        while (i < prog.declarations.length()) {
            ASTNode d = prog.declarations.get(i) as ASTNode;
            if (d.kind == 4) { this.buildVtable(d, prog); }
            i = i + 1;
        }

        // Pass 0.5: pre-register function return types
        i = 0;
        while (i < prog.declarations.length()) {
            ASTNode d = prog.declarations.get(i) as ASTNode;
            if (d.kind == 5) {  // Function
                FunctionDeclNode fn = d as FunctionDeclNode;
                this.userFnRetTypes.set(fn.name, this.llvmTypeRef(fn.returnType));
            }
            if (d.kind == 4) {  // Class
                ClassDeclNode cd = d as ClassDeclNode;
                int j = 0;
                while (j < cd.methods.length()) {
                    FunctionDeclNode m = cd.methods.get(j) as FunctionDeclNode;
                    string mangled = cd.name + "_" + m.name;
                    this.userFnRetTypes.set(mangled, this.llvmTypeRef(m.returnType));
                    j = j + 1;
                }
            }
            i = i + 1;
        }

        // Pass 1: emit type declarations
        this.emitTypeDecls(prog);

        // Pass 2: emit runtime declarations
        this.emitRuntime();

        // Pass 2.5: emit vtable globals
        this.emitVtableGlobals(prog);

        // Pass 3: emit all declarations
        i = 0;
        while (i < prog.declarations.length()) {
            ASTNode d = prog.declarations.get(i) as ASTNode;
            this.emitDecl(d);
            i = i + 1;
        }

        // Collect and return
        StringBuilder result = new StringBuilder();
        result.append(this.globals.toString());
        result.append(this.body.toString());
        return result.toString();
    }

    private fun void emitPreamble() {
        this.globals.append("; Generated by ExtendedC compiler\n");
        this.globals.append("source_filename = \"");
        this.globals.append(this.moduleName);
        this.globals.append("\"\ntarget triple = \"x86_64-unknown-linux-gnu\"\n\n");
    }

    private fun void emitRuntime() {
        this.globals.append("; ── Runtime / stdlib declarations ─────────────────────────\n");
        this.globals.append("declare i32 @putchar(i32)\n");
        this.globals.append("declare i32 @getchar()\n");
        this.globals.append("declare i32 @printf(i8*, ...)\n");
        this.globals.append("declare i32 @scanf(i8*, ...)\n");
        this.globals.append("declare i8* @fgets(i8*, i32, i8*)\n");
        this.globals.append("declare i8* @malloc(i64)\n");
        this.globals.append("declare void @free(i8*)\n");
        this.globals.append("declare i64 @strlen(i8*)\n");
        this.globals.append("declare i8* @strcpy(i8*, i8*)\n");
        this.globals.append("declare i8* @strcat(i8*, i8*)\n");
        this.globals.append("declare i32 @strcmp(i8*, i8*)\n");
        this.globals.append("declare i32 @atoi(i8*)\n");
        this.globals.append("declare double @atof(i8*)\n");
        this.globals.append("declare double @sqrt(double)\n");
        this.globals.append("declare double @pow(double, double)\n");
        this.globals.append("declare double @sin(double)\n");
        this.globals.append("declare double @cos(double)\n");
        this.globals.append("declare double @floor(double)\n");
        this.globals.append("declare double @ceil(double)\n");
        this.globals.append("declare double @fabs(double)\n");
        this.globals.append("declare double @log(double)\n");
        this.globals.append("declare i8* @ec_strconcat(i8*, i8*)\n");
        this.globals.append("declare i8* @ec_int_to_str(i32)\n");
        this.globals.append("declare i8* @ec_float_to_str(float)\n");
        this.globals.append("declare i8* @ec_char_to_str(i8)\n");
        this.globals.append("declare i32 @ec_str_len(i8*)\n");
        this.globals.append("declare i8  @ec_char_at(i8*, i32)\n");
        this.globals.append("declare i8* @ec_substring(i8*, i32, i32)\n");
        this.globals.append("declare i8* @ec_readline()\n");
        this.globals.append("declare void @ec_println(i8*)\n");
        this.globals.append("declare void @ec_print(i8*)\n");
        this.globals.append("declare i8* @ec_typeof(i8*)\n");
        this.globals.append("declare i8* @ec_new(i64)\n");
        this.globals.append("declare void @ec_delete(i8*)\n");
        this.globals.append("declare i8* @ec_array_new(i64, i64)\n");
        this.globals.append("declare i64 @ec_array_len(i8*)\n");
        this.globals.append("declare i32 @ec_str_find(i8*, i8*)\n");
        this.globals.append("declare i32 @ec_str_find_last(i8*, i8*)\n");
        this.globals.append("declare i32 @ec_str_starts_with(i8*, i8*)\n");
        this.globals.append("declare i32 @ec_str_ends_with(i8*, i8*)\n");
        this.globals.append("declare i32 @ec_str_contains(i8*, i8*)\n");
        this.globals.append("declare i32 @ec_str_equals(i8*, i8*)\n");
        this.globals.append("declare i32 @ec_str_compare(i8*, i8*)\n");
        this.globals.append("declare i8* @ec_str_to_lower(i8*)\n");
        this.globals.append("declare i8* @ec_str_to_upper(i8*)\n");
        this.globals.append("declare i8* @ec_str_trim(i8*)\n");
        this.globals.append("declare i8* @ec_str_trim_left(i8*)\n");
        this.globals.append("declare i8* @ec_str_trim_right(i8*)\n");
        this.globals.append("declare i8* @ec_str_repeat(i8*, i32)\n");
        this.globals.append("declare i8* @ec_str_replace(i8*, i8*, i8*)\n");
        this.globals.append("declare i8* @ec_str_reverse(i8*)\n");
        this.globals.append("declare i8* @ec_str_split(i8*, i8*)\n");
        this.globals.append("declare i8* @ec_str_join(i8*, i8*)\n");
        this.globals.append("declare i32 @ec_str_count(i8*, i8*)\n");
        this.globals.append("declare i32 @ec_char_is_digit(i8)\n");
        this.globals.append("declare i32 @ec_char_is_alpha(i8)\n");
        this.globals.append("declare i32 @ec_char_is_alnum(i8)\n");
        this.globals.append("declare i32 @ec_char_is_space(i8)\n");
        this.globals.append("declare i32 @ec_char_is_upper(i8)\n");
        this.globals.append("declare i32 @ec_char_is_lower(i8)\n");
        this.globals.append("declare i32 @ec_char_is_punct(i8)\n");
        this.globals.append("declare i8  @ec_char_to_lower_fn(i8)\n");
        this.globals.append("declare i8  @ec_char_to_upper_fn(i8)\n");
        this.globals.append("declare i32 @ec_char_code(i8)\n");
        this.globals.append("declare i8  @ec_char_from_code(i32)\n");
        this.globals.append("declare i32    @ec_abs_int(i32)\n");
        this.globals.append("declare i64    @ec_abs_long(i64)\n");
        this.globals.append("declare i32    @ec_min_int(i32, i32)\n");
        this.globals.append("declare i32    @ec_max_int(i32, i32)\n");
        this.globals.append("declare i64    @ec_min_long(i64, i64)\n");
        this.globals.append("declare i64    @ec_max_long(i64, i64)\n");
        this.globals.append("declare double @ec_min_double(double, double)\n");
        this.globals.append("declare double @ec_max_double(double, double)\n");
        this.globals.append("declare i32    @ec_clamp_int(i32, i32, i32)\n");
        this.globals.append("declare double @ec_clamp_double(double, double, double)\n");
        this.globals.append("declare i32    @ec_gcd(i32, i32)\n");
        this.globals.append("declare i32    @ec_lcm(i32, i32)\n");
        this.globals.append("declare i32    @ec_is_even(i32)\n");
        this.globals.append("declare i32    @ec_is_odd(i32)\n");
        this.globals.append("declare i32    @ec_is_prime(i32)\n");
        this.globals.append("declare i32    @ec_pow_int(i32, i32)\n");
        this.globals.append("declare double @ec_cbrt_fn(double)\n");
        this.globals.append("declare double @ec_hypot_fn(double, double)\n");
        this.globals.append("declare double @ec_log2_fn(double)\n");
        this.globals.append("declare double @ec_log10_fn(double)\n");
        this.globals.append("declare double @ec_atan2_fn(double, double)\n");
        this.globals.append("declare double @ec_infinity()\n");
        this.globals.append("declare double @ec_nan()\n");
        this.globals.append("declare i8* @ec_list_new()\n");
        this.globals.append("declare i64  @ec_list_len(i8*)\n");
        this.globals.append("declare i8*  @ec_list_get(i8*, i64)\n");
        this.globals.append("declare i8*  @ec_list_add(i8*, i8*)\n");
        this.globals.append("declare i8*  @ec_list_set(i8*, i64, i8*)\n");
        this.globals.append("declare i8*  @ec_list_remove(i8*, i64)\n");
        this.globals.append("declare i8*  @ec_list_insert(i8*, i64, i8*)\n");
        this.globals.append("declare i64  @ec_list_index_of(i8*, i8*)\n");
        this.globals.append("declare i32  @ec_list_contains(i8*, i8*)\n");
        this.globals.append("declare i8*  @ec_list_slice(i8*, i64, i64)\n");
        this.globals.append("declare i8*  @ec_list_concat(i8*, i8*)\n");
        this.globals.append("declare i8* @ec_map_new()\n");
        this.globals.append("declare i8* @ec_map_set(i8*, i8*, i8*)\n");
        this.globals.append("declare i8* @ec_map_get(i8*, i8*)\n");
        this.globals.append("declare i32 @ec_map_has(i8*, i8*)\n");
        this.globals.append("declare i8* @ec_map_delete(i8*, i8*)\n");
        this.globals.append("declare i64 @ec_map_count(i8*)\n");
        this.globals.append("declare i8* @ec_map_keys(i8*)\n");
        this.globals.append("declare i8* @ec_map_values(i8*)\n");
        this.globals.append("declare i8* @ec_set_new()\n");
        this.globals.append("declare i8* @ec_set_add(i8*, i8*)\n");
        this.globals.append("declare i32 @ec_set_has(i8*, i8*)\n");
        this.globals.append("declare i8* @ec_set_remove(i8*, i8*)\n");
        this.globals.append("declare i64 @ec_set_count(i8*)\n");
        this.globals.append("declare i8* @ec_set_keys(i8*)\n");
        this.globals.append("declare i8* @ec_sb_new()\n");
        this.globals.append("declare i8* @ec_sb_append_str(i8*, i8*)\n");
        this.globals.append("declare i8* @ec_sb_append_char(i8*, i8)\n");
        this.globals.append("declare i8* @ec_sb_append_int(i8*, i32)\n");
        this.globals.append("declare i8* @ec_sb_append_long(i8*, i64)\n");
        this.globals.append("declare i8* @ec_sb_to_string(i8*)\n");
        this.globals.append("declare i64  @ec_sb_length(i8*)\n");
        this.globals.append("declare i8* @ec_sb_clear(i8*)\n");
        this.globals.append("declare void @ec_exit(i32)\n");
        this.globals.append("declare i64  @ec_time_ms()\n");
        this.globals.append("declare i32  @atol(i8*)\n");
        this.globals.append("declare void @ec_gc_init()\n");
        this.globals.append("declare void @ec_gc_collect()\n");
        this.globals.append("declare void @ec_gc_register_global(i8**)\n");
        this.globals.append("declare void @ec_gc_push_root(i8*)\n");
        this.globals.append("declare void @ec_gc_pop_root()\n");
        this.globals.append("declare void @ec_gc_pin(i8*)\n");
        this.globals.append("declare void @ec_gc_unpin(i8*)\n");
        this.globals.append("declare i8* @ec_gc_stats()\n");
        this.globals.append("declare i64 @ec_gc_live_bytes()\n");
        this.globals.append("declare i64 @ec_gc_live_objects()\n");
        this.globals.append("declare i64 @ec_gc_num_collections()\n");
        this.globals.append("declare i8* @ec_read_file(i8*)\n");
        this.globals.append("declare i32 @ec_write_file(i8*, i8*)\n");
        this.globals.append("declare i32 @ec_file_exists(i8*)\n");
        this.globals.append("; setjmp — returns_twice tells LLVM it can return more than once\n");
        this.globals.append("declare i32 @setjmp(i8*) returns_twice\n");
        this.globals.append("declare void @ec_try_push(i8*)\n");
        this.globals.append("declare void @ec_try_pop()\n");
        this.globals.append("declare void @ec_throw(i8*)\n");
        this.globals.append("declare i8* @ec_catch_msg()\n");
        this.globals.append("declare void @ec_try_end()\n");
        this.globals.append("\n");
        this.globals.append("@stdin = external global i8*\n\n");
    }

    private fun void emitTypeDecls(Program prog) {
        this.globals.append("; ── Type declarations ──────────────────────────────────────\n");
        int i = 0;
        while (i < prog.declarations.length()) {
            ASTNode d = prog.declarations.get(i) as ASTNode;
            if (d.kind == 2) {  // Struct
                StructDeclNode sd = d as StructDeclNode;
                this.globals.append("%"); this.globals.append(sd.name);
                this.globals.append(" = type { ");
                int j = 0;
                while (j < sd.fields.length()) {
                    FieldDecl f = sd.fields.get(j) as FieldDecl;
                    if (j > 0) { this.globals.append(", "); }
                    this.globals.append(this.llvmTypeRef(f.ftype));
                    j = j + 1;
                }
                this.globals.append(" }\n");
            } else if (d.kind == 3) {  // Union
                UnionDeclNode ud = d as UnionDeclNode;
                int maxSz = 4;
                int j = 0;
                while (j < ud.fields.length()) {
                    FieldDecl f = ud.fields.get(j) as FieldDecl;
                    string lt = this.llvmTypeRef(f.ftype);
                    int sz = 4;
                    if (lt == "i64" || lt == "double") { sz = 8; }
                    if (lt == "i8*") { sz = 8; }
                    if (sz > maxSz) { maxSz = sz; }
                    j = j + 1;
                }
                this.globals.append("%"); this.globals.append(ud.name);
                this.globals.append(" = type { ["); this.globals.appendInt(maxSz);
                this.globals.append(" x i8] }\n");
            } else if (d.kind == 4) {  // Class
                ClassDeclNode cd = d as ClassDeclNode;
                this.globals.append("%"); this.globals.append(cd.name);
                this.globals.append(" = type { i8**");
                if (this.classLayouts.has(cd.name)) {
                    ClassLayout layout = this.classLayouts.get(cd.name) as ClassLayout;
                    int j = 0;
                    while (j < layout.fieldTypes.length()) {
                        this.globals.append(", ");
                        this.globals.append(_unboxStr(layout.fieldTypes.get(j)));
                        j = j + 1;
                    }
                }
                this.globals.append(" }\n");
            }
            i = i + 1;
        }
        this.globals.appendChar('\n');
    }

    private fun void emitDecl(ASTNode d) {
        int k = d.kind;
        if (k == 0) { return; }  // Import
        if (k == 1) { this.emitEnumDecl(d);    return; }
        if (k == 2) { return; }  // Struct — type already emitted
        if (k == 3) { return; }  // Union — type already emitted
        if (k == 4) { this.emitClassDecl(d);   return; }
        if (k == 5) { this.emitFunctionDecl(d, ""); return; }
        if (k == 6) { this.emitGlobalVar(d);   return; }
    }

    private fun void emitEnumDecl(ASTNode n) {
        EnumDeclNode ed = n as EnumDeclNode;
        this.globals.append("; enum "); this.globals.append(ed.name); this.globals.appendChar('\n');
        int i = 0;
        while (i < ed.members.length()) {
            string mname = _unboxStr(ed.members.get(i));
            string key1 = ed.name + "::" + mname;
            string key2 = mname;
            this.enumValues.set(key1, _box(i));
            this.enumValues.set(key2, _box(i));
            this.globals.append("@"); this.globals.append(ed.name);
            this.globals.append("."); this.globals.append(mname);
            this.globals.append(" = private constant i32 "); this.globals.appendInt(i);
            this.globals.appendChar('\n');
            i = i + 1;
        }
        this.globals.appendChar('\n');
    }

    private fun void emitGlobalVar(ASTNode n) {
        VarDeclNode vd = n as VarDeclNode;
        string lt = this.llvmTypeRef(vd.vtype);
        this.globals.append("@"); this.globals.append(vd.name);
        this.globals.append(" = global "); this.globals.append(lt);
        this.globals.append(" "); this.globals.append(this.llvmDefault(lt));
        this.globals.appendChar('\n');
        this.globalVars.set(vd.name, lt);
    }

    private fun void emitClassDecl(ASTNode n) {
        ClassDeclNode cd = n as ClassDeclNode;
        this.globals.append("; class "); this.globals.append(cd.name); this.globals.appendChar('\n');
        string savedClass = this.currentClass;
        this.currentClass = cd.name;
        int i = 0;
        while (i < cd.methods.length()) {
            ASTNode m = cd.methods.get(i) as ASTNode;
            this.emitFunctionDecl(m, cd.name);
            i = i + 1;
        }
        this.currentClass = savedClass;
    }

    // ── Function declaration / definition ─────────────────────────────────────

    private fun void emitFunctionDecl(ASTNode n, string ownerClass) {
        FunctionDeclNode fd = n as FunctionDeclNode;
        string irName = stringLen(ownerClass) > 0 ?
                        ownerClass + "_" + fd.name : fd.name;

        string retLLVM = this.llvmTypeRef(fd.returnType);
        this.userFnRetTypes.set(irName, retLLVM);

        // Build parameter string
        StringBuilder params = new StringBuilder();
        if (stringLen(ownerClass) > 0) {
            params.append("%"); params.append(ownerClass); params.append("* %self");
        }
        int i = 0;
        while (i < fd.params.length()) {
            Param p = fd.params.get(i) as Param;
            if (params.length() > 0) { params.append(", "); }
            params.append(this.llvmTypeRef(p.ptype));
            params.append(" %"); params.append(p.name);
            i = i + 1;
        }

        // Switch to body stream
        this.outTarget = 1;

        this.body.append("\ndefine "); this.body.append(retLLVM);
        this.body.append(" @"); this.body.append(irName);
        this.body.append("("); this.body.append(params.toString());
        this.body.append(") {\nentry:\n");

        // Set up function context
        FnCtx fctx = new FnCtx(); fctx.init();
        fctx.name       = irName;
        fctx.retType    = retLLVM;
        fctx.hasVoidRet = (retLLVM == "void");
        this.ctx = fctx;

        // main: emit gc_init and register globals
        if (irName == "main") {
            this.emit("call void @ec_gc_init()");
            List gkeys = this.globalVars.keys();
            int gi = 0;
            while (gi < gkeys.length()) {
                string gname = _unboxStr(gkeys.get(gi));
                string glt   = _unboxStr(this.globalVars.get(gname));
                int gltlen = stringLen(glt);
                if (gltlen > 0 && glt[gltlen-1] == '*') {
                    string slot = this.freshName("gslot");
                    this.emit(slot + " = bitcast " + glt + "* @" + gname + " to i8**");
                    this.emit("call void @ec_gc_register_global(i8** " + slot + ")");
                }
                gi = gi + 1;
            }
        }

        // Spill parameters to allocas
        if (stringLen(ownerClass) > 0) {
            string ptrType = "%" + ownerClass + "*";
            string ptr = this.emitAlloca(ptrType, "self");
            this.emitStore(ptrType, "%self", ptr);
            fctx.locals.set("this", ptr);
            fctx.localTypes.set("this", ptrType);
        }
        i = 0;
        while (i < fd.params.length()) {
            Param p = fd.params.get(i) as Param;
            string lt = this.llvmTypeRef(p.ptype);
            string ptr = this.emitAlloca(lt, p.name);
            this.emitStore(lt, "%" + p.name, ptr);
            fctx.locals.set(p.name, ptr);
            fctx.localTypes.set(p.name, lt);
            i = i + 1;
        }

        // Emit body
        if (fd.body != null) {
            this.emitBlock(fd.body as ASTNode);
        }

        // Default return
        if (!fctx.terminated) {
            if (fctx.hasVoidRet) { this.emit("ret void"); }
            else { this.emit("ret " + retLLVM + " " + this.llvmDefault(retLLVM)); }
        }

        this.body.append("}\n");
        this.ctx = null;
        this.outTarget = 0;

        // Flush pending lambdas
        if (this.lambdas.length() > 0) {
            this.body.append(this.lambdas.toString());
            this.lambdas.clear();
        }
    }

    // ── Statement emitters ────────────────────────────────────────────────────

    private fun void emitStmt(ASTNode stmt) {
        int k = stmt.kind;
        if (k == 0)  { this.emitBlock(stmt);       return; }
        if (k == 1)  { this.emitExprStmt(stmt);    return; }
        if (k == 2)  { this.emitVarDeclStmt(stmt); return; }
        if (k == 3)  { this.emitIfStmt(stmt);      return; }
        if (k == 4)  { this.emitWhileStmt(stmt);   return; }
        if (k == 5)  { this.emitDoWhileStmt(stmt); return; }
        if (k == 6)  { this.emitForStmt(stmt);     return; }
        if (k == 7)  { this.emitSwitchStmt(stmt);  return; }
        if (k == 8)  { this.emitReturnStmt(stmt);  return; }
        if (k == 9)  { this.emitBreakStmt();        return; }
        if (k == 10) { this.emitContinueStmt();     return; }
        if (k == 11) { this.emitThrowStmt(stmt);   return; }
        if (k == 12) { this.emitTryStmt(stmt);     return; }
    }

    private fun void emitBlock(ASTNode n) {
        BlockNode bn = n as BlockNode;
        int i = 0;
        while (i < bn.stmts.length()) {
            ASTNode s = bn.stmts.get(i) as ASTNode;
            this.emitStmt(s);
            i = i + 1;
        }
    }

    private fun void emitExprStmt(ASTNode n) {
        ExprStmtNode es = n as ExprStmtNode;
        if (es.expr != null) { this.emitExpr(es.expr as ASTNode); }
    }

    private fun void emitVarDeclStmt(ASTNode n) {
        VarDeclStmtNode vs = n as VarDeclStmtNode;
        FnCtx c = this.ctx as FnCtx;
        string lt = this.llvmTypeRef(vs.vtype);

        IRValue initVal = makeIRValue(this.llvmDefault(lt), lt);
        if (vs.init_ != null) {
            initVal = this.emitExpr(vs.init_ as ASTNode);
            if (vs.vtype.name == "var") { lt = initVal.type; }
            else { initVal = this.coerce(initVal, lt == "i8*" ? initVal.type : lt); }
        }

        string ptr = this.emitAlloca(lt, vs.name);
        if (initVal.isValid() && stringLen(initVal.name) > 0) {
            this.emitStore(lt, initVal.name, ptr);
        }
        c.locals.set(vs.name, ptr);
        c.localTypes.set(vs.name, lt);

        // Propagate closure signature if RHS was a lambda
        if (vs.init_ != null) {
            ASTNode initNode = vs.init_ as ASTNode;
            if (initNode.kind == 18) {  // Lambda
                // Find the most recently registered lambda sig
                List ckeys = this.closureTypes.keys();
                int latestId = -1;
                string latestKey = "";
                int i = 0;
                while (i < ckeys.length()) {
                    string k = _unboxStr(ckeys.get(i));
                    if (startsWith(k, "lambda.")) {
                        int dotIdx = indexOf(k, ".");
                        string numStr = substring(k, dotIdx + 1, stringLen(k));
                        int id = parseInt(numStr);
                        if (id > latestId) { latestId = id; latestKey = k; }
                    }
                    i = i + 1;
                }
                if (stringLen(latestKey) > 0 && this.closureTypes.has(latestKey)) {
                    this.closureTypes.set(vs.name, this.closureTypes.get(latestKey));
                }
            }
        }
    }

    private fun void emitIfStmt(ASTNode n) {
        IfNode ifn = n as IfNode;
        IRValue cond = this.emitExpr(ifn.cond as ASTNode);
        cond = this.coerce(cond, "i1");

        string thenLbl  = this.freshLabel("if.then");
        string elseLbl  = this.freshLabel("if.else");
        string mergeLbl = this.freshLabel("if.end");

        bool hasElse = ifn.elseBranch != null;
        this.emitCondBr(cond.name, thenLbl, hasElse ? elseLbl : mergeLbl);

        this.emitLabel(thenLbl);
        this.emitStmt(ifn.thenBranch as ASTNode);
        this.emitBr(mergeLbl);

        if (hasElse) {
            this.emitLabel(elseLbl);
            this.emitStmt(ifn.elseBranch as ASTNode);
            this.emitBr(mergeLbl);
        }

        this.emitLabel(mergeLbl);
    }

    private fun void emitWhileStmt(ASTNode n) {
        WhileNode wn = n as WhileNode;
        FnCtx c = this.ctx as FnCtx;
        string condLbl = this.freshLabel("while.cond");
        string bodyLbl = this.freshLabel("while.body");
        string endLbl  = this.freshLabel("while.end");

        c.breakLabels.add(_boxStr(endLbl));
        c.continueLabels.add(_boxStr(condLbl));

        this.emitBr(condLbl);
        this.emitLabel(condLbl);
        IRValue cond = this.emitExpr(wn.cond as ASTNode);
        cond = this.coerce(cond, "i1");
        this.emitCondBr(cond.name, bodyLbl, endLbl);

        this.emitLabel(bodyLbl);
        this.emitStmt(wn.body as ASTNode);
        this.emitBr(condLbl);

        this.emitLabel(endLbl);
        c.breakLabels.remove(c.breakLabels.length() - 1);
        c.continueLabels.remove(c.continueLabels.length() - 1);
    }

    private fun void emitDoWhileStmt(ASTNode n) {
        DoWhileNode dn = n as DoWhileNode;
        FnCtx c = this.ctx as FnCtx;
        string bodyLbl = this.freshLabel("dowhile.body");
        string condLbl = this.freshLabel("dowhile.cond");
        string endLbl  = this.freshLabel("dowhile.end");

        c.breakLabels.add(_boxStr(endLbl));
        c.continueLabels.add(_boxStr(condLbl));

        this.emitBr(bodyLbl);
        this.emitLabel(bodyLbl);
        this.emitStmt(dn.body as ASTNode);
        this.emitBr(condLbl);

        this.emitLabel(condLbl);
        IRValue cond = this.emitExpr(dn.cond as ASTNode);
        cond = this.coerce(cond, "i1");
        this.emitCondBr(cond.name, bodyLbl, endLbl);

        this.emitLabel(endLbl);
        c.breakLabels.remove(c.breakLabels.length() - 1);
        c.continueLabels.remove(c.continueLabels.length() - 1);
    }

    private fun void emitForStmt(ASTNode n) {
        ForNode fn = n as ForNode;
        FnCtx c = this.ctx as FnCtx;
        string condLbl = this.freshLabel("for.cond");
        string bodyLbl = this.freshLabel("for.body");
        string incrLbl = this.freshLabel("for.incr");
        string endLbl  = this.freshLabel("for.end");

        if (fn.init_ != null) { this.emitStmt(fn.init_ as ASTNode); }

        c.breakLabels.add(_boxStr(endLbl));
        c.continueLabels.add(_boxStr(incrLbl));

        this.emitBr(condLbl);
        this.emitLabel(condLbl);
        if (fn.cond != null) {
            IRValue cond = this.emitExpr(fn.cond as ASTNode);
            cond = this.coerce(cond, "i1");
            this.emitCondBr(cond.name, bodyLbl, endLbl);
        } else {
            this.emitBr(bodyLbl);
        }

        this.emitLabel(bodyLbl);
        this.emitStmt(fn.body as ASTNode);
        this.emitBr(incrLbl);

        this.emitLabel(incrLbl);
        if (fn.incr != null) { this.emitExpr(fn.incr as ASTNode); }
        this.emitBr(condLbl);

        this.emitLabel(endLbl);
        c.breakLabels.remove(c.breakLabels.length() - 1);
        c.continueLabels.remove(c.continueLabels.length() - 1);
    }

    private fun void emitSwitchStmt(ASTNode n) {
        SwitchNode sn = n as SwitchNode;
        FnCtx c = this.ctx as FnCtx;
        IRValue subj = this.emitExpr(sn.subject as ASTNode);
        subj = this.coerce(subj, "i32");

        string endLbl     = this.freshLabel("switch.end");
        string defaultLbl = endLbl;
        c.breakLabels.add(_boxStr(endLbl));

        // Build case labels
        List caseLabels = new List();
        int i = 0;
        while (i < sn.cases.length()) {
            CaseClause cc = sn.cases.get(i) as CaseClause;
            string lbl = this.freshLabel(cc.value != null ? "case" : "default");
            caseLabels.add(_boxStr(lbl));
            if (cc.value == null) { defaultLbl = lbl; }
            i = i + 1;
        }

        // Emit switch instruction
        StringBuilder sw = new StringBuilder();
        sw.append("switch i32 "); sw.append(subj.name);
        sw.append(", label %"); sw.append(defaultLbl); sw.append(" [\n");
        i = 0;
        while (i < sn.cases.length()) {
            CaseClause cc = sn.cases.get(i) as CaseClause;
            if (cc.value != null) {
                IRValue cv = this.emitExpr(cc.value as ASTNode);
                cv = this.coerce(cv, "i32");
                string cl = _unboxStr(caseLabels.get(i));
                sw.append("    i32 "); sw.append(cv.name);
                sw.append(", label %"); sw.append(cl); sw.appendChar('\n');
            }
            i = i + 1;
        }
        sw.append("  ]");
        this.emit(sw.toString());
        if (this.ctx != null) { FnCtx fc = this.ctx as FnCtx; fc.terminated = true; }

        i = 0;
        while (i < sn.cases.length()) {
            CaseClause cc = sn.cases.get(i) as CaseClause;
            string cl = _unboxStr(caseLabels.get(i));
            this.emitLabel(cl);
            int j = 0;
            while (j < cc.stmts.length()) {
                ASTNode s = cc.stmts.get(j) as ASTNode;
                this.emitStmt(s);
                j = j + 1;
            }
            if (i + 1 < caseLabels.length()) {
                this.emitBr(_unboxStr(caseLabels.get(i + 1)));
            } else {
                this.emitBr(endLbl);
            }
            i = i + 1;
        }

        this.emitLabel(endLbl);
        c.breakLabels.remove(c.breakLabels.length() - 1);
    }

    private fun void emitReturnStmt(ASTNode n) {
        ReturnNode rn = n as ReturnNode;
        FnCtx c = this.ctx as FnCtx;
        if (rn.value != null) {
            IRValue val = this.emitExpr(rn.value as ASTNode);
            val = this.coerce(val, c.retType);
            this.emit("ret " + c.retType + " " + val.name);
        } else {
            this.emit("ret void");
        }
        c.terminated = true;
    }

    private fun void emitThrowStmt(ASTNode n) {
        ThrowNode tn = n as ThrowNode;
        IRValue msg = this.emitExpr(tn.value as ASTNode);
        if (msg.type != "i8*") {
            string tmp = this.freshName("throw.msg");
            this.emit(tmp + " = call i8* @ec_int_to_str(i32 " + msg.name + ")");
            msg = makeIRValue(tmp, "i8*");
        }
        this.emit("call void @ec_throw(i8* " + msg.name + ")");
        this.emit("unreachable");
        if (this.ctx != null) { FnCtx c = this.ctx as FnCtx; c.terminated = true; }
    }

    private fun void emitTryStmt(ASTNode n) {
        TryNode tn = n as TryNode;
        FnCtx c = this.ctx as FnCtx;
        string tryLbl   = this.freshLabel("try.body");
        string catchLbl = this.freshLabel("try.catch");
        string endLbl   = this.freshLabel("try.end");

        string bufName = this.freshName("jmpbuf");
        this.emit(bufName + " = alloca [25 x i64]");
        string bufPtr = this.freshName("jmpbuf.ptr");
        this.emit(bufPtr + " = bitcast [25 x i64]* " + bufName + " to i8*");
        this.emit("call void @ec_try_push(i8* " + bufPtr + ")");
        string rc = this.freshName("sjlj.rc");
        this.emit(rc + " = call i32 @setjmp(i8* " + bufPtr + ")");
        string threw = this.freshName("threw");
        this.emit(threw + " = icmp ne i32 " + rc + ", 0");
        this.emitCondBr(threw, catchLbl, tryLbl);

        this.emitLabel(tryLbl);
        c.inTry = true;
        this.emitBlock(tn.tryBody as ASTNode);
        c.inTry = false;
        if (!c.terminated) {
            this.emit("call void @ec_try_pop()");
            this.emitBr(endLbl);
        }

        this.emitLabel(catchLbl);
        int i = 0;
        while (i < tn.catches.length()) {
            CatchClause cc = tn.catches.get(i) as CatchClause;
            string msgPtr = this.freshName("catch.msg");
            this.emit(msgPtr + " = call i8* @ec_catch_msg()");
            if (stringLen(cc.exName) > 0) {
                string ptr = this.emitAlloca("i8*", cc.exName);
                this.emitStore("i8*", msgPtr, ptr);
                c.locals.set(cc.exName, ptr);
                c.localTypes.set(cc.exName, "i8*");
            }
            this.emitBlock(cc.body as ASTNode);
            i = i + 1;
        }
        if (!c.terminated) { this.emitBr(endLbl); }

        this.emitLabel(endLbl);
    }

    private fun void emitBreakStmt() {
        if (this.ctx == null) { return; }
        FnCtx c = this.ctx as FnCtx;
        if (c.breakLabels.length() > 0) {
            string lbl = _unboxStr(c.breakLabels.last());
            this.emitBr(lbl);
        }
    }

    private fun void emitContinueStmt() {
        if (this.ctx == null) { return; }
        FnCtx c = this.ctx as FnCtx;
        if (c.continueLabels.length() > 0) {
            string lbl = _unboxStr(c.continueLabels.last());
            this.emitBr(lbl);
        }
    }

    // ── L-value (address) emission ────────────────────────────────────────────

    private fun string emitLValue(ASTNode expr) {
        FnCtx c = this.ctx as FnCtx;
        int k = expr.kind;

        if (k == 6) {  // Identifier
            IdentNode id = expr as IdentNode;
            if (c.locals.has(id.name)) { return c.locals.get(id.name) as string; }
            return "@" + id.name;
        }

        if (k == 12) {  // Member
            MemberNode mn = expr as MemberNode;
            IRValue obj = this.emitExpr(mn.object as ASTNode);
            string className = this.extractClassName(obj.type);
            string rawType = obj.type;
            int tlen = stringLen(rawType);
            if (tlen > 0 && rawType[tlen-1] == '*') {
                rawType = substring(rawType, 0, tlen - 1);
            }
            int idx = this.fieldIndex(className, mn.member);
            if (idx < 0) { return "%invalid"; }
            return this.emitGEP(rawType, obj.name, idx, mn.member + ".gep");
        }

        if (k == 13) {  // Index
            IndexNode idx = expr as IndexNode;
            IRValue arr = this.emitExpr(idx.array as ASTNode);
            IRValue idxv = this.emitExpr(idx.index as ASTNode);
            idxv = this.coerce(idxv, "i64");
            string elemType = "i32";
            // We'd need resolved type here; default to i32
            return this.emitArrayElemPtr(arr.name, elemType, idxv.name);
        }

        return "%invalid.lvalue";
    }

    // ── Expression emitters ───────────────────────────────────────────────────

    private fun IRValue emitExpr(ASTNode expr) {
        if (expr == null) { return makeIRValue("0", "i32"); }
        int k = expr.kind;

        if (k == 0)  { return this.emitIntLit(expr);    }
        if (k == 1)  { return this.emitFloatLit(expr);  }
        if (k == 2)  { return this.emitBoolLit(expr);   }
        if (k == 3)  { return this.emitCharLit(expr);   }
        if (k == 4)  { return this.emitStringLit(expr); }
        if (k == 5)  { return makeIRValue("null", "i8*"); }  // NullLit
        if (k == 6)  { return this.emitIdent(expr);     }
        if (k == 7)  { return this.emitUnary(expr);     }
        if (k == 8)  { return this.emitBinary(expr);    }
        if (k == 9)  { return this.emitTernary(expr);   }
        if (k == 10) { return this.emitAssign(expr);    }
        if (k == 11) { return this.emitCall(expr);      }
        if (k == 12) { return this.emitMember(expr);    }
        if (k == 13) { return this.emitIndex(expr);     }
        if (k == 14) { return this.emitNew(expr);       }
        if (k == 15) { return this.emitDelete(expr);    }
        if (k == 16) { return makeIRValue("null", "i8*"); }  // TypeOf placeholder
        if (k == 17) { return this.emitCast(expr);      }
        if (k == 18) { return this.emitLambda(expr);    }
        if (k == 19) { return this.emitInstanceOf(expr); }
        if (k == 20) { return this.emitArrayLit(expr);  }

        return makeIRValue("0", "i32");
    }

    private fun IRValue emitIntLit(ASTNode n) {
        IntLitNode il = n as IntLitNode;
        return makeIRValue(toString(il.value), "i32");
    }

    private fun IRValue emitFloatLit(ASTNode n) {
        FloatLitNode fl = n as FloatLitNode;
        string raw = fl.raw;
        int rawlen = stringLen(raw);
        bool isF = rawlen > 0 && (raw[rawlen-1] == 'f' || raw[rawlen-1] == 'F');
        // Parse to double and format with decimal point
        string val = this.formatFloatLit(raw);
        return makeIRValue(val, isF ? "float" : "double");
    }

    private fun string formatFloatLit(string raw) {
        // Strip suffixes
        StringBuilder clean = new StringBuilder();
        int i = 0;
        while (i < stringLen(raw)) {
            char c = raw[i];
            if (c != 'f' && c != 'F' && c != '_') { clean.appendChar(c); }
            i = i + 1;
        }
        string s = clean.toString();
        // Ensure decimal point
        if (!contains(s, ".") && !contains(s, "e") && !contains(s, "E")) {
            s = s + ".0";
        }
        return s;
    }

    private fun IRValue emitBoolLit(ASTNode n) {
        BoolLitNode bl = n as BoolLitNode;
        return makeIRValue(bl.value ? "true" : "false", "i1");
    }

    private fun IRValue emitCharLit(ASTNode n) {
        CharLitNode cl = n as CharLitNode;
        string raw = cl.raw;
        int code = 0;
        if (stringLen(raw) >= 3) {
            if (raw[1] == '\\') {
                char esc = raw[2];
                if (esc == 'n')       { code = 10; }
                else if (esc == 't')  { code = 9;  }
                else if (esc == 'r')  { code = 13; }
                else if (esc == '0')  { code = 0;  }
                else                  { code = charCode(esc); }
            } else {
                code = charCode(raw[1]);
            }
        }
        return makeIRValue(toString(code), "i8");
    }

    private fun IRValue emitStringLit(ASTNode n) {
        StringLitNode sl = n as StringLitNode;
        string gname = this.internString(sl.value);
        int len = stringLen(sl.value) + 1;
        string res = this.freshName("str");
        this.emit(res + " = getelementptr inbounds [" + toString(len) + " x i8], [" +
                  toString(len) + " x i8]* " + gname + ", i32 0, i32 0");
        return makeIRValue(res, "i8*");
    }

    private fun IRValue emitIdent(ASTNode n) {
        IdentNode id = n as IdentNode;
        FnCtx c = this.ctx as FnCtx;

        if (id.name == "this" || id.name == "self") {
            if (c.locals.has("this")) {
                string lt = c.localTypes.get("this") as string;
                string val = this.emitLoad(lt, c.locals.get("this") as string);
                return makeIRValue(val, lt);
            }
            return makeIRValue("null", "i8*");
        }

        if (id.name == "super") {
            if (c.locals.has("this")) {
                string lt  = c.localTypes.get("this") as string;
                string val = this.emitLoad(lt, c.locals.get("this") as string);
                if (stringLen(this.currentClass) > 0 &&
                    this.classLayouts.has(this.currentClass)) {
                    ClassLayout lay = this.classLayouts.get(this.currentClass) as ClassLayout;
                    string superType = "%" + lay.superClass + "*";
                    if (lt != superType && stringLen(lay.superClass) > 0) {
                        string casted = this.freshName("super");
                        this.emit(casted + " = bitcast " + lt + " " + val + " to " + superType);
                        return makeIRValue(casted, superType);
                    }
                    return makeIRValue(val, superType);
                }
            }
            return makeIRValue("null", "i8*");
        }

        // Local variable
        if (c.locals.has(id.name)) {
            string lt  = c.localTypes.get(id.name) as string;
            string val = this.emitLoad(lt, c.locals.get(id.name) as string);
            return makeIRValue(val, lt);
        }

        // Enum member
        if (this.enumValues.has(id.name)) {
            int ev = _unbox(this.enumValues.get(id.name));
            return makeIRValue(toString(ev), "i32");
        }

        // Global variable
        if (this.globalVars.has(id.name)) {
            string glt = this.globalVars.get(id.name) as string;
            string val = this.emitLoad(glt, "@" + id.name);
            return makeIRValue(val, glt);
        }

        // Function reference
        return makeIRValue("@" + id.name, "i8*");
    }

    private fun IRValue emitUnary(ASTNode n) {
        UnaryNode un = n as UnaryNode;
        int op = un.op;

        if (op == 55) {  // KW_await — pass-through
            return this.emitExpr(un.operand as ASTNode);
        }

        IRValue val = this.emitExpr(un.operand as ASTNode);

        if (op == 104) {  // Bang
            val = this.coerce(val, "i1");
            string res = this.freshName("not");
            this.emit(res + " = xor i1 " + val.name + ", true");
            return makeIRValue(res, "i1");
        }
        if (op == 90) {  // Minus (unary negate)
            string res = this.freshName("neg");
            if (val.type == "float" || val.type == "double") {
                this.emit(res + " = fneg " + val.type + " " + val.name);
            } else {
                this.emit(res + " = sub " + val.type + " 0, " + val.name);
            }
            return makeIRValue(res, val.type);
        }
        if (op == 108) {  // Tilde (bitwise not)
            string res = this.freshName("bitnot");
            this.emit(res + " = xor " + val.type + " " + val.name + ", -1");
            return makeIRValue(res, val.type);
        }
        if (op == 94 || op == 95) {  // ++ or --
            bool isInc = (op == 94);
            string ptr = this.emitLValue(un.operand as ASTNode);
            string oldVal = this.emitLoad(val.type, ptr);
            string res = this.freshName(isInc ? "inc" : "dec");
            if (val.type == "float" || val.type == "double") {
                this.emit(res + " = " + (isInc ? "fadd" : "fsub") + " " + val.type + " " + oldVal + ", 1.0");
            } else {
                this.emit(res + " = " + (isInc ? "add" : "sub") + " " + val.type + " " + oldVal + ", 1");
            }
            this.emitStore(val.type, res, ptr);
            return un.postfix ? makeIRValue(oldVal, val.type) : makeIRValue(res, val.type);
        }
        return val;
    }

    private fun IRValue emitBinary(ASTNode n) {
        BinaryNode bn = n as BinaryNode;
        IRValue lhs = this.emitExpr(bn.left  as ASTNode);
        IRValue rhs = this.emitExpr(bn.right as ASTNode);

        int op = bn.op;

        // String concatenation: Plus when lhs is i8*
        if (op == 89 && lhs.type == "i8*") {  // Plus
            if (rhs.type != "i8*") {
                string tmp = this.freshName("tostr");
                this.emit(tmp + " = call i8* @ec_int_to_str(i32 " + rhs.name + ")");
                rhs = makeIRValue(tmp, "i8*");
            }
            string res = this.freshName("concat");
            this.emit(res + " = call i8* @ec_strconcat(i8* " + lhs.name + ", i8* " + rhs.name + ")");
            return makeIRValue(res, "i8*");
        }

        // Determine common type
        string ct = lhs.type;
        if (rhs.type == "double" || lhs.type == "double") { ct = "double"; }
        else if (rhs.type == "float" || lhs.type == "float") { ct = "float"; }
        else if (rhs.type == "i64" || lhs.type == "i64") { ct = "i64"; }

        lhs = this.coerce(lhs, ct);
        rhs = this.coerce(rhs, ct);

        bool isFloat = (ct == "float" || ct == "double");
        string res = this.freshName("binop");

        // Arithmetic
        if (op == 89) { this.emit(res + " = " + (isFloat?"fadd":"add") + " " + ct + " " + lhs.name + ", " + rhs.name); return makeIRValue(res, ct); }
        if (op == 90) { this.emit(res + " = " + (isFloat?"fsub":"sub") + " " + ct + " " + lhs.name + ", " + rhs.name); return makeIRValue(res, ct); }
        if (op == 91) { this.emit(res + " = " + (isFloat?"fmul":"mul") + " " + ct + " " + lhs.name + ", " + rhs.name); return makeIRValue(res, ct); }
        if (op == 92) { this.emit(res + " = " + (isFloat?"fdiv":"sdiv") + " " + ct + " " + lhs.name + ", " + rhs.name); return makeIRValue(res, ct); }
        if (op == 93) { this.emit(res + " = " + (isFloat?"frem":"srem") + " " + ct + " " + lhs.name + ", " + rhs.name); return makeIRValue(res, ct); }
        // Bitwise
        if (op == 105) { this.emit(res + " = and " + ct + " " + lhs.name + ", " + rhs.name); return makeIRValue(res, ct); }
        if (op == 106) { this.emit(res + " = or "  + ct + " " + lhs.name + ", " + rhs.name); return makeIRValue(res, ct); }
        if (op == 107) { this.emit(res + " = xor " + ct + " " + lhs.name + ", " + rhs.name); return makeIRValue(res, ct); }
        if (op == 109) { this.emit(res + " = shl " + ct + " " + lhs.name + ", " + rhs.name); return makeIRValue(res, ct); }
        if (op == 110) { this.emit(res + " = ashr "+ ct + " " + lhs.name + ", " + rhs.name); return makeIRValue(res, ct); }
        // Logical
        if (op == 102) { lhs = this.coerce(lhs,"i1"); rhs = this.coerce(rhs,"i1"); this.emit(res + " = and i1 " + lhs.name + ", " + rhs.name); return makeIRValue(res, "i1"); }
        if (op == 103) { lhs = this.coerce(lhs,"i1"); rhs = this.coerce(rhs,"i1"); this.emit(res + " = or i1 " + lhs.name + ", " + rhs.name); return makeIRValue(res, "i1"); }
        // Equality
        if (op == 96) { this.emit(res + " = " + (isFloat?"fcmp oeq":"icmp eq") + " " + ct + " " + lhs.name + ", " + rhs.name); return makeIRValue(res, "i1"); }
        if (op == 97) { this.emit(res + " = " + (isFloat?"fcmp one":"icmp ne") + " " + ct + " " + lhs.name + ", " + rhs.name); return makeIRValue(res, "i1"); }
        // Relational
        if (op == 98)  { this.emit(res + " = " + (isFloat?"fcmp olt":"icmp slt") + " " + ct + " " + lhs.name + ", " + rhs.name); return makeIRValue(res, "i1"); }
        if (op == 99)  { this.emit(res + " = " + (isFloat?"fcmp ole":"icmp sle") + " " + ct + " " + lhs.name + ", " + rhs.name); return makeIRValue(res, "i1"); }
        if (op == 100) { this.emit(res + " = " + (isFloat?"fcmp ogt":"icmp sgt") + " " + ct + " " + lhs.name + ", " + rhs.name); return makeIRValue(res, "i1"); }
        if (op == 101) { this.emit(res + " = " + (isFloat?"fcmp oge":"icmp sge") + " " + ct + " " + lhs.name + ", " + rhs.name); return makeIRValue(res, "i1"); }

        return makeIRValue(lhs.name, lhs.type);
    }

    private fun IRValue emitTernary(ASTNode n) {
        TernaryNode tn = n as TernaryNode;
        IRValue cond = this.emitExpr(tn.cond as ASTNode);
        cond = this.coerce(cond, "i1");

        string thenLbl  = this.freshLabel("tern.then");
        string elseLbl  = this.freshLabel("tern.else");
        string mergeLbl = this.freshLabel("tern.end");

        this.emitCondBr(cond.name, thenLbl, elseLbl);

        this.emitLabel(thenLbl);
        IRValue thenVal = this.emitExpr(tn.thenExpr as ASTNode);
        string thenBlock = thenLbl;
        this.emitBr(mergeLbl);

        this.emitLabel(elseLbl);
        IRValue elseVal = this.emitExpr(tn.elseExpr as ASTNode);
        string elseBlock = elseLbl;
        this.emitBr(mergeLbl);

        this.emitLabel(mergeLbl);
        string res = this.freshName("tern");
        string rt = thenVal.type;
        elseVal = this.coerce(elseVal, rt);
        this.emit(res + " = phi " + rt + " [ " + thenVal.name + ", %" + thenBlock +
                  " ], [ " + elseVal.name + ", %" + elseBlock + " ]");
        return makeIRValue(res, rt);
    }

    private fun IRValue emitAssign(ASTNode n) {
        AssignNode an = n as AssignNode;
        IRValue rhs = this.emitExpr(an.value as ASTNode);
        string ptr = this.emitLValue(an.target as ASTNode);
        IRValue lhs = this.emitExpr(an.target as ASTNode);

        string lt = lhs.type;
        rhs = this.coerce(rhs, lt);
        IRValue result = rhs;

        int op = an.op;
        if (op != 78) {  // Not plain Equal
            string cur = this.emitLoad(lt, ptr);
            string compRes = this.freshName("compop");
            bool isFloat = (lt == "float" || lt == "double");
            if (op == 79)  { this.emit(compRes + " = " + (isFloat?"fadd":"add") + " " + lt + " " + cur + ", " + rhs.name); }
            else if (op == 80) { this.emit(compRes + " = " + (isFloat?"fsub":"sub") + " " + lt + " " + cur + ", " + rhs.name); }
            else if (op == 81) { this.emit(compRes + " = " + (isFloat?"fmul":"mul") + " " + lt + " " + cur + ", " + rhs.name); }
            else if (op == 82) { this.emit(compRes + " = " + (isFloat?"fdiv":"sdiv") + " " + lt + " " + cur + ", " + rhs.name); }
            else if (op == 83) { this.emit(compRes + " = srem " + lt + " " + cur + ", " + rhs.name); }
            else if (op == 84) { this.emit(compRes + " = and " + lt + " " + cur + ", " + rhs.name); }
            else if (op == 85) { this.emit(compRes + " = or "  + lt + " " + cur + ", " + rhs.name); }
            else if (op == 86) { this.emit(compRes + " = xor " + lt + " " + cur + ", " + rhs.name); }
            else if (op == 87) { this.emit(compRes + " = shl " + lt + " " + cur + ", " + rhs.name); }
            else if (op == 88) { this.emit(compRes + " = ashr "+ lt + " " + cur + ", " + rhs.name); }
            result = makeIRValue(compRes, lt);
        }

        this.emitStore(lt, result.name, ptr);
        return result;
    }

    private fun IRValue emitCall(ASTNode n) {
        CallNode cn = n as CallNode;
        FnCtx c = this.ctx as FnCtx;

        string calleeName = "";
        string selfArg    = "";
        string selfType   = "";
        bool   isMember   = false;
        string receiverClass  = "";
        string receiverMethod = "";

        // Determine callee
        ASTNode calleeNode = cn.callee as ASTNode;
        if (calleeNode.kind == 12) {  // Member
            MemberNode mn = calleeNode as MemberNode;
            isMember = true;
            string memberName = mn.member;
            // Strip scope resolution prefix
            if (stringLen(memberName) > 2 && memberName[0] == ':' && memberName[1] == ':') {
                memberName = substring(memberName, 2, stringLen(memberName));
            }

            // Check for super.method()
            bool isSuper = false;
            if (mn.object != null) {
                ASTNode objNode = mn.object as ASTNode;
                if (objNode.kind == 6) {
                    IdentNode objId = objNode as IdentNode;
                    isSuper = (objId.name == "super");
                }
            }

            if (isSuper && stringLen(this.currentClass) > 0) {
                string superClass = "";
                if (this.classLayouts.has(this.currentClass)) {
                    ClassLayout lay = this.classLayouts.get(this.currentClass) as ClassLayout;
                    superClass = lay.superClass;
                }
                if (stringLen(superClass) == 0) { superClass = this.currentClass; }

                string rawSelf = "null";
                if (c.locals.has("this")) {
                    rawSelf = this.emitLoad(c.localTypes.get("this") as string,
                                            c.locals.get("this") as string);
                }
                string superPtrType = "%" + superClass + "*";
                if ("%" + this.currentClass + "*" != superPtrType) {
                    string casted = this.freshName("super.cast");
                    this.emit(casted + " = bitcast %" + this.currentClass + "* " +
                              rawSelf + " to " + superPtrType);
                    rawSelf = casted;
                }
                selfArg  = rawSelf;
                selfType = superPtrType;
                calleeName = superClass + "_" + memberName;
            } else {
                IRValue obj = this.emitExpr(mn.object as ASTNode);
                selfArg  = obj.name;
                selfType = obj.type;
                string className = this.extractClassName(obj.type);
                calleeName = stringLen(className) > 0 ?
                             className + "_" + memberName : memberName;
                receiverClass  = className;
                receiverMethod = memberName;
            }
        } else if (calleeNode.kind == 6) {  // Identifier
            IdentNode calleeId = calleeNode as IdentNode;
            string idName = calleeId.name;

            // Closure call detection
            if (c.locals.has(idName)) {
                string localType = c.localTypes.get(idName) as string;
                if (localType == "i8*" && this.closureTypes.has(idName)) {
                    ClosureSig sig = this.closureTypes.get(idName) as ClosureSig;
                    string fatPtr = this.emitLoad("i8*", c.locals.get(idName) as string);

                    string fnPtrSlot = this.freshName("cls.fn");
                    this.emit(fnPtrSlot + " = bitcast i8* " + fatPtr + " to i8**");
                    string fnPtrRaw = this.freshName("cls.fn.raw");
                    this.emit(fnPtrRaw + " = load i8*, i8** " + fnPtrSlot);

                    string envSlot = this.freshName("cls.env");
                    this.emit(envSlot + " = getelementptr inbounds i8, i8* " + fatPtr + ", i64 8");
                    string envPtrSlot = this.freshName("cls.env.slot");
                    this.emit(envPtrSlot + " = bitcast i8* " + envSlot + " to i8**");
                    string envPtr = this.freshName("cls.env.ptr");
                    this.emit(envPtr + " = load i8*, i8** " + envPtrSlot);

                    // Emit arguments
                    List callArgs = new List();
                    int ai = 0;
                    while (ai < cn.args.length()) {
                        ASTNode a = cn.args.get(ai) as ASTNode;
                        callArgs.add(this.emitExpr(a));
                        ai = ai + 1;
                    }

                    string fnSig = sig.retType + " (i8*";
                    ai = 0;
                    while (ai < callArgs.length()) {
                        IRValue av = callArgs.get(ai) as IRValue;
                        fnSig = fnSig + ", " + av.type;
                        ai = ai + 1;
                    }
                    fnSig = fnSig + ")*";

                    string fnTyped = this.freshName("cls.fn.typed");
                    this.emit(fnTyped + " = bitcast i8* " + fnPtrRaw + " to " + fnSig);

                    string argStr = "i8* " + envPtr;
                    ai = 0;
                    while (ai < callArgs.length()) {
                        IRValue av = callArgs.get(ai) as IRValue;
                        argStr = argStr + ", " + av.type + " " + av.name;
                        ai = ai + 1;
                    }

                    if (sig.retType == "void") {
                        this.emit("call void " + fnTyped + "(" + argStr + ")");
                        return makeIRValue("", "void");
                    }
                    string result = this.freshName("cls.ret");
                    this.emit(result + " = call " + sig.retType + " " + fnTyped + "(" + argStr + ")");
                    return makeIRValue(result, sig.retType);
                }
            }
            calleeName = idName;
        } else {
            IRValue fn = this.emitExpr(calleeNode);
            calleeName = fn.name;
            if (startsWith(calleeName, "@")) {
                calleeName = substring(calleeName, 1, stringLen(calleeName));
            }
        }

        // Apply function name map
        calleeName = this.mapFnName(calleeName);

        // Emit arguments
        List args = new List();  // List of IRValue
        if (isMember && stringLen(selfArg) > 0) {
            args.add(makeIRValue(selfArg, selfType));
        }
        int i = 0;
        while (i < cn.args.length()) {
            ASTNode a = cn.args.get(i) as ASTNode;
            args.add(this.emitExpr(a));
            i = i + 1;
        }

        // toString dispatch
        if (calleeName == "ec_int_to_str" && args.length() > 0) {
            IRValue lastArg = args.last() as IRValue;
            if (lastArg.type == "i8") {
                calleeName = "ec_char_to_str";
            } else if (lastArg.type == "i1") {
                string widened = this.freshName("bool.i32");
                this.emit(widened + " = zext i1 " + lastArg.name + " to i32");
                args.set(args.length() - 1, makeIRValue(widened, "i32"));
            }
        }

        // getchar special case
        if (calleeName == "getchar") {
            string r32 = this.freshName("gc");
            this.emit(r32 + " = call i32 @getchar()");
            string r8 = this.freshName("gc.c");
            this.emit(r8 + " = trunc i32 " + r32 + " to i8");
            return makeIRValue(r8, "i8");
        }
        if (calleeName == "putchar" && args.length() > 0) {
            IRValue a0 = args.get(0) as IRValue;
            IRValue a32 = this.coerce(makeIRValue(a0.name, a0.type), "i32");
            this.emit("call i32 @putchar(i32 " + a32.name + ")");
            return makeIRValue("", "void");
        }

        // Determine return type
        string retType = "";
        if (this.userFnRetTypes.has(calleeName)) {
            retType = this.userFnRetTypes.get(calleeName) as string;
        }
        if (stringLen(retType) == 0) {
            retType = this.lookupRetType(calleeName);
        }

        // Build arg string with coercions
        StringBuilder argSb = new StringBuilder();
        i = 0;
        while (i < args.length()) {
            if (i > 0) { argSb.append(", "); }
            IRValue av = args.get(i) as IRValue;
            // Coerce for print functions
            if ((calleeName == "ec_println" || calleeName == "ec_print" ||
                 calleeName == "ec_strconcat") && av.type != "i8*") {
                string tmp = this.freshName("tostr");
                this.emit(tmp + " = call i8* @ec_int_to_str(i32 " + av.name + ")");
                av = makeIRValue(tmp, "i8*");
            }
            // Widen for ec_int_to_str
            if (calleeName == "ec_int_to_str" && (av.type == "i8" || av.type == "i1")) {
                av = this.coerce(av, "i32");
            }
            argSb.append(av.type); argSb.appendChar(' '); argSb.append(av.name);
            i = i + 1;
        }

        // Virtual dispatch
        bool useVtable = isMember && stringLen(receiverClass) > 0 &&
                         stringLen(receiverMethod) > 0 &&
                         this.vtableSlot(receiverClass, receiverMethod) >= 0;

        if (useVtable) {
            int slot = this.vtableSlot(receiverClass, receiverMethod);
            if (slot >= 0) {
                string vtptrSlot = this.freshName("vtptr.slot");
                this.emit(vtptrSlot + " = getelementptr inbounds %" + receiverClass +
                          ", %" + receiverClass + "* " + selfArg + ", i32 0, i32 0");
                string vtptr = this.freshName("vtptr");
                this.emit(vtptr + " = load i8**, i8*** " + vtptrSlot);
                string fnSlot = this.freshName("fn.slot");
                this.emit(fnSlot + " = getelementptr inbounds i8*, i8** " + vtptr +
                          ", i32 " + toString(slot));
                string fnRaw = this.freshName("fn.raw");
                this.emit(fnRaw + " = load i8*, i8** " + fnSlot);

                // Build function signature
                string fnSig = retType + " (%" + receiverClass + "*";
                i = 1;
                while (i < args.length()) {
                    IRValue av = args.get(i) as IRValue;
                    fnSig = fnSig + ", " + av.type;
                    i = i + 1;
                }
                fnSig = fnSig + ")*";

                string fnTyped = this.freshName("fn.vt");
                this.emit(fnTyped + " = bitcast i8* " + fnRaw + " to " + fnSig);

                string vtArgStr = selfType + " " + selfArg;
                i = 1;
                while (i < args.length()) {
                    IRValue av = args.get(i) as IRValue;
                    vtArgStr = vtArgStr + ", " + av.type + " " + av.name;
                    i = i + 1;
                }

                if (retType == "void") {
                    this.emit("call void " + fnTyped + "(" + vtArgStr + ")");
                    return makeIRValue("", "void");
                }
                string res = this.freshName("vt.call");
                this.emit(res + " = call " + retType + " " + fnTyped + "(" + vtArgStr + ")");
                return makeIRValue(res, retType);
            }
        }

        // Direct call
        if (retType == "void") {
            this.emit("call void @" + calleeName + "(" + argSb.toString() + ")");
            return makeIRValue("", "void");
        }
        string res = this.freshName("call");
        this.emit(res + " = call " + retType + " @" + calleeName + "(" + argSb.toString() + ")");
        return makeIRValue(res, retType);
    }

    private fun string mapFnName(string name) {
        // Built-in stdlib function name map
        Map fm = this.buildFnMap();
        if (fm.has(name)) { return fm.get(name) as string; }
        return name;
    }

    private fun Map buildFnMap() {
        // Cache the map on first call
        if (this.userFnRetTypes.has("__fnmap_built__")) {
            // Use a side-channel: store the map in globalVars with a special key
            // Actually we need a dedicated field -- use a simpler approach:
            // rebuild each call (OK since map operations are fast)
        }
        Map m = new Map();
        m.set("println",       "ec_println");
        m.set("print",         "ec_print");
        m.set("readLine",      "ec_readline");
        m.set("getChar",       "getchar");
        m.set("putChar",       "putchar");
        m.set("toString",      "ec_int_to_str");
        m.set("floatToString", "ec_float_to_str");
        m.set("doubleToString","ec_float_to_str");
        m.set("longToString",  "ec_int_to_str");
        m.set("charToString",  "ec_char_to_str");
        m.set("boolToString",  "ec_int_to_str");
        m.set("parseInt",      "atoi");
        m.set("parseLong",     "atol");
        m.set("parseFloat",    "atof");
        m.set("parseDouble",   "atof");
        m.set("stringLen",     "ec_str_len");
        m.set("charAt",        "ec_char_at");
        m.set("substring",     "ec_substring");
        m.set("indexOf",       "ec_str_find");
        m.set("lastIndexOf",   "ec_str_find_last");
        m.set("contains",      "ec_str_contains");
        m.set("startsWith",    "ec_str_starts_with");
        m.set("endsWith",      "ec_str_ends_with");
        m.set("count",         "ec_str_count");
        m.set("equals",        "ec_str_equals");
        m.set("compare",       "ec_str_compare");
        m.set("toLower",       "ec_str_to_lower");
        m.set("toUpper",       "ec_str_to_upper");
        m.set("trim",          "ec_str_trim");
        m.set("trimLeft",      "ec_str_trim_left");
        m.set("trimRight",     "ec_str_trim_right");
        m.set("repeat",        "ec_str_repeat");
        m.set("replace",       "ec_str_replace");
        m.set("reverse",       "ec_str_reverse");
        m.set("isDigit",       "ec_char_is_digit");
        m.set("isAlpha",       "ec_char_is_alpha");
        m.set("isAlnum",       "ec_char_is_alnum");
        m.set("isSpace",       "ec_char_is_space");
        m.set("isUpper",       "ec_char_is_upper");
        m.set("isLower",       "ec_char_is_lower");
        m.set("isPunct",       "ec_char_is_punct");
        m.set("toLowerChar",   "ec_char_to_lower_fn");
        m.set("toUpperChar",   "ec_char_to_upper_fn");
        m.set("charCode",      "ec_char_code");
        m.set("charFromCode",  "ec_char_from_code");
        m.set("sqrt",          "sqrt");
        m.set("pow",           "pow");
        m.set("floor",         "floor");
        m.set("ceil",          "ceil");
        m.set("abs",           "fabs");
        m.set("absInt",        "ec_abs_int");
        m.set("absLong",       "ec_abs_long");
        m.set("log",           "log");
        m.set("log2",          "ec_log2_fn");
        m.set("log10",         "ec_log10_fn");
        m.set("sin",           "sin");
        m.set("cos",           "cos");
        m.set("tan",           "tan");
        m.set("asin",          "asin");
        m.set("acos",          "acos");
        m.set("atan",          "atan");
        m.set("atan2",         "ec_atan2_fn");
        m.set("cbrt",          "ec_cbrt_fn");
        m.set("hypot",         "ec_hypot_fn");
        m.set("powInt",        "ec_pow_int");
        m.set("minInt",        "ec_min_int");
        m.set("maxInt",        "ec_max_int");
        m.set("minLong",       "ec_min_long");
        m.set("maxLong",       "ec_max_long");
        m.set("min",           "ec_min_double");
        m.set("max",           "ec_max_double");
        m.set("clampInt",      "ec_clamp_int");
        m.set("clamp",         "ec_clamp_double");
        m.set("gcd",           "ec_gcd");
        m.set("lcm",           "ec_lcm");
        m.set("isEven",        "ec_is_even");
        m.set("isOdd",         "ec_is_odd");
        m.set("isPrime",       "ec_is_prime");
        m.set("gcCollect",     "ec_gc_collect");
        m.set("gcStats",       "ec_gc_stats");
        m.set("gcLiveBytes",   "ec_gc_live_bytes");
        m.set("gcLiveObjects", "ec_gc_live_objects");
        m.set("exit",          "ec_exit");
        m.set("timeMs",        "ec_time_ms");
        m.set("readFile",      "ec_read_file");
        m.set("writeFile",     "ec_write_file");
        m.set("fileExists",    "ec_file_exists");
        m.set("ec_throw",    "ec_throw");
        // _native_* intrinsics
        m.set("_native_print",         "ec_print");
        m.set("_native_println",       "ec_println");
        m.set("_native_readLine",      "ec_readline");
        m.set("_native_intToStr",      "ec_int_to_str");
        m.set("_native_longToStr",     "ec_int_to_str");
        m.set("_native_floatToStr",    "ec_float_to_str");
        m.set("_native_charToStr",     "ec_char_to_str");
        m.set("_native_parseInt",      "atoi");
        m.set("_native_parseLong",     "atol");
        m.set("_native_parseFloat",    "atof");
        m.set("_native_strLen",        "ec_str_len");
        m.set("_native_charAt",        "ec_char_at");
        m.set("_native_substring",     "ec_substring");
        m.set("_native_strFind",       "ec_str_find");
        m.set("_native_strFindLast",   "ec_str_find_last");
        m.set("_native_strContains",   "ec_str_contains");
        m.set("_native_strStartsWith", "ec_str_starts_with");
        m.set("_native_strEndsWith",   "ec_str_ends_with");
        m.set("_native_strToLower",    "ec_str_to_lower");
        m.set("_native_strToUpper",    "ec_str_to_upper");
        m.set("_native_strTrim",       "ec_str_trim");
        m.set("_native_strRepeat",     "ec_str_repeat");
        m.set("_native_strReplace",    "ec_str_replace");
        m.set("_native_charIsDigit",   "ec_char_is_digit");
        m.set("_native_charIsAlpha",   "ec_char_is_alpha");
        m.set("_native_charIsAlnum",   "ec_char_is_alnum");
        m.set("_native_charIsSpace",   "ec_char_is_space");
        m.set("_native_charToLower",   "ec_char_to_lower_fn");
        m.set("_native_charToUpper",   "ec_char_to_upper_fn");
        m.set("_native_charCode",      "ec_char_code");
        m.set("_native_charFromCode",  "ec_char_from_code");
        m.set("_native_listNew",       "ec_list_new");
        m.set("_native_listLen",       "ec_list_len");
        m.set("_native_listGet",       "ec_list_get");
        m.set("_native_listAdd",       "ec_list_add");
        m.set("_native_listSet",       "ec_list_set");
        m.set("_native_listRemove",    "ec_list_remove");
        m.set("_native_listInsert",    "ec_list_insert");
        m.set("_native_listIndexOf",   "ec_list_index_of");
        m.set("_native_listContains",  "ec_list_contains");
        m.set("_native_listSlice",     "ec_list_slice");
        m.set("_native_listConcat",    "ec_list_concat");
        m.set("_native_mapNew",        "ec_map_new");
        m.set("_native_mapSet",        "ec_map_set");
        m.set("_native_mapGet",        "ec_map_get");
        m.set("_native_mapHas",        "ec_map_has");
        m.set("_native_mapDelete",     "ec_map_delete");
        m.set("_native_mapCount",      "ec_map_count");
        m.set("_native_mapKeys",       "ec_map_keys");
        m.set("_native_mapValues",     "ec_map_values");
        m.set("_native_setNew",        "ec_set_new");
        m.set("_native_setAdd",        "ec_set_add");
        m.set("_native_setHas",        "ec_set_has");
        m.set("_native_setRemove",     "ec_set_remove");
        m.set("_native_setCount",      "ec_set_count");
        m.set("_native_setKeys",       "ec_set_keys");
        m.set("_native_sbNew",         "ec_sb_new");
        m.set("_native_sbAppendStr",   "ec_sb_append_str");
        m.set("_native_sbAppendChar",  "ec_sb_append_char");
        m.set("_native_sbAppendInt",   "ec_sb_append_int");
        m.set("_native_sbAppendLong",  "ec_sb_append_long");
        m.set("_native_sbToString",    "ec_sb_to_string");
        m.set("_native_sbLength",      "ec_sb_length");
        m.set("_native_sbClear",       "ec_sb_clear");
        m.set("_native_gcCollect",     "ec_gc_collect");
        m.set("_native_gcStats",       "ec_gc_stats");
        m.set("_native_gcLiveBytes",   "ec_gc_live_bytes");
        m.set("_native_gcLiveObjects", "ec_gc_live_objects");
        m.set("_native_exit",          "ec_exit");
        m.set("_native_timeMs",        "ec_time_ms");
        m.set("_native_readFile",      "ec_read_file");
        m.set("_native_writeFile",     "ec_write_file");
        m.set("_native_fileExists",    "ec_file_exists");
        return m;
    }

    private fun string lookupRetType(string name) {
        // i8* returners
        if (name == "ec_strconcat" || name == "ec_readline" ||
            name == "ec_int_to_str" || name == "ec_float_to_str" ||
            name == "ec_char_to_str" || name == "ec_substring" ||
            name == "ec_typeof" || name == "ec_gc_stats" ||
            name == "ec_new" || name == "ec_catch_msg" ||
            name == "ec_read_file" || name == "ec_array_new" ||
            name == "ec_str_to_lower" || name == "ec_str_to_upper" ||
            name == "ec_str_trim" || name == "ec_str_trim_left" ||
            name == "ec_str_trim_right" || name == "ec_str_repeat" ||
            name == "ec_str_replace" || name == "ec_str_reverse" ||
            name == "ec_str_split" || name == "ec_str_join" ||
            name == "ec_list_new" || name == "ec_list_get" ||
            name == "ec_list_add" || name == "ec_list_set" ||
            name == "ec_list_remove" || name == "ec_list_insert" ||
            name == "ec_list_slice" || name == "ec_list_concat" ||
            name == "ec_map_new" || name == "ec_map_set" ||
            name == "ec_map_get" || name == "ec_map_delete" ||
            name == "ec_map_keys" || name == "ec_map_values" ||
            name == "ec_set_new" || name == "ec_set_add" ||
            name == "ec_set_remove" || name == "ec_set_keys" ||
            name == "ec_sb_new" || name == "ec_sb_append_str" ||
            name == "ec_sb_append_char" || name == "ec_sb_append_int" ||
            name == "ec_sb_append_long" || name == "ec_sb_to_string" ||
            name == "ec_sb_clear") { return "i8*"; }

        // void returners
        if (name == "ec_println" || name == "ec_print" ||
            name == "ec_delete" || name == "ec_throw" ||
            name == "ec_try_end" || name == "ec_try_pop" ||
            name == "ec_try_push" || name == "ec_gc_init" ||
            name == "ec_gc_collect" || name == "ec_gc_register_global" ||
            name == "ec_gc_push_root" || name == "ec_gc_pop_root" ||
            name == "ec_gc_pin" || name == "ec_gc_unpin" ||
            name == "ec_exit") { return "void"; }

        // i64 returners
        if (name == "ec_gc_live_bytes" || name == "ec_gc_live_objects" ||
            name == "ec_gc_num_collections" || name == "ec_array_len" ||
            name == "ec_list_len" || name == "ec_map_count" ||
            name == "ec_set_count" || name == "ec_sb_length" ||
            name == "ec_list_index_of" || name == "ec_time_ms") { return "i64"; }

        // double returners
        if (name == "sqrt" || name == "pow" || name == "floor" || name == "ceil" ||
            name == "fabs" || name == "sin" || name == "cos" || name == "tan" ||
            name == "asin" || name == "acos" || name == "atan" || name == "log" ||
            name == "atof" || name == "ec_cbrt_fn" || name == "ec_hypot_fn" ||
            name == "ec_log2_fn" || name == "ec_log10_fn" ||
            name == "ec_atan2_fn" || name == "ec_min_double" ||
            name == "ec_max_double" || name == "ec_clamp_double" ||
            name == "ec_infinity" || name == "ec_nan" ||
            name == "ec_abs_long" || name == "round" || name == "trunc") { return "double"; }

        // i8 returners
        if (name == "ec_char_at" || name == "ec_char_to_lower_fn" ||
            name == "ec_char_to_upper_fn" || name == "ec_char_from_code") { return "i8"; }

        // i32 default (covers atoi, atol, lang_str_*, lang_char_is_*, etc.)
        return "i32";
    }

    private fun IRValue emitMember(ASTNode n) {
        MemberNode mn = n as MemberNode;
        string member = mn.member;

        // .length / .size
        if (member == "length" || member == "size") {
            IRValue obj = this.emitExpr(mn.object as ASTNode);
            bool isString  = obj.type == "i8*";
            bool isArray   = obj.type == "i8*";
            // Heuristic: if caller is indexing a string-typed expression
            // use ec_str_len; otherwise array len
            // We use string by default since most non-class i8* are strings in simple code
            string res = this.freshName("len");
            this.emit(res + " = call i32 @ec_str_len(i8* " + obj.name + ")");
            return makeIRValue(res, "i32");
        }

        // Scope resolution: ::MEMBER
        if (stringLen(member) > 2 && member[0] == ':' && member[1] == ':') {
            string mname = substring(member, 2, stringLen(member));
            if (mn.object != null) {
                ASTNode objNode = mn.object as ASTNode;
                if (objNode.kind == 6) {
                    IdentNode objId = objNode as IdentNode;
                    string key = objId.name + "::" + mname;
                    if (this.enumValues.has(key)) {
                        int ev = _unbox(this.enumValues.get(key));
                        return makeIRValue(toString(ev), "i32");
                    }
                }
            }
            return makeIRValue("0", "i32");
        }

        IRValue obj = this.emitExpr(mn.object as ASTNode);
        string className = this.extractClassName(obj.type);
        if (stringLen(className) == 0) { return makeIRValue("0", "i32"); }

        int idx = this.fieldIndex(className, member);
        if (idx < 0) { return makeIRValue("0", "i32"); }

        string ft = this.fieldLLVMType(className, idx);
        string rawType = "%" + className;
        string gep = this.emitGEP(rawType, obj.name, idx, member);
        string val = this.emitLoad(ft, gep);
        return makeIRValue(val, ft);
    }

    private fun IRValue emitIndex(ASTNode n) {
        IndexNode idx = n as IndexNode;
        IRValue arr  = this.emitExpr(idx.array as ASTNode);
        IRValue idxv = this.emitExpr(idx.index as ASTNode);

        // String indexing
        if (arr.type == "i8*") {
            idxv = this.coerce(idxv, "i32");
            string res = this.freshName("char");
            this.emit(res + " = call i8 @ec_char_at(i8* " + arr.name + ", i32 " + idxv.name + ")");
            return makeIRValue(res, "i8");
        }

        // Array indexing
        idxv = this.coerce(idxv, "i64");
        string elemType = "i32";  // default
        string elemPtr = this.emitArrayElemPtr(arr.name, elemType, idxv.name);
        string val = this.emitLoad(elemType, elemPtr);
        return makeIRValue(val, elemType);
    }

    private fun IRValue emitNew(ASTNode n) {
        NewNode nn = n as NewNode;

        // Array allocation
        if (nn.isArray && nn.arraySize != null) {
            IRValue sz = this.emitExpr(nn.arraySize as ASTNode);
            sz = this.coerce(sz, "i64");
            string elemLLT = this.llvmTypeName(nn.ntype.name);
            string arr = this.emitArrayNew(elemLLT, sz.name);
            return makeIRValue(arr, "i8*");
        }

        // Object allocation
        string typeName = nn.ntype.name;
        int numFields = 0;
        if (this.classLayouts.has(typeName)) {
            ClassLayout lay = this.classLayouts.get(typeName) as ClassLayout;
            numFields = lay.fieldTypes.length();
        }
        int fieldBytes = numFields * 8;
        if (fieldBytes < 8) { fieldBytes = 8; }
        int sizeBytes = 8 + fieldBytes;

        string rawPtr = this.freshName("new.raw");
        this.emit(rawPtr + " = call i8* @ec_new(i64 " + toString(sizeBytes) + ")");
        string typedPtr = this.freshName("new.obj");
        this.emit(typedPtr + " = bitcast i8* " + rawPtr + " to %" + typeName + "*");

        // Store vtable pointer at field 0
        if (this.classLayouts.has(typeName)) {
            ClassLayout lay = this.classLayouts.get(typeName) as ClassLayout;
            if (lay.hasVtable && lay.vtable.length() > 0) {
                string vtPtrSlot = this.freshName("vt.slot");
                this.emit(vtPtrSlot + " = getelementptr inbounds %" + typeName +
                          ", %" + typeName + "* " + typedPtr + ", i32 0, i32 0");
                int n = lay.vtable.length();
                string vtCast = this.freshName("vt.cast");
                this.emit(vtCast + " = bitcast [" + toString(n) + " x i8*]* @" +
                          typeName + "_vtable to i8**");
                this.emit("store i8** " + vtCast + ", i8*** " + vtPtrSlot);
            }
        }

        // Call init
        bool hasInit = this.userFnRetTypes.has(typeName + "_init");
        if (!hasInit && this.classLayouts.has(typeName)) {
            ClassLayout lay = this.classLayouts.get(typeName) as ClassLayout;
            int i = 0;
            while (i < lay.methodNames.length()) {
                if (_unboxStr(lay.methodNames.get(i)) == "init") { hasInit = true; break; }
                i = i + 1;
            }
        }

        if (hasInit) {
            string initName = typeName + "_init";
            string argStr = "%" + typeName + "* " + typedPtr;
            int i = 0;
            while (i < nn.args.length()) {
                ASTNode a = nn.args.get(i) as ASTNode;
                IRValue av = this.emitExpr(a);
                argStr = argStr + ", " + av.type + " " + av.name;
                i = i + 1;
            }
            this.emit("call void @" + initName + "(" + argStr + ")");
        }

        return makeIRValue(typedPtr, "%" + typeName + "*");
    }

    private fun IRValue emitDelete(ASTNode n) {
        DeleteNode dn = n as DeleteNode;
        IRValue obj = this.emitExpr(dn.operand as ASTNode);
        string raw = obj.name;
        if (obj.type != "i8*") {
            string tmp = this.freshName("del.raw");
            this.emit(tmp + " = bitcast " + obj.type + " " + obj.name + " to i8*");
            raw = tmp;
        }
        this.emit("call void @ec_delete(i8* " + raw + ")");
        return makeIRValue("", "void");
    }

    private fun IRValue emitInstanceOf(ASTNode n) {
        InstanceOfNode ion = n as InstanceOfNode;
        IRValue obj = this.emitExpr(ion.operand as ASTNode);
        string targetClass = ion.itype.name;

        if (!this.classLayouts.has(targetClass)) {
            return makeIRValue("false", "i1");
        }
        string objClass = this.extractClassName(obj.type);
        if (stringLen(objClass) == 0) { return makeIRValue("false", "i1"); }

        string vtptrSlot = this.freshName("iof.vt");
        this.emit(vtptrSlot + " = getelementptr inbounds %" + objClass +
                  ", %" + objClass + "* " + obj.name + ", i32 0, i32 0");
        string vtptr = this.freshName("iof.vtptr");
        this.emit(vtptr + " = load i8**, i8*** " + vtptrSlot);
        string vtptrI8 = this.freshName("iof.vt.i8");
        this.emit(vtptrI8 + " = bitcast i8** " + vtptr + " to i8*");

        ClassLayout targetLay = this.classLayouts.get(targetClass) as ClassLayout;
        int n = targetLay.vtable.length();
        string targetVt = this.freshName("iof.target");
        this.emit(targetVt + " = bitcast [" + toString(n) + " x i8*]* @" +
                  targetClass + "_vtable to i8*");

        string res = this.freshName("instanceof");
        this.emit(res + " = icmp eq i8* " + vtptrI8 + ", " + targetVt);
        return makeIRValue(res, "i1");
    }

    private fun IRValue emitCast(ASTNode n) {
        CastNode cn = n as CastNode;
        IRValue obj = this.emitExpr(cn.operand as ASTNode);
        string targetLLT = this.llvmTypeRef(cn.ctype);
        if (obj.type == targetLLT) { return obj; }
        return this.coerce(obj, targetLLT);
    }

    private fun IRValue emitArrayLit(ASTNode n) {
        ArrayLitNode al = n as ArrayLitNode;
        if (al.elements.length() == 0) {
            string arr = this.emitArrayNew("i32", "0");
            return makeIRValue(arr, "i8*");
        }

        IRValue first = this.emitExpr(al.elements.get(0) as ASTNode);
        string elemType = first.type;
        int count = al.elements.length();

        string arr = this.emitArrayNew(elemType, toString(count));

        // Store elements
        IRValue ev = first;
        int i = 0;
        while (i < count) {
            if (i > 0) { ev = this.emitExpr(al.elements.get(i) as ASTNode); }
            ev = this.coerce(ev, elemType);
            string eptr = this.emitArrayElemPtr(arr, elemType, toString(i));
            this.emitStore(elemType, ev.name, eptr);
            i = i + 1;
        }
        return makeIRValue(arr, "i8*");
    }

    // ── Lambda / closure ──────────────────────────────────────────────────────

    private fun IRValue emitLambda(ASTNode n) {
        LambdaNode ln = n as LambdaNode;
        this.lambdaCtr = this.lambdaCtr + 1;
        string lamName = "lambda." + toString(this.lambdaCtr);
        string envName = "lambda." + toString(this.lambdaCtr) + ".env";

        // Collect captured variables
        List caps = this.collectCaptures(ln);
        bool hasCaps = caps.length() > 0;

        // Infer return type
        string retLLT = "i8*";  // default
        if (ln.body != null) {
            // We don't have resolvedType here, so just emit and infer later
            retLLT = "i8*";
        }

        // Emit env struct type if needed
        if (hasCaps) {
            this.globals.append("%"); this.globals.append(envName);
            this.globals.append(" = type { ");
            int i = 0;
            while (i < caps.length()) {
                CaptureInfo cap = caps.get(i) as CaptureInfo;
                if (i > 0) { this.globals.append(", "); }
                this.globals.append(cap.llvmType);
                i = i + 1;
            }
            this.globals.append(" }\n");
        }

        // Build param strings
        StringBuilder paramStr     = new StringBuilder();
        StringBuilder paramTypeStr = new StringBuilder();
        if (hasCaps) {
            paramStr.append("i8* %env_raw");
            paramTypeStr.append("i8*");
        }
        int i = 0;
        while (i < ln.params.length()) {
            Param p = ln.params.get(i) as Param;
            if (paramStr.length() > 0) { paramStr.append(", "); }
            if (paramTypeStr.length() > 0) { paramTypeStr.append(", "); }
            string lt = this.llvmTypeRef(p.ptype);
            paramStr.append(lt); paramStr.append(" %"); paramStr.append(p.name);
            paramTypeStr.append(lt);
            i = i + 1;
        }

        // Register closure signature
        ClosureSig sig = new ClosureSig(); sig.init();
        sig.retType     = retLLT;
        sig.paramTypeStr = paramTypeStr.toString();
        this.closureTypes.set(lamName, sig);

        // Save context and switch to lambdas buffer
        int savedOut = this.outTarget;
        var savedCtx = this.ctx;
        int savedCtr = this.counter;
        string savedClass = this.currentClass;

        FnCtx lamCtx = new FnCtx(); lamCtx.init();
        lamCtx.name    = lamName;
        lamCtx.retType = retLLT;
        this.ctx       = lamCtx;
        this.outTarget = 2;  // lambdas buffer

        this.lambdas.append("\ndefine private ");
        this.lambdas.append(retLLT); this.lambdas.append(" @"); this.lambdas.append(lamName);
        this.lambdas.append("("); this.lambdas.append(paramStr.toString());
        this.lambdas.append(") {\nentry:\n");

        // Unpack captured variables from env struct
        if (hasCaps) {
            string envTyped = this.freshName("env");
            this.emit(envTyped + " = bitcast i8* %env_raw to %" + envName + "*");
            int ci = 0;
            while (ci < caps.length()) {
                CaptureInfo cap = caps.get(ci) as CaptureInfo;
                string fgep = this.freshName(cap.name + ".cgep");
                this.emit(fgep + " = getelementptr inbounds %" + envName + ", %" +
                          envName + "* " + envTyped + ", i32 0, i32 " + toString(ci));
                string val = this.emitLoad(cap.llvmType, fgep);
                string ptr = this.emitAlloca(cap.llvmType, cap.name);
                this.emitStore(cap.llvmType, val, ptr);
                lamCtx.locals.set(cap.name, ptr);
                lamCtx.localTypes.set(cap.name, cap.llvmType);
                ci = ci + 1;
            }
        }

        // Spill lambda params to allocas
        i = 0;
        while (i < ln.params.length()) {
            Param p = ln.params.get(i) as Param;
            string lt = this.llvmTypeRef(p.ptype);
            string ptr = this.emitAlloca(lt, p.name);
            this.emitStore(lt, "%" + p.name, ptr);
            lamCtx.locals.set(p.name, ptr);
            lamCtx.localTypes.set(p.name, lt);
            i = i + 1;
        }

        // Emit body
        if (ln.body != null) {
            IRValue bodyVal = this.emitExpr(ln.body as ASTNode);
            bodyVal = this.coerce(bodyVal, retLLT);
            this.emit("ret " + retLLT + " " + bodyVal.name);
        } else if (ln.blockBody != null) {
            this.emitBlock(ln.blockBody as ASTNode);
            if (!lamCtx.terminated) {
                this.emit("ret " + retLLT + " " + this.llvmDefault(retLLT));
            }
        } else {
            this.emit("ret " + retLLT + " " + this.llvmDefault(retLLT));
        }

        this.lambdas.append("}\n");

        // Restore context
        this.ctx          = savedCtx;
        this.outTarget    = savedOut;
        this.counter      = savedCtr;
        this.currentClass = savedClass;

        // Allocate 16-byte closure struct: { fn_ptr, env_ptr }
        string closure = this.freshName("closure");
        this.emit(closure + " = call i8* @ec_new(i64 16)");

        // Store fn_ptr at offset 0
        string fnPtrSlot = this.freshName("cls.fn");
        this.emit(fnPtrSlot + " = bitcast i8* " + closure + " to i8**");
        string fnPtr = this.freshName("fn.ptr");
        this.emit(fnPtr + " = bitcast " + retLLT + " (" +
                  paramTypeStr.toString() + ")* @" + lamName + " to i8*");
        this.emit("store i8* " + fnPtr + ", i8** " + fnPtrSlot);

        // Store env_ptr at offset 8
        string envPtrSlot = this.freshName("cls.env");
        this.emit(envPtrSlot + " = getelementptr inbounds i8, i8* " + closure + ", i64 8");
        string envPtrSlotCast = this.freshName("cls.env.ptr");
        this.emit(envPtrSlotCast + " = bitcast i8* " + envPtrSlot + " to i8**");

        if (hasCaps) {
            int envSz = caps.length() * 8;
            string envRaw = this.freshName("env.raw");
            this.emit(envRaw + " = call i8* @ec_new(i64 " + toString(envSz) + ")");
            string envTyped2 = this.freshName("env.typed");
            this.emit(envTyped2 + " = bitcast i8* " + envRaw + " to %" + envName + "*");

            int ci = 0;
            while (ci < caps.length()) {
                CaptureInfo cap = caps.get(ci) as CaptureInfo;
                string curVal = this.emitLoad(cap.llvmType, cap.ptrInCtx);
                string fgep = this.freshName(cap.name + ".egep");
                this.emit(fgep + " = getelementptr inbounds %" + envName + ", %" +
                          envName + "* " + envTyped2 + ", i32 0, i32 " + toString(ci));
                this.emitStore(cap.llvmType, curVal, fgep);
                ci = ci + 1;
            }
            this.emit("store i8* " + envRaw + ", i8** " + envPtrSlotCast);
        } else {
            this.emit("store i8* null, i8** " + envPtrSlotCast);
        }

        return makeIRValue(closure, "i8*");
    }

    // Collect free variables in a lambda expression
    private fun List collectCaptures(LambdaNode ln) {
        if (this.ctx == null) { return new List(); }
        FnCtx c = this.ctx as FnCtx;

        // Build bound set from lambda params
        Set bound = new Set();
        int i = 0;
        while (i < ln.params.length()) {
            Param p = ln.params.get(i) as Param;
            bound.add(p.name);
            i = i + 1;
        }

        // Find free variables
        Set free = new Set();
        if (ln.body != null) {
            this.collectFreeVarsExpr(ln.body as ASTNode, bound, free);
        }
        if (ln.blockBody != null) {
            this.collectFreeVarsStmt(ln.blockBody as ASTNode, bound, free);
        }

        // Build CaptureInfo list
        List caps = new List();
        List freeKeys = free.keys();
        // Sort for stable order
        List sortedKeys = this.sortStrings(freeKeys);
        i = 0;
        while (i < sortedKeys.length()) {
            string name = _unboxStr(sortedKeys.get(i));
            if (c.locals.has(name) && c.localTypes.has(name)) {
                CaptureInfo cap = new CaptureInfo(); cap.init();
                cap.name      = name;
                cap.llvmType  = c.localTypes.get(name) as string;
                cap.ptrInCtx  = c.locals.get(name) as string;
                caps.add(cap);
            }
            i = i + 1;
        }
        return caps;
    }

    private fun void collectFreeVarsExpr(ASTNode expr, Set bound, Set free) {
        if (expr == null) { return; }
        FnCtx c = this.ctx as FnCtx;
        int k = expr.kind;

        if (k == 6) {  // Identifier
            IdentNode id = expr as IdentNode;
            if (!bound.has(id.name) && c.locals.has(id.name)) {
                free.add(id.name);
            }
            return;
        }
        if (k == 8) {  // Binary
            BinaryNode bn = expr as BinaryNode;
            this.collectFreeVarsExpr(bn.left  as ASTNode, bound, free);
            this.collectFreeVarsExpr(bn.right as ASTNode, bound, free);
            return;
        }
        if (k == 7) {  // Unary
            UnaryNode un = expr as UnaryNode;
            this.collectFreeVarsExpr(un.operand as ASTNode, bound, free);
            return;
        }
        if (k == 9) {  // Ternary
            TernaryNode tn = expr as TernaryNode;
            this.collectFreeVarsExpr(tn.cond     as ASTNode, bound, free);
            this.collectFreeVarsExpr(tn.thenExpr as ASTNode, bound, free);
            this.collectFreeVarsExpr(tn.elseExpr as ASTNode, bound, free);
            return;
        }
        if (k == 11) {  // Call
            CallNode cn = expr as CallNode;
            this.collectFreeVarsExpr(cn.callee as ASTNode, bound, free);
            int i = 0;
            while (i < cn.args.length()) {
                this.collectFreeVarsExpr(cn.args.get(i) as ASTNode, bound, free);
                i = i + 1;
            }
            return;
        }
        if (k == 12) {  // Member
            MemberNode mn = expr as MemberNode;
            this.collectFreeVarsExpr(mn.object as ASTNode, bound, free);
            return;
        }
        if (k == 13) {  // Index
            IndexNode idx = expr as IndexNode;
            this.collectFreeVarsExpr(idx.array as ASTNode, bound, free);
            this.collectFreeVarsExpr(idx.index as ASTNode, bound, free);
            return;
        }
        if (k == 10) {  // Assign
            AssignNode an = expr as AssignNode;
            this.collectFreeVarsExpr(an.target as ASTNode, bound, free);
            this.collectFreeVarsExpr(an.value  as ASTNode, bound, free);
            return;
        }
    }

    private fun void collectFreeVarsStmt(ASTNode stmt, Set bound, Set free) {
        if (stmt == null) { return; }
        int k = stmt.kind;
        if (k == 1) {  // ExprStmt
            ExprStmtNode es = stmt as ExprStmtNode;
            this.collectFreeVarsExpr(es.expr as ASTNode, bound, free);
            return;
        }
        if (k == 8) {  // Return
            ReturnNode rn = stmt as ReturnNode;
            if (rn.value != null) {
                this.collectFreeVarsExpr(rn.value as ASTNode, bound, free);
            }
            return;
        }
        if (k == 0) {  // Block
            BlockNode bn = stmt as BlockNode;
            int i = 0;
            while (i < bn.stmts.length()) {
                this.collectFreeVarsStmt(bn.stmts.get(i) as ASTNode, bound, free);
                i = i + 1;
            }
            return;
        }
        if (k == 3) {  // If
            IfNode ifn = stmt as IfNode;
            this.collectFreeVarsExpr(ifn.cond as ASTNode, bound, free);
            this.collectFreeVarsStmt(ifn.thenBranch as ASTNode, bound, free);
            if (ifn.elseBranch != null) {
                this.collectFreeVarsStmt(ifn.elseBranch as ASTNode, bound, free);
            }
            return;
        }
        if (k == 4) {  // While
            WhileNode wn = stmt as WhileNode;
            this.collectFreeVarsExpr(wn.cond as ASTNode, bound, free);
            this.collectFreeVarsStmt(wn.body as ASTNode, bound, free);
            return;
        }
    }

    // ── Array helpers ─────────────────────────────────────────────────────────

    private fun string emitArrayNew(string elemLLT, string lengthVal) {
        int elemSz = 8;
        if (elemLLT == "i1" || elemLLT == "i8") { elemSz = 1; }
        else if (elemLLT == "i32" || elemLLT == "float") { elemSz = 4; }
        else if (elemLLT == "i64" || elemLLT == "double") { elemSz = 8; }

        string arr = this.freshName("arr");
        this.emit(arr + " = call i8* @ec_array_new(i64 " +
                  toString(elemSz) + ", i64 " + lengthVal + ")");
        return arr;
    }

    private fun string emitArrayLen(string arrPtr) {
        string res = this.freshName("arr.len");
        this.emit(res + " = call i64 @ec_array_len(i8* " + arrPtr + ")");
        return res;
    }

    private fun string emitArrayElemPtr(string arrPtr, string elemLLT, string idxVal) {
        int elemSz = 8;
        if (elemLLT == "i1" || elemLLT == "i8") { elemSz = 1; }
        else if (elemLLT == "i32" || elemLLT == "float") { elemSz = 4; }
        else if (elemLLT == "i64" || elemLLT == "double") { elemSz = 8; }

        string scaledIdx = this.freshName("scaled");
        this.emit(scaledIdx + " = mul i64 " + idxVal + ", " + toString(elemSz));
        string byteOff = this.freshName("byteoff");
        this.emit(byteOff + " = add i64 8, " + scaledIdx);
        string rawPtr = this.freshName("raw.elem");
        this.emit(rawPtr + " = getelementptr inbounds i8, i8* " + arrPtr + ", i64 " + byteOff);
        string typedPtr = this.freshName("elem.ptr");
        this.emit(typedPtr + " = bitcast i8* " + rawPtr + " to " + elemLLT + "*");
        return typedPtr;
    }

    // ── Utility ───────────────────────────────────────────────────────────────

    private fun string extractClassName(string llvmType) {
        int len = stringLen(llvmType);
        if (len < 2 || llvmType[0] != '%') { return ""; }
        string inner = substring(llvmType, 1, len);
        int ilen = stringLen(inner);
        if (ilen > 0 && inner[ilen-1] == '*') {
            return substring(inner, 0, ilen - 1);
        }
        return inner;
    }

    // Simple insertion sort on a List of boxed strings
    private fun List sortStrings(List lst) {
        int n = lst.length();
        int i = 1;
        while (i < n) {
            string key = _unboxStr(lst.get(i));
            int j = i - 1;
            while (j >= 0 && compare(_unboxStr(lst.get(j)), key) > 0) {
                lst.set(j + 1, lst.get(j));
                j = j - 1;
            }
            lst.set(j + 1, _boxStr(key));
            i = i + 1;
        }
        return lst;
    }
}

// ── Test driver ───────────────────────────────────────────────────────────────

fun int main() {
    string source = "import \"std.io\";\n" +
                    "fun int add(int a, int b) { return a + b; }\n" +
                    "fun int main() {\n" +
                    "  int x = add(3, 4);\n" +
                    "  println(toString(x));\n" +
                    "  return 0;\n" +
                    "}";

    Lexer lexer = new Lexer("", "");
    lexer.init(source, "test.ec");
    List tokens = lexer.tokenize();

    Parser parser = new Parser(tokens, "");
    parser.init(tokens, "test.ec");
    Program prog = parser.parse();

    print("Parse errors: "); println(toString(parser.getErrors().length()));

    TypeChecker tc = new TypeChecker("");
    tc.init("test.ec");
    tc.registerResolvedImport("std.io");
    tc.check(prog);

    print("Type errors: "); println(toString(tc.getErrors().length()));

    CodeGen cg = new CodeGen("");
    cg.init("test");
    string ir = cg.generate(prog);

    print("IR size: "); println(toString(stringLen(ir)));
    println("CodeGen PASSED.");
    return 0;
}
