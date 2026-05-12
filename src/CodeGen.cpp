//===----------------------------------------------------------------------===//
//  CodeGen.cpp  –  LLVM IR (textual) code generator
//===----------------------------------------------------------------------===//
#include "CodeGen.h"

#include <cassert>
#include <sstream>
#include <algorithm>
#include <stdexcept>

// ─────────────────────────────────────────────────────────────────────────────
//  Helpers
// ─────────────────────────────────────────────────────────────────────────────
static std::string escapeString(const std::string& s) {
    // Produce LLVM hex-escape form suitable for c-string constants
    std::string out;
    for (unsigned char c : s) {
        if (c == '\n')       out += "\\0A";
        else if (c == '\t')  out += "\\09";
        else if (c == '\r')  out += "\\0D";
        else if (c == '\\')  out += "\\5C";
        else if (c == '"')   out += "\\22";
        else if (c < 32 || c > 126) {
            char buf[8];
            snprintf(buf, sizeof(buf), "\\%02X", c);
            out += buf;
        } else {
            out += (char)c;
        }
    }
    return out;
}


// ─────────────────────────────────────────────────────────────────────────────
//  Constructor
// ─────────────────────────────────────────────────────────────────────────────
CodeGen::CodeGen(std::string moduleName)
    : moduleName_(std::move(moduleName)), out_(&globals_), currentBlock_("entry") {}

