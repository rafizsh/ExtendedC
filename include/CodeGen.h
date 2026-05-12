#pragma once
//===----------------------------------------------------------------------===//
//  CodeGen.h  –  LLVM IR code generator (textual .ll output)
//===----------------------------------------------------------------------===//
#ifndef CODEGEN_H
#define CODEGEN_H

#include "AST.h"
#include "SymbolTable.h"

#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <sstream>
#include <memory>
#include <functional>

// ─────────────────────────────────────────────────────────────────────────────
//  IRValue  –  an SSA value in the generated IR
// ─────────────────────────────────────────────────────────────────────────────
struct IRValue {
    std::string name;   // e.g. "%x.1", "@global", "42", "true"
    std::string type;   // LLVM type string e.g. "i32", "i8*", "%Dog*"

    bool isValid() const { return !name.empty(); }
    static IRValue invalid() { return {"", ""}; }
};

// ─────────────────────────────────────────────────────────────────────────────
//  CodeGen
// ─────────────────────────────────────────────────────────────────────────────
class CodeGen {
public:
    explicit CodeGen(std::string moduleName = "ec_module");

    /// Generate IR for the entire program. Returns the full .ll text.
    std::string generate(const Program& program);

private:
    // ── Module-level emission ────────────────────────────────────────────────
    void emitPreamble();
    void emitRuntime();
    void emitTypeDecls(const Program& program);
    void emitGlobalVar(const VarDecl& decl);
    void emitDecl(const Decl& decl);
    void emitEnumDecl(const EnumDecl& decl);
    void emitStructDecl(const StructDecl& decl);
    void emitUnionDecl(const UnionDecl& decl);
    void emitClassDecl(const ClassDecl& decl);
    void emitFunctionDecl(const FunctionDecl& decl,
                          const std::string& ownerClass = "");
    void emitAsyncFunctionDecl(const FunctionDecl& decl,
                               const std::string& irName,
                               const std::string& retLLVM,
                               const std::string& ownerClass);
    void emitUsingDecl(const UsingDecl& decl);
    void emitCollectionStubs();

    // ── Statement emission ───────────────────────────────────────────────────
    void emitStmt(const Stmt& stmt);
    void emitBlock(const BlockStmt& stmt);
    void emitVarDeclStmt(const VarDeclStmt& stmt);
    void emitIfStmt(const IfStmt& stmt);
    void emitWhileStmt(const WhileStmt& stmt);
    void emitDoWhileStmt(const DoWhileStmt& stmt);
    void emitForStmt(const ForStmt& stmt);
    void emitSwitchStmt(const SwitchStmt& stmt);
    void emitReturnStmt(const ReturnStmt& stmt);
    void emitThrowStmt(const ThrowStmt& stmt);
    void emitTryStmt(const TryStmt& stmt);
    void emitWithStmt(const WithStmt& stmt);
    void emitBreakStmt(const BreakStmt& stmt);
    void emitContinueStmt(const ContinueStmt& stmt);
    void emitExprStmt(const ExprStmt& stmt);

    // ── Expression emission ──────────────────────────────────────────────────
    IRValue emitExpr(const Expr& expr);
    IRValue emitIntLit(const IntLitExpr& e);
    IRValue emitFloatLit(const FloatLitExpr& e);
    IRValue emitBoolLit(const BoolLitExpr& e);
    IRValue emitCharLit(const CharLitExpr& e);
    IRValue emitStringLit(const StringLitExpr& e);
    IRValue emitNullLit(const NullLitExpr& e);
    IRValue emitIdent(const IdentExpr& e);
    IRValue emitUnary(const UnaryExpr& e);
    IRValue emitBinary(const BinaryExpr& e);
    IRValue emitTernary(const TernaryExpr& e);
    IRValue emitAssign(const AssignExpr& e);
    IRValue emitCall(const CallExpr& e);
    IRValue emitMember(const MemberExpr& e);
    IRValue emitIndex(const IndexExpr& e);
    IRValue emitNew(const NewExpr& e);
    IRValue emitDelete(const DeleteExpr& e);
    IRValue emitTypeOf(const TypeOfExpr& e);
    IRValue emitInstanceOf(const InstanceOfExpr& e);
    IRValue emitCast(const CastExpr& e);
    IRValue emitLambda(const LambdaExpr& e);
    IRValue emitArrayLit(const ArrayLitExpr& e);