// ─────────────────────────────────────────────────────────────────────────────
//  Output routing
// ─────────────────────────────────────────────────────────────────────────────
void CodeGen::emit(const std::string& line) {
    *out_ << "  " << line << "\n";
}
void CodeGen::emitLine() { *out_ << "\n"; }
void CodeGen::emitGlobal(const std::string& line) {
    globals_ << line << "\n";
}
void CodeGen::emitLabel(const std::string& label) {
    *out_ << label << ":\n";
    if (ctx_) ctx_->terminated = false;
    currentBlock_ = label;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Name / label / string factories
// ─────────────────────────────────────────────────────────────────────────────
std::string CodeGen::freshName(const std::string& hint) {
    return "%" + hint + "." + std::to_string(++counter_);
}
std::string CodeGen::freshLabel(const std::string& hint) {
    return hint + "." + std::to_string(++labelCtr_);
}
std::string CodeGen::freshStrConst() {
    return "@.str." + std::to_string(++strCtr_);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Type mapping
// ─────────────────────────────────────────────────────────────────────────────
std::string CodeGen::llvmType(const std::string& name) const {
    if (name == "void")   return "void";
    if (name == "bool")   return "i1";
    if (name == "char")   return "i8";
    if (name == "int")    return "i32";
    if (name == "uint")   return "i32";
    if (name == "long")   return "i64";
    if (name == "ulong")  return "i64";
    if (name == "float")  return "float";
    if (name == "ufloat") return "float";
    if (name == "double") return "double";
    if (name == "string") return "i8*";
    if (name == "null")   return "i8*";
    if (name == "var")    return "i8*";   // erased type
    if (name == "<unknown>") return "i32";
    // Array types like "int[]", "string[]" → fat pointer
    if (name.size() > 2 && name[name.size()-2] == '[' && name.back() == ']')
        return "i8*";
    // user-defined → pointer to struct
    return "%" + name + "*";
}

std::string CodeGen::llvmType(const TypeRef& ref) const {
    std::string base = llvmType(ref.name);
    if (ref.isArray)   return "i8*";  // arrays are GC fat pointers {len, data}
    if (ref.isPointer) return base + "*";
    return base;
}

std::string CodeGen::llvmDefault(const std::string& t) const {
    if (t == "i1")     return "false";
    if (t == "i8")     return "0";
    if (t == "i32")    return "0";
    if (t == "i64")    return "0";
    if (t == "float")  return "0.0";
    if (t == "double") return "0.0";
    return "null";   // pointers
}

// ─────────────────────────────────────────────────────────────────────────────
//  Instruction helpers
// ─────────────────────────────────────────────────────────────────────────────
std::string CodeGen::emitAlloca(const std::string& llType, const std::string& hint) {
    std::string ptr = freshName(hint + ".ptr");
    emit(ptr + " = alloca " + llType);
    return ptr;
}

void CodeGen::emitStore(const std::string& valType, const std::string& val,
                        const std::string& /*ptrType*/, const std::string& ptr) {
    emit("store " + valType + " " + val + ", " + valType + "* " + ptr);
}

std::string CodeGen::emitLoad(const std::string& valType, const std::string& ptr) {
    std::string res = freshName("load");
    emit(res + " = load " + valType + ", " + valType + "* " + ptr);
    return res;
}

std::string CodeGen::emitGEP(const std::string& structType,
                              const std::string& ptr,
                              int fieldIndex,
                              const std::string& hint) {
    std::string res = freshName(hint);
    emit(res + " = getelementptr inbounds " + structType + ", " +
         structType + "* " + ptr +
         ", i32 0, i32 " + std::to_string(fieldIndex));
    return res;
}

void CodeGen::emitBr(const std::string& label) {
    if (ctx_ && ctx_->terminated) return;
    emit("br label %" + label);
    if (ctx_) ctx_->terminated = true;
}

void CodeGen::emitCondBr(const std::string& cond,
                         const std::string& trueLabel,
                         const std::string& falseLabel) {
    if (ctx_ && ctx_->terminated) return;
    emit("br i1 " + cond + ", label %" + trueLabel + ", label %" + falseLabel);
    if (ctx_) ctx_->terminated = true;
}

IRValue CodeGen::coerce(const IRValue& val, const std::string& targetType) {
    if (val.type == targetType) return val;

    // integer zero-extend / sign-extend / truncate
    auto isInt = [](const std::string& t) {
        return t == "i1" || t == "i8" || t == "i32" || t == "i64";
    };
    auto isFloat = [](const std::string& t) {
        return t == "float" || t == "double";
    };

    if (isInt(val.type) && isInt(targetType)) {
        // size comparison by string is fragile; use explicit map
        std::unordered_map<std::string,int> sz = {{"i1",1},{"i8",8},{"i32",32},{"i64",64}};
        std::string res = freshName("conv");
        int from = sz.count(val.type)   ? sz[val.type]   : 32;
        int to   = sz.count(targetType) ? sz[targetType] : 32;
        if (to > from)
            emit(res + " = zext " + val.type + " " + val.name + " to " + targetType);
        else if (to < from)
            emit(res + " = trunc " + val.type + " " + val.name + " to " + targetType);
        else
            return val;
        return {res, targetType};
    }
    if (isInt(val.type) && isFloat(targetType)) {
        std::string res = freshName("sitofp");
        emit(res + " = sitofp " + val.type + " " + val.name + " to " + targetType);
        return {res, targetType};
    }
    if (isFloat(val.type) && isFloat(targetType)) {
        std::string res = freshName("fpext");
        if (targetType == "double" && val.type == "float")
            emit(res + " = fpext float " + val.name + " to double");
        else
            emit(res + " = fptrunc double " + val.name + " to float");
        return {res, targetType};
    }
    // pointer cast
    if (targetType.back() == '*' || val.type.back() == '*') {
        std::string res = freshName("bc");
        emit(res + " = bitcast " + val.type + " " + val.name + " to " + targetType);
        return {res, targetType};
    }
    return val; // give up, return as-is
}

// ─────────────────────────────────────────────────────────────────────────────
//  String constant interning
// ─────────────────────────────────────────────────────────────────────────────
std::string CodeGen::internString(const std::string& s) {
    auto it = stringPool_.find(s);
    if (it != stringPool_.end()) return it->second;

    std::string gname = freshStrConst();
    std::string escaped = escapeString(s);
    int len = (int)s.size() + 1; // include null terminator
    globals_ << gname << " = private unnamed_addr constant ["
             << len << " x i8] c\"" << escaped << "\\00\"\n";

    // Also emit a getelementptr alias for easy i8* use
    std::string pname = gname + ".ptr";
    // We'll compute it inline at use-site instead.
    stringPool_[s] = gname;
    return gname;
}

// Helper: get i8* pointer to an interned string

// ─────────────────────────────────────────────────────────────────────────────
//  Class layout builder
// ─────────────────────────────────────────────────────────────────────────────
void CodeGen::buildClassLayout(const ClassDecl& decl) {
    ClassLayout layout;
    layout.llvmName   = "%" + decl.name;
    layout.superClass = decl.superClass.value_or("");
    layout.hasVtable  = true; // all classes get a vtable pointer

    // Field 0 is always the vtable pointer: i8**
    // (inserted implicitly — fieldNames/fieldTypes only cover user fields,
    //  but fieldIndex() adds 1 to account for the vtptr at slot 0)

    // If there's a superclass, copy its user fields first
    if (decl.superClass) {
        auto it = classLayouts_.find(*decl.superClass);
        if (it != classLayouts_.end()) {
            layout.fieldNames = it->second.fieldNames;
            layout.fieldTypes = it->second.fieldTypes;
        }
    }

    // Add own fields
    for (const auto& f : decl.fields) {
        layout.fieldNames.push_back(f.name);
        layout.fieldTypes.push_back(llvmType(f.type));
    }

    // All method names (for vtable slot lookup)
    for (const auto& m : decl.methods)
        layout.methodNames.push_back(m->name);

    classLayouts_[decl.name] = std::move(layout);
}

int CodeGen::fieldIndex(const std::string& className,
                        const std::string& field) const {
    auto it = classLayouts_.find(className);
    if (it == classLayouts_.end()) return -1;
    const auto& names = it->second.fieldNames;
    for (int i = 0; i < (int)names.size(); ++i)
        if (names[i] == field)
            return i + 1; // +1 because slot 0 is the vtable pointer
    return -1;
}

std::string CodeGen::fieldLLVMType(const std::string& className, int idx) const {
    // idx is the GEP index (slot 0 = vtptr, slots 1+ = user fields)
    if (idx == 0) return "i8**"; // vtable pointer
    auto it = classLayouts_.find(className);
    if (it == classLayouts_.end() || idx < 1 ||
        idx - 1 >= (int)it->second.fieldTypes.size())
        return "i32";
    return it->second.fieldTypes[idx - 1];
}

// ─────────────────────────────────────────────────────────────────────────────
//  Vtable construction
// ─────────────────────────────────────────────────────────────────────────────
// A method is virtual (goes in the vtable) if it is:
//   - public or protected
//   - non-static
//   - not "init" (constructors are never virtual)
//   - has a body (not abstract)
static bool isVirtualMethod(const FunctionDecl& m) {
    if (m.name == "init") return false;
    if (m.isStatic)       return false;
    if (m.access == AccessMod::Private) return false;
    return true;
}

void CodeGen::buildVtable(const ClassDecl& decl, const Program& program) {
    auto& layout = classLayouts_[decl.name];

    // Inherit superclass vtable slots
    if (!layout.superClass.empty()) {
        auto superIt = classLayouts_.find(layout.superClass);
        if (superIt != classLayouts_.end())
            layout.vtable = superIt->second.vtable;
    }

    // Fast lookup: slot name -> index  (O(n) instead of O(n²))
    std::unordered_map<std::string, std::size_t> slotIdx;
    for (std::size_t i = 0; i < layout.vtable.size(); ++i)
        slotIdx[layout.vtable[i].methodName] = i;

    // Process this class's virtual methods
    for (const auto& m : decl.methods) {
        if (!isVirtualMethod(*m)) continue;
        auto it = slotIdx.find(m->name);
        if (it != slotIdx.end()) {
            // Override existing inherited slot
            layout.vtable[it->second].ownerClass = decl.name;
        } else {
            // New slot for this class
            ClassLayout::VtSlot slot;
            slot.index = (int)layout.vtable.size();
            slot.methodName = m->name;
            slot.ownerClass = decl.name;
            slot.retType = "";
            slot.paramTypes = "";
            slotIdx[m->name] = layout.vtable.size();
            layout.vtable.push_back(std::move(slot));
        }
    }
}


void CodeGen::emitVtableGlobals(const Program& program) {
    globals_ << "; ── Vtable globals ─────────────────────────────────────────\n";
    for (const auto& d : program.declarations) {
        if (d->kind != DeclKind::Class) continue;
        const auto& cls = static_cast<const ClassDecl&>(*d);
        auto it = classLayouts_.find(cls.name);
        if (it == classLayouts_.end() || !it->second.hasVtable) continue;

        const auto& layout = it->second;
        int n = (int)layout.vtable.size();
        if (n == 0) continue;

        // @ClassName_vtable = global [N x i8*] [ ... ]
        globals_ << "@" << cls.name << "_vtable = global ["
                 << n << " x i8*] [\n";
        for (int i = 0; i < n; ++i) {
            const auto& slot = layout.vtable[i];
            if (i) globals_ << ",\n";
            // Build the full function type for this slot
            // Signature: retType (ownerClass*, params...)
            std::string fnType = slot.retType + " (%" + slot.ownerClass + "*";
            if (!slot.paramTypes.empty()) fnType += ", " + slot.paramTypes;
            fnType += ")*";
            std::string mangledName = slot.ownerClass + "_" + slot.methodName;
            globals_ << "  i8* bitcast (" << fnType << " @" << mangledName
                     << " to i8*)";
        }
        globals_ << "\n]\n";
    }
    globals_ << "\n";
}

int CodeGen::vtableSlot(const std::string& className,
                        const std::string& method) const {
    auto it = classLayouts_.find(className);
    if (it == classLayouts_.end()) return -1;
    for (const auto& slot : it->second.vtable)
        if (slot.methodName == method) return slot.index;
    return -1;
}

bool CodeGen::isVirtualCall(const std::string& receiverClass,
                            const std::string& method) const {
    // A call is virtual if the method appears in the vtable of the receiver's
    // class AND there is at least one subclass that overrides it.
    // For simplicity: any call through a pointer to a class that has a vtable
    // slot for this method is virtual.
    return vtableSlot(receiverClass, method) >= 0;
}


// ─────────────────────────────────────────────────────────────────────────────
//  Collection method stubs
//  Emit LLVM IR function bodies for List/Map/Set/StringBuilder methods.
//  These classes live in std.collections which is not code-generated, so we
//  emit the method bodies here directly, each forwarding to the ec_* runtime.
// ─────────────────────────────────────────────────────────────────────────────
void CodeGen::emitCollectionStubs() {
    // Helper macro (inline lambda) to emit a stub
    // All collection structs have layout: { i8** vtptr, i8* data }
    // GEP index 1 = the data field

    auto load_data = [&](const std::string& cls, const std::string& selfArg) -> std::string {
        // %data_gep = GEP %Cls, %Cls* %self, i32 0, i32 1
        // %data     = load i8*, i8** %data_gep
        body_ << "  %data_gep = getelementptr inbounds %" << cls
              << ", %" << cls << "* " << selfArg << ", i32 0, i32 1\n";
        body_ << "  %data = load i8*, i8** %data_gep\n";
        return "%data";
    };

    auto store_data = [&](const std::string& cls, const std::string& selfArg,
                          const std::string& val) {
        body_ << "  %data_gep2 = getelementptr inbounds %" << cls
              << ", %" << cls << "* " << selfArg << ", i32 0, i32 1\n";
        body_ << "  store i8* " << val << ", i8** %data_gep2\n";
    };

    auto store_vtable = [&](const std::string& cls, const std::string& selfArg) {
        // Store vtable pointer at field 0 during init
        auto it = classLayouts_.find(cls);
        if (it != classLayouts_.end() && it->second.hasVtable &&
            !it->second.vtable.empty()) {
            int n = (int)it->second.vtable.size();
            body_ << "  %vt_slot = getelementptr inbounds %" << cls
                  << ", %" << cls << "* " << selfArg << ", i32 0, i32 0\n";
            body_ << "  %vt_cast = bitcast [" << n << " x i8*]* @" << cls
                  << "_vtable to i8**\n";
            body_ << "  store i8** %vt_cast, i8*** %vt_slot\n";
        }
    };

    // ── List ──────────────────────────────────────────────────────────────────

    body_ << "\ndefine void @List_init(%List* %self) {\nentry:\n";
    store_vtable("List", "%self");
    body_ << "  %d = call i8* @ec_list_new()\n";
    body_ << "  %gep = getelementptr inbounds %List, %List* %self, i32 0, i32 1\n";
    body_ << "  store i8* %d, i8** %gep\n";
    body_ << "  ret void\n}\n";

    body_ << "\ndefine i32 @List_length(%List* %self) {\nentry:\n";
    load_data("List", "%self");
    body_ << "  %r = call i64 @ec_list_len(i8* %data)\n";
    body_ << "  %r32 = trunc i64 %r to i32\n";
    body_ << "  ret i32 %r32\n}\n";

    body_ << "\ndefine i32 @List_size(%List* %self) {\nentry:\n";
    load_data("List", "%self");
    body_ << "  %r = call i64 @ec_list_len(i8* %data)\n";
    body_ << "  %r32 = trunc i64 %r to i32\n";
    body_ << "  ret i32 %r32\n}\n";

    body_ << "\ndefine i1 @List_isEmpty(%List* %self) {\nentry:\n";
    load_data("List", "%self");
    body_ << "  %r = call i64 @ec_list_len(i8* %data)\n";
    body_ << "  %b = icmp eq i64 %r, 0\n";
    body_ << "  ret i1 %b\n}\n";

    body_ << "\ndefine void @List_add(%List* %self, i8* %item) {\nentry:\n";
    load_data("List", "%self");
    body_ << "  %nd = call i8* @ec_list_add(i8* %data, i8* %item)\n";
    store_data("List", "%self", "%nd");
    body_ << "  ret void\n}\n";

    body_ << "\ndefine i8* @List_get(%List* %self, i32 %index) {\nentry:\n";
    load_data("List", "%self");
    body_ << "  %i64 = sext i32 %index to i64\n";
    body_ << "  %r = call i8* @ec_list_get(i8* %data, i64 %i64)\n";
    body_ << "  ret i8* %r\n}\n";

    body_ << "\ndefine void @List_set(%List* %self, i32 %index, i8* %item) {\nentry:\n";
    load_data("List", "%self");
    body_ << "  %i64 = sext i32 %index to i64\n";
    body_ << "  %nd = call i8* @ec_list_set(i8* %data, i64 %i64, i8* %item)\n";
    store_data("List", "%self", "%nd");
    body_ << "  ret void\n}\n";

    body_ << "\ndefine void @List_remove(%List* %self, i32 %index) {\nentry:\n";
    load_data("List", "%self");
    body_ << "  %i64 = sext i32 %index to i64\n";
    body_ << "  %nd = call i8* @ec_list_remove(i8* %data, i64 %i64)\n";
    store_data("List", "%self", "%nd");
    body_ << "  ret void\n}\n";

    body_ << "\ndefine void @List_insert(%List* %self, i32 %index, i8* %item) {\nentry:\n";
    load_data("List", "%self");
    body_ << "  %i64 = sext i32 %index to i64\n";
    body_ << "  %nd = call i8* @ec_list_insert(i8* %data, i64 %i64, i8* %item)\n";
    store_data("List", "%self", "%nd");
    body_ << "  ret void\n}\n";

    body_ << "\ndefine i64 @List_indexOf(%List* %self, i8* %item) {\nentry:\n";
    load_data("List", "%self");
    body_ << "  %r = call i64 @ec_list_index_of(i8* %data, i8* %item)\n";
    body_ << "  ret i64 %r\n}\n";

    body_ << "\ndefine i32 @List_contains(%List* %self, i8* %item) {\nentry:\n";
    load_data("List", "%self");
    body_ << "  %r = call i32 @ec_list_contains(i8* %data, i8* %item)\n";
    body_ << "  ret i32 %r\n}\n";

    body_ << "\ndefine i8* @List_last(%List* %self) {\nentry:\n";
    load_data("List", "%self");
    body_ << "  %n = call i64 @ec_list_len(i8* %data)\n";
    body_ << "  %nm1 = sub i64 %n, 1\n";
    body_ << "  %r = call i8* @ec_list_get(i8* %data, i64 %nm1)\n";
    body_ << "  ret i8* %r\n}\n";

    body_ << "\ndefine i8* @List_first(%List* %self) {\nentry:\n";
    load_data("List", "%self");
    body_ << "  %r = call i8* @ec_list_get(i8* %data, i64 0)\n";
    body_ << "  ret i8* %r\n}\n";

    body_ << "\ndefine i8* @List_pop(%List* %self) {\nentry:\n";
    load_data("List", "%self");
    body_ << "  %n = call i64 @ec_list_len(i8* %data)\n";
    body_ << "  %nm1 = sub i64 %n, 1\n";
    body_ << "  %item = call i8* @ec_list_get(i8* %data, i64 %nm1)\n";
    body_ << "  %nd = call i8* @ec_list_remove(i8* %data, i64 %nm1)\n";
    store_data("List", "%self", "%nd");
    body_ << "  ret i8* %item\n}\n";

    body_ << "\ndefine void @List_prepend(%List* %self, i8* %item) {\nentry:\n";
    load_data("List", "%self");
    body_ << "  %nd = call i8* @ec_list_insert(i8* %data, i64 0, i8* %item)\n";
    store_data("List", "%self", "%nd");
    body_ << "  ret void\n}\n";

    body_ << "\ndefine void @List_clear(%List* %self) {\nentry:\n";
    body_ << "  %d = call i8* @ec_list_new()\n";
    body_ << "  %gep = getelementptr inbounds %List, %List* %self, i32 0, i32 1\n";
    body_ << "  store i8* %d, i8** %gep\n";
    body_ << "  ret void\n}\n";

    // List_keys — returns a List containing its own elements (for compatibility)
    body_ << "\ndefine i8* @List_keys(%List* %self) {\nentry:\n";
    load_data("List", "%self");
    body_ << "  ret i8* %data\n}\n";

    // ── Map ───────────────────────────────────────────────────────────────────

    body_ << "\ndefine void @Map_init(%Map* %self) {\nentry:\n";
    store_vtable("Map", "%self");
    body_ << "  %d = call i8* @ec_map_new()\n";
    body_ << "  %gep = getelementptr inbounds %Map, %Map* %self, i32 0, i32 1\n";
    body_ << "  store i8* %d, i8** %gep\n";
    body_ << "  ret void\n}\n";

    body_ << "\ndefine void @Map_set(%Map* %self, i8* %key, i8* %val) {\nentry:\n";
    load_data("Map", "%self");
    body_ << "  %nd = call i8* @ec_map_set(i8* %data, i8* %key, i8* %val)\n";
    store_data("Map", "%self", "%nd");
    body_ << "  ret void\n}\n";

    body_ << "\ndefine i8* @Map_get(%Map* %self, i8* %key) {\nentry:\n";
    load_data("Map", "%self");
    body_ << "  %r = call i8* @ec_map_get(i8* %data, i8* %key)\n";
    body_ << "  ret i8* %r\n}\n";

    body_ << "\ndefine i32 @Map_has(%Map* %self, i8* %key) {\nentry:\n";
    load_data("Map", "%self");
    body_ << "  %r = call i32 @ec_map_has(i8* %data, i8* %key)\n";
    body_ << "  ret i32 %r\n}\n";

    body_ << "\ndefine void @Map_remove(%Map* %self, i8* %key) {\nentry:\n";
    load_data("Map", "%self");
    body_ << "  %nd = call i8* @ec_map_delete(i8* %data, i8* %key)\n";
    store_data("Map", "%self", "%nd");
    body_ << "  ret void\n}\n";

    body_ << "\ndefine i64 @Map_count(%Map* %self) {\nentry:\n";
    load_data("Map", "%self");
    body_ << "  %r = call i64 @ec_map_count(i8* %data)\n";
    body_ << "  ret i64 %r\n}\n";

    body_ << "\ndefine i1 @Map_isEmpty(%Map* %self) {\nentry:\n";
    load_data("Map", "%self");
    body_ << "  %r = call i64 @ec_map_count(i8* %data)\n";
    body_ << "  %b = icmp eq i64 %r, 0\n";
    body_ << "  ret i1 %b\n}\n";

    body_ << "\ndefine i8* @Map_keys(%Map* %self) {\nentry:\n";
    load_data("Map", "%self");
    body_ << "  %r = call i8* @ec_map_keys(i8* %data)\n";
    body_ << "  ret i8* %r\n}\n";

    body_ << "\ndefine i8* @Map_values(%Map* %self) {\nentry:\n";
    load_data("Map", "%self");
    body_ << "  %r = call i8* @ec_map_values(i8* %data)\n";
    body_ << "  ret i8* %r\n}\n";

    body_ << "\ndefine i8* @Map_getOrDefault(%Map* %self, i8* %key, i8* %def) {\nentry:\n";
    load_data("Map", "%self");
    body_ << "  %has = call i32 @ec_map_has(i8* %data, i8* %key)\n";
    body_ << "  %b = icmp ne i32 %has, 0\n";
    body_ << "  br i1 %b, label %has_it, label %use_def\n";
    body_ << "has_it:\n";
    body_ << "  %r = call i8* @ec_map_get(i8* %data, i8* %key)\n";
    body_ << "  ret i8* %r\n";
    body_ << "use_def:\n";
    body_ << "  ret i8* %def\n}\n";

    // ── Set ───────────────────────────────────────────────────────────────────

    body_ << "\ndefine void @Set_init(%Set* %self) {\nentry:\n";
    store_vtable("Set", "%self");
    body_ << "  %d = call i8* @ec_set_new()\n";
    body_ << "  %gep = getelementptr inbounds %Set, %Set* %self, i32 0, i32 1\n";
    body_ << "  store i8* %d, i8** %gep\n";
    body_ << "  ret void\n}\n";

    body_ << "\ndefine void @Set_add(%Set* %self, i8* %item) {\nentry:\n";
    load_data("Set", "%self");
    body_ << "  %nd = call i8* @ec_set_add(i8* %data, i8* %item)\n";
    store_data("Set", "%self", "%nd");
    body_ << "  ret void\n}\n";

    body_ << "\ndefine i32 @Set_has(%Set* %self, i8* %item) {\nentry:\n";
    load_data("Set", "%self");
    body_ << "  %r = call i32 @ec_set_has(i8* %data, i8* %item)\n";
    body_ << "  ret i32 %r\n}\n";

    body_ << "\ndefine void @Set_remove(%Set* %self, i8* %item) {\nentry:\n";
    load_data("Set", "%self");
    body_ << "  %nd = call i8* @ec_set_remove(i8* %data, i8* %item)\n";
    store_data("Set", "%self", "%nd");
    body_ << "  ret void\n}\n";

    body_ << "\ndefine i64 @Set_count(%Set* %self) {\nentry:\n";
    load_data("Set", "%self");
    body_ << "  %r = call i64 @ec_set_count(i8* %data)\n";
    body_ << "  ret i64 %r\n}\n";

    body_ << "\ndefine i1 @Set_isEmpty(%Set* %self) {\nentry:\n";
    load_data("Set", "%self");
    body_ << "  %r = call i64 @ec_set_count(i8* %data)\n";
    body_ << "  %b = icmp eq i64 %r, 0\n";
    body_ << "  ret i1 %b\n}\n";

    body_ << "\ndefine i8* @Set_keys(%Set* %self) {\nentry:\n";
    load_data("Set", "%self");
    body_ << "  %r = call i8* @ec_set_keys(i8* %data)\n";
    body_ << "  ret i8* %r\n}\n";

    // ── StringBuilder ─────────────────────────────────────────────────────────

    body_ << "\ndefine void @StringBuilder_init(%StringBuilder* %self) {\nentry:\n";
    store_vtable("StringBuilder", "%self");
    body_ << "  %d = call i8* @ec_sb_new()\n";
    body_ << "  %gep = getelementptr inbounds %StringBuilder, %StringBuilder* %self, i32 0, i32 1\n";
    body_ << "  store i8* %d, i8** %gep\n";
    body_ << "  ret void\n}\n";

    body_ << "\ndefine %StringBuilder* @StringBuilder_append(%StringBuilder* %self, i8* %s) {\nentry:\n";
    load_data("StringBuilder", "%self");
    body_ << "  %nd = call i8* @ec_sb_append_str(i8* %data, i8* %s)\n";
    store_data("StringBuilder", "%self", "%nd");
    body_ << "  ret %StringBuilder* %self\n}\n";

    body_ << "\ndefine %StringBuilder* @StringBuilder_appendChar(%StringBuilder* %self, i8 %c) {\nentry:\n";
    load_data("StringBuilder", "%self");
    body_ << "  %nd = call i8* @ec_sb_append_char(i8* %data, i8 %c)\n";
    store_data("StringBuilder", "%self", "%nd");
    body_ << "  ret %StringBuilder* %self\n}\n";

    body_ << "\ndefine %StringBuilder* @StringBuilder_appendInt(%StringBuilder* %self, i32 %n) {\nentry:\n";
    load_data("StringBuilder", "%self");
    body_ << "  %nd = call i8* @ec_sb_append_int(i8* %data, i32 %n)\n";
    store_data("StringBuilder", "%self", "%nd");
    body_ << "  ret %StringBuilder* %self\n}\n";

    body_ << "\ndefine %StringBuilder* @StringBuilder_appendLong(%StringBuilder* %self, i64 %n) {\nentry:\n";
    load_data("StringBuilder", "%self");
    body_ << "  %nd = call i8* @ec_sb_append_long(i8* %data, i64 %n)\n";
    store_data("StringBuilder", "%self", "%nd");
    body_ << "  ret %StringBuilder* %self\n}\n";

    body_ << "\ndefine %StringBuilder* @StringBuilder_appendBool(%StringBuilder* %self, i1 %b) {\nentry:\n";
    load_data("StringBuilder", "%self");
    body_ << "  %bext = zext i1 %b to i32\n";
    body_ << "  %bs = call i8* @ec_int_to_str(i32 %bext)\n";
    body_ << "  %nd = call i8* @ec_sb_append_str(i8* %data, i8* %bs)\n";
    store_data("StringBuilder", "%self", "%nd");
    body_ << "  ret %StringBuilder* %self\n}\n";

    body_ << "\ndefine %StringBuilder* @StringBuilder_appendLine(%StringBuilder* %self, i8* %s) {\nentry:\n";
    load_data("StringBuilder", "%self");
    body_ << "  %nd1 = call i8* @ec_sb_append_str(i8* %data, i8* %s)\n";
    body_ << "  %nd2 = call i8* @ec_sb_append_char(i8* %nd1, i8 10)\n";
    store_data("StringBuilder", "%self", "%nd2");
    body_ << "  ret %StringBuilder* %self\n}\n";

    body_ << "\ndefine i8* @StringBuilder_toString(%StringBuilder* %self) {\nentry:\n";
    load_data("StringBuilder", "%self");
    body_ << "  %r = call i8* @ec_sb_to_string(i8* %data)\n";
    body_ << "  ret i8* %r\n}\n";

    body_ << "\ndefine %StringBuilder* @StringBuilder_clear(%StringBuilder* %self) {\nentry:\n";
    load_data("StringBuilder", "%self");
    body_ << "  %nd = call i8* @ec_sb_clear(i8* %data)\n";
    store_data("StringBuilder", "%self", "%nd");
    body_ << "  ret %StringBuilder* %self\n}\n";

    body_ << "\ndefine i64 @StringBuilder_length(%StringBuilder* %self) {\nentry:\n";
    load_data("StringBuilder", "%self");
    body_ << "  %r = call i64 @ec_sb_length(i8* %data)\n";
    body_ << "  ret i64 %r\n}\n";

    body_ << "\ndefine i1 @StringBuilder_isEmpty(%StringBuilder* %self) {\nentry:\n";
    load_data("StringBuilder", "%self");
    body_ << "  %r = call i64 @ec_sb_length(i8* %data)\n";
    body_ << "  %b = icmp eq i64 %r, 0\n";
    body_ << "  ret i1 %b\n}\n";
}

// ─────────────────────────────────────────────────────────────────────────────
//  Top-level generate
// ─────────────────────────────────────────────────────────────────────────────
std::string CodeGen::generate(const Program& program) {
    emitPreamble();

    // Pass 0: build class layouts so field indices are known
    for (const auto& d : program.declarations)
        if (d->kind == DeclKind::Class)
            buildClassLayout(static_cast<const ClassDecl&>(*d));

    // Pass 0.25: build vtable slot assignments for all classes
    for (const auto& d : program.declarations)
        if (d->kind == DeclKind::Class)
            buildVtable(static_cast<const ClassDecl&>(*d), program);

    // Pre-register built-in collection method return types
    // (List, Map, Set, StringBuilder are class wrappers around _native_* calls;
    // their class definitions may not be in program.declarations since
    // std.collections is loaded on demand.  Register their known signatures here.)
    {
        // List methods
        userFnRetTypes_["List_get"]      = "i8*";
        userFnRetTypes_["List_last"]     = "i8*";
        userFnRetTypes_["List_pop"]      = "i8*";
        userFnRetTypes_["List_add"]      = "i8*";
        userFnRetTypes_["List_set"]      = "i8*";
        userFnRetTypes_["List_remove"]   = "i8*";
        userFnRetTypes_["List_insert"]   = "i8*";
        userFnRetTypes_["List_slice"]    = "i8*";
        userFnRetTypes_["List_concat"]   = "i8*";
        userFnRetTypes_["List_keys"]     = "i8*";
        userFnRetTypes_["List_length"]   = "i32";
        userFnRetTypes_["List_size"]     = "i32";
        userFnRetTypes_["List_indexOf"]  = "i64";
        userFnRetTypes_["List_contains"] = "i32";
        userFnRetTypes_["List_isEmpty"]  = "i1";
        userFnRetTypes_["List_init"]     = "void";
        userFnRetTypes_["List_clear"]    = "void";
        userFnRetTypes_["List_prepend"]  = "void";
        userFnRetTypes_["List_addAll"]   = "void";
        // Map methods
        userFnRetTypes_["Map_get"]           = "i8*";
        userFnRetTypes_["Map_set"]           = "i8*";
        userFnRetTypes_["Map_delete"]        = "i8*";
        userFnRetTypes_["Map_keys"]          = "i8*";
        userFnRetTypes_["Map_values"]        = "i8*";
        userFnRetTypes_["Map_getOrDefault"]  = "i8*";
        userFnRetTypes_["Map_has"]           = "i32";
        userFnRetTypes_["Map_count"]         = "i64";
        userFnRetTypes_["Map_isEmpty"]       = "i1";
        userFnRetTypes_["Map_init"]          = "void";
        userFnRetTypes_["Map_clear"]         = "void";
        // Set methods
        userFnRetTypes_["Set_add"]     = "i8*";
        userFnRetTypes_["Set_remove"]  = "i8*";
        userFnRetTypes_["Set_keys"]    = "i8*";
        userFnRetTypes_["Set_union"]   = "i8*";
        userFnRetTypes_["Set_has"]     = "i32";
        userFnRetTypes_["Set_count"]   = "i64";
        userFnRetTypes_["Set_isEmpty"] = "i1";
        userFnRetTypes_["Set_init"]    = "void";
        // StringBuilder methods
        userFnRetTypes_["StringBuilder_append"]      = "i8*";
        userFnRetTypes_["StringBuilder_appendChar"]  = "i8*";
        userFnRetTypes_["StringBuilder_appendInt"]   = "i8*";
        userFnRetTypes_["StringBuilder_appendLong"]  = "i8*";
        userFnRetTypes_["StringBuilder_appendBool"]  = "i8*";
        userFnRetTypes_["StringBuilder_appendLine"]  = "i8*";
        userFnRetTypes_["StringBuilder_toString"]    = "i8*";
        userFnRetTypes_["StringBuilder_clear"]       = "i8*";
        userFnRetTypes_["StringBuilder_length"]      = "i64";
        userFnRetTypes_["StringBuilder_isEmpty"]     = "i1";
        userFnRetTypes_["StringBuilder_init"]        = "void";
    }

    // Pass 0.5: pre-register all function return types so forward calls resolve
    for (const auto& d : program.declarations) {
        if (d->kind == DeclKind::Function) {
            const auto& fn = static_cast<const FunctionDecl&>(*d);\
            userFnRetTypes_[fn.name] = llvmType(fn.returnType);
        }
        if (d->kind == DeclKind::Class) {
            const auto& cls = static_cast<const ClassDecl&>(*d);
            for (const auto& m : cls.methods) {
                std::string mangled = cls.name + "_" + m->name;
                userFnRetTypes_[mangled] = llvmType(m->returnType);
            }
        }
    }

    // Pass 1: emit type declarations (structs with vtptr at field 0)
    emitTypeDecls(program);

    // Pass 2: emit runtime declarations
    emitRuntime();

    // Pass 2.5: emit vtable global arrays (after runtime, before fn bodies)
    emitVtableGlobals(program);

    // Pass 2.75: emit collection class method stubs
    emitCollectionStubs();

    // Pass 3: emit all declarations
    for (const auto& d : program.declarations)
        emitDecl(*d);

    // Collect everything
    std::string result = globals_.str() + body_.str();
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Preamble
// ─────────────────────────────────────────────────────────────────────────────
void CodeGen::emitPreamble() {
    globals_ << "; Generated by ExtendedC compiler\n";
    globals_ << "source_filename = \"" << moduleName_ << "\"\n";
    globals_ << "target triple = \"x86_64-unknown-linux-gnu\"\n\n";

    // Forward-declare built-in collection struct types.
    // These classes (List, Map, Set, StringBuilder) are defined in std.collections
    // but that file is not code-generated (it exists only for type-checking).
    // Their layout is: vtptr (i8**) + one private var data field (i8*).
    // Emit them here so any user code that holds a List/Map/Set/StringBuilder
    // field in a class can reference the struct type.
    globals_ << "; ── Built-in collection type declarations ──────────────────\n";
    globals_ << "%List          = type { i8**, i8* }\n";
    globals_ << "%Map           = type { i8**, i8* }\n";
    globals_ << "%Set           = type { i8**, i8* }\n";
    globals_ << "%StringBuilder = type { i8**, i8* }\n";
    globals_ << "\n";
}

// ─────────────────────────────────────────────────────────────────────────────
//  Runtime declarations (map stdlib functions → C library / our runtime.c)
// ─────────────────────────────────────────────────────────────────────────────
void CodeGen::emitRuntime() {
    globals_ << "; ── Runtime / stdlib declarations ─────────────────────────\n";
    // C stdio
    globals_ << "declare i32 @putchar(i32)\n";
    globals_ << "declare i32 @getchar()\n";
    globals_ << "declare i32 @printf(i8*, ...)\n";
    globals_ << "declare i32 @scanf(i8*, ...)\n";
    globals_ << "declare i8* @fgets(i8*, i32, i8*)\n";
    globals_ << "declare i8* @malloc(i64)\n";
    globals_ << "declare void @free(i8*)\n";
    globals_ << "declare i64 @strlen(i8*)\n";
    globals_ << "declare i8* @strcpy(i8*, i8*)\n";
    globals_ << "declare i8* @strcat(i8*, i8*)\n";
    globals_ << "declare i32 @strcmp(i8*, i8*)\n";
    globals_ << "declare i32 @atoi(i8*)\n";
    globals_ << "declare double @atof(i8*)\n";
    globals_ << "declare double @sqrt(double)\n";
    globals_ << "declare double @pow(double, double)\n";
    globals_ << "declare double @sin(double)\n";
    globals_ << "declare double @cos(double)\n";
    globals_ << "declare double @floor(double)\n";
    globals_ << "declare double @ceil(double)\n";
    globals_ << "declare double @fabs(double)\n";
    globals_ << "declare double @log(double)\n";

    // our runtime.c helpers
    globals_ << "declare i8* @ec_strconcat(i8*, i8*)\n";
    globals_ << "declare i8* @ec_int_to_str(i32)\n";
    globals_ << "declare i8* @ec_float_to_str(float)\n";
    globals_ << "declare i8* @ec_char_to_str(i8)\n";
    globals_ << "declare i32 @ec_str_len(i8*)\n";
    globals_ << "declare i8 @ec_char_at(i8*, i32)\n";
    globals_ << "declare i8* @ec_substring(i8*, i32, i32)\n";
    globals_ << "declare i8* @ec_readline()\n";
    globals_ << "declare void @ec_println(i8*)\n";
    globals_ << "declare void @ec_print(i8*)\n";
    globals_ << "declare i8* @ec_typeof(i8*)\n";

    // GC-aware allocation / deallocation
    globals_ << "declare i8* @ec_new(i64)\n";
    globals_ << "declare void @ec_delete(i8*)\n";

    // Array runtime helpers
    globals_ << "declare i8* @ec_array_new(i64, i64)\n"; // elem_size, length → array ptr
    globals_ << "declare i64 @ec_array_len(i8*)\n";      // array ptr → length

    // Extended string operations
    globals_ << "declare i32 @ec_str_find(i8*, i8*)\n";
    globals_ << "declare i32 @ec_str_find_last(i8*, i8*)\n";
    globals_ << "declare i32 @ec_str_starts_with(i8*, i8*)\n";
    globals_ << "declare i32 @ec_str_ends_with(i8*, i8*)\n";
    globals_ << "declare i32 @ec_str_contains(i8*, i8*)\n";
    globals_ << "declare i32 @ec_str_equals(i8*, i8*)\n";
    globals_ << "declare i32 @ec_str_compare(i8*, i8*)\n";
    globals_ << "declare i8* @ec_str_to_lower(i8*)\n";
    globals_ << "declare i8* @ec_str_to_upper(i8*)\n";
    globals_ << "declare i8* @ec_str_trim(i8*)\n";
    globals_ << "declare i8* @ec_str_trim_left(i8*)\n";
    globals_ << "declare i8* @ec_str_trim_right(i8*)\n";
    globals_ << "declare i8* @ec_str_repeat(i8*, i32)\n";
    globals_ << "declare i8* @ec_str_replace(i8*, i8*, i8*)\n";
    globals_ << "declare i8* @ec_str_reverse(i8*)\n";
    globals_ << "declare i8* @ec_str_split(i8*, i8*)\n";
    globals_ << "declare i8* @ec_str_join(i8*, i8*)\n";
    globals_ << "declare i32 @ec_str_count(i8*, i8*)\n";

    // Char operations
    globals_ << "declare i32 @ec_char_is_digit(i8)\n";
    globals_ << "declare i32 @ec_char_is_alpha(i8)\n";
    globals_ << "declare i32 @ec_char_is_alnum(i8)\n";
    globals_ << "declare i32 @ec_char_is_space(i8)\n";
    globals_ << "declare i32 @ec_char_is_upper(i8)\n";
    globals_ << "declare i32 @ec_char_is_lower(i8)\n";
    globals_ << "declare i32 @ec_char_is_punct(i8)\n";
    globals_ << "declare i8  @ec_char_to_lower_fn(i8)\n";
    globals_ << "declare i8  @ec_char_to_upper_fn(i8)\n";
    globals_ << "declare i32 @ec_char_code(i8)\n";
    globals_ << "declare i8  @ec_char_from_code(i32)\n";

    // Extended math
    globals_ << "declare i32    @ec_abs_int(i32)\n";
    globals_ << "declare i64    @ec_abs_long(i64)\n";
    globals_ << "declare i32    @ec_min_int(i32, i32)\n";
    globals_ << "declare i32    @ec_max_int(i32, i32)\n";
    globals_ << "declare i64    @ec_min_long(i64, i64)\n";
    globals_ << "declare i64    @ec_max_long(i64, i64)\n";
    globals_ << "declare double @ec_min_double(double, double)\n";
    globals_ << "declare double @ec_max_double(double, double)\n";
    globals_ << "declare i32    @ec_clamp_int(i32, i32, i32)\n";
    globals_ << "declare double @ec_clamp_double(double, double, double)\n";
    globals_ << "declare i32    @ec_gcd(i32, i32)\n";
    globals_ << "declare i32    @ec_lcm(i32, i32)\n";
    globals_ << "declare i32    @ec_is_even(i32)\n";
    globals_ << "declare i32    @ec_is_odd(i32)\n";
    globals_ << "declare i32    @ec_is_prime(i32)\n";
    globals_ << "declare i32    @ec_pow_int(i32, i32)\n";
    globals_ << "declare double @ec_cbrt_fn(double)\n";
    globals_ << "declare double @ec_hypot_fn(double, double)\n";
    globals_ << "declare double @ec_log2_fn(double)\n";
    globals_ << "declare double @ec_log10_fn(double)\n";
    globals_ << "declare double @ec_atan2_fn(double, double)\n";
    globals_ << "declare double @ec_infinity()\n";
    globals_ << "declare double @ec_nan()\n";

    // List
    globals_ << "declare i8* @ec_list_new()\n";
    globals_ << "declare i64  @ec_list_len(i8*)\n";
    globals_ << "declare i8*  @ec_list_get(i8*, i64)\n";
    globals_ << "declare i8*  @ec_list_add(i8*, i8*)\n";
    globals_ << "declare i8*  @ec_list_set(i8*, i64, i8*)\n";
    globals_ << "declare i8*  @ec_list_remove(i8*, i64)\n";
    globals_ << "declare i8*  @ec_list_insert(i8*, i64, i8*)\n";
    globals_ << "declare i64  @ec_list_index_of(i8*, i8*)\n";
    globals_ << "declare i32  @ec_list_contains(i8*, i8*)\n";
    globals_ << "declare i8*  @ec_list_slice(i8*, i64, i64)\n";
    globals_ << "declare i8*  @ec_list_concat(i8*, i8*)\n";

    // Map
    globals_ << "declare i8* @ec_map_new()\n";
    globals_ << "declare i8* @ec_map_set(i8*, i8*, i8*)\n";
    globals_ << "declare i8* @ec_map_get(i8*, i8*)\n";
    globals_ << "declare i32 @ec_map_has(i8*, i8*)\n";
    globals_ << "declare i8* @ec_map_delete(i8*, i8*)\n";
    globals_ << "declare i64 @ec_map_count(i8*)\n";
    globals_ << "declare i8* @ec_map_keys(i8*)\n";
    globals_ << "declare i8* @ec_map_values(i8*)\n";

    // Set
    globals_ << "declare i8* @ec_set_new()\n";
    globals_ << "declare i8* @ec_set_add(i8*, i8*)\n";
    globals_ << "declare i32 @ec_set_has(i8*, i8*)\n";
    globals_ << "declare i8* @ec_set_remove(i8*, i8*)\n";
    globals_ << "declare i64 @ec_set_count(i8*)\n";
    globals_ << "declare i8* @ec_set_keys(i8*)\n";

    // StringBuilder
    globals_ << "declare i8* @ec_sb_new()\n";
    globals_ << "declare i8* @ec_sb_append_str(i8*, i8*)\n";
    globals_ << "declare i8* @ec_sb_append_char(i8*, i8)\n";
    globals_ << "declare i8* @ec_sb_append_int(i8*, i32)\n";
    globals_ << "declare i8* @ec_sb_append_long(i8*, i64)\n";
    globals_ << "declare i8* @ec_sb_to_string(i8*)\n";
    globals_ << "declare i64  @ec_sb_length(i8*)\n";
    globals_ << "declare i8* @ec_sb_clear(i8*)\n";

    // System
    globals_ << "declare void @ec_exit(i32)\n";
    globals_ << "declare i64  @ec_time_ms()\n";
    globals_ << "declare i32  @atol(i8*)\n";

    // GC control API
    globals_ << "declare void @ec_gc_init()\n";
    globals_ << "declare void @ec_gc_collect()\n";
    globals_ << "declare void @ec_gc_register_global(i8**)\n";
    globals_ << "declare void @ec_gc_push_root(i8*)\n";
    globals_ << "declare void @ec_gc_pop_root()\n";
    globals_ << "declare void @ec_gc_pin(i8*)\n";
    globals_ << "declare void @ec_gc_unpin(i8*)\n";
    globals_ << "declare i8* @ec_gc_stats()\n";
    globals_ << "declare i64 @ec_gc_live_bytes()\n";
    globals_ << "declare i64 @ec_gc_live_objects()\n";
    globals_ << "declare i64 @ec_gc_num_collections()\n";

    // File I/O
    globals_ << "declare i8* @ec_read_file(i8*)\n";
    globals_ << "declare i32 @ec_write_file(i8*, i8*)\n";
    globals_ << "declare i32 @ec_file_exists(i8*)\n";

    // exception support — caller-owns-jmpbuf design:
    globals_ << "; setjmp — returns_twice tells LLVM it can return more than once\n";
    globals_ << "declare i32 @setjmp(i8*) returns_twice\n";
    globals_ << "declare void @ec_try_push(i8*)\n";
    globals_ << "declare void @ec_try_pop()\n";
    globals_ << "declare void @ec_throw(i8*)\n";
    globals_ << "declare i8* @ec_catch_msg()\n";
    globals_ << "declare void @ec_try_end()\n";
    globals_ << "\n";

    // stdin global
    globals_ << "@stdin = external global i8*\n\n";

    // Terminal / editor primitives (for ecvim.ec)
    globals_ << "declare void @ec_term_raw()\n";
    globals_ << "declare void @ec_term_restore()\n";
    globals_ << "declare i32  @ec_term_rows()\n";
    globals_ << "declare i32  @ec_term_cols()\n";
    globals_ << "declare i32  @ec_read_key()\n";
    globals_ << "declare void @ec_write_raw(i8*)\n";
    globals_ << "declare void @ec_flush()\n";
    globals_ << "declare i32  @ec_system(i8*)\n";
    globals_ << "declare void @ec_atexit_restore()\n";
    globals_ << "declare void @ec_register_winch()\n";
    globals_ << "declare i32  @ec_winch_pending()\n";

    // std.time primitives
    globals_ << "declare i64  @ec_time_wallclock_ms()\n";
    globals_ << "declare i32  @ec_time_year(i64)\n";
    globals_ << "declare i32  @ec_time_month(i64)\n";
    globals_ << "declare i32  @ec_time_day(i64)\n";
    globals_ << "declare i32  @ec_time_hour(i64)\n";
    globals_ << "declare i32  @ec_time_minute(i64)\n";
    globals_ << "declare i32  @ec_time_second(i64)\n";
    globals_ << "declare i32  @ec_time_weekday(i64)\n";
    globals_ << "declare i32  @ec_time_yearday(i64)\n";
    globals_ << "declare i8*  @ec_time_format(i64, i8*)\n";
    globals_ << "declare i32  @ec_time_year_local(i64)\n";
    globals_ << "declare i32  @ec_time_month_local(i64)\n";
    globals_ << "declare i32  @ec_time_day_local(i64)\n";
    globals_ << "declare i32  @ec_time_hour_local(i64)\n";
    globals_ << "declare i32  @ec_time_minute_local(i64)\n";
    globals_ << "declare i32  @ec_time_second_local(i64)\n";
    globals_ << "declare i8*  @ec_time_format_local(i64, i8*)\n";
    // std.algorithm native helpers
    globals_ << "declare void @ec_list_sort_fn(i8*, i8*, i8*)\n";
    globals_ << "declare void @ec_list_reverse_fn(i8*)\n";
    globals_ << "declare i64  @ec_list_binary_search(i8*, i8*, i8*, i8*)\n";
    globals_ << "declare void @ec_list_shuffle(i8*)\n";
    globals_ << "declare i64  @ec_list_sum_fn(i8*, i8*, i8*)\n";
    globals_ << "declare void @ec_random_seed(i32)\n";
    globals_ << "declare i32  @ec_random_int(i32, i32)\n";
    globals_ << "declare double @ec_random_double()\n";

    // Threading / async primitives
    globals_ << "declare i8* @ec_future_new(i8*, i8*)\n";  // fn_ptr, env_ptr → Future
    globals_ << "declare i8* @ec_future_await(i8*)\n";     // Future → result (i8*)
    globals_ << "declare i32 @ec_future_done(i8*)\n";      // Future → bool
    globals_ << "declare i8* @ec_mutex_new()\n";
    globals_ << "declare void @ec_mutex_lock(i8*)\n";
    globals_ << "declare void @ec_mutex_unlock(i8*)\n";
    globals_ << "declare i32 @ec_mutex_trylock(i8*)\n";
    globals_ << "declare void @ec_mutex_destroy(i8*)\n";
    globals_ << "declare void @ec_thread_sleep_ms(i32)\n";
    globals_ << "declare void @ec_thread_yield()\n";
}

// ─────────────────────────────────────────────────────────────────────────────
//  Type declarations
// ─────────────────────────────────────────────────────────────────────────────
void CodeGen::emitTypeDecls(const Program& program) {
    globals_ << "; ── Type declarations ──────────────────────────────────────\n";
    for (const auto& d : program.declarations) {
        if (d->kind == DeclKind::Struct) {
            const auto& s = static_cast<const StructDecl&>(*d);
            globals_ << "%" << s.name << " = type { ";
            for (std::size_t i = 0; i < s.fields.size(); ++i) {
                if (i) globals_ << ", ";
                globals_ << llvmType(s.fields[i].type);
            }
            globals_ << " }\n";
        } else if (d->kind == DeclKind::Union) {
            const auto& u = static_cast<const UnionDecl&>(*d);
            // Union: use the largest field type, pad to alignment
            // Simple: just use an i8 array sized to the largest member
            std::size_t maxSz = 4; // at least i32
            for (const auto& f : u.fields) {
                std::string lt = llvmType(f.type);
                std::size_t sz = (lt=="i64"||lt=="double") ? 8 :
                                 (lt=="i8*") ? 8 : 4;
                if (sz > maxSz) maxSz = sz;
            }
            globals_ << "%" << u.name << " = type { [" << maxSz << " x i8] }\n";
        } else if (d->kind == DeclKind::Class) {
            const auto& c = static_cast<const ClassDecl&>(*d);
            auto it = classLayouts_.find(c.name);
            globals_ << "%" << c.name << " = type { ";
            // Field 0: vtable pointer i8**
            globals_ << "i8**";
            if (it != classLayouts_.end() && !it->second.fieldTypes.empty()) {
                for (std::size_t i = 0; i < it->second.fieldTypes.size(); ++i)
                    globals_ << ", " << it->second.fieldTypes[i];
            }
            globals_ << " }\n";
        }
    }
    globals_ << "\n";
}

// ─────────────────────────────────────────────────────────────────────────────
//  Declaration dispatch
// ─────────────────────────────────────────────────────────────────────────────
void CodeGen::emitDecl(const Decl& decl) {
    switch (decl.kind) {
    case DeclKind::Import:   break; // handled by type checker
    case DeclKind::Using:    emitUsingDecl(static_cast<const UsingDecl&>(decl)); break;
    case DeclKind::Enum:     emitEnumDecl(static_cast<const EnumDecl&>(decl));     break;
    case DeclKind::Struct:   emitStructDecl(static_cast<const StructDecl&>(decl)); break;
    case DeclKind::Union:    emitUnionDecl(static_cast<const UnionDecl&>(decl));   break;
    case DeclKind::Class:    emitClassDecl(static_cast<const ClassDecl&>(decl));   break;
    case DeclKind::Function: emitFunctionDecl(static_cast<const FunctionDecl&>(decl)); break;
    case DeclKind::Variable: emitGlobalVar(static_cast<const VarDecl&>(decl));     break;
    }
}

void CodeGen::emitUsingDecl(const UsingDecl& decl) {
    // Type aliases are resolved at compile time — emit a comment for readability
    globals_ << "; using " << decl.alias << " = " << decl.target.name << "\n";
    // Register the alias in the LLVM type system as a synonym
    // (no separate LLVM struct needed; references just use the target type)
}

void CodeGen::emitEnumDecl(const EnumDecl& decl) {
    globals_ << "; enum " << decl.name << "\n";
    for (int i = 0; i < (int)decl.members.size(); ++i) {
        enumValues_[decl.name + "::" + decl.members[i]] = i;
        enumValues_[decl.members[i]] = i;
        globals_ << "@" << decl.name << "." << decl.members[i]
                 << " = private constant i32 " << i << "\n";
    }
    globals_ << "\n";
}

void CodeGen::emitStructDecl(const StructDecl&) {
    // Type already emitted in emitTypeDecls; nothing more needed here
}

void CodeGen::emitUnionDecl(const UnionDecl&) {
    // Same
}

void CodeGen::emitClassDecl(const ClassDecl& decl) {
    globals_ << "; class " << decl.name << "\n";
    currentClass_ = decl.name;
    for (const auto& m : decl.methods)
        emitFunctionDecl(*m, decl.name);
    currentClass_ = "";
}

void CodeGen::emitGlobalVar(const VarDecl& decl) {
    std::string lt = llvmType(decl.type);
    globals_ << "@" << decl.name << " = global " << lt
             << " " << llvmDefault(lt) << "\n";
    globalVars_[decl.name] = lt;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Async function emission
//
//  async fun RetType name(params) { body }
//
//  Generates two LLVM functions:
//
//  1. @name_async_body(i8* %env_raw)  → i8*
//     The actual function body, wrapped as a closure thunk.
//     Parameters are packed into a GC env struct before spawning.
//
//  2. @name(params) → i8*   (Future)
//     Packs params into an env struct, calls ec_future_new(@name_async_body, env),
//     and returns the Future i8* immediately.
//
//  Call site:  var f = asyncFn(args);   // returns Future immediately
//              var r = await f;          // blocks, returns result as i8*
// ─────────────────────────────────────────────────────────────────────────────
void CodeGen::emitAsyncFunctionDecl(const FunctionDecl& decl,
                                    const std::string& irName,
                                    const std::string& retLLVM,
                                    const std::string& ownerClass) {
    std::string bodyName = irName + "_async_body";
    std::string envName  = irName + "_async_env";

    // ── Emit env struct type ─────────────────────────────────────────────────
    // Packs all parameters (and self if method) into an i8* GC block.
    // %irName_async_env = type { [self_type,] param0_type, param1_type, ... }
    out_ = &globals_;
    globals_ << "%" << envName << " = type { ";
    bool firstField = true;
    if (!ownerClass.empty()) {
        globals_ << "%" << ownerClass << "*";
        firstField = false;
    }
    for (const auto& p : decl.params) {
        if (!firstField) globals_ << ", ";
        globals_ << llvmType(p.type);
        firstField = false;
    }
    globals_ << " }\n";

    // ── Emit body thunk: @irName_async_body(i8* %env_raw) → i8* ─────────────
    out_ = &body_;
    userFnRetTypes_[bodyName] = "i8*";  // thunk always returns i8*

    body_ << "\ndefine private i8* @" << bodyName << "(i8* %env_raw) {\n";
    body_ << "entry:\n";

    FnCtx bodyCtx;
    bodyCtx.name       = bodyName;
    bodyCtx.retType    = "i8*";
    bodyCtx.hasVoidRet = false;
    ctx_ = &bodyCtx;

    // Unpack env struct
    std::string envTyped = freshName("env");
    emit(envTyped + " = bitcast i8* %env_raw to %" + envName + "*");

    int fieldIdx = 0;
    if (!ownerClass.empty()) {
        std::string gep = freshName("self.ep");
        emit(gep + " = getelementptr inbounds %" + envName + ", %" +
             envName + "* " + envTyped + ", i32 0, i32 " + std::to_string(fieldIdx++));
        std::string selfType = "%" + ownerClass + "*";
        std::string val = emitLoad(selfType, gep);
        std::string ptr = emitAlloca(selfType, "self");
        emitStore(selfType, val, selfType + "*", ptr);
        bodyCtx.locals["this"]     = ptr;
        bodyCtx.localTypes["this"] = selfType;
    }
    for (const auto& p : decl.params) {
        std::string lt = llvmType(p.type);
        std::string gep = freshName(p.name + ".ep");
        emit(gep + " = getelementptr inbounds %" + envName + ", %" +
             envName + "* " + envTyped + ", i32 0, i32 " + std::to_string(fieldIdx++));
        std::string val = emitLoad(lt, gep);
        std::string ptr = emitAlloca(lt, p.name);
        emitStore(lt, val, lt + "*", ptr);
        bodyCtx.locals[p.name]     = ptr;
        bodyCtx.localTypes[p.name] = lt;
    }

    // Emit actual body
    std::string savedClass = currentClass_;
    currentClass_ = ownerClass;
    if (decl.body) emitBlock(*decl.body);
    currentClass_ = savedClass;

    // Default return: null (for void async functions)
    if (!bodyCtx.terminated) {
        if (retLLVM == "void" || retLLVM.empty()) {
            emit("ret i8* null");
        } else {
            // Box the default return value as i8*
            emit("ret i8* null");
        }
    }

    body_ << "}\n";
    ctx_ = nullptr;

    // Flush lambdas
    if (lambdas_.tellp() > 0) {
        body_ << lambdas_.str();
        lambdas_.str(""); lambdas_.clear();
    }

    // ── Emit spawner wrapper: @irName(params) → i8* (Future) ─────────────────
    userFnRetTypes_[irName] = "i8*";  // spawner returns Future (i8*)

    std::string params;
    if (!ownerClass.empty()) params = "%" + ownerClass + "* %self";
    for (const auto& p : decl.params) {
        if (!params.empty()) params += ", ";
        params += llvmType(p.type) + " %" + p.name;
    }

    body_ << "\ndefine i8* @" << irName << "(" << params << ") {\n";
    body_ << "entry:\n";

    FnCtx spawnerCtx;
    spawnerCtx.name    = irName;
    spawnerCtx.retType = "i8*";
    ctx_ = &spawnerCtx;

    // Allocate env struct in GC heap
    int numFields = (ownerClass.empty() ? 0 : 1) + (int)decl.params.size();
    int envSize   = std::max(numFields * 8, 8);
    std::string envRaw = freshName("async.env.raw");
    emit(envRaw + " = call i8* @ec_new(i64 " + std::to_string(envSize) + ")");
    std::string envTyped2 = freshName("async.env");
    emit(envTyped2 + " = bitcast i8* " + envRaw + " to %" + envName + "*");

    // Pack parameters into env
    int packIdx = 0;
    if (!ownerClass.empty()) {
        std::string gep = freshName("pack.self");
        emit(gep + " = getelementptr inbounds %" + envName + ", %" +
             envName + "* " + envTyped2 + ", i32 0, i32 " + std::to_string(packIdx++));
        emit("store %" + ownerClass + "* %self, %" + ownerClass + "** " + gep);
    }
    for (const auto& p : decl.params) {
        std::string lt = llvmType(p.type);
        std::string gep = freshName("pack." + p.name);
        emit(gep + " = getelementptr inbounds %" + envName + ", %" +
             envName + "* " + envTyped2 + ", i32 0, i32 " + std::to_string(packIdx++));
        emit("store " + lt + " %" + p.name + ", " + lt + "* " + gep);
    }

    // Get fn_ptr as i8*
    std::string fnPtr = freshName("async.fn");
    emit(fnPtr + " = bitcast i8* (i8*)* @" + bodyName + " to i8*");

    // Spawn: ec_future_new(fn_ptr, env_ptr) → Future i8*
    std::string future = freshName("future");
    emit(future + " = call i8* @ec_future_new(i8* " + fnPtr + ", i8* " + envRaw + ")");
    emit("ret i8* " + future);

    body_ << "}\n";
    ctx_ = nullptr;
    out_ = &globals_;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Function declaration / definition
// ─────────────────────────────────────────────────────────────────────────────
void CodeGen::emitFunctionDecl(const FunctionDecl& decl,
                                const std::string& ownerClass) {
    // Mangle method names:  ClassName_methodName
    std::string irName = ownerClass.empty()
                       ? decl.name
                       : ownerClass + "_" + decl.name;
    // Special: main → @main
    if (irName == "main") irName = "main";

    std::string retLLVM = llvmType(decl.returnType);

    // async functions: emit the real body as a closure-compatible thunk
    // named @irName_async_body, then emit a wrapper @irName that
    // spawns it via ec_future_new and returns the Future (i8*).
    if (decl.isAsync && irName != "main") {
        emitAsyncFunctionDecl(decl, irName, retLLVM, ownerClass);
        return;
    }

    // Build parameter list
    std::string params;
    // Methods get an implicit 'self' first parameter
    if (!ownerClass.empty()) {
        params = "%" + ownerClass + "* %self";
    }
    for (const auto& p : decl.params) {
        if (!params.empty()) params += ", ";
        params += llvmType(p.type) + " %" + p.name;
    }

    // Switch output to body stream
    out_ = &body_;

    // Register return type so callers can look it up
    userFnRetTypes_[irName] = retLLVM;

    // Emit export linkage comment (functions are always externally visible in .ll)
    if (decl.isExport)
        body_ << "; export\n";
    body_ << "\ndefine " << retLLVM << " @" << irName
          << "(" << params << ") {\n";
    body_ << "entry:\n";

    // Set up function context
    FnCtx ctx;
    ctx.name       = irName;
    ctx.retType    = retLLVM;
    ctx.hasVoidRet = (retLLVM == "void");
    ctx_ = &ctx;

    // For main: emit ec_gc_init() first, then register all global object vars
    if (irName == "main") {
        emit("call void @ec_gc_init()");
        for (const auto& [name, lt] : globalVars_) {
            if (!lt.empty() && lt.back() == '*') {
                std::string slot = freshName("gslot");
                emit(slot + " = bitcast " + lt + "* @" + name + " to i8**");
                emit("call void @ec_gc_register_global(i8** " + slot + ")");
            }
        }
    }

    // Spill parameters to allocas (SSA → mutable locals)
    if (!ownerClass.empty()) {
        std::string ptr = emitAlloca("%" + ownerClass + "*", "self");
        emitStore("%" + ownerClass + "*", "%self", "%" + ownerClass + "**", ptr);
        ctx.locals["this"]     = ptr;
        ctx.localTypes["this"] = "%" + ownerClass + "*";
    }
    for (const auto& p : decl.params) {
        std::string lt  = llvmType(p.type);
        std::string ptr = emitAlloca(lt, p.name);
        emitStore(lt, "%" + p.name, lt + "*", ptr);
        ctx.locals[p.name]     = ptr;
        ctx.localTypes[p.name] = lt;
    }

    // Emit body
    if (decl.body)
        emitBlock(*decl.body);

    // Default return if not already terminated
    if (!ctx.terminated) {
        if (ctx.hasVoidRet)
            emit("ret void");
        else
            emit("ret " + retLLVM + " " + llvmDefault(retLLVM));
    }

    body_ << "}\n";
    ctx_ = nullptr;
    out_ = &globals_;

    // Flush any lambda/closure definitions that were queued while processing
    // this function.  They must appear at the top level — never inside a define.
    if (lambdas_.tellp() > 0) {
        body_ << lambdas_.str();
        lambdas_.str("");
        lambdas_.clear();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Statement emission
// ─────────────────────────────────────────────────────────────────────────────
void CodeGen::emitStmt(const Stmt& stmt) {
    switch (stmt.kind) {
    case StmtKind::Block:      emitBlock(static_cast<const BlockStmt&>(stmt));         break;
    case StmtKind::Expression: emitExprStmt(static_cast<const ExprStmt&>(stmt));       break;
    case StmtKind::VarDecl:    emitVarDeclStmt(static_cast<const VarDeclStmt&>(stmt)); break;
    case StmtKind::If:         emitIfStmt(static_cast<const IfStmt&>(stmt));           break;
    case StmtKind::While:      emitWhileStmt(static_cast<const WhileStmt&>(stmt));     break;
    case StmtKind::DoWhile:    emitDoWhileStmt(static_cast<const DoWhileStmt&>(stmt)); break;
    case StmtKind::For:        emitForStmt(static_cast<const ForStmt&>(stmt));         break;
    case StmtKind::Switch:     emitSwitchStmt(static_cast<const SwitchStmt&>(stmt));   break;
    case StmtKind::Return:     emitReturnStmt(static_cast<const ReturnStmt&>(stmt));   break;
    case StmtKind::Throw:      emitThrowStmt(static_cast<const ThrowStmt&>(stmt));     break;
    case StmtKind::Try:        emitTryStmt(static_cast<const TryStmt&>(stmt));         break;
    case StmtKind::With:       emitWithStmt(static_cast<const WithStmt&>(stmt));       break;
    case StmtKind::Break:      emitBreakStmt(static_cast<const BreakStmt&>(stmt));     break;
    case StmtKind::Continue:   emitContinueStmt(static_cast<const ContinueStmt&>(stmt)); break;
    }
}

void CodeGen::emitBlock(const BlockStmt& stmt) {
    for (const auto& s : stmt.stmts) emitStmt(*s);
}

void CodeGen::emitVarDeclStmt(const VarDeclStmt& stmt) {
    std::string lt = llvmType(stmt.type);

    IRValue initVal = {llvmDefault(lt), lt};
    if (stmt.init) {
        initVal = emitExpr(**stmt.init);
        initVal = coerce(initVal, lt == "i8*" ? initVal.type : lt);
        // if var (type inferred), use init type
        if (stmt.type.name == "var") lt = initVal.type;
    }

    std::string ptr = emitAlloca(lt, stmt.name);
    if (initVal.isValid() && !initVal.name.empty())
        emitStore(lt, initVal.name, lt + "*", ptr);

    ctx_->locals[stmt.name]     = ptr;
    ctx_->localTypes[stmt.name] = lt;

    // If the init expression was a closure (lambda), propagate its signature
    // to this variable name so call sites can find it.
    if (stmt.init && (*stmt.init)->kind == ExprKind::Lambda) {
        // The most recently registered lambda signature maps to this variable
        // Find it by scanning closureTypes_ for the latest lambda.N entry
        std::string latestLam;
        int latestId = -1;
        for (const auto& [k, v] : closureTypes_) {
            // Keys are like "lambda.1", "lambda.2" etc.
            if (k.substr(0, 7) == "lambda.") {
                try {
                    int id = std::stoi(k.substr(7));
                    if (id > latestId) { latestId = id; latestLam = k; }
                } catch (...) {}
            }
        }
        if (!latestLam.empty())
            closureTypes_[stmt.name] = closureTypes_[latestLam];
    }
}

void CodeGen::emitIfStmt(const IfStmt& stmt) {
    IRValue cond = emitExpr(*stmt.cond);
    // ensure i1
    cond = coerce(cond, "i1");

    std::string thenLbl  = freshLabel("if.then");
    std::string elseLbl  = freshLabel("if.else");
    std::string mergeLbl = freshLabel("if.end");

    emitCondBr(cond.name, thenLbl, stmt.elseBranch ? elseLbl : mergeLbl);

    emitLabel(thenLbl);
    emitStmt(*stmt.thenBranch);
    emitBr(mergeLbl);

    if (stmt.elseBranch) {
        emitLabel(elseLbl);
        emitStmt(**stmt.elseBranch);
        emitBr(mergeLbl);
    }

    emitLabel(mergeLbl);
}

void CodeGen::emitWhileStmt(const WhileStmt& stmt) {
    std::string condLbl = freshLabel("while.cond");
    std::string bodyLbl = freshLabel("while.body");
    std::string endLbl  = freshLabel("while.end");

    ctx_->breakLabels.push_back(endLbl);
    ctx_->continueLabels.push_back(condLbl);

    emitBr(condLbl);
    emitLabel(condLbl);
    IRValue cond = emitExpr(*stmt.cond);
    cond = coerce(cond, "i1");
    emitCondBr(cond.name, bodyLbl, endLbl);

    emitLabel(bodyLbl);
    emitStmt(*stmt.body);
    emitBr(condLbl);

    emitLabel(endLbl);
    ctx_->breakLabels.pop_back();
    ctx_->continueLabels.pop_back();
}

void CodeGen::emitDoWhileStmt(const DoWhileStmt& stmt) {
    std::string bodyLbl = freshLabel("dowhile.body");
    std::string condLbl = freshLabel("dowhile.cond");
    std::string endLbl  = freshLabel("dowhile.end");

    ctx_->breakLabels.push_back(endLbl);
    ctx_->continueLabels.push_back(condLbl);

    emitBr(bodyLbl);
    emitLabel(bodyLbl);
    emitStmt(*stmt.body);
    emitBr(condLbl);

    emitLabel(condLbl);
    IRValue cond = emitExpr(*stmt.cond);
    cond = coerce(cond, "i1");
    emitCondBr(cond.name, bodyLbl, endLbl);

    emitLabel(endLbl);
    ctx_->breakLabels.pop_back();
    ctx_->continueLabels.pop_back();
}

void CodeGen::emitForStmt(const ForStmt& stmt) {
    std::string condLbl = freshLabel("for.cond");
    std::string bodyLbl = freshLabel("for.body");
    std::string incrLbl = freshLabel("for.incr");
    std::string endLbl  = freshLabel("for.end");

    if (stmt.init) emitStmt(**stmt.init);

    ctx_->breakLabels.push_back(endLbl);
    ctx_->continueLabels.push_back(incrLbl);

    emitBr(condLbl);
    emitLabel(condLbl);
    if (stmt.cond) {
        IRValue cond = emitExpr(**stmt.cond);
        cond = coerce(cond, "i1");
        emitCondBr(cond.name, bodyLbl, endLbl);
    } else {
        emitBr(bodyLbl);
    }

    emitLabel(bodyLbl);
    emitStmt(*stmt.body);
    emitBr(incrLbl);

    emitLabel(incrLbl);
    if (stmt.incr) emitExpr(**stmt.incr);
    emitBr(condLbl);

    emitLabel(endLbl);
    ctx_->breakLabels.pop_back();
    ctx_->continueLabels.pop_back();
}

void CodeGen::emitSwitchStmt(const SwitchStmt& stmt) {
    IRValue subj = emitExpr(*stmt.subject);
    subj = coerce(subj, "i32");

    std::string endLbl = freshLabel("switch.end");
    ctx_->breakLabels.push_back(endLbl);

    // Build case labels
    std::vector<std::string> caseLabels;
    std::string defaultLbl = endLbl;
    for (const auto& c : stmt.cases) {
        caseLabels.push_back(freshLabel(c.value ? "case" : "default"));
        if (!c.value) defaultLbl = caseLabels.back();
    }

    // Emit switch instruction
    std::string sw = "switch i32 " + subj.name + ", label %" + defaultLbl + " [\n";
    for (std::size_t i = 0; i < stmt.cases.size(); ++i) {
        if (!stmt.cases[i].value) continue;
        IRValue cv = emitExpr(**stmt.cases[i].value);
        cv = coerce(cv, "i32");
        sw += "    i32 " + cv.name + ", label %" + caseLabels[i] + "\n";
    }
    sw += "  ]";
    emit(sw);
    if (ctx_) ctx_->terminated = true;

    for (std::size_t i = 0; i < stmt.cases.size(); ++i) {
        emitLabel(caseLabels[i]);
        for (const auto& s : stmt.cases[i].stmts) emitStmt(*s);
        // fall-through to next case unless break was hit
        if (i + 1 < caseLabels.size()) emitBr(caseLabels[i+1]);
        else emitBr(endLbl);
    }

    emitLabel(endLbl);
    ctx_->breakLabels.pop_back();
}

void CodeGen::emitReturnStmt(const ReturnStmt& stmt) {
    if (stmt.value) {
        IRValue val = emitExpr(**stmt.value);
        val = coerce(val, ctx_->retType);
        emit("ret " + ctx_->retType + " " + val.name);
    } else {
        emit("ret void");
    }
    if (ctx_) ctx_->terminated = true;
}

void CodeGen::emitThrowStmt(const ThrowStmt& stmt) {
    IRValue msg = emitExpr(*stmt.value);
    // Coerce to i8*
    if (msg.type != "i8*") {
        std::string tmp = freshName("throw.msg");
        emit(tmp + " = call i8* @ec_int_to_str(i32 " + msg.name + ")");
        msg = {tmp, "i8*"};
    }
    emit("call void @ec_throw(i8* " + msg.name + ")");
    emit("unreachable");
    if (ctx_) ctx_->terminated = true;
}

void CodeGen::emitTryStmt(const TryStmt& stmt) {
    // Correct setjmp/longjmp pattern:
    //
    //   The jmp_buf is allocated as a local alloca in THIS function so that
    //   setjmp and longjmp share the same call-frame depth.  The C standard
    //   (and LLVM) require setjmp to be called in the function that will
    //   handle the longjmp — calling setjmp in a helper that then returns
    //   is undefined behavior.
    //
    //   Generated IR pattern:
    //     %buf = alloca [25 x i64]          ; jmp_buf (200 bytes on x86_64)
    //     %buf.ptr = bitcast [25 x i64]* %buf to i8*
    //     call void @ec_try_push(i8* %buf.ptr)   ; register buf
    //     %rc = call i32 @setjmp(i8* %buf.ptr)     ; returns_twice
    //     %threw = icmp ne i32 %rc, 0
    //     br %threw → catch, else → try.body
    //   try.body:
    //     ... body ...
    //     call void @ec_try_pop()         ; success: pop frame
    //     br → try.end
    //   try.catch:
    //     %msg = call i8* @ec_catch_msg() ; pops frame, returns message
    //     ... catch body ...
    //     br → try.end
    //   try.end:

    std::string tryLbl   = freshLabel("try.body");
    std::string catchLbl = freshLabel("try.catch");
    std::string endLbl   = freshLabel("try.end");

    // Allocate jmp_buf as local alloca (200 bytes = [25 x i64] on x86_64)
    std::string bufName = freshName("jmpbuf");
    emit(bufName + " = alloca [25 x i64]");

    // Bitcast to i8* for the function calls
    std::string bufPtr = freshName("jmpbuf.ptr");
    emit(bufPtr + " = bitcast [25 x i64]* " + bufName + " to i8*");

    // Register the buffer with the runtime
    emit("call void @ec_try_push(i8* " + bufPtr + ")");

    // Call setjmp directly — returns_twice attribute tells LLVM it can
    // return more than once, preventing it from optimizing away the branch
    std::string rc = freshName("sjlj.rc");
    emit(rc + " = call i32 @setjmp(i8* " + bufPtr + ")");

    // Branch on setjmp result
    std::string threw = freshName("threw");
    emit(threw + " = icmp ne i32 " + rc + ", 0");
    emitCondBr(threw, catchLbl, tryLbl);

    // ── try body ──────────────────────────────────────────────────────────
    emitLabel(tryLbl);
    ctx_->inTry = true;
    emitBlock(static_cast<const BlockStmt&>(*stmt.tryBody));
    ctx_->inTry = false;
    if (!ctx_->terminated) {
        emit("call void @ec_try_pop()");
        emitBr(endLbl);
    }

    // ── catch body ────────────────────────────────────────────────────────
    emitLabel(catchLbl);
    for (const auto& cc : stmt.catches) {
        // ec_catch_msg pops the frame and returns the exception string
        std::string msgPtr = freshName("catch.msg");
        emit(msgPtr + " = call i8* @ec_catch_msg()");

        if (!cc.exName.empty()) {
            std::string ptr = emitAlloca("i8*", cc.exName);
            emitStore("i8*", msgPtr, "i8**", ptr);
            ctx_->locals[cc.exName]     = ptr;
            ctx_->localTypes[cc.exName] = "i8*";
        }
        emitBlock(static_cast<const BlockStmt&>(*cc.body));
    }
    if (!ctx_->terminated) emitBr(endLbl);

    emitLabel(endLbl);
}

void CodeGen::emitWithStmt(const WithStmt& stmt) {
    // Desugar: with (Type name = init) { body }
    // → allocate resource, try { body } finally { name.close(); }
    std::string lt = llvmType(stmt.resType);

    // Evaluate and store the resource
    IRValue initVal = emitExpr(*stmt.init);
    initVal = coerce(initVal, lt);
    std::string ptr = emitAlloca(lt, stmt.resName);
    emitStore(lt, initVal.name, lt + "*", ptr);
    ctx_->locals[stmt.resName]     = ptr;
    ctx_->localTypes[stmt.resName] = lt;

    // try { body }
    std::string tryLbl   = freshLabel("with.body");
    std::string catchLbl = freshLabel("with.catch");
    std::string endLbl   = freshLabel("with.end");

    std::string bufName = freshName("wjmpbuf");
    emit(bufName + " = alloca [25 x i64]");
    std::string bufPtr = freshName("wjmpbuf.ptr");
    emit(bufPtr + " = bitcast [25 x i64]* " + bufName + " to i8*");
    emit("call void @ec_try_push(i8* " + bufPtr + ")");
    std::string rc = freshName("with.rc");
    emit(rc + " = call i32 @setjmp(i8* " + bufPtr + ")");
    std::string threw = freshName("with.threw");
    emit(threw + " = icmp ne i32 " + rc + ", 0");
    emitCondBr(threw, catchLbl, tryLbl);

    emitLabel(tryLbl);
    emitStmt(*stmt.body);  // body may be a block or any statement
    if (!ctx_->terminated) {
        emit("call void @ec_try_pop()");
    }

    // finally: call name.close() on normal exit
    // Determine class name from resource type
    std::string className;
    if (lt.size() > 1 && lt[0] == '%') {
        className = lt.substr(1);
        if (!className.empty() && className.back() == '*') className.pop_back();
    }
    if (!className.empty() && !ctx_->terminated) {
        std::string resVal = emitLoad(lt, ptr);
        std::string closeFn = className + "_close";
        // Only emit if we know the class has a close method
        if (userFnRetTypes_.count(closeFn)) {
            emit("call void @" + closeFn + "(" + lt + " " + resVal + ")");
        }
    }
    if (!ctx_->terminated) emitBr(endLbl);

    // catch: re-throw then call close
    emitLabel(catchLbl);
    {
        std::string msgPtr = freshName("with.msg");
        emit(msgPtr + " = call i8* @ec_catch_msg()");
        // Call close on exception path too
        if (!className.empty()) {
            std::string resVal = emitLoad(lt, ptr);
            std::string closeFn = className + "_close";
            if (userFnRetTypes_.count(closeFn)) {
                emit("call void @" + closeFn + "(" + lt + " " + resVal + ")");
            }
        }
        emit("call void @ec_throw(i8* " + msgPtr + ")");
        emit("unreachable");
        if (ctx_) ctx_->terminated = true;
    }

    emitLabel(endLbl);
}

void CodeGen::emitBreakStmt(const BreakStmt&) {
    if (!ctx_->breakLabels.empty())
        emitBr(ctx_->breakLabels.back());
}

void CodeGen::emitContinueStmt(const ContinueStmt&) {
    if (!ctx_->continueLabels.empty())
        emitBr(ctx_->continueLabels.back());
}

void CodeGen::emitExprStmt(const ExprStmt& stmt) {
    emitExpr(*stmt.expr);
}

// ─────────────────────────────────────────────────────────────────────────────
//  L-value (address) emission
// ─────────────────────────────────────────────────────────────────────────────
std::string CodeGen::emitLValue(const Expr& expr) {
    if (expr.kind == ExprKind::Identifier) {
        const auto& id = static_cast<const IdentExpr&>(expr);
        auto it = ctx_->locals.find(id.name);
        if (it != ctx_->locals.end()) return it->second;
        // global
        return "@" + id.name;
    }
    if (expr.kind == ExprKind::Member) {
        const auto& me = static_cast<const MemberExpr&>(expr);
        IRValue obj = emitExpr(*me.object);

        // Determine class name from object type
        std::string className;
        if (obj.type.size() > 1 && obj.type[0] == '%')
            className = obj.type.substr(1, obj.type.find('*') - 1);

        // Dereference pointer-to-struct
        std::string rawType = obj.type;
        std::string objVal  = obj.name;
        if (rawType.back() == '*') rawType = rawType.substr(0, rawType.size()-1);

        std::string memberName = me.member;
        if (memberName.size() > 2 && memberName[0] == ':' && memberName[1] == ':')
            memberName = memberName.substr(2);

        int idx = fieldIndex(className, memberName);
        if (idx < 0) return "%invalid";

        return emitGEP(rawType, objVal, idx, memberName + ".gep");
    }
    if (expr.kind == ExprKind::Index) {
        const auto& ie = static_cast<const IndexExpr&>(expr);
        IRValue arr = emitExpr(*ie.array);
        IRValue idx = emitExpr(*ie.index);
        const std::string& rtn = ie.array->resolvedType.name;

        // String characters are immutable — assignment to str[i] is unsupported.
        // Return invalid so the store becomes a no-op.
        if (rtn == "string") return "%invalid.lvalue";

        idx = coerce(idx, "i64");

        // Determine element type
        std::string elemType = "i32";
        if (!rtn.empty() && rtn.size() > 2 && rtn.back() == ']') {
            std::string baseName = rtn.substr(0, rtn.size() - 2);
            elemType = llvmType(baseName);
        }
        return emitArrayElemPtr(arr.name, elemType, idx.name);
    }
    return "%invalid.lvalue";
}

// ─────────────────────────────────────────────────────────────────────────────
//  Expression emission
// ─────────────────────────────────────────────────────────────────────────────
IRValue CodeGen::emitExpr(const Expr& expr) {
    switch (expr.kind) {
    case ExprKind::IntLit:     return emitIntLit(static_cast<const IntLitExpr&>(expr));
    case ExprKind::FloatLit:   return emitFloatLit(static_cast<const FloatLitExpr&>(expr));
    case ExprKind::BoolLit:    return emitBoolLit(static_cast<const BoolLitExpr&>(expr));
    case ExprKind::CharLit:    return emitCharLit(static_cast<const CharLitExpr&>(expr));
    case ExprKind::StringLit:  return emitStringLit(static_cast<const StringLitExpr&>(expr));
    case ExprKind::NullLit:    return emitNullLit(static_cast<const NullLitExpr&>(expr));
    case ExprKind::Identifier: return emitIdent(static_cast<const IdentExpr&>(expr));
    case ExprKind::Unary:      return emitUnary(static_cast<const UnaryExpr&>(expr));
    case ExprKind::Binary:     return emitBinary(static_cast<const BinaryExpr&>(expr));
    case ExprKind::Ternary:    return emitTernary(static_cast<const TernaryExpr&>(expr));
    case ExprKind::Assign:     return emitAssign(static_cast<const AssignExpr&>(expr));
    case ExprKind::Call:       return emitCall(static_cast<const CallExpr&>(expr));
    case ExprKind::Member:     return emitMember(static_cast<const MemberExpr&>(expr));
    case ExprKind::Index:      return emitIndex(static_cast<const IndexExpr&>(expr));
    case ExprKind::New:        return emitNew(static_cast<const NewExpr&>(expr));
    case ExprKind::Delete:     return emitDelete(static_cast<const DeleteExpr&>(expr));
    case ExprKind::TypeOf:     return emitTypeOf(static_cast<const TypeOfExpr&>(expr));
    case ExprKind::Lambda:     return emitLambda(static_cast<const LambdaExpr&>(expr));
    case ExprKind::ArrayLit:   return emitArrayLit(static_cast<const ArrayLitExpr&>(expr));
    case ExprKind::InstanceOf: return emitInstanceOf(static_cast<const InstanceOfExpr&>(expr));
    case ExprKind::Cast:       return emitCast(static_cast<const CastExpr&>(expr));
    default:                   return {"0", "i32"};
    }
}

IRValue CodeGen::emitIntLit(const IntLitExpr& e) {
    return {std::to_string(e.value), "i32"};
}

IRValue CodeGen::emitFloatLit(const FloatLitExpr& e) {
    bool isF = !e.raw.empty() && (e.raw.back()=='f'||e.raw.back()=='F');
    std::string val = std::to_string(e.value);
    // LLVM needs explicit decimal point
    if (val.find('.') == std::string::npos) val += ".0";
    return {val, isF ? "float" : "double"};
}

IRValue CodeGen::emitBoolLit(const BoolLitExpr& e) {
    return {e.value ? "true" : "false", "i1"};
}

IRValue CodeGen::emitCharLit(const CharLitExpr& e) {
    // e.raw is e.g. "'A'" or "'\n'" or "'\x1b'"
    char c = 0;
    if (e.raw.size() >= 3) {
        if (e.raw[1] == '\\' && e.raw.size() >= 4) {
            char esc = e.raw[2];
            switch (esc) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case '0': c = '\0'; break;
            case 'a': c = '\a'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'v': c = '\v'; break;
            case '\\': c = '\\'; break;
            case '\'': c = '\''; break;
            case '"': c = '"'; break;
            case 'x': {
                // \xHH
                unsigned int code = 0;
                for (size_t i = 3; i < e.raw.size() - 1 && i < 5; ++i) {
                    char h = e.raw[i];
                    if (!std::isxdigit((unsigned char)h)) break;
                    code = code * 16 + (std::isdigit((unsigned char)h)
                                        ? h - '0'
                                        : std::tolower((unsigned char)h) - 'a' + 10);
                }
                c = (char)(unsigned char)code;
                break;
            }
            default: c = esc; break;
            }
        } else if (e.raw[1] == '\\' && e.raw.size() == 4) {
            // short form: already handled above
            c = e.raw[2];
        } else {
            c = e.raw[1];
        }
    }
    return {std::to_string((int)(unsigned char)c), "i8"};
}

IRValue CodeGen::emitStringLit(const StringLitExpr& e) {
    std::string gname = internString(e.value);
    int len = (int)e.value.size() + 1;
    std::string res = freshName("str");
    emit(res + " = getelementptr inbounds [" + std::to_string(len) + " x i8], [" +
         std::to_string(len) + " x i8]* " + gname + ", i32 0, i32 0");
    return {res, "i8*"};
}

IRValue CodeGen::emitNullLit(const NullLitExpr&) {
    return {"null", "i8*"};
}

IRValue CodeGen::emitIdent(const IdentExpr& e) {
    // this / self
    if (e.name == "this" || e.name == "self") {
        auto it = ctx_->locals.find("this");
        if (it != ctx_->locals.end()) {
            std::string lt = ctx_->localTypes.at("this");
            std::string val = emitLoad(lt, it->second);
            return {val, lt};
        }
        return {"null", "i8*"};
    }
    if (e.name == "super") {
        // Return self pointer cast to the superclass type
        auto it = ctx_->locals.find("this");
        std::string superClass;
        if (!currentClass_.empty()) {
            auto lit = classLayouts_.find(currentClass_);
            if (lit != classLayouts_.end()) superClass = lit->second.superClass;
        }
        if (it != ctx_->locals.end() && !superClass.empty()) {
            std::string lt  = ctx_->localTypes.at("this");
            std::string val = emitLoad(lt, it->second);
            std::string superType = "%" + superClass + "*";
            if (lt != superType) {
                std::string casted = freshName("super");
                emit(casted + " = bitcast " + lt + " " + val + " to " + superType);
                return {casted, superType};
            }
            return {val, superType};
        }
        return {"null", "i8*"};
    }
    // local variable
    auto lit = ctx_->locals.find(e.name);
    if (lit != ctx_->locals.end()) {
        std::string lt = ctx_->localTypes.at(e.name);
        std::string val = emitLoad(lt, lit->second);
        return {val, lt};
    }
    // enum member
    auto eit = enumValues_.find(e.name);
    if (eit != enumValues_.end())
        return {std::to_string(eit->second), "i32"};

    // global variable
    auto git = globalVars_.find(e.name);
    if (git != globalVars_.end()) {
        std::string val = emitLoad(git->second, "@" + e.name);
        return {val, git->second};
    }

    // function reference — return as i8* (function pointer, untyped for now)
    return {"@" + e.name, "i8*"};
}

IRValue CodeGen::emitUnary(const UnaryExpr& e) {
    // await: block on a Future and return its result
    if (e.op == TokenKind::KW_await) {
        IRValue futureVal = emitExpr(*e.operand);
        // coerce to i8* (Future is always i8*)
        futureVal = coerce(futureVal, "i8*");
        std::string res = freshName("await.result");
        emit(res + " = call i8* @ec_future_await(i8* " + futureVal.name + ")");
        return {res, "i8*"};
    }

    IRValue val = emitExpr(*e.operand);

    switch (e.op) {
    case TokenKind::Bang: {
        val = coerce(val, "i1");
        std::string res = freshName("not");
        emit(res + " = xor i1 " + val.name + ", true");
        return {res, "i1"};
    }
    case TokenKind::Minus: {
        std::string res = freshName("neg");
        if (val.type == "float" || val.type == "double")
            emit(res + " = fneg " + val.type + " " + val.name);
        else
            emit(res + " = sub " + val.type + " 0, " + val.name);
        return {res, val.type};
    }
    case TokenKind::Tilde: {
        std::string res = freshName("bitnot");
        emit(res + " = xor " + val.type + " " + val.name + ", -1");
        return {res, val.type};
    }
    case TokenKind::PlusPlus:
    case TokenKind::MinusMinus: {
        bool isInc = (e.op == TokenKind::PlusPlus);
        std::string ptr = emitLValue(*e.operand);
        std::string oldVal = emitLoad(val.type, ptr);
        std::string res = freshName(isInc ? "inc" : "dec");
        if (val.type == "float" || val.type == "double") {
            std::string one = val.type == "float" ? "1.0" : "1.0";
            emit(res + " = " + (isInc ? "fadd" : "fsub") + " " + val.type + " " + oldVal + ", " + one);
        } else {
            emit(res + " = " + (isInc ? "add" : "sub") + " " + val.type + " " + oldVal + ", 1");
        }
        emitStore(val.type, res, val.type + "*", ptr);
        // postfix returns old, prefix returns new
        return e.postfix ? IRValue{oldVal, val.type} : IRValue{res, val.type};
    }
    default:
        return val;
    }
}

IRValue CodeGen::emitBinary(const BinaryExpr& e) {
    IRValue lhs = emitExpr(*e.left);
    IRValue rhs = emitExpr(*e.right);

    // String concatenation: i8* + i8* → call ec_strconcat
    if (e.op == TokenKind::Plus && lhs.type == "i8*") {
        if (rhs.type != "i8*") {
            // coerce rhs to string
            std::string tmp = freshName("tostr");
            emit(tmp + " = call i8* @ec_int_to_str(i32 " + rhs.name + ")");
            rhs = {tmp, "i8*"};
        }
        std::string res = freshName("concat");
        emit(res + " = call i8* @ec_strconcat(i8* " + lhs.name + ", i8* " + rhs.name + ")");
        return {res, "i8*"};
    }

    // Widen to common type
    std::string ct = lhs.type;
    if (rhs.type == "double" || lhs.type == "double") ct = "double";
    else if (rhs.type == "float" || lhs.type == "float") ct = "float";
    else if (rhs.type == "i64" || lhs.type == "i64") ct = "i64";

    lhs = coerce(lhs, ct);
    rhs = coerce(rhs, ct);

    bool isFloat = (ct == "float" || ct == "double");
    std::string res = freshName("binop");

    switch (e.op) {
    case TokenKind::Plus:    emit(res + " = " + (isFloat?"fadd":"add") + " " + ct + " " + lhs.name + ", " + rhs.name); return {res, ct};
    case TokenKind::Minus:   emit(res + " = " + (isFloat?"fsub":"sub") + " " + ct + " " + lhs.name + ", " + rhs.name); return {res, ct};
    case TokenKind::Star:    emit(res + " = " + (isFloat?"fmul":"mul") + " " + ct + " " + lhs.name + ", " + rhs.name); return {res, ct};
    case TokenKind::Slash:   emit(res + " = " + (isFloat?"fdiv":"sdiv") + " " + ct + " " + lhs.name + ", " + rhs.name); return {res, ct};
    case TokenKind::Percent: emit(res + " = " + (isFloat?"frem":"srem") + " " + ct + " " + lhs.name + ", " + rhs.name); return {res, ct};

    case TokenKind::Amp:    emit(res + " = and " + ct + " " + lhs.name + ", " + rhs.name); return {res, ct};
    case TokenKind::Pipe:   emit(res + " = or "  + ct + " " + lhs.name + ", " + rhs.name); return {res, ct};
    case TokenKind::Caret:  emit(res + " = xor " + ct + " " + lhs.name + ", " + rhs.name); return {res, ct};
    case TokenKind::LShift: emit(res + " = shl " + ct + " " + lhs.name + ", " + rhs.name); return {res, ct};
    case TokenKind::RShift: emit(res + " = ashr "+ ct + " " + lhs.name + ", " + rhs.name); return {res, ct};

    case TokenKind::AmpAmp: {
        lhs = coerce(lhs, "i1"); rhs = coerce(rhs, "i1");
        emit(res + " = and i1 " + lhs.name + ", " + rhs.name);
        return {res, "i1"};
    }
    case TokenKind::PipePipe: {
        lhs = coerce(lhs, "i1"); rhs = coerce(rhs, "i1");
        emit(res + " = or i1 " + lhs.name + ", " + rhs.name);
        return {res, "i1"};
    }

    // Comparisons
    case TokenKind::EqualEqual:
        if (isFloat) emit(res + " = fcmp oeq " + ct + " " + lhs.name + ", " + rhs.name);
        else         emit(res + " = icmp eq "  + ct + " " + lhs.name + ", " + rhs.name);
        return {res, "i1"};
    case TokenKind::BangEqual:
        if (isFloat) emit(res + " = fcmp one " + ct + " " + lhs.name + ", " + rhs.name);
        else         emit(res + " = icmp ne "  + ct + " " + lhs.name + ", " + rhs.name);
        return {res, "i1"};
    case TokenKind::Less:
        if (isFloat) emit(res + " = fcmp olt " + ct + " " + lhs.name + ", " + rhs.name);
        else         emit(res + " = icmp slt " + ct + " " + lhs.name + ", " + rhs.name);
        return {res, "i1"};
    case TokenKind::LessEqual:
        if (isFloat) emit(res + " = fcmp ole " + ct + " " + lhs.name + ", " + rhs.name);
        else         emit(res + " = icmp sle " + ct + " " + lhs.name + ", " + rhs.name);
        return {res, "i1"};
    case TokenKind::Greater:
        if (isFloat) emit(res + " = fcmp ogt " + ct + " " + lhs.name + ", " + rhs.name);
        else         emit(res + " = icmp sgt " + ct + " " + lhs.name + ", " + rhs.name);
        return {res, "i1"};
    case TokenKind::GreaterEqual:
        if (isFloat) emit(res + " = fcmp oge " + ct + " " + lhs.name + ", " + rhs.name);
        else         emit(res + " = icmp sge " + ct + " " + lhs.name + ", " + rhs.name);
        return {res, "i1"};

    default:
        return {lhs.name, lhs.type};
    }
}

IRValue CodeGen::emitTernary(const TernaryExpr& e) {
    IRValue cond = emitExpr(*e.cond);
    cond = coerce(cond, "i1");

    std::string thenLbl  = freshLabel("tern.then");
    std::string elseLbl  = freshLabel("tern.else");
    std::string mergeLbl = freshLabel("tern.end");

    emitCondBr(cond.name, thenLbl, elseLbl);

    emitLabel(thenLbl);
    IRValue thenVal = emitExpr(*e.thenExpr);
    std::string thenBlock = currentBlock_; // capture block AFTER nested exprs
    emitBr(mergeLbl);

    emitLabel(elseLbl);
    IRValue elseVal = emitExpr(*e.elseExpr);
    std::string elseBlock = currentBlock_; // capture block AFTER nested exprs
    emitBr(mergeLbl);

    emitLabel(mergeLbl);
    std::string res = freshName("tern");
    // Coerce to common type
    std::string rt = thenVal.type;
    elseVal = coerce(elseVal, rt);
    emit(res + " = phi " + rt + " [ " + thenVal.name + ", %" + thenBlock +
         " ], [ " + elseVal.name + ", %" + elseBlock + " ]");
    return {res, rt};
}

IRValue CodeGen::emitAssign(const AssignExpr& e) {
    IRValue rhs = emitExpr(*e.value);
    std::string ptr = emitLValue(*e.target);
    IRValue lhs = emitExpr(*e.target); // for compound

    std::string lt = lhs.type;
    rhs = coerce(rhs, lt);

    IRValue result = rhs;

    if (e.op != TokenKind::Equal) {
        std::string cur = emitLoad(lt, ptr);
        std::string res = freshName("compop");
        bool isFloat = (lt == "float" || lt == "double");
        switch (e.op) {
        case TokenKind::PlusEq:   emit(res + " = " + (isFloat?"fadd":"add") + " " + lt + " " + cur + ", " + rhs.name); break;
        case TokenKind::MinusEq:  emit(res + " = " + (isFloat?"fsub":"sub") + " " + lt + " " + cur + ", " + rhs.name); break;
        case TokenKind::StarEq:   emit(res + " = " + (isFloat?"fmul":"mul") + " " + lt + " " + cur + ", " + rhs.name); break;
        case TokenKind::SlashEq:  emit(res + " = " + (isFloat?"fdiv":"sdiv") + " " + lt + " " + cur + ", " + rhs.name); break;
        case TokenKind::PercentEq:emit(res + " = srem " + lt + " " + cur + ", " + rhs.name); break;
        case TokenKind::AmpEq:    emit(res + " = and " + lt + " " + cur + ", " + rhs.name); break;
        case TokenKind::PipeEq:   emit(res + " = or "  + lt + " " + cur + ", " + rhs.name); break;
        case TokenKind::CaretEq:  emit(res + " = xor " + lt + " " + cur + ", " + rhs.name); break;
        case TokenKind::LShiftEq: emit(res + " = shl " + lt + " " + cur + ", " + rhs.name); break;
        case TokenKind::RShiftEq: emit(res + " = ashr "+ lt + " " + cur + ", " + rhs.name); break;
        default: break;
        }
        result = {res, lt};
    }

    emitStore(lt, result.name, lt + "*", ptr);
    return result;
}

IRValue CodeGen::emitCall(const CallExpr& e) {
    // Determine callee name and whether it's a method call
    std::string calleeName;
    std::string selfArg;
    std::string selfType;
    bool isMember = false;
    std::string receiverClass_;   // class of the receiver object (for vtable)
    std::string receiverMethod_;  // unmangled method name (for vtable)

    if (e.callee->kind == ExprKind::Member) {
        const auto& me = static_cast<const MemberExpr&>(*e.callee);
        isMember = true;

        std::string memberName = me.member;
        if (memberName.size() > 2 && memberName[0] == ':' && memberName[1] == ':')
            memberName = memberName.substr(2); // scope resolution

        // ── super.method(args) ────────────────────────────────────────────
        bool isSuper = (me.object->kind == ExprKind::Identifier &&
                        static_cast<const IdentExpr&>(*me.object).name == "super");

        if (isSuper && !currentClass_.empty()) {
            // Look up the superclass name from the layout
            std::string superClass;
            auto it = classLayouts_.find(currentClass_);
            if (it != classLayouts_.end()) superClass = it->second.superClass;

            if (superClass.empty()) superClass = currentClass_; // fallback

            // self pointer is %currentClass*; bitcast to %superClass*
            auto selfIt = ctx_->locals.find("this");
            std::string rawSelf;
            if (selfIt != ctx_->locals.end()) {
                rawSelf = emitLoad(ctx_->localTypes.at("this"), selfIt->second);
            } else {
                rawSelf = "%self";
            }

            // Bitcast self to superclass pointer if types differ
            std::string superPtrType = "%" + superClass + "*";
            if ("%" + currentClass_ + "*" != superPtrType) {
                std::string casted = freshName("super.cast");
                emit(casted + " = bitcast %" + currentClass_ + "* " +
                     rawSelf + " to " + superPtrType);
                rawSelf = casted;
            }

            selfArg  = rawSelf;
            selfType = superPtrType;
            calleeName = superClass + "_" + memberName;
        } else {
            // ── Regular obj.method(args) ──────────────────────────────────
            IRValue obj = emitExpr(*me.object);
            selfArg  = obj.name;
            selfType = obj.type;

            // Extract class name from LLVM type  %ClassName*  → ClassName
            std::string className;
            if (obj.type.size() > 1 && obj.type[0] == '%') {
                className = obj.type.substr(1);
                if (!className.empty() && className.back() == '*')
                    className.pop_back();
            }

            calleeName = className.empty() ? memberName
                                           : className + "_" + memberName;
            // Remember receiver class and method for vtable dispatch check
            receiverClass_ = className;
            receiverMethod_ = memberName;
        }
    } else if (e.callee->kind == ExprKind::Identifier) {
        const std::string& idName = static_cast<const IdentExpr&>(*e.callee).name;

        // ── Closure call detection ─────────────────────────────────────────
        // If 'idName' is a local variable of type i8* (the closure fat-pointer
        // type), emit an indirect closure call rather than a direct @name call.
        bool isClosure = false;
        if (ctx_) {
            auto tit = ctx_->localTypes.find(idName);
            auto pit = ctx_->locals.find(idName);
            if (tit != ctx_->localTypes.end() && tit->second == "i8*" &&
                pit != ctx_->locals.end()) {
                // Check if this is a closure (has a registered signature)
                auto sigIt = closureTypes_.find(idName);
                if (sigIt != closureTypes_.end()) {
                    isClosure = true;
                    const ClosureSig& sig = sigIt->second;

                    // Load the fat pointer: { fn_ptr@[0], env_ptr@[8] }
                    std::string fatPtr = emitLoad("i8*", pit->second);

                    // Extract fn_ptr from byte 0
                    std::string fnPtrSlot = freshName("cls.fn");
                    emit(fnPtrSlot + " = bitcast i8* " + fatPtr + " to i8**");
                    std::string fnPtrRaw = freshName("cls.fn.raw");
                    emit(fnPtrRaw + " = load i8*, i8** " + fnPtrSlot);

                    // Extract env_ptr from byte 8
                    std::string envSlot = freshName("cls.env");
                    emit(envSlot + " = getelementptr inbounds i8, i8* " + fatPtr + ", i64 8");
                    std::string envPtrSlot = freshName("cls.env.slot");
                    emit(envPtrSlot + " = bitcast i8* " + envSlot + " to i8**");
                    std::string envPtr = freshName("cls.env.ptr");
                    emit(envPtr + " = load i8*, i8** " + envPtrSlot);

                    // Evaluate call arguments
                    std::vector<IRValue> callArgs;
                    for (const auto& a : e.args)
                        callArgs.push_back(emitExpr(*a));

                    // Build the full fn signature using the stored sig.
                    // The lambda takes (i8* env, arg_types...) and returns sig.retType
                    std::string fnSig = sig.retType + " (i8*";
                    for (const auto& av : callArgs)
                        fnSig += ", " + av.type;
                    fnSig += ")*";

                    std::string fnTyped = freshName("cls.fn.typed");
                    emit(fnTyped + " = bitcast i8* " + fnPtrRaw + " to " + fnSig);

                    // Call: fn(env_ptr, arg0, arg1, ...)
                    std::string argStr = "i8* " + envPtr;
                    for (const auto& av : callArgs)
                        argStr += ", " + av.type + " " + av.name;

                    if (sig.retType == "void") {
                        emit("call void " + fnTyped + "(" + argStr + ")");
                        return {"", "void"};
                    }
                    std::string result = freshName("cls.ret");
                    emit(result + " = call " + sig.retType + " " + fnTyped +
                         "(" + argStr + ")");
                    return {result, sig.retType};
                }
            }
        }

        if (!isClosure)
            calleeName = idName;
    } else {
        IRValue fn = emitExpr(*e.callee);
        calleeName = fn.name.substr(1); // strip '@' if present
    }

    // Map stdlib function names to our runtime
    static const std::unordered_map<std::string, std::string> fnMap = {
        // ── I/O ──────────────────────────────────────────────────────────────
        {"println",              "ec_println"},
        {"print",                "ec_print"},
        {"readLine",             "ec_readline"},
        {"getChar",              "getchar"},
        {"putChar",              "putchar"},
        {"_native_print",        "ec_print"},
        {"_native_println",      "ec_println"},
        {"_native_readLine",     "ec_readline"},
        {"_native_getChar",      "getchar"},
        {"_native_putChar",      "putchar"},
        {"_native_readFile",     "ec_read_file"},
        {"_native_writeFile",    "ec_write_file"},
        {"_native_fileExists",   "ec_file_exists"},
        {"readFile",             "ec_read_file"},
        {"writeFile",            "ec_write_file"},
        {"fileExists",           "ec_file_exists"},
        // ── String conversion ─────────────────────────────────────────────────
        {"toString",             "ec_int_to_str"},
        {"floatToString",        "ec_float_to_str"},
        {"doubleToString",       "ec_float_to_str"},
        {"longToString",         "ec_int_to_str"},
        {"charToString",         "ec_char_to_str"},
        {"boolToString",         "ec_int_to_str"},
        {"parseInt",             "atoi"},
        {"parseLong",            "atol"},
        {"parseFloat",           "atof"},
        {"parseDouble",          "atof"},
        {"_native_intToStr",     "ec_int_to_str"},
        {"_native_longToStr",    "ec_int_to_str"},
        {"_native_floatToStr",   "ec_float_to_str"},
        {"_native_doubleToStr",  "ec_float_to_str"},
        {"_native_charToStr",    "ec_char_to_str"},
        {"_native_parseInt",     "atoi"},
        {"_native_parseLong",    "atol"},
        {"_native_parseFloat",   "atof"},
        {"_native_parseDouble",  "atof"},
        // ── String operations ─────────────────────────────────────────────────
        {"stringLen",            "ec_str_len"},
        {"charAt",               "ec_char_at"},
        {"substring",            "ec_substring"},
        {"indexOf",              "ec_str_find"},
        {"lastIndexOf",          "ec_str_find_last"},
        {"contains",             "ec_str_contains"},
        {"startsWith",           "ec_str_starts_with"},
        {"endsWith",             "ec_str_ends_with"},
        {"count",                "ec_str_count"},
        {"equals",               "ec_str_equals"},
        {"compare",              "ec_str_compare"},
        {"toLower",              "ec_str_to_lower"},
        {"toUpper",              "ec_str_to_upper"},
        {"trim",                 "ec_str_trim"},
        {"trimLeft",             "ec_str_trim_left"},
        {"trimRight",            "ec_str_trim_right"},
        {"repeat",               "ec_str_repeat"},
        {"replace",              "ec_str_replace"},
        {"reverse",              "ec_str_reverse"},
        {"join",                 "ec_str_join"},
        {"_native_strLen",       "ec_str_len"},
        {"_native_charAt",       "ec_char_at"},
        {"_native_substring",    "ec_substring"},
        {"_native_strFind",      "ec_str_find"},
        {"_native_strFindLast",  "ec_str_find_last"},
        {"_native_strContains",  "ec_str_contains"},
        {"_native_strStartsWith","ec_str_starts_with"},
        {"_native_strEndsWith",  "ec_str_ends_with"},
        {"_native_strCount",     "ec_str_count"},
        {"_native_strEquals",    "ec_str_equals"},
        {"_native_strCompare",   "ec_str_compare"},
        {"_native_strToLower",   "ec_str_to_lower"},
        {"_native_strToUpper",   "ec_str_to_upper"},
        {"_native_strTrim",      "ec_str_trim"},
        {"_native_strTrimLeft",  "ec_str_trim_left"},
        {"_native_strTrimRight", "ec_str_trim_right"},
        {"_native_strRepeat",    "ec_str_repeat"},
        {"_native_strReplace",   "ec_str_replace"},
        {"_native_strReverse",   "ec_str_reverse"},
        {"_native_strJoin",      "ec_str_join"},
        // ── Char operations ───────────────────────────────────────────────────
        {"isDigit",              "ec_char_is_digit"},
        {"isAlpha",              "ec_char_is_alpha"},
        {"isAlnum",              "ec_char_is_alnum"},
        {"isSpace",              "ec_char_is_space"},
        {"isUpper",              "ec_char_is_upper"},
        {"isLower",              "ec_char_is_lower"},
        {"isPunct",              "ec_char_is_punct"},
        {"toLowerChar",          "ec_char_to_lower_fn"},
        {"toUpperChar",          "ec_char_to_upper_fn"},
        {"charCode",             "ec_char_code"},
        {"charFromCode",         "ec_char_from_code"},
        {"_native_charIsDigit",  "ec_char_is_digit"},
        {"_native_charIsAlpha",  "ec_char_is_alpha"},
        {"_native_charIsAlnum",  "ec_char_is_alnum"},
        {"_native_charIsSpace",  "ec_char_is_space"},
        {"_native_charIsUpper",  "ec_char_is_upper"},
        {"_native_charIsLower",  "ec_char_is_lower"},
        {"_native_charIsPunct",  "ec_char_is_punct"},
        {"_native_charToLower",  "ec_char_to_lower_fn"},
        {"_native_charToUpper",  "ec_char_to_upper_fn"},
        {"_native_charCode",     "ec_char_code"},
        {"_native_charFromCode", "ec_char_from_code"},
        // ── Math ─────────────────────────────────────────────────────────────
        {"sqrt",                 "sqrt"},
        {"cbrt",                 "ec_cbrt_fn"},
        {"pow",                  "pow"},
        {"powInt",               "ec_pow_int"},
        {"hypot",                "ec_hypot_fn"},
        {"floor",                "floor"},
        {"ceil",                 "ceil"},
        {"round",                "round"},
        {"trunc",                "trunc"},
        {"abs",                  "fabs"},
        {"absInt",               "ec_abs_int"},
        {"absLong",              "ec_abs_long"},
        {"log",                  "log"},
        {"log2",                 "ec_log2_fn"},
        {"log10",                "ec_log10_fn"},
        {"sin",                  "sin"},
        {"cos",                  "cos"},
        {"tan",                  "tan"},
        {"asin",                 "asin"},
        {"acos",                 "acos"},
        {"atan",                 "atan"},
        {"atan2",                "ec_atan2_fn"},
        {"minInt",               "ec_min_int"},
        {"maxInt",               "ec_max_int"},
        {"minLong",              "ec_min_long"},
        {"maxLong",              "ec_max_long"},
        {"min",                  "ec_min_double"},
        {"max",                  "ec_max_double"},
        {"clampInt",             "ec_clamp_int"},
        {"clamp",                "ec_clamp_double"},
        {"gcd",                  "ec_gcd"},
        {"lcm",                  "ec_lcm"},
        {"isEven",               "ec_is_even"},
        {"isOdd",                "ec_is_odd"},
        {"isPrime",              "ec_is_prime"},
        {"_native_floor",        "floor"},
        {"_native_ceil",         "ceil"},
        {"_native_round",        "round"},
        {"_native_trunc",        "trunc"},
        {"_native_fabs",         "fabs"},
        {"_native_absInt",       "ec_abs_int"},
        {"_native_absLong",      "ec_abs_long"},
        {"_native_sqrt",         "sqrt"},
        {"_native_cbrt",         "ec_cbrt_fn"},
        {"_native_pow",          "pow"},
        {"_native_powInt",       "ec_pow_int"},
        {"_native_hypot",        "ec_hypot_fn"},
        {"_native_log",          "log"},
        {"_native_log2",         "ec_log2_fn"},
        {"_native_log10",        "ec_log10_fn"},
        {"_native_sin",          "sin"},
        {"_native_cos",          "cos"},
        {"_native_tan",          "tan"},
        {"_native_asin",         "asin"},
        {"_native_acos",         "acos"},
        {"_native_atan",         "atan"},
        {"_native_atan2",        "ec_atan2_fn"},
        {"_native_minInt",       "ec_min_int"},
        {"_native_maxInt",       "ec_max_int"},
        {"_native_minLong",      "ec_min_long"},
        {"_native_maxLong",      "ec_max_long"},
        {"_native_minDouble",    "ec_min_double"},
        {"_native_maxDouble",    "ec_max_double"},
        {"_native_clampInt",     "ec_clamp_int"},
        {"_native_clampDouble",  "ec_clamp_double"},
        {"_native_gcd",          "ec_gcd"},
        {"_native_lcm",          "ec_lcm"},
        {"_native_isEven",       "ec_is_even"},
        {"_native_isOdd",        "ec_is_odd"},
        {"_native_isPrime",      "ec_is_prime"},
        // ── Collections -- List ───────────────────────────────────────────────
        {"_native_listNew",      "ec_list_new"},
        {"_native_listLen",      "ec_list_len"},
        {"_native_listGet",      "ec_list_get"},
        {"_native_listAdd",      "ec_list_add"},
        {"_native_listSet",      "ec_list_set"},
        {"_native_listRemove",   "ec_list_remove"},
        {"_native_listInsert",   "ec_list_insert"},
        {"_native_listIndexOf",  "ec_list_index_of"},
        {"_native_listContains", "ec_list_contains"},
        {"_native_listSlice",    "ec_list_slice"},
        {"_native_listConcat",   "ec_list_concat"},
        // ── Collections -- Map ────────────────────────────────────────────────
        {"_native_mapNew",       "ec_map_new"},
        {"_native_mapSet",       "ec_map_set"},
        {"_native_mapGet",       "ec_map_get"},
        {"_native_mapHas",       "ec_map_has"},
        {"_native_mapDelete",    "ec_map_delete"},
        {"_native_mapCount",     "ec_map_count"},
        {"_native_mapKeys",      "ec_map_keys"},
        {"_native_mapValues",    "ec_map_values"},
        // ── Collections -- Set ────────────────────────────────────────────────
        {"_native_setNew",       "ec_set_new"},
        {"_native_setAdd",       "ec_set_add"},
        {"_native_setHas",       "ec_set_has"},
        {"_native_setRemove",    "ec_set_remove"},
        {"_native_setCount",     "ec_set_count"},
        {"_native_setKeys",      "ec_set_keys"},
        // ── StringBuilder ─────────────────────────────────────────────────────
        {"_native_sbNew",        "ec_sb_new"},
        {"_native_sbAppendStr",  "ec_sb_append_str"},
        {"_native_sbAppendChar", "ec_sb_append_char"},
        {"_native_sbAppendInt",  "ec_sb_append_int"},
        {"_native_sbAppendLong", "ec_sb_append_long"},
        {"_native_sbToString",   "ec_sb_to_string"},
        {"_native_sbLength",     "ec_sb_length"},
        {"_native_sbClear",      "ec_sb_clear"},
        // ── GC API ────────────────────────────────────────────────────────────
        {"gcCollect",            "ec_gc_collect"},
        {"gcStats",              "ec_gc_stats"},
        {"gcLiveBytes",          "ec_gc_live_bytes"},
        {"gcLiveObjects",        "ec_gc_live_objects"},
        {"gcPin",                "ec_gc_pin"},
        {"gcUnpin",              "ec_gc_unpin"},
        {"_native_gcCollect",    "ec_gc_collect"},
        {"_native_gcStats",      "ec_gc_stats"},
        {"_native_gcLiveBytes",  "ec_gc_live_bytes"},
        {"_native_gcLiveObjects","ec_gc_live_objects"},
        // ── System ────────────────────────────────────────────────────────────
        {"exit",                 "ec_exit"},
        {"timeMs",               "ec_time_ms"},
        {"_native_exit",         "ec_exit"},
        {"_native_timeMs",       "ec_time_ms"},
        // ── Terminal / editor primitives ──────────────────────────────────────
        {"termRaw",              "ec_term_raw"},
        {"termRestore",          "ec_term_restore"},
        {"getTermRows",          "ec_term_rows"},
        {"getTermCols",          "ec_term_cols"},
        {"readKey",              "ec_read_key"},
        {"writeRaw",             "ec_write_raw"},
        {"flushOutput",          "ec_flush"},
        {"runSystem",            "ec_system"},
        {"atexitRestore",        "ec_atexit_restore"},
        {"registerWinch",        "ec_register_winch"},
        {"winchPending",         "ec_winch_pending"},
        // ── Internal ──────────────────────────────────────────────────────────
        {"ec_throw",           "ec_throw"},
    };
    auto mapIt = fnMap.find(calleeName);
    if (mapIt != fnMap.end()) calleeName = mapIt->second;

    // Emit arguments
    std::vector<std::pair<std::string,std::string>> args; // (type, name)
    if (isMember && !selfArg.empty()) {
        args.push_back({selfType, selfArg});
    }
    for (const auto& a : e.args) {
        IRValue av = emitExpr(*a);
        args.push_back({av.type, av.name});
    }

    // toString dispatch: route based on argument type
    // toString(char) → ec_char_to_str(i8)
    // toString(bool) → ec_int_to_str after zext to i32
    if (calleeName == "ec_int_to_str" && !args.empty()) {
        const std::string& argType = args.back().first;
        if (argType == "i8") {
            // char → use char-to-str
            calleeName = "ec_char_to_str";
        } else if (argType == "i1") {
            // bool → widen to i32 first
            std::string widened = freshName("bool.i32");
            emit(widened + " = zext i1 " + args.back().second + " to i32");
            args.back() = {"i32", widened};
        }
    }

    // Special handling for known signatures
    // getchar returns i32, we need i8
    if (calleeName == "getchar") {
        std::string r32 = freshName("gc");
        emit(r32 + " = call i32 @getchar()");
        std::string r8 = freshName("gc.c");
        emit(r8 + " = trunc i32 " + r32 + " to i8");
        return {r8, "i8"};
    }
    if (calleeName == "putchar" && !args.empty()) {
        // putchar needs i32
        IRValue a = coerce({args[0].second, args[0].first}, "i32");
        emit("call i32 @putchar(i32 " + a.name + ")");
        return {"", "void"};
    }

    // Build call instruction
    // 1. Check if it's a known user-defined function
    std::string retType;
    {
        auto uit = userFnRetTypes_.find(calleeName);
        if (uit != userFnRetTypes_.end()) retType = uit->second;
    }
    // 2. Fall back to well-known runtime signatures
    if (retType.empty()) {
        // Functions returning i8* (string or pointer)
        static const std::unordered_set<std::string> returnsPtr = {
            "ec_strconcat","ec_readline","ec_int_to_str","ec_float_to_str",
            "ec_char_to_str","ec_substring","ec_typeof","ec_gc_stats",
            "ec_new","ec_catch_msg","ec_read_file","ec_array_new",
            "ec_str_to_lower","ec_str_to_upper","ec_str_trim","ec_str_trim_left",
            "ec_str_trim_right","ec_str_repeat","ec_str_replace","ec_str_reverse",
            "ec_str_split","ec_str_join",
            "ec_list_new","ec_list_get","ec_list_add","ec_list_set",
            "ec_list_remove","ec_list_insert","ec_list_slice","ec_list_concat",
            "ec_map_new","ec_map_set","ec_map_get","ec_map_delete",
            "ec_map_keys","ec_map_values",
            "ec_set_new","ec_set_add","ec_set_remove","ec_set_keys",
            "ec_sb_new","ec_sb_append_str","ec_sb_append_char","ec_sb_append_int",
            "ec_sb_append_long","ec_sb_to_string","ec_sb_clear",
        };
        // Functions returning void
        static const std::unordered_set<std::string> returnsVoid = {
            "ec_println","ec_print","ec_delete","ec_throw",
            "ec_try_end","ec_try_pop","ec_try_push","ec_gc_init",
            "ec_gc_collect","ec_gc_register_global","ec_gc_push_root",
            "ec_gc_pop_root","ec_gc_pin","ec_gc_unpin","ec_exit",
            "putchar",
            "ec_mutex_lock","ec_mutex_unlock","ec_mutex_destroy",
            "ec_thread_sleep_ms","ec_thread_yield",
            "ec_term_raw","ec_term_restore","ec_write_raw","ec_flush",
            "ec_atexit_restore","ec_register_winch",
            "ec_list_sort_fn","ec_list_reverse_fn","ec_list_shuffle",
            "ec_random_seed",
        };
        // Functions returning i64
        static const std::unordered_set<std::string> returnsI64 = {
            "ec_gc_live_bytes","ec_gc_live_objects","ec_gc_num_collections",
            "ec_array_len","ec_list_len","ec_map_count","ec_set_count",
            "ec_sb_length","ec_list_index_of","ec_time_ms",
        };
        // Functions returning i32
        static const std::unordered_set<std::string> returnsI32 = {
            "ec_str_len","atoi","atol","lang_try_begin",
            "ec_str_find","ec_str_find_last","ec_str_starts_with","ec_str_ends_with",
            "ec_str_contains","ec_str_equals","ec_str_compare","ec_str_count",
            "ec_char_is_digit","ec_char_is_alpha","ec_char_is_alnum","ec_char_is_space",
            "ec_char_is_upper","ec_char_is_lower","ec_char_is_punct",
            "ec_char_code","ec_char_from_code",
            "ec_abs_int","ec_min_int","ec_max_int","ec_clamp_int",
            "ec_gcd","ec_lcm","ec_is_even","ec_is_odd","ec_is_prime","ec_pow_int",
            "ec_map_has","ec_set_has","ec_list_contains",
            "ec_write_file","ec_file_exists","putchar",
            "ec_future_done","ec_mutex_trylock",
            "ec_read_key","ec_winch_pending","ec_system",
            "ec_term_rows","ec_term_cols",
        };
        // Functions returning double
        static const std::unordered_set<std::string> returnsDouble = {
            "sqrt","pow","floor","ceil","round","trunc","fabs","sin","cos","tan",
            "asin","acos","atan","atof","log",
            "ec_cbrt_fn","ec_hypot_fn","ec_log2_fn","ec_log10_fn","ec_atan2_fn",
            "ec_min_double","ec_max_double","ec_clamp_double",
            "ec_infinity","ec_nan","ec_abs_long",
        };
        // Functions returning i8
        static const std::unordered_set<std::string> returnsI8 = {
            "ec_char_at","ec_char_to_lower_fn","ec_char_to_upper_fn",
            "getchar",
        };

        if (returnsPtr.count(calleeName))    retType = "i8*";
        else if (returnsVoid.count(calleeName))   retType = "void";
        else if (returnsI64.count(calleeName))    retType = "i64";
        else if (returnsI32.count(calleeName))    retType = "i32";
        else if (returnsDouble.count(calleeName)) retType = "double";
        else if (returnsI8.count(calleeName))     retType = "i8";
        else retType = "i32"; // unknown — default
    }

    // Build arg string
    std::string argStr;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (i) argStr += ", ";
        IRValue av = {args[i].second, args[i].first};
        // Coerce narrow integers to i8* string for print functions
        if ((calleeName == "ec_println" || calleeName == "ec_print" ||
             calleeName == "ec_strconcat") && av.type != "i8*") {
            std::string tmp = freshName("tostr");
            emit(tmp + " = call i8* @ec_int_to_str(i32 " + av.name + ")");
            av = {tmp, "i8*"};
        }
        // ec_int_to_str expects i32 — widen i8/i1 if needed
        if (calleeName == "ec_int_to_str" && (av.type == "i8" || av.type == "i1")) {
            av = coerce(av, "i32");
        }
        argStr += av.type + " " + av.name;
    }

    // ── Emit the call instruction ─────────────────────────────────────────────
    // Check whether this is a virtual method call that should go through
    // the vtable.  Conditions for virtual dispatch:
    //   1. It's a member call (isMember = true)
    //   2. The receiver class has a vtable slot for this method
    //   3. It's not a super.method() call (super is always static)
    bool useVtable = isMember &&
                     !receiverClass_.empty() &&
                     !receiverMethod_.empty() &&
                     isVirtualCall(receiverClass_, receiverMethod_);

    if (useVtable) {
        int slot = vtableSlot(receiverClass_, receiverMethod_);
        auto layoutIt = classLayouts_.find(receiverClass_);
        if (slot >= 0 && layoutIt != classLayouts_.end()) {

            // 1. Load vtable pointer from object field 0:
            //    %vtptr_slot = GEP %ReceiverClass*, i32 0, i32 0
            //    %vtptr      = load i8**, i8***  %vtptr_slot
            std::string vtptrSlot = freshName("vtptr.slot");
            emit(vtptrSlot + " = getelementptr inbounds %" + receiverClass_ +
                 ", %" + receiverClass_ + "* " + selfArg + ", i32 0, i32 0");
            std::string vtptr = freshName("vtptr");
            emit(vtptr + " = load i8**, i8*** " + vtptrSlot);

            // 2. GEP to slot index within the vtable array:
            //    %fn_slot = GEP i8*, i8** %vtptr, i32 slot
            std::string fnSlot = freshName("fn.slot");
            emit(fnSlot + " = getelementptr inbounds i8*, i8** " + vtptr +
                 ", i32 " + std::to_string(slot));

            // 3. Load the function pointer:
            //    %fn_raw = load i8*, i8** %fn_slot
            std::string fnRaw = freshName("fn.raw");
            emit(fnRaw + " = load i8*, i8** " + fnSlot);

            // 4. Bitcast to the correct function type and call indirectly:
            //    %fn_typed = bitcast i8* %fn_raw to retType (%ReceiverClass*, params...)*
            //    %result   = call retType %fn_typed(%ReceiverClass* self, args...)
            std::string fnSig = retType + " (%" + receiverClass_ + "*";
            // Build param types from args (skip first arg which is self)
            for (std::size_t i = 1; i < args.size(); ++i)
                fnSig += ", " + args[i].first;
            fnSig += ")*";

            std::string fnTyped = freshName("fn.vt");
            emit(fnTyped + " = bitcast i8* " + fnRaw + " to " + fnSig);

            // Build full argument string
            std::string vtArgStr = selfType + " " + selfArg;
            for (std::size_t i = 1; i < args.size(); ++i) {
                vtArgStr += ", ";
                IRValue av = {args[i].second, args[i].first};
                if ((calleeName == "ec_println" || calleeName == "ec_print" ||
                     calleeName == "ec_strconcat") && av.type != "i8*") {
                    std::string tmp = freshName("tostr");
                    emit(tmp + " = call i8* @ec_int_to_str(i32 " + av.name + ")");
                    av = {tmp, "i8*"};
                }
                vtArgStr += av.type + " " + av.name;
            }

            if (retType == "void") {
                emit("call void " + fnTyped + "(" + vtArgStr + ")");
                return {"", "void"};
            }
            std::string res = freshName("vt.call");
            emit(res + " = call " + retType + " " + fnTyped + "(" + vtArgStr + ")");
            return {res, retType};
        }
    }

    // ── Static (direct) call ──────────────────────────────────────────────────
    if (retType == "void") {
        emit("call void @" + calleeName + "(" + argStr + ")");
        return {"", "void"};
    }
    std::string res = freshName("call");
    emit(res + " = call " + retType + " @" + calleeName + "(" + argStr + ")");
    return {res, retType};
}

IRValue CodeGen::emitMember(const MemberExpr& e) {
    // .length / .size pseudo-field — works for both arrays and strings
    if (e.member == "length" || e.member == "size") {
        IRValue obj = emitExpr(*e.object);
        const std::string& rtn = e.object->resolvedType.name;

        // String .length → ec_str_len(str) → i32
        if (rtn == "string") {
            std::string res = freshName("str.len");
            emit(res + " = call i32 @ec_str_len(i8* " + obj.name + ")");
            return {res, "i32"};
        }

        // Array .length → ec_array_len(arr) → i64, truncate to i32
        bool isArr = (!rtn.empty() && rtn.size() > 2 && rtn.back() == ']');
        if (isArr) {
            std::string len = emitArrayLen(obj.name);
            std::string len32 = freshName("len32");
            emit(len32 + " = trunc i64 " + len + " to i32");
            return {len32, "i32"};
        }
    }
    // Scope resolution: Direction::NORTH → enum value
    if (e.member.size() > 2 && e.member[0] == ':' && e.member[1] == ':') {
        std::string memberName = e.member.substr(2);
        if (e.object->kind == ExprKind::Identifier) {
            std::string enumName = static_cast<const IdentExpr&>(*e.object).name;
            auto it = enumValues_.find(enumName + "::" + memberName);
            if (it != enumValues_.end())
                return {std::to_string(it->second), "i32"};
        }
        return {"0", "i32"};
    }

    IRValue obj = emitExpr(*e.object);

    // Determine class name
    std::string className;
    if (obj.type.size() > 1 && obj.type[0] == '%') {
        className = obj.type.substr(1);
        if (!className.empty() && className.back() == '*') className.pop_back();
    }

    if (className.empty()) return {"0", "i32"};

    int idx = fieldIndex(className, e.member);
    if (idx < 0) return {"0", "i32"};

    std::string ft = fieldLLVMType(className, idx);
    std::string gepType = "%" + className;
    std::string gepPtr  = obj.name;
    // If obj is already a pointer, dereference
    std::string gep = emitGEP(gepType, gepPtr, idx, e.member);
    std::string val = emitLoad(ft, gep);
    return {val, ft};
}

IRValue CodeGen::emitIndex(const IndexExpr& e) {
    IRValue arr = emitExpr(*e.array);
    IRValue idx = emitExpr(*e.index);
    const std::string& rtn = e.array->resolvedType.name;

    // ── String indexing: str[i] → ec_char_at(str, i32) ─────────────────────
    if (rtn == "string") {
        idx = coerce(idx, "i32");
        std::string res = freshName("char");
        emit(res + " = call i8 @ec_char_at(i8* " + arr.name +
             ", i32 " + idx.name + ")");
        return {res, "i8"};
    }

    // ── Array indexing: fat-pointer layout ───────────────────────────────────
    idx = coerce(idx, "i64");

    // Determine element LLVM type from resolved type annotation on the array expr
    std::string elemType = "i32"; // default
    if (!rtn.empty() && rtn.size() > 2 && rtn.back() == ']') {
        // e.g. "int[]" → "int"
        std::string baseName = rtn.substr(0, rtn.size() - 2);
        elemType = llvmType(baseName);
    }

    std::string elemPtr = emitArrayElemPtr(arr.name, elemType, idx.name);
    std::string val = emitLoad(elemType, elemPtr);
    return {val, elemType};
}

// ── Array helper implementations ─────────────────────────────────────────────

std::string CodeGen::emitArrayNew(const std::string& elemLLVMType,
                                  const std::string& lengthVal) {
    // Compute element size in bytes
    static const std::unordered_map<std::string,int> szMap = {
        {"i1",1},{"i8",1},{"i16",2},{"i32",4},{"i64",8},
        {"float",4},{"double",8}
    };
    int elemSz = 8; // pointer default
    auto it = szMap.find(elemLLVMType);
    if (it != szMap.end()) elemSz = it->second;

    // ec_array_new(elem_size, length) → i8*
    std::string arr = freshName("arr");
    emit(arr + " = call i8* @ec_array_new(i64 " +
         std::to_string(elemSz) + ", i64 " + lengthVal + ")");
    return arr;
}

std::string CodeGen::emitArrayLen(const std::string& arrPtr) {
    std::string res = freshName("arr.len");
    emit(res + " = call i64 @ec_array_len(i8* " + arrPtr + ")");
    return res;
}

std::string CodeGen::emitArrayElemPtr(const std::string& arrPtr,
                                       const std::string& elemLLVMType,
                                       const std::string& idxVal) {
    // Fat-pointer layout: first 8 bytes = i64 length, then elements.
    // elem_ptr = (elemType*)(arrPtr + 8 + idx * sizeof(elem))
    static const std::unordered_map<std::string,int> szMap = {
        {"i1",1},{"i8",1},{"i16",2},{"i32",4},{"i64",8},
        {"float",4},{"double",8}
    };
    int elemSz = 8;
    auto it = szMap.find(elemLLVMType);
    if (it != szMap.end()) elemSz = it->second;

    // byte offset = 8 + idx * elemSz
    std::string idx64 = idxVal;
    std::string scaledIdx = freshName("scaled");
    emit(scaledIdx + " = mul i64 " + idx64 + ", " + std::to_string(elemSz));
    std::string byteOff = freshName("byteoff");
    emit(byteOff + " = add i64 8, " + scaledIdx);

    // raw byte ptr = GEP into the i8 array
    std::string rawPtr = freshName("raw.elem");
    emit(rawPtr + " = getelementptr inbounds i8, i8* " + arrPtr + ", i64 " + byteOff);

    // cast to elemType*
    std::string typedPtr = freshName("elem.ptr");
    emit(typedPtr + " = bitcast i8* " + rawPtr + " to " + elemLLVMType + "*");
    return typedPtr;
}

IRValue CodeGen::emitArrayLit(const ArrayLitExpr& e) {
    if (e.elements.empty()) {
        std::string arr = emitArrayNew("i32", "0");
        return {arr, "i8*"};
    }

    // Evaluate first element to determine type
    IRValue first = emitExpr(*e.elements[0]);
    std::string elemType = first.type;
    int n = (int)e.elements.size();

    // Allocate the array
    std::string arr = emitArrayNew(elemType, std::to_string(n));

    // Store each element
    for (int i = 0; i < n; i++) {
        IRValue ev = (i == 0) ? first : emitExpr(*e.elements[i]);
        ev = coerce(ev, elemType);
        std::string idxVal = std::to_string(i);
        std::string eptr = emitArrayElemPtr(arr, elemType, idxVal);
        emitStore(elemType, ev.name, elemType + "*", eptr);
    }

    return {arr, "i8*"};
}

IRValue CodeGen::emitNew(const NewExpr& e) {
    // ── Array allocation:  new int[n] ────────────────────────────────────────
    if (e.isArray && e.arraySize) {
        IRValue sz = emitExpr(*e.arraySize);
        sz = coerce(sz, "i64");
        std::string elemLLT = llvmType(e.type);
        std::string arr = emitArrayNew(elemLLT, sz.name);
        return {arr, "i8*"};
    }

    // ── Object allocation:  new ClassName(args) ──────────────────────────────
    std::string typeName = e.type.name;
    auto it = classLayouts_.find(typeName);

    // Size = 8 bytes for vtptr + 8 bytes per user field (conservative)
    int numFields = it != classLayouts_.end() ? (int)it->second.fieldTypes.size() : 0;
    int sizeBytes = 8 + std::max(numFields * 8, 8); // vtptr + fields

    std::string rawPtr = freshName("new.raw");
    emit(rawPtr + " = call i8* @ec_new(i64 " + std::to_string(sizeBytes) + ")");

    std::string typedPtr = freshName("new.obj");
    emit(typedPtr + " = bitcast i8* " + rawPtr + " to %" + typeName + "*");

    // ── Store vtable pointer at field 0 ──────────────────────────────────────
    // GEP to field 0 (i8**), then store @ClassName_vtable cast to i8**
    if (it != classLayouts_.end() && it->second.hasVtable &&
        !it->second.vtable.empty()) {
        std::string vtPtrSlot = freshName("vt.slot");
        emit(vtPtrSlot + " = getelementptr inbounds %" + typeName +
             ", %" + typeName + "* " + typedPtr + ", i32 0, i32 0");
        // Cast @ClassName_vtable to i8** (it's [N x i8*]*, need i8**)
        int n = (int)it->second.vtable.size();
        std::string vtCast = freshName("vt.cast");
        emit(vtCast + " = bitcast [" + std::to_string(n) + " x i8*]* @" +
             typeName + "_vtable to i8**");
        emit("store i8** " + vtCast + ", i8*** " + vtPtrSlot);
    }

    // ── Call init if it exists ────────────────────────────────────────────────
    bool hasInit = false;
    if (it != classLayouts_.end()) {
        for (const auto& m : it->second.methodNames)
            if (m == "init") { hasInit = true; break; }
    }
    if (!hasInit && userFnRetTypes_.count(typeName + "_init"))
        hasInit = true;

    if (hasInit) {
        std::string initName = typeName + "_init";
        std::string argStr   = "%" + typeName + "* " + typedPtr;
        for (const auto& a : e.args) {
            IRValue av = emitExpr(*a);
            argStr += ", " + av.type + " " + av.name;
        }
        emit("call void @" + initName + "(" + argStr + ")");
    }

    return {typedPtr, "%" + typeName + "*"};
}

IRValue CodeGen::emitDelete(const DeleteExpr& e) {
    IRValue obj = emitExpr(*e.operand);
    std::string raw = freshName("del.raw");
    if (obj.type != "i8*")
        emit(raw + " = bitcast " + obj.type + " " + obj.name + " to i8*");
    else
        raw = obj.name;
    emit("call void @ec_delete(i8* " + raw + ")");
    return {"", "void"};
}

IRValue CodeGen::emitTypeOf(const TypeOfExpr& e) {
    IRValue obj = emitExpr(*e.operand);
    std::string typeName = obj.type;
    std::string gname = internString(typeName);
    int len = (int)typeName.size() + 1;
    std::string res = freshName("typeof");
    emit(res + " = getelementptr inbounds [" + std::to_string(len) + " x i8], [" +
         std::to_string(len) + " x i8]* " + gname + ", i32 0, i32 0");
    return {res, "i8*"};
}

// ── instanceof ────────────────────────────────────────────────────────────────
// Compares the object's vtable pointer (field 0) to the target class's
// known vtable global.  If they match the object IS an instance of that class.
// Also returns true if the object's vtable is a subclass vtable whose slot 0
// matches -- for full correctness we compare the actual vtable address.
IRValue CodeGen::emitInstanceOf(const InstanceOfExpr& e) {
    IRValue obj = emitExpr(*e.operand);
    std::string targetClass = e.type.name;

    // If the target class has no vtable (unknown class), return false
    auto it = classLayouts_.find(targetClass);
    if (it == classLayouts_.end() || !it->second.hasVtable) {
        return {"false", "i1"};
    }

    // Extract class name from object's LLVM type  %ClassName*  → ClassName
    std::string objClass;
    if (obj.type.size() > 1 && obj.type[0] == '%') {
        objClass = obj.type.substr(1);
        if (!objClass.empty() && objClass.back() == '*') objClass.pop_back();
    }
    if (objClass.empty()) return {"false", "i1"};

    // Load the vtable pointer from field 0 of the object
    std::string vtptrSlot = freshName("iof.vt");
    emit(vtptrSlot + " = getelementptr inbounds %" + objClass +
         ", %" + objClass + "* " + obj.name + ", i32 0, i32 0");
    std::string vtptr = freshName("iof.vtptr");
    emit(vtptr + " = load i8**, i8*** " + vtptrSlot);

    // Cast vtable pointer to i8* for comparison
    std::string vtptrI8 = freshName("iof.vt.i8");
    emit(vtptrI8 + " = bitcast i8** " + vtptr + " to i8*");

    // Get target class's vtable as i8*
    int n = (int)it->second.vtable.size();
    std::string targetVt = freshName("iof.target");
    emit(targetVt + " = bitcast [" + std::to_string(n) + " x i8*]* @" +
         targetClass + "_vtable to i8*");

    // Compare: vtptr == &TargetClass_vtable
    std::string res = freshName("instanceof");
    emit(res + " = icmp eq i8* " + vtptrI8 + ", " + targetVt);
    return {res, "i1"};
}

// ── cast  expr as Type ────────────────────────────────────────────────────────
// Downcast: bitcast the pointer to the target type.
// This is unsafe (no runtime check) -- use after instanceof.
IRValue CodeGen::emitCast(const CastExpr& e) {
    IRValue obj = emitExpr(*e.operand);
    std::string targetLLT = llvmType(e.type);

    if (obj.type == targetLLT) return obj;

    // Pointer-to-pointer bitcast (class hierarchy cast)
    if (obj.type.back() == '*' && targetLLT.back() == '*') {
        std::string res = freshName("cast");
        emit(res + " = bitcast " + obj.type + " " + obj.name + " to " + targetLLT);
        return {res, targetLLT};
    }

    // Numeric cast
    return coerce(obj, targetLLT);
}

// ── Closure / Lambda implementation ──────────────────────────────────────────
//
// A closure is a GC-allocated struct:
//   { i8* fn_ptr, i8* env_ptr }
//   stored as a plain i8* (16 bytes)
//
// fn_ptr points to a function with signature:
//   retType @lambda_N(i8* env, param1, param2, ...)
//
// env_ptr points to a GC allocation containing the captured values,
// laid out sequentially as their LLVM types.
//
// Calling a closure stored in an i8*:
//   %cls  = bitcast i8* %lam to { i8*, i8* }*
//   %fn   = load fn ptr from cls[0]
//   %env  = load env ptr from cls[1]
//   %ret  = call fn(env, args...)

void CodeGen::collectFreeVars(const Expr& e,
                               const std::unordered_set<std::string>& bound,
                               std::unordered_set<std::string>& free) {
    switch (e.kind) {
    case ExprKind::Identifier: {
        const auto& id = static_cast<const IdentExpr&>(e);
        if (!bound.count(id.name) && ctx_ && ctx_->locals.count(id.name))
            free.insert(id.name);
        break;
    }
    case ExprKind::Binary: {
        const auto& b = static_cast<const BinaryExpr&>(e);
        collectFreeVars(*b.left, bound, free);
        collectFreeVars(*b.right, bound, free);
        break;
    }
    case ExprKind::Unary: {
        collectFreeVars(*static_cast<const UnaryExpr&>(e).operand, bound, free);
        break;
    }
    case ExprKind::Ternary: {
        const auto& t = static_cast<const TernaryExpr&>(e);
        collectFreeVars(*t.cond, bound, free);
        collectFreeVars(*t.thenExpr, bound, free);
        collectFreeVars(*t.elseExpr, bound, free);
        break;
    }
    case ExprKind::Call: {
        const auto& c = static_cast<const CallExpr&>(e);
        collectFreeVars(*c.callee, bound, free);
        for (const auto& a : c.args) collectFreeVars(*a, bound, free);
        break;
    }
    case ExprKind::Member: {
        collectFreeVars(*static_cast<const MemberExpr&>(e).object, bound, free);
        break;
    }
    case ExprKind::Index: {
        const auto& ix = static_cast<const IndexExpr&>(e);
        collectFreeVars(*ix.array, bound, free);
        collectFreeVars(*ix.index, bound, free);
        break;
    }
    case ExprKind::Assign: {
        const auto& a = static_cast<const AssignExpr&>(e);
        collectFreeVars(*a.target, bound, free);
        collectFreeVars(*a.value, bound, free);
        break;
    }
    default: break;
    }
}

void CodeGen::collectFreeVarsStmt(const Stmt& s,
                                   const std::unordered_set<std::string>& bound,
                                   std::unordered_set<std::string>& free) {
    switch (s.kind) {
    case StmtKind::Expression:
        collectFreeVars(*static_cast<const ExprStmt&>(s).expr, bound, free);
        break;
    case StmtKind::Return:
        if (static_cast<const ReturnStmt&>(s).value)
            collectFreeVars(**static_cast<const ReturnStmt&>(s).value, bound, free);
        break;
    case StmtKind::Block:
        for (const auto& st : static_cast<const BlockStmt&>(s).stmts)
            collectFreeVarsStmt(*st, bound, free);
        break;
    case StmtKind::If: {
        const auto& is = static_cast<const IfStmt&>(s);
        collectFreeVars(*is.cond, bound, free);
        collectFreeVarsStmt(*is.thenBranch, bound, free);
        if (is.elseBranch) collectFreeVarsStmt(**is.elseBranch, bound, free);
        break;
    }
    case StmtKind::While: {
        const auto& ws = static_cast<const WhileStmt&>(s);
        collectFreeVars(*ws.cond, bound, free);
        collectFreeVarsStmt(*ws.body, bound, free);
        break;
    }
    default: break;
    }
}

std::vector<CodeGen::CaptureInfo> CodeGen::collectCaptures(const LambdaExpr& e) {
    // Build the set of names bound by the lambda's own parameters
    std::unordered_set<std::string> bound;
    for (const auto& p : e.params) bound.insert(p.name);

    // Collect free variable names
    std::unordered_set<std::string> freeNames;
    if (e.body)      collectFreeVars(*e.body, bound, freeNames);
    if (e.blockBody) collectFreeVarsStmt(*e.blockBody, bound, freeNames);

    // Build CaptureInfo for each free variable that exists in the current ctx
    std::vector<CaptureInfo> caps;
    if (!ctx_) return caps;
    for (const auto& name : freeNames) {
        auto pit = ctx_->locals.find(name);
        auto tit = ctx_->localTypes.find(name);
        if (pit != ctx_->locals.end() && tit != ctx_->localTypes.end()) {
            caps.push_back({name, tit->second, pit->second});
        }
    }
    // Sort for stable layout
    std::sort(caps.begin(), caps.end(),
              [](const CaptureInfo& a, const CaptureInfo& b){ return a.name < b.name; });
    return caps;
}

IRValue CodeGen::emitLambda(const LambdaExpr& e) {
    int lamId = ++lambdaCtr_;
    std::string lamName = "lambda." + std::to_string(lamId);
    std::string envName = "lambda." + std::to_string(lamId) + ".env";

    // ── Capture analysis ────────────────────────────────────────────────────
    std::vector<CaptureInfo> caps = collectCaptures(e);
    bool hasCaps = !caps.empty();

    // ── Infer return type from body expression ───────────────────────────────
    // We temporarily emit the body into a scratch buffer to learn its type,
    // then discard those instructions.  For simple expression bodies we can
    // also just look at the AST node's resolvedType (set by the type checker).
    std::string retLLT = "i32"; // default — will be refined below
    if (e.body) {
        const std::string& rtn = e.body->resolvedType.name;
        if (!rtn.empty() && rtn != "<unknown>")
            retLLT = llvmType(rtn);
        else
            retLLT = "i8*"; // fallback for complex expressions
    }

    // ── Build LLVM env struct type ───────────────────────────────────────────
    // %lambda_N_env = type { cap0_type, cap1_type, ... }
    if (hasCaps) {
        globals_ << "%" << envName << " = type { ";
        for (std::size_t i = 0; i < caps.size(); i++) {
            if (i) globals_ << ", ";
            globals_ << caps[i].llvmType;
        }
        globals_ << " }\n";
    }

    // ── Emit the lambda function body ────────────────────────────────────────
    // Signature: retType @lambda_N(i8* %env_raw, param_types...)
    std::string paramStr;      // with names — for the define
    std::string paramTypeStr;  // types only  — for the bitcast in caller
    if (hasCaps) { paramStr = "i8* %env_raw"; paramTypeStr = "i8*"; }
    for (std::size_t i = 0; i < e.params.size(); i++) {
        if (!paramStr.empty())     paramStr     += ", ";
        if (!paramTypeStr.empty()) paramTypeStr += ", ";
        std::string lt = llvmType(e.params[i].type);
        paramStr     += lt + " %" + e.params[i].name;
        paramTypeStr += lt;
    }

    // Register closure signature so call sites can look it up
    closureTypes_[lamName] = {retLLT, paramTypeStr};

    // Save enclosing function context
    auto* savedOut  = out_;
    auto* savedCtx  = ctx_;
    int   savedCtr  = counter_;
    std::string savedClass = currentClass_;

    FnCtx lamCtx;
    lamCtx.name    = lamName;
    lamCtx.retType = retLLT;
    ctx_           = &lamCtx;
    // Write the lambda DEFINITION into the lambdas_ buffer so it is
    // emitted AFTER the enclosing function's closing '}'.
    // body_ is currently mid-function; putting a 'define' there is illegal.
    out_           = &lambdas_;

    lambdas_ << "\ndefine private " << retLLT << " @" << lamName
             << "(" << paramStr << ") {\n";
    lambdas_ << "entry:\n";

    // Unpack captured variables from env struct
    if (hasCaps) {
        std::string envTyped = freshName("env");
        emit(envTyped + " = bitcast i8* %env_raw to %" + envName + "*");
        for (int i = 0; i < (int)caps.size(); i++) {
            // GEP field i
            std::string fgep = freshName(caps[i].name + ".cgep");
            emit(fgep + " = getelementptr inbounds %" + envName + ", %" +
                 envName + "* " + envTyped + ", i32 0, i32 " + std::to_string(i));
            // Load it into a local alloca
            std::string val = emitLoad(caps[i].llvmType, fgep);
            std::string ptr = emitAlloca(caps[i].llvmType, caps[i].name);
            emitStore(caps[i].llvmType, val, caps[i].llvmType + "*", ptr);
            lamCtx.locals[caps[i].name]     = ptr;
            lamCtx.localTypes[caps[i].name] = caps[i].llvmType;
        }
    }

    // Spill lambda params to allocas
    for (const auto& p : e.params) {
        std::string lt  = llvmType(p.type);
        std::string ptr = emitAlloca(lt, p.name);
        emitStore(lt, "%" + p.name, lt + "*", ptr);
        lamCtx.locals[p.name]     = ptr;
        lamCtx.localTypes[p.name] = lt;
    }

    // Emit body — return the actual typed value, no boxing needed
    if (e.body) {
        IRValue bodyVal = emitExpr(*e.body);
        bodyVal = coerce(bodyVal, retLLT);
        emit("ret " + retLLT + " " + bodyVal.name);
    } else if (e.blockBody) {
        emitStmt(*e.blockBody);
        if (!lamCtx.terminated) emit("ret " + retLLT + " " + llvmDefault(retLLT));
    } else {
        emit("ret " + retLLT + " " + llvmDefault(retLLT));
    }

    lambdas_ << "}\n";

    // Restore enclosing context — out_ goes back to body_ (mid-function)
    ctx_          = savedCtx;
    out_          = savedOut;
    counter_      = savedCtr;
    currentClass_ = savedClass;

    // ── Allocate closure struct: { fn_ptr, env_ptr } = 16 bytes ─────────────
    std::string closure = freshName("closure");
    emit(closure + " = call i8* @ec_new(i64 16)");

    // Store fn_ptr at offset 0
    std::string fnPtrSlot = freshName("cls.fn");
    emit(fnPtrSlot + " = bitcast i8* " + closure + " to i8**");

    // Get function pointer — cast lambda function to i8* using type-only signature
    // The actual function signature is retLLT (i8* env, params...)
    std::string fnPtr = freshName("fn.ptr");
    std::string fullSig = retLLT + " (" + (hasCaps ? "i8*" : "");
    if (hasCaps && !paramTypeStr.empty() &&
        paramTypeStr != "i8*") {  // paramTypeStr starts with "i8*" when hasCaps
        // paramTypeStr = "i8*, arg1, arg2..." — strip leading "i8*, "
        std::string argOnly = paramTypeStr;
        if (argOnly.substr(0, 4) == "i8*, ") argOnly = argOnly.substr(5);
        else if (argOnly == "i8*") argOnly = "";
        if (!argOnly.empty()) fullSig += ", " + argOnly;
    } else if (!hasCaps && !paramTypeStr.empty()) {
        fullSig += paramTypeStr;
    }
    fullSig += ")*";
    emit(fnPtr + " = bitcast " + retLLT + " (" + paramTypeStr + ")* @" + lamName + " to i8*");
    emit("store i8* " + fnPtr + ", i8** " + fnPtrSlot);

    // Build env struct and store env_ptr at offset 8
    std::string envPtrSlot = freshName("cls.env");
    emit(envPtrSlot + " = getelementptr inbounds i8, i8* " + closure + ", i64 8");
    std::string envPtrSlotCast = freshName("cls.env.ptr");
    emit(envPtrSlotCast + " = bitcast i8* " + envPtrSlot + " to i8**");

    if (hasCaps) {
        // Allocate env struct
        int envSz = (int)caps.size() * 8; // conservative: 8 bytes per field
        std::string envRaw = freshName("env.raw");
        emit(envRaw + " = call i8* @ec_new(i64 " + std::to_string(envSz) + ")");
        std::string envTyped = freshName("env.typed");
        emit(envTyped + " = bitcast i8* " + envRaw + " to %" + envName + "*");

        // Store captured values into env
        for (int i = 0; i < (int)caps.size(); i++) {
            // Load current value from enclosing ctx's alloca
            std::string curVal = emitLoad(caps[i].llvmType, caps[i].ptrInCtx);
            std::string fgep = freshName(caps[i].name + ".egep");
            emit(fgep + " = getelementptr inbounds %" + envName + ", %" +
                 envName + "* " + envTyped + ", i32 0, i32 " + std::to_string(i));
            emitStore(caps[i].llvmType, curVal, caps[i].llvmType + "*", fgep);
        }

        emit("store i8* " + envRaw + ", i8** " + envPtrSlotCast);
    } else {
        emit("store i8* null, i8** " + envPtrSlotCast);
    }

    return {closure, "i8*"};
}