    // ── Array helpers ────────────────────────────────────────────────────────
    // Array fat-pointer layout in GC memory:
    //   [0..7]  = i64 length
    //   [8..]   = element data (element_size * length bytes)
    // ec_array_new(elem_size, length) → i8*  (fat pointer to the array)
    // ec_array_len(arr)               → i64
    // Elem ptr: arr + 8 + index * elem_size
    std::string emitArrayNew(const std::string& elemLLVMType,
                             const std::string& lengthVal);
    std::string emitArrayLen(const std::string& arrPtr);
    std::string emitArrayElemPtr(const std::string& arrPtr,
                                 const std::string& elemLLVMType,
                                 const std::string& idxVal);

    // ── Closure helpers ──────────────────────────────────────────────────────
    // Closure fat pointer layout in GC memory:
    //   [0..7]  = i8* fn_ptr
    //   [8..15] = i8* env_ptr
    // env_ptr points to another GC allocation with captured values.
    // collectCaptures: walk lambda body AST to find free variable names
    struct CaptureInfo {
        std::string name;
        std::string llvmType;
        std::string ptrInCtx;  // alloca pointer in the enclosing function
    };
    std::vector<CaptureInfo> collectCaptures(const LambdaExpr& e);
    void collectFreeVars(const Expr& e,
                         const std::unordered_set<std::string>& bound,
                         std::unordered_set<std::string>& free);
    void collectFreeVarsStmt(const Stmt& s,
                             const std::unordered_set<std::string>& bound,
                             std::unordered_set<std::string>& free);

    // ── Type mapping ─────────────────────────────────────────────────────────
    std::string llvmType(const TypeRef& ref) const;
    std::string llvmType(const std::string& typeName) const;
    std::string llvmDefault(const std::string& llType) const;

    // ── Instruction helpers ──────────────────────────────────────────────────
    // Emit a raw IR line with indentation
    void emit(const std::string& line);
    // Emit a blank line
    void emitLine();
    // Emit at global scope (no indent)
    void emitGlobal(const std::string& line);

    // Allocate a new SSA name  %name.N
    std::string freshName(const std::string& hint = "t");
    // Allocate a new basic-block label
    std::string freshLabel(const std::string& hint = "bb");
    // Allocate a new global string constant name
    std::string freshStrConst();

    // Emit alloca for a local variable, return the pointer name
    std::string emitAlloca(const std::string& llType, const std::string& hint);
    // Emit store
    void emitStore(const std::string& valType, const std::string& val,
                   const std::string& ptrType, const std::string& ptr);
    // Emit load, return result name
    std::string emitLoad(const std::string& valType, const std::string& ptr);
    // Emit GEP for struct field
    std::string emitGEP(const std::string& structType, const std::string& ptr,
                        int fieldIndex, const std::string& hint = "gep");
    // Emit a label
    void emitLabel(const std::string& label);
    // Emit unconditional branch
    void emitBr(const std::string& label);
    // Emit conditional branch
    void emitCondBr(const std::string& cond,
                    const std::string& trueLabel,
                    const std::string& falseLabel);
    // Cast an IRValue to a target llvm type if needed
    IRValue coerce(const IRValue& val, const std::string& targetType);

    // ── Address-of (for assignment targets) ─────────────────────────────────
    // Returns the alloca/GEP pointer for an l-value expression
    std::string emitLValue(const Expr& expr);

    // ── String constant pool ─────────────────────────────────────────────────
    // Intern a string literal, return a global pointer name
    std::string internString(const std::string& s);

    // ── Class layout helpers ─────────────────────────────────────────────────
    struct ClassLayout {
        std::string              llvmName;    // %ClassName
        std::vector<std::string> fieldNames;  // user field names (offset by 1 for vtptr)
        std::vector<std::string> fieldTypes;  // LLVM types of user fields
        std::vector<std::string> methodNames; // all method names
        std::string              superClass;

        // Vtable: ordered list of (slot_index, mangled_fn_name, fn_llvm_type)
        // Inherited slots occupy the same indices as in the superclass.
        struct VtSlot {
            int         index;
            std::string methodName;    // unmangled (e.g. "speak")
            std::string ownerClass;    // which class defines this slot's fn
            std::string retType;       // LLVM return type
            std::string paramTypes;    // comma-separated LLVM param types (no self)
        };
        std::vector<VtSlot> vtable;      // ordered by slot index
        bool hasVtable = false;          // true if any virtual methods exist
    };
    void buildClassLayout(const ClassDecl& decl);
    void buildVtable(const ClassDecl& decl, const Program& program);
    void emitVtableGlobals(const Program& program);
    int  fieldIndex(const std::string& className, const std::string& field) const;
    std::string fieldLLVMType(const std::string& className, int idx) const;
    // vtable slot index for a method (-1 if not in vtable)
    int  vtableSlot(const std::string& className, const std::string& method) const;
    // Is this a virtual method call? (receiver is base class, method is overridden)
    bool isVirtualCall(const std::string& receiverClass,
                       const std::string& method) const;

    // ── Function context ─────────────────────────────────────────────────────
    struct FnCtx {
        std::string              name;
        std::string              retType;   // LLVM return type
        bool                     hasVoidRet = false;
        // local variable → alloca pointer name
        std::unordered_map<std::string, std::string> locals;  // name → ptr
        std::unordered_map<std::string, std::string> localTypes; // name → lltype
        // loop labels for break/continue
        std::vector<std::string> breakLabels;
        std::vector<std::string> continueLabels;
        // try/catch landing pad info
        bool                     inTry   = false;
        std::string              unwindLabel;
        // whether last emitted instruction was a terminator
        bool                     terminated = false;
    };

    // ── State ────────────────────────────────────────────────────────────────
    std::string   moduleName_;
    std::ostringstream globals_;   // global declarations / type defs
    std::ostringstream body_;      // function bodies (current output target)
    std::ostringstream lambdas_;   // lambda/closure definitions — flushed after each top-level fn
    std::ostringstream* out_;      // points to globals_ or body_

    int  counter_    = 0;         // SSA counter
    int  labelCtr_   = 0;
    int  strCtr_     = 0;

    // string constant pool: content → global name
    std::unordered_map<std::string, std::string> stringPool_;

    // class layouts
    std::unordered_map<std::string, ClassLayout> classLayouts_;

    // enum member values: "EnumName::Member" → integer
    std::unordered_map<std::string, int> enumValues_;

    // global variables: name → llvm type
    std::unordered_map<std::string, std::string> globalVars_;

    // current function context
    FnCtx* ctx_ = nullptr;

    // set of declared functions (to avoid double-declare)
    std::unordered_set<std::string> declaredFns_;

    // lambda counter for naming
    int lambdaCtr_ = 0;

    // all lambdas to emit after current function
    struct LambdaDef {
        std::string              irName;
        std::vector<Parameter>   params;
        std::string              retType;
        const Expr*              bodyExpr  = nullptr;
        const Stmt*              bodyBlock = nullptr;
    };
    std::vector<LambdaDef> pendingLambdas_;

    // known user-defined function return types (populated as functions are emitted)
    std::unordered_map<std::string, std::string> userFnRetTypes_;

    // Closure type registry: alloca_ptr_name → {retType, paramTypes}
    // Populated when a closure is assigned to a local variable.
    struct ClosureSig {
        std::string retType;       // actual LLVM return type (not always i8*)
        std::string paramTypeStr;  // comma-sep LLVM param types (no env)
    };
    std::unordered_map<std::string, ClosureSig> closureTypes_; // var_name → sig

    // current class being emitted (for 'this')
    std::string currentClass_;

    // current basic block label (updated by emitLabel, used by emitTernary
    // to get correct phi predecessors when nested ternaries emit multiple blocks)
    std::string currentBlock_;
};

#endif // CODEGEN_H
