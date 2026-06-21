#include "CodeGenerator.h"
#include "ast/Declaration.h"
#include "ast/Expression.h"
#include "ast/Statement.h"
#include "semantic/Literals.h"
#include "semantic/Prelude.h"
#include "semantic/Symbol.h"
#include "semantic/Type.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IR/DIBuilder.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"
#include "llvm/BinaryFormat/Dwarf.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/CodeGen.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/TargetParser/Host.h"

#include <filesystem>
#include <optional>
#include <unordered_map>
#include <unordered_set>

static std::string asAscii(std::u16string_view s) {
    std::string r;
    r.reserve(s.size());
    for (char16_t c : s) r.push_back(c < 128 ? static_cast<char>(c) : '?');
    return r;
}

static std::string sanitizeModulePath(std::u16string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char16_t c : s) {
        bool ok = (c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z') ||
                  (c >= u'0' && c <= u'9') || c == u'_' || c == u'.';
        out.push_back(ok ? static_cast<char>(c) : '_');
    }
    return out;
}

// Globally-stable descriptor symbol name, qualified by the defining module so a
// class caught in one module and defined in another resolve to one address.
static std::string descriptorSymbolName(StructInfo* si) {
    return "_typedesc_" + sanitizeModulePath(si->modulePath) + "_" + asAscii(si->name);
}

static uint32_t fnv1a32(const std::string& s) {
    uint32_t h = 2166136261u;
    for (unsigned char c : s) { h ^= c; h *= 16777619u; }
    return h ? h : 1u;
}

struct CodeGenerator::Impl {
    std::string moduleName;
    std::string sourceFilename;
    const SourceFile& sourceFile;
    const AnalysisResult& analysis;

    llvm::LLVMContext ctx;
    std::unique_ptr<llvm::Module> module;
    std::unique_ptr<llvm::IRBuilder<>> builder;
    std::unique_ptr<llvm::DIBuilder> diBuilder;
    std::unique_ptr<llvm::TargetMachine> targetMachine;
    std::unordered_map<Symbol*, llvm::Value*> values;
    std::unordered_map<int, llvm::DIType*> diTypeCache;
    std::unordered_map<::Type*, llvm::StructType*> structTypeCache;
    std::unordered_map<::Type*, llvm::DIType*> diStructTypeCache;
    std::unordered_map<::Type*, llvm::DIType*> diClassPointerCache;
    std::unordered_map<StructInfo*, llvm::GlobalVariable*> descriptorCache;
    std::unordered_map<std::string, llvm::Constant*> stringLiteralCache;
    llvm::StructType* typeDescriptorTy = nullptr;

    // Refcount sentinel for immortal objects (string literals). Real refcounts
    // start at 1 and move by 1, so they never reach this value; ens_retain and
    // ens_release detect it and leave the object untouched.
    static constexpr int64_t kImmortalRefcount = INT64_MIN;
    llvm::StructType* symEntryTy = nullptr;
    llvm::StructType* symChunkTy = nullptr;
    llvm::StructType* lineEntryTy = nullptr;
    std::vector<std::pair<llvm::Constant*, int>> lineRecords;
    ::Type* stackFrameType = nullptr;
    struct SymtabRecord {
        llvm::Function* fn;
        std::string name;
        std::string file;
        int line;
        bool isEntry;
    };
    std::vector<SymtabRecord> symtabRecords;
    uint32_t nextTypeId = 1;
    llvm::Function* currentFunction = nullptr;
    ::Type* currentReturnType = nullptr;
    llvm::DIScope* currentDIScope = nullptr;
    llvm::DICompileUnit* diCU = nullptr;
    llvm::DIFile* diFile = nullptr;
    bool debugEnabled = true;
    std::vector<Diagnostic> diagnostics;

    struct OwnedLocal {
        llvm::Value* alloca;
        ::Type* type;
        bool isStackArray = false;
    };
    std::vector<std::vector<OwnedLocal>> cleanupStack;
    std::unordered_set<Symbol*> byPointerParams;

    // === Checked-exception lowering state (reset per function) ===
    // Address of the caller's error slot (trailing param) when this function may throw.
    llvm::Value* incomingErrorSlot = nullptr;
    // A function with catch clauses owns a local slot the body writes into; it is
    // consumed by the catch-dispatch block. Null when the function has no catches.
    llvm::Value* localErrorSlot = nullptr;
    llvm::BasicBlock* catchDispatchBB = nullptr;
    // Prefix of cleanupStack[0] holding param copies; catch clauses see params but
    // not body locals, so unwinding into dispatch keeps this prefix alive.
    size_t paramCleanupWatermark = 0;
    bool currentHasCatch = false;
    // Where a throw / propagating call writes the error, and whether the error
    // path branches to catch dispatch (vs. cleanup-and-return to the caller).
    llvm::Value* throwTargetSlot = nullptr;
    bool unwindToDispatch = false;
    // The alloca holding the in-flight exception while emitting a catch body
    // (used by `rethrow`); null outside catch bodies.
    llvm::Value* currentCatchVarSlot = nullptr;

    struct DefaultInitContext {
        ::Type* structType;
        llvm::Value* basePtr;
    };
    std::vector<DefaultInitContext> defaultInitStack;

    std::u16string modulePath;
    std::string targetTriple;   // empty = host default; set by --target for cross-compilation

    Impl(std::string mn, std::string sf, const SourceFile& src, const AnalysisResult& an,
         std::u16string mp, std::string triple)
        : moduleName(std::move(mn)), sourceFilename(std::move(sf)),
          sourceFile(src), analysis(an), modulePath(std::move(mp)), targetTriple(std::move(triple)) {
        module = std::make_unique<llvm::Module>(moduleName, ctx);
        module->setSourceFileName(sourceFilename);
        builder = std::make_unique<llvm::IRBuilder<>>(ctx);

        if (debugEnabled) {
            module->addModuleFlag(llvm::Module::Warning, "Debug Info Version", llvm::DEBUG_METADATA_VERSION);
            module->addModuleFlag(llvm::Module::Warning, "Dwarf Version", 4);
            diBuilder = std::make_unique<llvm::DIBuilder>(*module);
            std::filesystem::path p(sourceFilename);
            std::string fname = p.filename().string();
            std::string dir   = p.parent_path().string();
            if (fname.empty()) fname = sourceFilename;
            diFile = diBuilder->createFile(fname, dir);
            diCU = diBuilder->createCompileUnit(
                llvm::dwarf::DW_LANG_C, diFile, "Ens compiler",
                /*isOptimized*/ false, /*flags*/ "", /*runtimeVersion*/ 0);
            currentDIScope = diCU;
        }
    }

    std::pair<int,int> posOf(uint32_t offset) const {
        return sourceFile.offsetToPosition(offset);
    }

    void setLocation(uint32_t offset) {
        if (!debugEnabled || !currentDIScope) return;
        auto [line, col] = posOf(offset);
        builder->SetCurrentDebugLocation(
            llvm::DILocation::get(ctx, static_cast<unsigned>(line),
                                  static_cast<unsigned>(col), currentDIScope));
    }

    void setLocationFromNode(const SyntaxNode& n) { setLocation(n.startOffset()); }

    void error(uint32_t offset, std::string msg) {
        auto [line, col] = posOf(offset);
        diagnostics.emplace_back(DiagnosticLevel::Error, SourceSpan{line, col, 1}, std::move(msg));
    }

    bool isUnsupportedType(::Type* t) {
        if (!t) return true;
        switch (t->kind) {
            case TypeKind::Decimal:
            case TypeKind::Null:
            case TypeKind::Error:
                return true;
            case TypeKind::Optional:
                // Nullable class, external, string, and array types are pointer-sized.
                if (t->inner && (t->inner->isClass() || t->inner->isExternal() || t->inner->isString())) return false;
                if (t->inner && t->inner->isArray()) return isUnsupportedType(t->inner);
                return true;
            case TypeKind::Array:
                return isUnsupportedType(t->inner);
            default:
                return false;
        }
    }

    bool isReferenceType(::Type* t) {
        if (!t) return false;
        if (t->kind == TypeKind::Class) return true;
        if (t->kind == TypeKind::Array) return true;
        if (t->kind == TypeKind::String) return true;
        if (t->kind == TypeKind::Optional && t->inner &&
            (t->inner->isClass() || t->inner->isArray() || t->inner->isString())) return true;
        return false;
    }

    llvm::Type* mapType(::Type* t) {
        if (!t) return llvm::Type::getVoidTy(ctx);
        switch (t->kind) {
            case TypeKind::Bool:    return llvm::Type::getInt1Ty(ctx);
            case TypeKind::Byte:    return llvm::Type::getInt8Ty(ctx);
            case TypeKind::Short:
            case TypeKind::UShort:  return llvm::Type::getInt16Ty(ctx);
            case TypeKind::Int:
            case TypeKind::UInt:    return llvm::Type::getInt32Ty(ctx);
            case TypeKind::Long:
            case TypeKind::ULong:   return llvm::Type::getInt64Ty(ctx);
            case TypeKind::Char:    return llvm::Type::getInt32Ty(ctx);
            case TypeKind::Float:   return llvm::Type::getFloatTy(ctx);
            case TypeKind::Double:  return llvm::Type::getDoubleTy(ctx);
            case TypeKind::Void:    return llvm::Type::getVoidTy(ctx);
            case TypeKind::String:  return llvm::PointerType::get(ctx, 0);
            case TypeKind::Struct:  return mapStructType(t);
            case TypeKind::Class:   return llvm::PointerType::get(ctx, 0);
            case TypeKind::External: return llvm::PointerType::get(ctx, 0);
            case TypeKind::Array:   return llvm::PointerType::get(ctx, 0);
            case TypeKind::Optional:
                if (t->inner && (t->inner->isClass() || t->inner->isExternal() || t->inner->isArray() || t->inner->isString()))
                    return llvm::PointerType::get(ctx, 0);
                return nullptr;
            default:                return nullptr;
        }
    }

    llvm::StructType* mapStructType(::Type* t) {
        auto it = structTypeCache.find(t);
        if (it != structTypeCache.end()) return it->second;
        std::string sname = t->structInfo ? asAscii(t->structInfo->name) : "struct.anon";
        auto* st = llvm::StructType::create(ctx, sname);
        structTypeCache[t] = st;
        std::vector<llvm::Type*> fieldTypes;
        if (t->structInfo) {
            fieldTypes.reserve(t->structInfo->fields.size());
            for (const auto& f : t->structInfo->fields) fieldTypes.push_back(mapType(f.type));
        }
        st->setBody(fieldTypes);
        return st;
    }

    llvm::DIType* mapDIType(::Type* t) {
        if (!debugEnabled || !diBuilder || !t) return nullptr;
        if (t->kind == TypeKind::Optional && t->inner && t->inner->isClass()) {
            return mapDIType(t->inner);
        }
        if (t->kind == TypeKind::Optional && t->inner && t->inner->isArray()) {
            return mapDIType(t->inner);
        }
        if (t->kind == TypeKind::Optional && t->inner && t->inner->isString()) {
            return mapDIType(t->inner);
        }
        if (t->kind == TypeKind::Array) {
            llvm::DIType* elem = mapDIType(t->inner);
            if (!elem) return nullptr;
            uint64_t ptrBits = module->getDataLayout().getPointerSizeInBits();
            return diBuilder->createPointerType(elem, ptrBits);
        }
        if (t->kind == TypeKind::Struct) return mapDIStructType(t);
        if (t->kind == TypeKind::Class) {
            auto it = diClassPointerCache.find(t);
            if (it != diClassPointerCache.end()) return it->second;
            // Pre-cache a forward-declared pointer to break mutual recursion
            // when two classes reference each other (e.g. parent<->child).
            std::string sname = t->structInfo ? asAscii(t->structInfo->name) : "class.anon";
            auto* fwd = diBuilder->createReplaceableCompositeType(
                llvm::dwarf::DW_TAG_structure_type,
                sname, diCU, diFile, 0);
            uint64_t ptrBits = module->getDataLayout().getPointerSizeInBits();
            auto* ptrTy = diBuilder->createPointerType(fwd, ptrBits);
            diClassPointerCache[t] = ptrTy;
            auto* underlying = mapDIStructType(t);
            if (underlying) {
                fwd->replaceAllUsesWith(underlying);
            }
            return ptrTy;
        }
        int key = static_cast<int>(t->kind);
        auto it = diTypeCache.find(key);
        if (it != diTypeCache.end()) return it->second;
        llvm::DIType* result = nullptr;
        switch (t->kind) {
            case TypeKind::Bool:   result = diBuilder->createBasicType("bool",   1,  llvm::dwarf::DW_ATE_boolean); break;
            case TypeKind::Byte:   result = diBuilder->createBasicType("byte",   8,  llvm::dwarf::DW_ATE_unsigned); break;
            case TypeKind::Short:  result = diBuilder->createBasicType("short",  16, llvm::dwarf::DW_ATE_signed); break;
            case TypeKind::UShort: result = diBuilder->createBasicType("ushort", 16, llvm::dwarf::DW_ATE_unsigned); break;
            case TypeKind::Int:    result = diBuilder->createBasicType("int",    32, llvm::dwarf::DW_ATE_signed); break;
            case TypeKind::UInt:   result = diBuilder->createBasicType("uint",   32, llvm::dwarf::DW_ATE_unsigned); break;
            case TypeKind::Long:   result = diBuilder->createBasicType("long",   64, llvm::dwarf::DW_ATE_signed); break;
            case TypeKind::ULong:  result = diBuilder->createBasicType("ulong",  64, llvm::dwarf::DW_ATE_unsigned); break;
            case TypeKind::Char:   result = diBuilder->createBasicType("char",   32, llvm::dwarf::DW_ATE_UTF); break;
            case TypeKind::Float:  result = diBuilder->createBasicType("float",  32, llvm::dwarf::DW_ATE_float); break;
            case TypeKind::Double: result = diBuilder->createBasicType("double", 64, llvm::dwarf::DW_ATE_float); break;
            case TypeKind::String: {
                auto* byteCharTy = diBuilder->createBasicType("char", 8, llvm::dwarf::DW_ATE_signed_char);
                result = diBuilder->createPointerType(byteCharTy, 64);
                break;
            }
            default: break;
        }
        if (result) diTypeCache[key] = result;
        return result;
    }

    llvm::DIType* mapDIStructType(::Type* t) {
        auto it = diStructTypeCache.find(t);
        if (it != diStructTypeCache.end()) return it->second;
        if (!t->structInfo) return nullptr;
        std::string sname = asAscii(t->structInfo->name);
        auto* st = mapStructType(t);
        const llvm::DataLayout& dl = module->getDataLayout();
        const llvm::StructLayout* sl = dl.getStructLayout(st);

        unsigned structLine = static_cast<unsigned>(t->structInfo->line);
        std::vector<llvm::Metadata*> members;
        members.reserve(t->structInfo->fields.size());
        for (size_t i = 0; i < t->structInfo->fields.size(); ++i) {
            const auto& f = t->structInfo->fields[i];
            std::string fname = asAscii(f.name);
            auto* memberType = mapDIType(f.type);
            uint64_t offsetBits = sl->getElementOffsetInBits(static_cast<unsigned>(i));
            uint64_t sizeBits = memberType ? memberType->getSizeInBits()
                                           : dl.getTypeSizeInBits(st->getElementType(static_cast<unsigned>(i)));
            uint32_t alignBits = static_cast<uint32_t>(
                dl.getABITypeAlign(st->getElementType(static_cast<unsigned>(i))).value() * 8);
            members.push_back(diBuilder->createMemberType(
                diCU, fname, diFile, static_cast<unsigned>(f.line),
                sizeBits, alignBits, offsetBits,
                llvm::DINode::FlagZero, memberType));
        }
        uint64_t totalBits = dl.getTypeSizeInBits(st);
        uint32_t structAlignBits = static_cast<uint32_t>(dl.getABITypeAlign(st).value() * 8);
        auto* finalTy = diBuilder->createStructType(
            diCU, sname, diFile, structLine,
            totalBits, structAlignBits,
            llvm::DINode::FlagZero, /*derivedFrom*/ nullptr,
            diBuilder->getOrCreateArray(members));
        diStructTypeCache[t] = finalTy;
        return finalTy;
    }

    llvm::DISubroutineType* createDISubroutineType(Symbol* fn) {
        if (!debugEnabled || !diBuilder) return nullptr;
        std::vector<llvm::Metadata*> elems;
        elems.push_back(mapDIType(fn->returnType));
        for (auto* pt : fn->paramTypes) elems.push_back(mapDIType(pt));
        return diBuilder->createSubroutineType(diBuilder->getOrCreateTypeArray(elems));
    }

    Symbol* symbolOf(const SyntaxNode& node) const {
        const auto* info = analysis.find(node.greenNode());
        return info ? info->resolvedSymbol : nullptr;
    }

    Symbol* methodSymbolOf(const SyntaxNode& node) const {
        const auto* info = analysis.find(node.greenNode());
        return info ? info->resolvedMethodSymbol : nullptr;
    }

    ::Type* typeOf(const SyntaxNode& node) const {
        return analysis.typeOf(node.greenNode());
    }

    bool initializeTargetsOnce() {
        static const bool ok = []() {
            llvm::InitializeAllTargetInfos();
            llvm::InitializeAllTargets();
            llvm::InitializeAllTargetMCs();
            llvm::InitializeAllAsmPrinters();
            llvm::InitializeAllAsmParsers();
            return true;
        }();
        return ok;
    }

    bool initializeTargetMachine() {
        if (targetMachine) return true;
        initializeTargetsOnce();
        std::string triple = targetTriple.empty()
            ? llvm::sys::getDefaultTargetTriple() : targetTriple;
        std::string lookupErr;
        const llvm::Target* target = llvm::TargetRegistry::lookupTarget(triple, lookupErr);
        if (!target) {
            error(0, "Failed to find target '" + triple + "': " + lookupErr);
            return false;
        }
        llvm::TargetOptions opts;
        opts.UseInitArray = true;   // emit .init_array (run by the loader) not legacy .ctors
        std::optional<llvm::Reloc::Model> rm = llvm::Reloc::PIC_;
        targetMachine.reset(target->createTargetMachine(
            llvm::Triple(triple), "generic", "", opts, rm));
        if (!targetMachine) {
            error(0, "Failed to create TargetMachine for '" + triple + "'");
            return false;
        }
        module->setDataLayout(targetMachine->createDataLayout());
        module->setTargetTriple(llvm::Triple(triple));
        return true;
    }

    llvm::AllocaInst* createEntryAlloca(llvm::Function* fn, llvm::Type* t, const std::string& name) {
        llvm::IRBuilder<> tmp(&fn->getEntryBlock(), fn->getEntryBlock().begin());
        return tmp.CreateAlloca(t, nullptr, name);
    }

    bool paramIsByPointer(Symbol* sym, size_t i) {
        if (!sym || i >= sym->paramTypes.size()) return false;
        if (!structHasClassFields(sym->paramTypes[i])) return false;
        if (!sym->escapeInfo.analyzed) return false;
        if (i >= sym->escapeInfo.params.size()) return false;
        if (sym->escapeInfo.params[i] != EscapeKind::NoEscape) return false;
        if (i < sym->escapeInfo.paramMutated.size() && sym->escapeInfo.paramMutated[i]) return false;
        return true;
    }

    llvm::Function* getOrDeclareExternalFunction(Symbol* sym, ::Type* /*receiver*/) {
        auto it = values.find(sym);
        if (it != values.end()) return llvm::cast<llvm::Function>(it->second);
        if (!sym) return nullptr;

        // A method/constructor takes `this` and is mangled by its owning class,
        // independent of the static receiver, so identity holds across modules.
        StructInfo* owner = sym->methodOwner;
        std::vector<llvm::Type*> paramTypes;
        if (owner) paramTypes.push_back(llvm::PointerType::get(ctx, 0));
        for (size_t i = 0; i < sym->paramTypes.size(); ++i) {
            auto* pt = sym->paramTypes[i];
            if (isUnsupportedType(pt)) return nullptr;
            bool isOut = sym->isExternal && i < sym->paramIsOut.size() && sym->paramIsOut[i];
            if (isOut || (!sym->isExternal && paramIsByPointer(sym, i))) {
                paramTypes.push_back(llvm::PointerType::get(ctx, 0));
            } else {
                paramTypes.push_back(mapType(pt));
            }
        }
        if (sym->returnType && !sym->returnType->isVoid() && isUnsupportedType(sym->returnType)) {
            return nullptr;
        }
        if (sym->abiThrows && !sym->isExternal) paramTypes.push_back(llvm::PointerType::get(ctx, 0));
        llvm::Type* retType = mapType(sym->returnType);
        auto* fnType = llvm::FunctionType::get(retType, paramTypes, false);

        std::string mangled = asAscii(sym->name);
        if (owner) mangled = asAscii(owner->name) + "_" + mangled;

        if (auto* existing = module->getFunction(mangled)) {
            values[sym] = existing;
            return existing;
        }
        auto* func = llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, mangled, module.get());
        values[sym] = func;
        return func;
    }

    void declareFunction(const ast::FuncDecl& fn) {
        Symbol* sym = symbolOf(fn.node);
        if (!sym) return;
        ::Type* receiver = analysis.receiverOf(fn.node.greenNode());

        std::vector<llvm::Type*> paramTypes;
        if (receiver) paramTypes.push_back(llvm::PointerType::get(ctx, 0));
        auto fname = fn.nameText().value_or(std::u16string{});
        for (size_t i = 0; i < sym->paramTypes.size(); ++i) {
            auto* pt = sym->paramTypes[i];
            if (isUnsupportedType(pt)) {
                error(fn.node.startOffset(),
                      "Function '" + asAscii(fname) +
                      "' has unsupported parameter type '" + (pt ? pt->toString() : "<null>") + "'");
                return;
            }
            if (paramIsByPointer(sym, i)) {
                paramTypes.push_back(llvm::PointerType::get(ctx, 0));
            } else {
                paramTypes.push_back(mapType(pt));
            }
        }
        if (sym->returnType && !sym->returnType->isVoid() && isUnsupportedType(sym->returnType)) {
            error(fn.node.startOffset(),
                  "Function '" + asAscii(fname) + "' has unsupported return type '" + sym->returnType->toString() + "'");
            return;
        }
        if (sym->abiThrows && !sym->isExternal) paramTypes.push_back(llvm::PointerType::get(ctx, 0));
        llvm::Type* retType = mapType(sym->returnType);
        auto* fnType = llvm::FunctionType::get(retType, paramTypes, false);

        std::string mangled = asAscii(fname);
        if (receiver && receiver->structInfo) {
            mangled = asAscii(receiver->structInfo->name) + "_" + mangled;
        }
        // A throwing top-level `main` is renamed; a compiler-emitted `main`
        // wrapper handles the error slot and prints any escaping exception.
        if (!receiver && sym->abiThrows && mangled == "main") mangled = "ens.main";
        auto* func = llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, mangled, module.get());
        values[sym] = func;
    }

    static bool ctorHasExplicitSuper(const ast::FuncDecl& fn) {
        auto body = fn.body();
        if (!body) return false;
        for (auto& s : body->statements()) {
            auto es = s.asExpressionStmt();
            if (!es) continue;
            auto expr = es->expression();
            if (expr && expr->asCall()) {
                if (auto cal = expr->asCall()->callee())
                    if (cal->asSuper()) return true;
            }
        }
        return false;
    }

    void emitFunction(const ast::FuncDecl& fn) {
        Symbol* sym = symbolOf(fn.node);
        if (!sym) return;
        if (isInterceptedTraceMethod(sym)) return;
        auto it = values.find(sym);
        if (it == values.end()) return;

        byPointerParams.clear();
        incomingErrorSlot = nullptr;
        localErrorSlot = nullptr;
        catchDispatchBB = nullptr;
        currentHasCatch = !fn.catchClauses().empty();
        paramCleanupWatermark = 0;
        currentFunction = llvm::cast<llvm::Function>(it->second);
        currentReturnType = sym->returnType;
        auto* entry = llvm::BasicBlock::Create(ctx, "entry", currentFunction);
        builder->SetInsertPoint(entry);

        llvm::DISubprogram* sp = nullptr;
        llvm::DIScope* prevScope = currentDIScope;
        if (debugEnabled && diBuilder) {
            auto fname = fn.nameText().value_or(std::u16string{});
            auto [line, col] = posOf(fn.node.startOffset());
            sp = diBuilder->createFunction(
                diCU, asAscii(fname), asAscii(fname), diFile,
                static_cast<unsigned>(line),
                createDISubroutineType(sym),
                /*scopeLine*/ static_cast<unsigned>(line),
                llvm::DINode::FlagPrototyped,
                llvm::DISubprogram::SPFlagDefinition);
            currentFunction->setSubprogram(sp);
            currentDIScope = sp;
        }

        setLocationFromNode(fn.node);

        ::Type* receiver = analysis.receiverOf(fn.node.greenNode());

        {
            auto rawName = fn.nameText().value_or(std::u16string{});
            std::string display = asAscii(rawName);
            if (receiver && receiver->structInfo)
                display = asAscii(receiver->structInfo->name) + "." + display;
            bool isEntry = !receiver && rawName == u"main";
            auto nameTok = fn.nameToken();
            uint32_t lineOffset = nameTok ? nameTok->startOffset() : fn.node.startOffset();
            auto [eline, ecol] = posOf(lineOffset);
            recordSymtabEntry(currentFunction, display, static_cast<int>(eline), isEntry);
        }

        auto argIter = currentFunction->args().begin();
        auto argEnd  = currentFunction->args().end();

        // Implicit `this` parameter for methods
        Symbol* thisSym = analysis.thisSymbolOf(fn.node.greenNode());
        if (receiver && argIter != argEnd) {
            argIter->setName("this");
            llvm::Type* ptrTy = llvm::PointerType::get(ctx, 0);
            auto* thisAlloca = createEntryAlloca(currentFunction, ptrTy, "this.addr");
            builder->CreateStore(&*argIter, thisAlloca);
            if (thisSym) values[thisSym] = thisAlloca;
            ++argIter;
        }

        // Construct the base subobject first when a constructor omits an explicit super(...)
        // and the base has a zero-argument constructor.
        if (receiver && receiver->structInfo && receiver->structInfo->baseInfo && thisSym &&
            sym->name == receiver->structInfo->name && !ctorHasExplicitSuper(fn)) {
            StructInfo* base = receiver->structInfo->baseInfo;
            int bidx = base->findMethodIndex(base->name);
            if (bidx >= 0) {
                Symbol* baseCtor = base->methods[bidx].symbol;
                if (baseCtor && baseCtor->paramTypes.empty()) {
                    if (llvm::Function* bfn = getOrDeclareExternalFunction(baseCtor, nullptr)) {
                        llvm::Value* thisVal = builder->CreateLoad(
                            llvm::PointerType::get(ctx, 0), values[thisSym], "this");
                        builder->CreateCall(bfn, { thisVal });
                    }
                }
            }
        }

        cleanupStack.emplace_back();

        auto params = fn.parameters();
        for (size_t i = 0; i < params.size() && argIter != argEnd; ++i, ++argIter) {
            auto& param = params[i];
            Symbol* psym = symbolOf(param.node);
            if (!psym) continue;
            std::string pname = asAscii(psym->name);
            argIter->setName(pname);
            bool byPointer = paramIsByPointer(sym, i);
            llvm::Type* lt = byPointer ? llvm::PointerType::get(ctx, 0) : mapType(psym->type);
            auto* alloca = createEntryAlloca(currentFunction, lt, pname);
            builder->CreateStore(&*argIter, alloca);
            values[psym] = alloca;

            if (byPointer) {
                byPointerParams.insert(psym);
            } else if (structHasClassFields(psym->type)) {
                emitStructFieldRetain(psym->type, alloca);
                cleanupStack.back().push_back({ alloca, psym->type });
            }

            if (debugEnabled && diBuilder && sp) {
                llvm::DIType* paramDIType = mapDIType(psym->type);
                if (paramDIType) {
                    auto [pline, pcol] = posOf(param.node.startOffset());
                    auto* diVar = diBuilder->createParameterVariable(
                        sp, pname, static_cast<unsigned>(i + 1), diFile,
                        static_cast<unsigned>(pline), paramDIType,
                        /*AlwaysPreserve*/ true);
                    diBuilder->insertDeclare(
                        alloca, diVar, diBuilder->createExpression(),
                        llvm::DILocation::get(ctx, static_cast<unsigned>(pline),
                                              static_cast<unsigned>(pcol), sp),
                        builder->GetInsertBlock());
                }
            }
        }

        // The trailing error-slot parameter, then the watermark separating param
        // copies (live for catch clauses) from body locals.
        if (sym->abiThrows && argIter != argEnd) {
            argIter->setName("err.slot.in");
            incomingErrorSlot = &*argIter;
            ++argIter;
        }
        paramCleanupWatermark = cleanupStack.back().size();
        if (currentHasCatch) {
            auto* ptrTy = llvm::PointerType::get(ctx, 0);
            localErrorSlot = createEntryAlloca(currentFunction, ptrTy, "err.slot");
            builder->CreateStore(llvm::ConstantPointerNull::get(ptrTy), localErrorSlot);
            catchDispatchBB = llvm::BasicBlock::Create(ctx, "catch.dispatch", currentFunction);
            throwTargetSlot = localErrorSlot;
            unwindToDispatch = true;
        } else {
            throwTargetSlot = incomingErrorSlot;
            unwindToDispatch = false;
        }

        // Implicit constructor assignments for `this.field` parameters.
        if (receiver && receiver->structInfo) {
            for (size_t i = 0; i < params.size(); ++i) {
                auto& p = params[i];
                if (!p.isThisField()) continue;
                auto fname = p.nameText();
                if (!fname) continue;
                int idx = receiver->structInfo->findFieldIndex(*fname);
                if (idx < 0) continue;
                Symbol* psym = symbolOf(p.node);
                if (!psym || !thisSym) continue;
                llvm::Type* ptrTy = llvm::PointerType::get(ctx, 0);
                llvm::Value* thisVal = builder->CreateLoad(ptrTy, values[thisSym], "this");
                llvm::StructType* st = mapStructType(receiver);
                llvm::Value* fieldAddr = builder->CreateStructGEP(st, thisVal, static_cast<unsigned>(idx),
                                                                  asAscii(*fname) + ".addr");
                llvm::Value* paramVal = builder->CreateLoad(mapType(psym->type), values[psym]);
                if (isReferenceType(psym->type)) {
                    emitRetain(paramVal);
                }
                builder->CreateStore(paramVal, fieldAddr);
                if (structHasClassFields(psym->type)) {
                    emitStructFieldRetain(psym->type, fieldAddr);
                }
            }
        }

        // Body.
        if (auto body = fn.body()) {
            for (auto& s : body->statements()) {
                emitStatement(s);
                if (builder->GetInsertBlock()->getTerminator()) break;
            }
        }
        if (!builder->GetInsertBlock()->getTerminator()) {
            emitFrameCleanup(cleanupStack.back());
            if (currentFunction->getReturnType()->isVoidTy()) builder->CreateRetVoid();
            else if (currentHasCatch) emitReturnZero();
            else builder->CreateUnreachable();
        }

        if (currentHasCatch) emitCatchDispatch(fn);
        cleanupStack.pop_back();

        if (debugEnabled && diBuilder && sp) diBuilder->finalizeSubprogram(sp);
        currentDIScope = prevScope;
        currentFunction = nullptr;
        currentReturnType = nullptr;
    }

    // Walk the thrown object's TypeDescriptor chain against each clause in order;
    // a match binds the object into an owned catch variable and runs the clause.
    void emitCatchDispatch(const ast::FuncDecl& fn) {
        auto* ptrTy = llvm::PointerType::get(ctx, 0);
        builder->SetInsertPoint(catchDispatchBB);
        // Body locals were released at each unwind edge; drop their stale entries.
        if (cleanupStack.back().size() > paramCleanupWatermark)
            cleanupStack.back().resize(paramCleanupWatermark);

        llvm::Value* exc = builder->CreateLoad(ptrTy, localErrorSlot, "exc");
        builder->CreateStore(llvm::ConstantPointerNull::get(ptrTy), localErrorSlot);
        llvm::Value* desc = loadDescriptor(exc);
        auto* typeIs = getOrDefineEnsTypeIs();

        auto clauses = fn.catchClauses();
        for (auto& cc : clauses) {
            ::Type* ct = cc.typeReference()
                ? analysis.typeOf(cc.typeReference()->node.greenNode()) : nullptr;
            StructInfo* csi = (ct && ct->structInfo) ? ct->structInfo : nullptr;
            auto* bodyBB = llvm::BasicBlock::Create(ctx, "catch.body", currentFunction);
            auto* nextBB = llvm::BasicBlock::Create(ctx, "catch.next", currentFunction);
            llvm::Value* match = csi
                ? static_cast<llvm::Value*>(builder->CreateCall(typeIs,
                      { desc, getOrEmitTypeDescriptor(csi) }, "match"))
                : static_cast<llvm::Value*>(llvm::ConstantInt::getFalse(ctx));
            builder->CreateCondBr(match, bodyBB, nextBB);

            builder->SetInsertPoint(bodyBB);
            Symbol* var = nullptr;
            if (auto* info = analysis.find(cc.node.greenNode())) var = info->resolvedSymbol;
            auto* varSlot = createEntryAlloca(currentFunction, ptrTy, "catch.var");
            builder->CreateStore(exc, varSlot);
            if (var) values[var] = varSlot;
            cleanupStack.emplace_back();
            cleanupStack.back().push_back({ varSlot, var ? var->type : ct });

            llvm::Value* prevTarget = throwTargetSlot;
            bool prevDispatch = unwindToDispatch;
            llvm::Value* prevCatchVar = currentCatchVarSlot;
            throwTargetSlot = incomingErrorSlot;
            unwindToDispatch = false;
            currentCatchVarSlot = varSlot;
            if (auto cb = cc.body()) {
                for (auto& s : cb->statements()) {
                    emitStatement(s);
                    if (builder->GetInsertBlock()->getTerminator()) break;
                }
            }
            if (!builder->GetInsertBlock()->getTerminator()) {
                emitFrameCleanup(cleanupStack.back());            // catch variable
                emitFrameCleanupFrom(cleanupStack.front(), 0);    // parameter copies
                emitReturnZero();
            }
            throwTargetSlot = prevTarget;
            unwindToDispatch = prevDispatch;
            currentCatchVarSlot = prevCatchVar;
            cleanupStack.pop_back();

            builder->SetInsertPoint(nextBB);
        }

        // No clause matched: propagate if the function may throw, else unreachable.
        if (incomingErrorSlot) {
            builder->CreateStore(exc, incomingErrorSlot);
            emitFullCleanup();
            emitReturnZero();
        } else {
            builder->CreateUnreachable();
        }
    }

    void emitThrowStmt(const ast::ThrowStatement& s) {
        auto value = s.value();
        if (!value || !throwTargetSlot) return;
        bool borrowed = !expressionProducesOwnedRef(*value);
        llvm::Value* v = emitExpr(*value);
        if (!v) return;
        if (borrowed) emitRetain(v);
        builder->CreateStore(v, throwTargetSlot);
        captureTraceInto(v);
        emitErrorUnwind();
    }

    // Capture the originating stack onto the thrown Error. Error.frames lives at
    // payload offset 8 (message occupies offset 0), flattened the same for every
    // subclass. Releases any prior trace so a re-thrown object does not leak.
    void captureTraceInto(llvm::Value* errObj) {
        auto* ptrTy = llvm::PointerType::get(ctx, 0);
        auto* i64 = llvm::Type::getInt64Ty(ctx);
        llvm::Value* slot = builder->CreateGEP(llvm::Type::getInt8Ty(ctx), errObj,
            llvm::ConstantInt::get(i64, 8), "frames.slot");
        builder->CreateCall(getOrDefineEnsRelease(), { builder->CreateLoad(ptrTy, slot, "frames.old") });
        llvm::Value* trace = builder->CreateCall(captureTraceFn(),
            { llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), 64) }, "trace");
        builder->CreateStore(trace, slot);
    }

    void emitRethrowStmt() {
        if (!currentCatchVarSlot || !incomingErrorSlot) return;
        auto* ptrTy = llvm::PointerType::get(ctx, 0);
        llvm::Value* obj = builder->CreateLoad(ptrTy, currentCatchVarSlot, "rethrow.obj");
        builder->CreateStore(obj, incomingErrorSlot);
        // Null the catch variable so the upcoming cleanup does not release the
        // object we are re-propagating (ens_release(null) is a no-op).
        builder->CreateStore(llvm::ConstantPointerNull::get(ptrTy), currentCatchVarSlot);
        emitFullCleanup();
        emitReturnZero();
    }

    // Real entry point for a program whose `main` may throw: call the renamed
    // user main with a root error slot; if an exception escapes, report it on
    // stderr and exit 1.
    void emitMainWrapper(Symbol* msym) {
        auto* ptrTy = llvm::PointerType::get(ctx, 0);
        auto* i32Ty = llvm::Type::getInt32Ty(ctx);
        auto* userMain = llvm::dyn_cast_or_null<llvm::Function>(
            values.count(msym) ? values[msym] : nullptr);
        if (!userMain) return;
        bool retsVoid = !msym->returnType || msym->returnType->isVoid();

        auto* wrapper = llvm::Function::Create(llvm::FunctionType::get(i32Ty, {}, false),
            llvm::Function::ExternalLinkage, "main", module.get());
        builder->SetInsertPoint(llvm::BasicBlock::Create(ctx, "entry", wrapper));
        auto* slot = builder->CreateAlloca(ptrTy, nullptr, "err.slot");
        builder->CreateStore(llvm::ConstantPointerNull::get(ptrTy), slot);
        llvm::Value* r = builder->CreateCall(userMain, { slot });
        llvm::Value* err = builder->CreateLoad(ptrTy, slot, "err");
        llvm::Value* thrown = builder->CreateICmpNE(
            err, llvm::ConstantPointerNull::get(ptrTy), "thrown");
        auto* okBB = llvm::BasicBlock::Create(ctx, "ok", wrapper);
        auto* errBB = llvm::BasicBlock::Create(ctx, "uncaught", wrapper);
        builder->CreateCondBr(thrown, errBB, okBB);

        builder->SetInsertPoint(okBB);
        builder->CreateRet(retsVoid ? llvm::ConstantInt::get(i32Ty, 0) : r);

        builder->SetInsertPoint(errBB);
        llvm::Value* stderrF = getStderr();
        auto* fputs = getOrDeclareFputs();
        llvm::Value* desc = loadDescriptor(err);
        llvm::Value* nameAddr = builder->CreateGEP(getTypeDescriptorTy(), desc,
            { llvm::ConstantInt::get(i32Ty, 0), llvm::ConstantInt::get(i32Ty, 0) }, "name.addr");
        llvm::Value* typeName = builder->CreateLoad(ptrTy, nameAddr, "type.name");
        llvm::Value* msg = builder->CreateLoad(ptrTy, err, "exc.message");  // Error.message at offset 0
        builder->CreateCall(fputs, { builder->CreateGlobalString("Unhandled exception ", ".uex.pfx"), stderrF });
        builder->CreateCall(fputs, { typeName, stderrF });
        builder->CreateCall(fputs, { builder->CreateGlobalString(": ", ".uex.sep"), stderrF });
        builder->CreateCall(fputs, { emitStringDataPtr(msg), stderrF });
        builder->CreateCall(fputs, { builder->CreateGlobalString("\n", ".uex.nl"), stderrF });
        llvm::Value* frames = builder->CreateLoad(ptrTy,
            builder->CreateGEP(llvm::Type::getInt8Ty(ctx), err,
                llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx), 8)), "exc.frames");
        llvm::Value* traceStr = builder->CreateCall(formatTraceFn(), { frames }, "trace.str");
        builder->CreateCall(fputs, { emitStringDataPtr(traceStr), stderrF });
        builder->CreateCall(getOrDefineEnsRelease(), { err });
        builder->CreateRet(llvm::ConstantInt::get(i32Ty, 1));
    }

    // ===== Statements =====

    void emitStatement(const ast::Statement& s) {
        setLocationFromNode(s.node);
        if (const auto b = s.asBlock()) { emitBlock(*b); return; }
        if (const auto l = s.asLet()) { emitLetStmt(*l); return; }
        if (const auto v = s.asTypedVarDecl()) { emitTypedVarDecl(*v); return; }
        if (const auto i = s.asIf()) { emitIfStmt(*i); return; }
        if (const auto w = s.asWhile()) { emitWhileStmt(*w); return; }
        if (const auto r = s.asReturn()) { emitReturnStmt(*r); return; }
        if (const auto th = s.asThrow()) { emitThrowStmt(*th); return; }
        if (s.asRethrow()) { emitRethrowStmt(); return; }
        if (const auto e = s.asExpressionStmt()) {
            if (const auto expr = e->expression()) emitExpr(*expr);
        }
    }

    void emitBlock(const ast::Block& block) {
        llvm::DIScope* prev = currentDIScope;
        if (debugEnabled && diBuilder && currentDIScope) {
            auto [line, col] = posOf(block.node.startOffset());
            currentDIScope = diBuilder->createLexicalBlock(
                currentDIScope, diFile,
                static_cast<unsigned>(line),
                static_cast<unsigned>(col));
        }
        cleanupStack.emplace_back();
        for (auto& child : block.statements()) {
            emitStatement(child);
            if (builder->GetInsertBlock()->getTerminator()) break;
        }
        if (!builder->GetInsertBlock()->getTerminator()) {
            emitFrameCleanup(cleanupStack.back());
        }
        cleanupStack.pop_back();
        currentDIScope = prev;
    }

    void emitRetain(llvm::Value* val) {
        if (!val || llvm::isa<llvm::ConstantPointerNull>(val)) return;
        builder->CreateCall(getOrDefineEnsRetain(), { val });
    }

    void emitRelease(llvm::Value* val) {
        if (!val || llvm::isa<llvm::ConstantPointerNull>(val)) return;
        builder->CreateCall(getOrDefineEnsRelease(), { val });
    }

    llvm::Value* emitWeakInit(llvm::Value* val) {
        if (!val || llvm::isa<llvm::ConstantPointerNull>(val)) {
            return llvm::ConstantPointerNull::get(llvm::PointerType::get(ctx, 0));
        }
        return builder->CreateCall(getOrDefineEnsWeakInit(), { val });
    }

    void emitWeakRelease(llvm::Value* val) {
        if (!val || llvm::isa<llvm::ConstantPointerNull>(val)) return;
        builder->CreateCall(getOrDefineEnsWeakRelease(), { val });
    }

    bool classLetCanBorrow(Symbol* lhs, const ast::Expression& init) {
        if (!lhs || !isReferenceType(lhs->type)) return false;
        if (lhs->localEscape != EscapeKind::NoEscape) return false;
        if (lhs->reassigned) return false;
        auto id = init.asIdent();
        if (!id) {
            if (auto p = init.asParen()) {
                if (auto inner = p->inner()) return classLetCanBorrow(lhs, *inner);
            }
            return false;
        }
        Symbol* src = symbolOf(id->node);
        if (!src || !isReferenceType(src->type)) return false;
        if (src->reassigned) return false;
        return true;
    }

    bool isClassBorrowMode(Symbol* sym) {
        if (!sym || sym->kind != SymbolKind::Variable) return false;
        if (!isReferenceType(sym->type)) return false;
        if (sym->localEscape != EscapeKind::NoEscape) return false;
        if (!sym->allAssignsFromParam) return false;
        return true;
    }

    bool isStructBorrowMode(Symbol* sym, const ast::Expression& init) {
        if (!sym || sym->kind != SymbolKind::Variable) return false;
        if (!structHasClassFields(sym->type)) return false;
        if (sym->localEscape != EscapeKind::NoEscape) return false;
        if (sym->reassigned) return false;
        if (sym->structFieldsMutated) return false;
        auto id = init.asIdent();
        if (!id) {
            if (auto p = init.asParen()) {
                if (auto inner = p->inner()) return isStructBorrowMode(sym, *inner);
            }
            return false;
        }
        Symbol* src = symbolOf(id->node);
        if (!src) return false;
        if (!structHasClassFields(src->type)) return false;
        if (src->reassigned) return false;
        return true;
    }

    Symbol* moveSourceSymbol(const ast::Expression& rhs) {
        if (auto id = rhs.asIdent()) {
            Symbol* src = symbolOf(id->node);
            if (!src || src->kind != SymbolKind::Variable) return nullptr;
            if (!isReferenceType(src->type)) return nullptr;
            if (src->lastUseInLoop) return nullptr;
            if (src->lastUseRef != id->node.greenNode()) return nullptr;
            return src;
        }
        if (auto p = rhs.asParen()) {
            if (auto inner = p->inner()) return moveSourceSymbol(*inner);
        }
        return nullptr;
    }

    void emitDebugDeclareForLocal(Symbol* sym, llvm::Value* alloca, uint32_t offset) {
        if (!debugEnabled || !diBuilder || !currentDIScope) return;
        llvm::DIType* diVarType = mapDIType(sym->type);
        if (!diVarType) return;
        auto [line, col] = posOf(offset);
        auto* diVar = diBuilder->createAutoVariable(
            currentDIScope, asAscii(sym->name), diFile,
            static_cast<unsigned>(line), diVarType);
        diBuilder->insertDeclare(
            alloca, diVar, diBuilder->createExpression(),
            llvm::DILocation::get(ctx, static_cast<unsigned>(line),
                                  static_cast<unsigned>(col), currentDIScope),
            builder->GetInsertBlock());
    }

    void emitLetStmt(const ast::LetStatement& s) {
        Symbol* sym = symbolOf(s.node);
        if (!sym) return;
        if (isUnsupportedType(sym->type)) {
            error(s.node.startOffset(),
                  "Variable '" + asAscii(sym->name) + "' has unsupported type '" +
                  (sym->type ? sym->type->toString() : "<null>") + "'");
            return;
        }
        llvm::Type* lt = mapType(sym->type);
        auto* alloca = createEntryAlloca(currentFunction, lt, asAscii(sym->name));
        values[sym] = alloca;

        if (sym->stackPromoted) {
            setLocationFromNode(s.node);
            emitDebugDeclareForLocal(sym, alloca, s.node.startOffset());
            if (auto init = s.initializer()) {
                emitStackArrayInit(sym, sym->type, *init, alloca);
            }
            return;
        }

        bool elideClassRetain = false;
        bool elideStructRetain = false;
        if (auto init = s.initializer()) {
            elideClassRetain = classLetCanBorrow(sym, *init);
            elideStructRetain = isStructBorrowMode(sym, *init);
        }
        if (!elideClassRetain && isClassBorrowMode(sym)) {
            elideClassRetain = true;
        }
        if (!elideClassRetain && !elideStructRetain) {
            registerOwnedLocal(alloca, sym->type);
        }

        if (debugEnabled && diBuilder && currentDIScope) {
            llvm::DIType* diVarType = mapDIType(sym->type);
            if (diVarType) {
                auto [line, col] = posOf(s.node.startOffset());
                auto* diVar = diBuilder->createAutoVariable(
                    currentDIScope, asAscii(sym->name), diFile,
                    static_cast<unsigned>(line), diVarType);
                diBuilder->insertDeclare(
                    alloca, diVar, diBuilder->createExpression(),
                    llvm::DILocation::get(ctx, static_cast<unsigned>(line),
                                          static_cast<unsigned>(col), currentDIScope),
                    builder->GetInsertBlock());
            }
        }

        if (auto init = s.initializer()) {
            setLocationFromNode(s.node);
            bool borrowedSource = !expressionProducesOwnedRef(*init);
            Symbol* moveSrc = !elideClassRetain && isReferenceType(sym->type)
                ? moveSourceSymbol(*init) : nullptr;
            bool needsClassRetain = isReferenceType(sym->type) && borrowedSource && !elideClassRetain && !moveSrc;
            bool needsStructRetain = structHasClassFields(sym->type) && borrowedSource && !elideStructRetain;
            llvm::Value* v = emitExpr(*init);
            if (v) {
                setLocationFromNode(s.node);
                if (needsClassRetain) {
                    emitRetain(v);
                }
                builder->CreateStore(v, alloca);
                if (moveSrc) {
                    auto it = values.find(moveSrc);
                    if (it != values.end()) {
                        builder->CreateStore(
                            llvm::ConstantPointerNull::get(llvm::PointerType::get(ctx, 0)),
                            it->second);
                    }
                }
                if (needsStructRetain) {
                    emitStructFieldRetain(sym->type, alloca);
                }
            }
        } else if (isReferenceType(sym->type)) {
            builder->CreateStore(llvm::ConstantPointerNull::get(llvm::PointerType::get(ctx, 0)), alloca);
        } else if (structHasClassFields(sym->type)) {
            initStructFieldDefaults(sym->type, alloca);
        }
    }

    void emitTypedVarDecl(const ast::TypedVarDeclStatement& s) {
        Symbol* sym = symbolOf(s.node);
        if (!sym) return;
        if (isUnsupportedType(sym->type)) {
            error(s.node.startOffset(),
                  "Variable '" + asAscii(sym->name) + "' has unsupported type '" +
                  (sym->type ? sym->type->toString() : "<null>") + "'");
            return;
        }
        llvm::Type* lt = mapType(sym->type);
        auto* alloca = createEntryAlloca(currentFunction, lt, asAscii(sym->name));
        values[sym] = alloca;

        if (sym->stackPromoted) {
            setLocationFromNode(s.node);
            emitDebugDeclareForLocal(sym, alloca, s.node.startOffset());
            if (auto init = s.initializer()) {
                emitStackArrayInit(sym, sym->type, *init, alloca);
            }
            return;
        }

        bool elideClassRetain = false;
        bool elideStructRetain = false;
        if (auto init = s.initializer()) {
            elideClassRetain = classLetCanBorrow(sym, *init);
            elideStructRetain = isStructBorrowMode(sym, *init);
        }
        if (!elideClassRetain && !elideStructRetain) {
            registerOwnedLocal(alloca, sym->type);
        }

        if (debugEnabled && diBuilder && currentDIScope) {
            llvm::DIType* diVarType = mapDIType(sym->type);
            if (diVarType) {
                auto [line, col] = posOf(s.node.startOffset());
                auto* diVar = diBuilder->createAutoVariable(
                    currentDIScope, asAscii(sym->name), diFile,
                    static_cast<unsigned>(line), diVarType);
                diBuilder->insertDeclare(
                    alloca, diVar, diBuilder->createExpression(),
                    llvm::DILocation::get(ctx, static_cast<unsigned>(line),
                                          static_cast<unsigned>(col), currentDIScope),
                    builder->GetInsertBlock());
            }
        }

        if (auto init = s.initializer()) {
            setLocationFromNode(s.node);
            bool borrowedSource = !expressionProducesOwnedRef(*init);
            Symbol* moveSrc = !elideClassRetain && isReferenceType(sym->type)
                ? moveSourceSymbol(*init) : nullptr;
            bool needsClassRetain = isReferenceType(sym->type) && borrowedSource && !elideClassRetain && !moveSrc;
            bool needsStructRetain = structHasClassFields(sym->type) && borrowedSource && !elideStructRetain;
            llvm::Value* v = emitExprConverted(*init, sym->type);
            if (v) {
                if (needsClassRetain) {
                    emitRetain(v);
                }
                builder->CreateStore(v, alloca);
                if (moveSrc) {
                    auto it = values.find(moveSrc);
                    if (it != values.end()) {
                        builder->CreateStore(
                            llvm::ConstantPointerNull::get(llvm::PointerType::get(ctx, 0)),
                            it->second);
                    }
                }
                if (needsStructRetain) {
                    emitStructFieldRetain(sym->type, alloca);
                }
            }
        } else if (sym->type && sym->type->isStruct() && sym->type->structInfo) {
            setLocationFromNode(s.node);
            initStructFieldDefaults(sym->type, alloca);
        } else if (isReferenceType(sym->type)) {
            builder->CreateStore(llvm::ConstantPointerNull::get(llvm::PointerType::get(ctx, 0)), alloca);
        }
    }

    void initStructFieldDefaults(::Type* t, llvm::Value* base) {
        if (!t || !t->structInfo) return;
        llvm::StructType* st = mapStructType(t);
        auto* ptrTy = llvm::PointerType::get(ctx, 0);
        const auto& fields = t->structInfo->fields;
        defaultInitStack.push_back({t, base});
        for (size_t i = 0; i < fields.size(); ++i) {
            const auto& fi = fields[i];
            bool wrote = false;
            if (fi.declaration) {
                auto fieldNode = SyntaxNode::makeRoot(fi.declaration);
                auto fd = ast::FieldDecl::cast(*fieldNode);
                if (fd) {
                    if (auto dv = fd->defaultValue()) {
                        if (auto dvExpr = dv->expression()) {
                            if (llvm::Value* v = emitExpr(*dvExpr)) {
                                llvm::Value* fieldAddr = builder->CreateStructGEP(
                                    st, base, static_cast<unsigned>(i), asAscii(fi.name) + ".addr");
                                bool borrowed = !expressionProducesOwnedRef(*dvExpr);
                                if (borrowed && isReferenceType(fi.type)) {
                                    emitRetain(v);
                                }
                                builder->CreateStore(v, fieldAddr);
                                if (borrowed && structHasClassFields(fi.type)) {
                                    emitStructFieldRetain(fi.type, fieldAddr);
                                }
                                wrote = true;
                            }
                        }
                    }
                }
            }
            if (!wrote && fi.type && fi.type->isClass()) {
                llvm::Value* fieldAddr = builder->CreateStructGEP(
                    st, base, static_cast<unsigned>(i), asAscii(fi.name) + ".addr");
                builder->CreateStore(llvm::ConstantPointerNull::get(ptrTy), fieldAddr);
            }
        }
        defaultInitStack.pop_back();
    }

    void emitIfStmt(const ast::IfStatement& s) {
        auto cond = s.condition();
        if (!cond) return;
        llvm::Value* condV = emitExpr(*cond);
        if (!condV) return;
        bool hasElse = s.elseClause().has_value();
        auto* thenBB = llvm::BasicBlock::Create(ctx, "if.then", currentFunction);
        auto* elseBB = hasElse ? llvm::BasicBlock::Create(ctx, "if.else", currentFunction) : nullptr;
        auto* mergeBB = llvm::BasicBlock::Create(ctx, "if.end", currentFunction);

        builder->CreateCondBr(condV, thenBB, elseBB ? elseBB : mergeBB);
        builder->SetInsertPoint(thenBB);
        if (auto b = s.thenBlock()) emitBlock(*b);
        if (!builder->GetInsertBlock()->getTerminator()) builder->CreateBr(mergeBB);

        if (elseBB) {
            builder->SetInsertPoint(elseBB);
            auto ec = *s.elseClause();
            if (auto innerIf = ec.ifStatement()) emitIfStmt(*innerIf);
            else if (auto bb = ec.block()) emitBlock(*bb);
            if (!builder->GetInsertBlock()->getTerminator()) builder->CreateBr(mergeBB);
        }
        builder->SetInsertPoint(mergeBB);
    }

    void emitWhileStmt(const ast::WhileStatement& s) {
        auto* condBB = llvm::BasicBlock::Create(ctx, "while.cond", currentFunction);
        auto* bodyBB = llvm::BasicBlock::Create(ctx, "while.body", currentFunction);
        auto* endBB  = llvm::BasicBlock::Create(ctx, "while.end",  currentFunction);

        builder->CreateBr(condBB);
        builder->SetInsertPoint(condBB);
        if (auto cond = s.condition()) {
            llvm::Value* condV = emitExpr(*cond);
            if (condV) builder->CreateCondBr(condV, bodyBB, endBB);
        }
        builder->SetInsertPoint(bodyBB);
        if (auto b = s.body()) emitBlock(*b);
        if (!builder->GetInsertBlock()->getTerminator()) builder->CreateBr(condBB);
        builder->SetInsertPoint(endBB);
    }

    void emitReturnStmt(const ast::ReturnStatement& s) {
        if (auto v = s.value()) {
            ::Type* retType = typeOf(v->node);
            bool borrowedSource = !expressionProducesOwnedRef(*v);
            bool needsClassRetain = isReferenceType(retType) && borrowedSource;
            bool needsStructRetain = structHasClassFields(retType) && borrowedSource;

            llvm::Value* val = emitExprConverted(*v, currentReturnType);
            if (!val) { builder->CreateUnreachable(); return; }

            if (needsClassRetain) {
                emitRetain(val);
            } else if (needsStructRetain) {
                emitStructFieldRetainOnValue(retType, val);
            }
            emitFullCleanup();
            builder->CreateRet(val);
        } else {
            emitFullCleanup();
            builder->CreateRetVoid();
        }
    }

    // ===== Expressions =====

    llvm::Value* emitExpr(const ast::Expression& e) {
        setLocationFromNode(e.node);
        if (auto lit = e.asLiteral()) return emitLiteral(*lit);
        if (auto id = e.asIdent()) return emitIdent(*id);
        if (auto th = e.asThis()) return emitThis(*th);
        if (auto su = e.asSuper()) return emitSuper(*su);
        if (auto b = e.asBinary()) return emitBinary(*b);
        if (auto p = e.asPrefix()) return emitPrefix(*p);
        if (auto c = e.asCall()) return emitCall(*c);
        if (auto m = e.asMember()) return emitMember(*m);
        if (auto sm = e.asSafeMember()) return emitSafeMember(*sm);
        if (auto su = e.asSubscript()) return emitSubscript(*su);
        if (auto ss = e.asSafeSubscript()) return emitSafeSubscript(*ss);
        if (auto c = e.asCast()) return emitCast(*c);
        if (auto a = e.asAssign()) return emitAssign(*a);
        if (auto t = e.asTernary()) return emitTernary(*t);
        if (auto n = e.asNew()) return emitNew(*n);
        if (auto tr = e.asTry()) {
            // `try` is transparent at runtime; the post-call check is keyed off
            // the callee's abiThrows, not the marker.
            if (auto operand = tr->operand()) return emitExpr(*operand);
            return nullptr;
        }
        if (auto pr = e.asParen()) {
            if (auto inner = pr->inner()) return emitExpr(*inner);
            return nullptr;
        }
        if (auto al = e.asArrayLiteral()) return emitArrayLiteral(*al);
        error(e.node.startOffset(), "Unsupported expression in codegen");
        return nullptr;
    }

    static long long parseIntText(std::u16string_view text) {
        std::string s; s.reserve(text.size());
        for (char16_t c : text) s.push_back(static_cast<char>(c));
        try {
            if (s.size() > 2 && s[0] == '0' && (s[1] == 'b' || s[1] == 'B'))
                return std::stoll(s.substr(2), nullptr, 2);
            // strip trailing l/L
            if (!s.empty() && (s.back() == 'l' || s.back() == 'L')) s.pop_back();
            return std::stoll(s, nullptr, 0);
        } catch (...) { return 0; }
    }

    static double parseDoubleText(std::u16string_view text) {
        std::string s; s.reserve(text.size());
        for (char16_t c : text) s.push_back(static_cast<char>(c));
        if (!s.empty() && (s.back() == 'f' || s.back() == 'F' || s.back() == 'd' || s.back() == 'D'))
            s.pop_back();
        return std::stod(s);
    }

    llvm::Value* emitLiteral(const ast::LiteralExpression& e) {
        ::Type* t = typeOf(e.node);
        SyntaxKind k = e.literalKind();
        auto tok = e.token();
        std::u16string text = tok ? std::u16string(tok->tokenText()) : std::u16string{};
        switch (k) {
            case SyntaxKind::IntLiteral:
            case SyntaxKind::LongLiteral: {
                llvm::Type* lt = mapType(t);
                long long v = parseIntText(text);
                return llvm::ConstantInt::get(lt, static_cast<uint64_t>(v), t && t->isSignedInteger());
            }
            case SyntaxKind::CharLiteral: {
                llvm::Type* lt = mapType(t);
                uint32_t cp = parseCharLiteralCodepoint(text);
                return llvm::ConstantInt::get(lt, cp, t && t->isSignedInteger());
            }
            case SyntaxKind::FloatLiteral:
            case SyntaxKind::DoubleLiteral:
                return llvm::ConstantFP::get(mapType(t), parseDoubleText(text));
            case SyntaxKind::KwTrue:  return builder->getInt1(true);
            case SyntaxKind::KwFalse: return builder->getInt1(false);
            case SyntaxKind::KwNull:
                return llvm::ConstantPointerNull::get(llvm::PointerType::get(ctx, 0));
            case SyntaxKind::StringLiteral: {
                std::string utf8;
                // Strip surrounding quotes from the lexed text.
                size_t start = 0, end = text.size();
                if (end >= 2 && text.front() == u'"' && text.back() == u'"') { start = 1; end--; }
                for (size_t i = start; i < end; ++i) {
                    char16_t c = text[i];
                    if (c == u'\\' && i + 1 < end) {
                        char16_t n = text[++i];
                        switch (n) {
                            case u'n': utf8.push_back('\n'); break;
                            case u't': utf8.push_back('\t'); break;
                            case u'r': utf8.push_back('\r'); break;
                            case u'\\': utf8.push_back('\\'); break;
                            case u'"': utf8.push_back('"'); break;
                            case u'\'': utf8.push_back('\''); break;
                            case u'{': utf8.push_back('{'); break;
                            case u'}': utf8.push_back('}'); break;
                            default: utf8.push_back(static_cast<char>(n)); break;
                        }
                        continue;
                    }
                    if (c < 0x80) utf8.push_back(static_cast<char>(c));
                    else if (c < 0x800) {
                        utf8.push_back(static_cast<char>(0xC0 | (c >> 6)));
                        utf8.push_back(static_cast<char>(0x80 | (c & 0x3F)));
                    } else {
                        utf8.push_back(static_cast<char>(0xE0 | (c >> 12)));
                        utf8.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
                        utf8.push_back(static_cast<char>(0x80 | (c & 0x3F)));
                    }
                }
                return emitStringLiteralObject(utf8);
            }
            default:
                error(e.node.startOffset(), "Unsupported literal");
                return nullptr;
        }
    }

    llvm::Value* emitIdent(const ast::IdentExpression& e) {
        Symbol* sym = symbolOf(e.node);
        if (!sym) return nullptr;
        if (sym->kind == SymbolKind::SiblingField) {
            if (defaultInitStack.empty()) {
                error(e.node.startOffset(),
                      "Internal: sibling-field reference outside a default-init context");
                return nullptr;
            }
            auto& ctx = defaultInitStack.back();
            llvm::StructType* st = mapStructType(ctx.structType);
            llvm::Value* fieldAddr = builder->CreateStructGEP(
                st, ctx.basePtr,
                static_cast<unsigned>(sym->siblingFieldIndex),
                asAscii(sym->name) + ".sib.addr");
            return builder->CreateLoad(mapType(sym->type), fieldAddr,
                                        asAscii(sym->name) + ".sib.load");
        }
        auto it = values.find(sym);
        if (it == values.end()) {
            error(e.node.startOffset(), "Internal: symbol has no LLVM value");
            return nullptr;
        }
        if (sym->kind == SymbolKind::Function) {
            error(e.node.startOffset(), "Function values are not yet first-class");
            return nullptr;
        }
        if (byPointerParams.count(sym)) {
            llvm::Value* ptr = builder->CreateLoad(
                llvm::PointerType::get(ctx, 0), it->second, asAscii(sym->name) + ".byptr");
            return builder->CreateLoad(mapType(sym->type), ptr, asAscii(sym->name) + ".load");
        }
        return builder->CreateLoad(mapType(sym->type), it->second, asAscii(sym->name) + ".load");
    }

    llvm::Value* emitThis(const ast::ThisExpression& e) {
        Symbol* sym = symbolOf(e.node);
        if (!sym) {
            error(e.node.startOffset(), "Internal: 'this' not bound");
            return nullptr;
        }
        auto it = values.find(sym);
        if (it == values.end()) {
            error(e.node.startOffset(), "Internal: 'this' has no LLVM value");
            return nullptr;
        }
        return builder->CreateLoad(llvm::PointerType::get(ctx, 0), it->second, "this");
    }

    // `super` evaluates to the same object pointer as `this`; the base-vs-derived distinction
    // is resolved statically at the call site (direct dispatch / base mangling).
    llvm::Value* emitSuper(const ast::SuperExpression& e) {
        Symbol* sym = symbolOf(e.node);
        if (!sym) {
            error(e.node.startOffset(), "Internal: 'super' not bound");
            return nullptr;
        }
        auto it = values.find(sym);
        if (it == values.end()) {
            error(e.node.startOffset(), "Internal: 'super' has no LLVM value");
            return nullptr;
        }
        return builder->CreateLoad(llvm::PointerType::get(ctx, 0), it->second, "super");
    }

    ::Type* commonNumericType(::Type* a, ::Type* b) {
        if (!a || !b) return nullptr;
        if (a->equals(b)) return a;
        if (a->widensTo(b)) return b;
        if (b->widensTo(a)) return a;
        return nullptr;
    }

    llvm::Value* emitBinary(const ast::BinaryExpression& e) {
        auto leftE = e.left();
        auto rightE = e.right();
        if (!leftE || !rightE) return nullptr;
        llvm::Value* L = emitExpr(*leftE);
        llvm::Value* R = emitExpr(*rightE);
        if (!L || !R) return nullptr;
        ::Type* leftType = typeOf(leftE->node);
        ::Type* rightType = typeOf(rightE->node);

        ::Type* opType = leftType;
        if (leftType && rightType && (leftType->isNumeric() || rightType->isNumeric())) {
            if (::Type* common = commonNumericType(leftType, rightType)) {
                if (!leftType->equals(common))  L = emitNumericConversion(L, leftType,  common);
                if (!rightType->equals(common)) R = emitNumericConversion(R, rightType, common);
                opType = common;
            }
        }
        bool flt = opType && opType->isFloat();
        bool sgn = opType && opType->isSignedInteger();
        auto opTok = e.operatorToken();
        SyntaxKind op = opTok ? opTok->kind() : SyntaxKind::Invalid;
        if ((op == SyntaxKind::EqEq || op == SyntaxKind::NotEq) &&
            (isStringLike(leftType) || isStringLike(rightType))) {
            llvm::Value* eq = builder->CreateCall(getOrDefineEnsStringEq(), { L, R }, "str.eq");
            releaseIfOwnedTemp(L, *leftE);
            releaseIfOwnedTemp(R, *rightE);
            return op == SyntaxKind::NotEq ? builder->CreateNot(eq) : eq;
        }
        if (op == SyntaxKind::Plus && leftType && leftType->isString()) {
            llvm::Value* result = builder->CreateCall(getOrDefineEnsStringConcat(), { L, R }, "str.concat");
            releaseIfOwnedTemp(L, *leftE);
            releaseIfOwnedTemp(R, *rightE);
            return result;
        }
        switch (op) {
            case SyntaxKind::Plus:    return flt ? builder->CreateFAdd(L, R) : builder->CreateAdd(L, R);
            case SyntaxKind::Minus:   return flt ? builder->CreateFSub(L, R) : builder->CreateSub(L, R);
            case SyntaxKind::Star:    return flt ? builder->CreateFMul(L, R) : builder->CreateMul(L, R);
            case SyntaxKind::Slash:   return flt ? builder->CreateFDiv(L, R) : (sgn ? builder->CreateSDiv(L, R) : builder->CreateUDiv(L, R));
            case SyntaxKind::Percent: return flt ? builder->CreateFRem(L, R) : (sgn ? builder->CreateSRem(L, R) : builder->CreateURem(L, R));
            case SyntaxKind::EqEq:    return flt ? builder->CreateFCmpOEQ(L, R) : builder->CreateICmpEQ(L, R);
            case SyntaxKind::NotEq:   return flt ? builder->CreateFCmpONE(L, R) : builder->CreateICmpNE(L, R);
            case SyntaxKind::Lt:      return flt ? builder->CreateFCmpOLT(L, R) : (sgn ? builder->CreateICmpSLT(L, R) : builder->CreateICmpULT(L, R));
            case SyntaxKind::Gt:      return flt ? builder->CreateFCmpOGT(L, R) : (sgn ? builder->CreateICmpSGT(L, R) : builder->CreateICmpUGT(L, R));
            case SyntaxKind::LtEq:    return flt ? builder->CreateFCmpOLE(L, R) : (sgn ? builder->CreateICmpSLE(L, R) : builder->CreateICmpULE(L, R));
            case SyntaxKind::GtEq:    return flt ? builder->CreateFCmpOGE(L, R) : (sgn ? builder->CreateICmpSGE(L, R) : builder->CreateICmpUGE(L, R));
            case SyntaxKind::AmpAmp:
            case SyntaxKind::Amp:     return builder->CreateAnd(L, R);
            case SyntaxKind::PipePipe:
            case SyntaxKind::Pipe:    return builder->CreateOr(L, R);
            case SyntaxKind::Caret:   return builder->CreateXor(L, R);
            case SyntaxKind::LtLt:    return builder->CreateShl(L, R);
            case SyntaxKind::GtGt:    return sgn ? builder->CreateAShr(L, R) : builder->CreateLShr(L, R);
            case SyntaxKind::GtGtGt:  return builder->CreateLShr(L, R);
            default:
                error(e.node.startOffset(), "Unsupported binary operator in codegen");
                return nullptr;
        }
    }

    llvm::Value* emitPrefix(const ast::PrefixExpression& e) {
        auto operand = e.operand();
        if (!operand) return nullptr;
        ::Type* t = typeOf(operand->node);
        bool flt = t && t->isFloat();
        auto opTok = e.operatorToken();
        if (!opTok) return nullptr;
        switch (opTok->kind()) {
            case SyntaxKind::Minus: {
                llvm::Value* v = emitExpr(*operand);
                if (!v) return nullptr;
                return flt ? builder->CreateFNeg(v) : builder->CreateNeg(v);
            }
            case SyntaxKind::Bang: {
                llvm::Value* v = emitExpr(*operand);
                if (!v) return nullptr;
                return builder->CreateNot(v);
            }
            case SyntaxKind::PlusPlus:
            case SyntaxKind::MinusMinus: {
                llvm::Value* lv = emitLValue(*operand);
                if (!lv) return nullptr;
                llvm::Type* lt = mapType(t);
                llvm::Value* v = builder->CreateLoad(lt, lv);
                llvm::Value* one = flt
                    ? static_cast<llvm::Value*>(llvm::ConstantFP::get(lt, 1.0))
                    : static_cast<llvm::Value*>(llvm::ConstantInt::get(lt, 1));
                bool inc = opTok->kind() == SyntaxKind::PlusPlus;
                llvm::Value* nv = inc
                    ? (flt ? builder->CreateFAdd(v, one) : builder->CreateAdd(v, one))
                    : (flt ? builder->CreateFSub(v, one) : builder->CreateSub(v, one));
                builder->CreateStore(nv, lv);
                return nv;
            }
            default:
                error(e.node.startOffset(), "Unsupported unary operator in codegen");
                return nullptr;
        }
    }

    // Wrapper that emits an expression and then numerically converts it to the
    // target type if widening (or any other lossless numeric conversion) was
    // accepted at the analyzer level. No-op when types already match.
    llvm::Value* emitExprConverted(const ast::Expression& e, ::Type* target) {
        llvm::Value* v = emitExpr(e);
        if (!v) return nullptr;
        ::Type* srcT = typeOf(e.node);
        if (!srcT || !target) return v;
        if (srcT->equals(target)) return v;
        bool srcNum = srcT->isInteger() || srcT->isFloat();
        bool dstNum = target->isInteger() || target->isFloat();
        if (!srcNum || !dstNum) return v;
        return emitNumericConversion(v, srcT, target);
    }

    llvm::Value* emitNumericConversion(llvm::Value* v, ::Type* srcT, ::Type* dstT) {
        if (!v || !srcT || !dstT) return v;
        if (srcT->equals(dstT)) return v;

        llvm::Type* srcLlvm = mapType(srcT);
        llvm::Type* dstLlvm = mapType(dstT);
        if (!srcLlvm || !dstLlvm) return v;

        bool srcFloat = srcT->isFloat();
        bool dstFloat = dstT->isFloat();
        bool srcSigned = srcT->isSignedInteger();
        bool dstSigned = dstT->isSignedInteger();

        if (!srcFloat && !dstFloat) {
            // Integer <-> integer.
            unsigned srcBits = srcLlvm->getIntegerBitWidth();
            unsigned dstBits = dstLlvm->getIntegerBitWidth();
            if (srcBits == dstBits) return v;  // signed/unsigned reinterpret - no IR op
            if (srcBits > dstBits) return builder->CreateTrunc(v, dstLlvm, "cast.trunc");
            return srcSigned ? builder->CreateSExt(v, dstLlvm, "cast.sext")
                             : builder->CreateZExt(v, dstLlvm, "cast.zext");
        }
        if (!srcFloat && dstFloat) {
            return srcSigned ? builder->CreateSIToFP(v, dstLlvm, "cast.sitofp")
                             : builder->CreateUIToFP(v, dstLlvm, "cast.uitofp");
        }
        if (srcFloat && !dstFloat) {
            return dstSigned ? builder->CreateFPToSI(v, dstLlvm, "cast.fptosi")
                             : builder->CreateFPToUI(v, dstLlvm, "cast.fptoui");
        }
        // float <-> float
        unsigned srcBits = srcLlvm->getPrimitiveSizeInBits();
        unsigned dstBits = dstLlvm->getPrimitiveSizeInBits();
        if (srcBits == dstBits) return v;
        if (srcBits > dstBits) return builder->CreateFPTrunc(v, dstLlvm, "cast.fptrunc");
        return builder->CreateFPExt(v, dstLlvm, "cast.fpext");
    }

    llvm::Value* emitCast(const ast::CastExpression& e) {
        auto src = e.source();
        if (!src) return nullptr;
        ::Type* srcT = typeOf(src->node);
        ::Type* dstT = typeOf(e.node);
        llvm::Value* v = emitExpr(*src);
        if (!v) return nullptr;
        return emitNumericConversion(v, srcT, dstT);
    }

    llvm::Function* getOrDeclarePuts() {
        if (auto* existing = module->getFunction("puts")) return existing;
        auto* ty = llvm::FunctionType::get(
            llvm::Type::getInt32Ty(ctx),
            { llvm::PointerType::get(ctx, 0) }, false);
        return llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "puts", module.get());
    }

    llvm::Function* getOrDeclareFputs() {
        if (auto* existing = module->getFunction("fputs")) return existing;
        auto* ptrTy = llvm::PointerType::get(ctx, 0);
        auto* ty = llvm::FunctionType::get(llvm::Type::getInt32Ty(ctx), { ptrTy, ptrTy }, false);
        return llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "fputs", module.get());
    }

    // The platform's stderr FILE*. glibc/ELF exposes `stderr`; macOS `__stderrp`;
    // Windows routes through __acrt_iob_func(2).
    llvm::Value* getStderr() {
        auto* ptrTy = llvm::PointerType::get(ctx, 0);
        const llvm::Triple& triple = module->getTargetTriple();
        if (triple.isOSWindows()) {
            auto* fnTy = llvm::FunctionType::get(ptrTy, { llvm::Type::getInt32Ty(ctx) }, false);
            llvm::Function* iob = module->getFunction("__acrt_iob_func");
            if (!iob) iob = llvm::Function::Create(fnTy, llvm::Function::ExternalLinkage,
                "__acrt_iob_func", module.get());
            return builder->CreateCall(iob, { llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), 2) });
        }
        const char* name = triple.isOSDarwin() ? "__stderrp" : "stderr";
        llvm::GlobalVariable* gv = module->getGlobalVariable(name);
        if (!gv) gv = new llvm::GlobalVariable(*module, ptrTy, /*isConstant=*/false,
            llvm::GlobalValue::ExternalLinkage, nullptr, name);
        return builder->CreateLoad(ptrTy, gv, "stderr");
    }

    llvm::Function* getOrDeclareCalloc() {
        if (auto* existing = module->getFunction("calloc")) return existing;
        auto* i64Ty = llvm::Type::getInt64Ty(ctx);
        auto* ty = llvm::FunctionType::get(
            llvm::PointerType::get(ctx, 0),
            { i64Ty, i64Ty }, false);
        return llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "calloc", module.get());
    }

    llvm::Function* getOrDeclareFree() {
        if (auto* existing = module->getFunction("free")) return existing;
        auto* ty = llvm::FunctionType::get(
            llvm::Type::getVoidTy(ctx),
            { llvm::PointerType::get(ctx, 0) }, false);
        return llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "free", module.get());
    }

    llvm::Function* getOrDeclareStrlen() {
        if (auto* existing = module->getFunction("strlen")) return existing;
        auto* ty = llvm::FunctionType::get(
            llvm::Type::getInt64Ty(ctx),
            { llvm::PointerType::get(ctx, 0) }, false);
        return llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "strlen", module.get());
    }

    // ===== Stack traces =====
    //
    // Capture stores raw return addresses on the thrown Error at the throw site
    // (and at panic); symbolication is lazy, driven by a compiler-emitted side
    // table. Capture itself is isolated in ens_capture_trace so it can be swapped
    // per target. The native backend here uses the Itanium unwinder
    // (_Unwind_Backtrace), which reads .eh_frame and works across glibc, musl,
    // and macOS, plus dlopen'd modules.

    bool isPreludeModule() const { return modulePath == kPreludeModulePath; }

    // Error.getStackTrace()/getStackFrames() are recognized at the call site and lowered
    // to the runtime; their prelude bodies are placeholders and never emitted.
    bool isInterceptedTraceMethod(Symbol* sym) const {
        if (!sym || !sym->methodOwner) return false;
        if (sym->methodOwner->name != u"Error" ||
            sym->methodOwner->modulePath != kPreludeModulePath) return false;
        return sym->name == u"getStackTrace" || sym->name == u"getStackFrames";
    }

    // { funcStart, name, file, line, isEntry }
    llvm::StructType* getSymEntryTy() {
        if (symEntryTy) return symEntryTy;
        auto* ptrTy = llvm::PointerType::get(ctx, 0);
        auto* i32 = llvm::Type::getInt32Ty(ctx);
        symEntryTy = llvm::StructType::create(ctx, { ptrTy, ptrTy, ptrTy, i32, i32 }, "EnsSymEntry");
        return symEntryTy;
    }

    // { next, entries, count, lines, lineCount }
    llvm::StructType* getSymChunkTy() {
        if (symChunkTy) return symChunkTy;
        auto* ptrTy = llvm::PointerType::get(ctx, 0);
        auto* i64 = llvm::Type::getInt64Ty(ctx);
        symChunkTy = llvm::StructType::create(ctx, { ptrTy, ptrTy, i64, ptrTy, i64 }, "EnsSymChunk");
        return symChunkTy;
    }

    // { addr, line } : maps a call return address to its precise source line.
    llvm::StructType* getLineEntryTy() {
        if (lineEntryTy) return lineEntryTy;
        auto* ptrTy = llvm::PointerType::get(ctx, 0);
        lineEntryTy = llvm::StructType::create(ctx, { ptrTy, llvm::Type::getInt32Ty(ctx) }, "EnsLineEntry");
        return lineEntryTy;
    }

    llvm::Function* getRuntimeFn(const std::string& name, llvm::FunctionType* ty) {
        if (auto* f = module->getFunction(name)) return f;
        return llvm::Function::Create(ty, llvm::Function::ExternalLinkage, name, module.get());
    }
    llvm::Function* captureTraceFn() {
        auto* ptrTy = llvm::PointerType::get(ctx, 0);
        return getRuntimeFn("ens_capture_trace",
            llvm::FunctionType::get(ptrTy, { llvm::Type::getInt32Ty(ctx) }, false));
    }
    llvm::Function* formatTraceFn() {
        auto* ptrTy = llvm::PointerType::get(ctx, 0);
        return getRuntimeFn("ens_format_trace", llvm::FunctionType::get(ptrTy, { ptrTy }, false));
    }
    llvm::Function* symbolicateFn() {
        auto* ptrTy = llvm::PointerType::get(ctx, 0);
        return getRuntimeFn("ens_symbolicate", llvm::FunctionType::get(ptrTy, { ptrTy }, false));
    }
    llvm::Function* symtabRegisterFn() {
        auto* ptrTy = llvm::PointerType::get(ctx, 0);
        return getRuntimeFn("ens_symtab_register",
            llvm::FunctionType::get(llvm::Type::getVoidTy(ctx), { ptrTy }, false));
    }

    llvm::Function* getOrDeclareMalloc() {
        if (auto* f = module->getFunction("malloc")) return f;
        auto* ptrTy = llvm::PointerType::get(ctx, 0);
        return llvm::Function::Create(
            llvm::FunctionType::get(ptrTy, { llvm::Type::getInt64Ty(ctx) }, false),
            llvm::Function::ExternalLinkage, "malloc", module.get());
    }
    llvm::Function* getOrDeclareMemcpy() {
        if (auto* f = module->getFunction("memcpy")) return f;
        auto* ptrTy = llvm::PointerType::get(ctx, 0);
        auto* i64 = llvm::Type::getInt64Ty(ctx);
        return llvm::Function::Create(
            llvm::FunctionType::get(ptrTy, { ptrTy, ptrTy, i64 }, false),
            llvm::Function::ExternalLinkage, "memcpy", module.get());
    }
    llvm::Function* getOrDeclareMemcmp() {
        if (auto* f = module->getFunction("memcmp")) return f;
        auto* ptrTy = llvm::PointerType::get(ctx, 0);
        auto* i32 = llvm::Type::getInt32Ty(ctx);
        auto* i64 = llvm::Type::getInt64Ty(ctx);
        return llvm::Function::Create(
            llvm::FunctionType::get(i32, { ptrTy, ptrTy, i64 }, false),
            llvm::Function::ExternalLinkage, "memcmp", module.get());
    }
    llvm::Function* getOrDeclareSnprintf() {
        if (auto* f = module->getFunction("snprintf")) return f;
        auto* ptrTy = llvm::PointerType::get(ctx, 0);
        auto* i32 = llvm::Type::getInt32Ty(ctx);
        auto* i64 = llvm::Type::getInt64Ty(ctx);
        return llvm::Function::Create(
            llvm::FunctionType::get(i32, { ptrTy, i64, ptrTy }, /*isVarArg=*/true),
            llvm::Function::ExternalLinkage, "snprintf", module.get());
    }
    llvm::Function* getOrDeclareUnwindBacktrace() {
        if (auto* f = module->getFunction("_Unwind_Backtrace")) return f;
        auto* ptrTy = llvm::PointerType::get(ctx, 0);
        auto* i32 = llvm::Type::getInt32Ty(ctx);
        return llvm::Function::Create(
            llvm::FunctionType::get(i32, { ptrTy, ptrTy }, false),
            llvm::Function::ExternalLinkage, "_Unwind_Backtrace", module.get());
    }
    llvm::Function* getOrDeclareUnwindGetIP() {
        if (auto* f = module->getFunction("_Unwind_GetIP")) return f;
        auto* ptrTy = llvm::PointerType::get(ctx, 0);
        auto* i64 = llvm::Type::getInt64Ty(ctx);
        return llvm::Function::Create(
            llvm::FunctionType::get(i64, { ptrTy }, false),
            llvm::Function::ExternalLinkage, "_Unwind_GetIP", module.get());
    }
    // USHORT RtlCaptureStackBackTrace(ULONG skip, ULONG count, PVOID* out, PULONG hash)
    llvm::Function* getOrDeclareRtlCaptureStackBackTrace() {
        if (auto* f = module->getFunction("RtlCaptureStackBackTrace")) return f;
        auto* ptrTy = llvm::PointerType::get(ctx, 0);
        auto* i32 = llvm::Type::getInt32Ty(ctx);
        return llvm::Function::Create(
            llvm::FunctionType::get(llvm::Type::getInt16Ty(ctx), { i32, i32, ptrTy, ptrTy }, false),
            llvm::Function::ExternalLinkage, "RtlCaptureStackBackTrace", module.get());
    }

    // Single registry head, internal to the prelude module (only prelude-defined
    // runtime functions touch it).
    llvm::GlobalVariable* getSymtabHead() {
        auto* ptrTy = llvm::PointerType::get(ctx, 0);
        if (auto* g = module->getNamedGlobal("_ens_symtab_head")) return g;
        return new llvm::GlobalVariable(*module, ptrTy, /*isConstant=*/false,
            llvm::GlobalValue::InternalLinkage, llvm::ConstantPointerNull::get(ptrTy),
            "_ens_symtab_head");
    }

    void defineSymtabRegister() {
        auto* fn = symtabRegisterFn();
        if (!fn->empty()) return;
        auto* ptrTy = llvm::PointerType::get(ctx, 0);
        fn->addFnAttr(llvm::Attribute::NoUnwind);
        auto saved = builder->saveIP();
        builder->SetInsertPoint(llvm::BasicBlock::Create(ctx, "entry", fn));
        llvm::Value* chunk = fn->getArg(0);
        llvm::Value* head = getSymtabHead();
        llvm::Value* prev = builder->CreateLoad(ptrTy, head, "head");
        builder->CreateStore(prev, chunk);          // chunk->next = head (field 0)
        builder->CreateStore(chunk, head);           // head = chunk
        builder->CreateRetVoid();
        builder->restoreIP(saved);
    }

    void defineUnwindCallback() {
        if (module->getFunction("ens_unwind_cb")) return;
        auto* ptrTy = llvm::PointerType::get(ctx, 0);
        auto* i32 = llvm::Type::getInt32Ty(ctx);
        auto* i64 = llvm::Type::getInt64Ty(ctx);
        auto* stateTy = llvm::StructType::get(ctx, { ptrTy, i32, i32 });  // buf, idx, max
        auto* fn = llvm::Function::Create(
            llvm::FunctionType::get(i32, { ptrTy, ptrTy }, false),
            llvm::Function::InternalLinkage, "ens_unwind_cb", module.get());
        fn->addFnAttr(llvm::Attribute::NoUnwind);
        auto saved = builder->saveIP();
        auto* entry = llvm::BasicBlock::Create(ctx, "entry", fn);
        auto* stop  = llvm::BasicBlock::Create(ctx, "full", fn);
        auto* cont  = llvm::BasicBlock::Create(ctx, "store", fn);
        builder->SetInsertPoint(entry);
        llvm::Value* uctx = fn->getArg(0);
        llvm::Value* state = fn->getArg(1);
        llvm::Value* idxPtr = builder->CreateStructGEP(stateTy, state, 1);
        llvm::Value* maxPtr = builder->CreateStructGEP(stateTy, state, 2);
        llvm::Value* idx = builder->CreateLoad(i32, idxPtr, "idx");
        llvm::Value* mx = builder->CreateLoad(i32, maxPtr, "max");
        builder->CreateCondBr(builder->CreateICmpSGE(idx, mx), stop, cont);
        builder->SetInsertPoint(stop);
        builder->CreateRet(llvm::ConstantInt::get(i32, 5));   // _URC_END_OF_STACK
        builder->SetInsertPoint(cont);
        llvm::Value* ip = builder->CreateCall(getOrDeclareUnwindGetIP(), { uctx }, "ip");
        llvm::Value* buf = builder->CreateLoad(ptrTy, builder->CreateStructGEP(stateTy, state, 0), "buf");
        llvm::Value* slot = builder->CreateGEP(i64, buf, builder->CreateSExt(idx, i64), "slot");
        builder->CreateStore(ip, slot);
        builder->CreateStore(builder->CreateAdd(idx, llvm::ConstantInt::get(i32, 1)), idxPtr);
        builder->CreateRet(llvm::ConstantInt::get(i32, 0));   // _URC_NO_REASON
        builder->restoreIP(saved);
    }

    // ptr ens_resolve_addr(i64): the entry with the greatest funcStart <= addr,
    // or null. Linear scan; traces are rare so no index is built.
    void defineResolveAddr() {
        if (module->getFunction("ens_resolve_addr")) return;
        auto* ptrTy = llvm::PointerType::get(ctx, 0);
        auto* i64 = llvm::Type::getInt64Ty(ctx);
        auto* entryTy = getSymEntryTy();
        auto* chunkTy = getSymChunkTy();
        auto* fn = llvm::Function::Create(
            llvm::FunctionType::get(ptrTy, { i64 }, false),
            llvm::Function::InternalLinkage, "ens_resolve_addr", module.get());
        fn->addFnAttr(llvm::Attribute::NoUnwind);
        auto saved = builder->saveIP();
        auto* entry = llvm::BasicBlock::Create(ctx, "entry", fn);
        auto* ckCond = llvm::BasicBlock::Create(ctx, "chunk.cond", fn);
        auto* ckBody = llvm::BasicBlock::Create(ctx, "chunk.body", fn);
        auto* inCond = llvm::BasicBlock::Create(ctx, "inner.cond", fn);
        auto* inBody = llvm::BasicBlock::Create(ctx, "inner.body", fn);
        auto* setIt  = llvm::BasicBlock::Create(ctx, "set", fn);
        auto* inNext = llvm::BasicBlock::Create(ctx, "inner.next", fn);
        auto* ckNext = llvm::BasicBlock::Create(ctx, "chunk.next", fn);
        auto* done   = llvm::BasicBlock::Create(ctx, "done", fn);
        builder->SetInsertPoint(entry);
        llvm::Value* addr = fn->getArg(0);
        llvm::Value* bestA = builder->CreateAlloca(ptrTy, nullptr, "best");
        llvm::Value* bestStartA = builder->CreateAlloca(i64, nullptr, "bestStart");
        llvm::Value* chunkA = builder->CreateAlloca(ptrTy, nullptr, "chunk");
        llvm::Value* iA = builder->CreateAlloca(i64, nullptr, "i");
        builder->CreateStore(llvm::ConstantPointerNull::get(ptrTy), bestA);
        builder->CreateStore(llvm::ConstantInt::get(i64, 0), bestStartA);
        builder->CreateStore(builder->CreateLoad(ptrTy, getSymtabHead()), chunkA);
        builder->CreateBr(ckCond);

        builder->SetInsertPoint(ckCond);
        llvm::Value* chunk = builder->CreateLoad(ptrTy, chunkA, "chunk.cur");
        builder->CreateCondBr(
            builder->CreateICmpEQ(chunk, llvm::ConstantPointerNull::get(ptrTy)), done, ckBody);

        builder->SetInsertPoint(ckBody);
        llvm::Value* entries = builder->CreateLoad(ptrTy, builder->CreateStructGEP(chunkTy, chunk, 1), "entries");
        llvm::Value* count = builder->CreateLoad(i64, builder->CreateStructGEP(chunkTy, chunk, 2), "count");
        builder->CreateStore(llvm::ConstantInt::get(i64, 0), iA);
        builder->CreateBr(inCond);

        builder->SetInsertPoint(inCond);
        llvm::Value* i = builder->CreateLoad(i64, iA, "i.cur");
        builder->CreateCondBr(builder->CreateICmpSLT(i, count), inBody, ckNext);

        builder->SetInsertPoint(inBody);
        llvm::Value* e = builder->CreateGEP(entryTy, entries, i, "e");
        llvm::Value* fsPtr = builder->CreateLoad(ptrTy, builder->CreateStructGEP(entryTy, e, 0), "fs.ptr");
        llvm::Value* fs = builder->CreatePtrToInt(fsPtr, i64, "fs");
        llvm::Value* le = builder->CreateICmpULE(fs, addr);
        llvm::Value* gt = builder->CreateICmpUGT(fs, builder->CreateLoad(i64, bestStartA));
        builder->CreateCondBr(builder->CreateAnd(le, gt), setIt, inNext);

        builder->SetInsertPoint(setIt);
        builder->CreateStore(e, bestA);
        builder->CreateStore(fs, bestStartA);
        builder->CreateBr(inNext);

        builder->SetInsertPoint(inNext);
        builder->CreateStore(builder->CreateAdd(i, llvm::ConstantInt::get(i64, 1)), iA);
        builder->CreateBr(inCond);

        builder->SetInsertPoint(ckNext);
        builder->CreateStore(builder->CreateLoad(ptrTy, builder->CreateStructGEP(chunkTy, chunk, 0)), chunkA);
        builder->CreateBr(ckCond);

        builder->SetInsertPoint(done);
        builder->CreateRet(builder->CreateLoad(ptrTy, bestA));
        builder->restoreIP(saved);
    }

    // i32 ens_resolve_line(i64 addr): precise source line for a return address, via
    // the greatest call-site marker <= addr, or 0 if none (caller falls back to the
    // function's declaration line).
    void defineResolveLine() {
        if (module->getFunction("ens_resolve_line")) return;
        auto* ptrTy = llvm::PointerType::get(ctx, 0);
        auto* i32 = llvm::Type::getInt32Ty(ctx);
        auto* i64 = llvm::Type::getInt64Ty(ctx);
        auto* lineTy = getLineEntryTy();
        auto* chunkTy = getSymChunkTy();
        auto* fn = llvm::Function::Create(
            llvm::FunctionType::get(i32, { i64 }, false),
            llvm::Function::InternalLinkage, "ens_resolve_line", module.get());
        fn->addFnAttr(llvm::Attribute::NoUnwind);
        auto saved = builder->saveIP();
        auto* entry = llvm::BasicBlock::Create(ctx, "entry", fn);
        auto* ckCond = llvm::BasicBlock::Create(ctx, "chunk.cond", fn);
        auto* ckBody = llvm::BasicBlock::Create(ctx, "chunk.body", fn);
        auto* inCond = llvm::BasicBlock::Create(ctx, "inner.cond", fn);
        auto* inBody = llvm::BasicBlock::Create(ctx, "inner.body", fn);
        auto* setIt  = llvm::BasicBlock::Create(ctx, "set", fn);
        auto* inNext = llvm::BasicBlock::Create(ctx, "inner.next", fn);
        auto* ckNext = llvm::BasicBlock::Create(ctx, "chunk.next", fn);
        auto* done   = llvm::BasicBlock::Create(ctx, "done", fn);
        builder->SetInsertPoint(entry);
        llvm::Value* addr = fn->getArg(0);
        llvm::Value* bestA = builder->CreateAlloca(i32, nullptr, "bestLine");
        llvm::Value* bestStartA = builder->CreateAlloca(i64, nullptr, "bestStart");
        llvm::Value* chunkA = builder->CreateAlloca(ptrTy, nullptr, "chunk");
        llvm::Value* iA = builder->CreateAlloca(i64, nullptr, "i");
        builder->CreateStore(llvm::ConstantInt::get(i32, 0), bestA);
        builder->CreateStore(llvm::ConstantInt::get(i64, 0), bestStartA);
        builder->CreateStore(builder->CreateLoad(ptrTy, getSymtabHead()), chunkA);
        builder->CreateBr(ckCond);

        builder->SetInsertPoint(ckCond);
        llvm::Value* chunk = builder->CreateLoad(ptrTy, chunkA, "chunk.cur");
        builder->CreateCondBr(
            builder->CreateICmpEQ(chunk, llvm::ConstantPointerNull::get(ptrTy)), done, ckBody);

        builder->SetInsertPoint(ckBody);
        llvm::Value* lines = builder->CreateLoad(ptrTy, builder->CreateStructGEP(chunkTy, chunk, 3), "lines");
        llvm::Value* count = builder->CreateLoad(i64, builder->CreateStructGEP(chunkTy, chunk, 4), "lineCount");
        builder->CreateStore(llvm::ConstantInt::get(i64, 0), iA);
        builder->CreateBr(inCond);

        builder->SetInsertPoint(inCond);
        llvm::Value* i = builder->CreateLoad(i64, iA, "i.cur");
        builder->CreateCondBr(builder->CreateICmpSLT(i, count), inBody, ckNext);

        builder->SetInsertPoint(inBody);
        llvm::Value* e = builder->CreateGEP(lineTy, lines, i, "e");
        llvm::Value* a = builder->CreatePtrToInt(
            builder->CreateLoad(ptrTy, builder->CreateStructGEP(lineTy, e, 0)), i64, "a");
        llvm::Value* le = builder->CreateICmpULE(a, addr);
        llvm::Value* gt = builder->CreateICmpUGT(a, builder->CreateLoad(i64, bestStartA));
        builder->CreateCondBr(builder->CreateAnd(le, gt), setIt, inNext);

        builder->SetInsertPoint(setIt);
        builder->CreateStore(
            builder->CreateLoad(i32, builder->CreateStructGEP(lineTy, e, 1)), bestA);
        builder->CreateStore(a, bestStartA);
        builder->CreateBr(inNext);

        builder->SetInsertPoint(inNext);
        builder->CreateStore(builder->CreateAdd(i, llvm::ConstantInt::get(i64, 1)), iA);
        builder->CreateBr(inCond);

        builder->SetInsertPoint(ckNext);
        builder->CreateStore(builder->CreateLoad(ptrTy, builder->CreateStructGEP(chunkTy, chunk, 0)), chunkA);
        builder->CreateBr(ckCond);

        builder->SetInsertPoint(done);
        builder->CreateRet(builder->CreateLoad(i32, bestA));
        builder->restoreIP(saved);
    }

    // Precise call-site line for a frame address, falling back to the function's
    // declaration line when no marker covers it.
    llvm::Value* resolvePreciseLine(llvm::Value* addr, llvm::Value* entryPtr) {
        auto* i32 = llvm::Type::getInt32Ty(ctx);
        llvm::Value* declLine = builder->CreateLoad(i32,
            builder->CreateStructGEP(getSymEntryTy(), entryPtr, 3), "declLine");
        llvm::Value* precise = builder->CreateCall(
            module->getFunction("ens_resolve_line"), { addr }, "precise");
        return builder->CreateSelect(
            builder->CreateICmpNE(precise, llvm::ConstantInt::get(i32, 0)),
            precise, declLine, "line");
    }

    // ptr ens_capture_trace(i32 maxDepth): unwind, drop the capture frame and any
    // frames past the program entry, return a long[] of the surviving addresses.
    void defineCaptureTrace() {
        auto* fn = captureTraceFn();
        if (!fn->empty()) return;
        auto* ptrTy = llvm::PointerType::get(ctx, 0);
        auto* i32 = llvm::Type::getInt32Ty(ctx);
        auto* i64 = llvm::Type::getInt64Ty(ctx);
        auto* entryTy = getSymEntryTy();
        bool windows = module->getTargetTriple().isOSWindows();
        fn->addFnAttr(llvm::Attribute::NoUnwind);
        auto saved = builder->saveIP();
        auto* entry = llvm::BasicBlock::Create(ctx, "entry", fn);
        auto* lpCond = llvm::BasicBlock::Create(ctx, "walk.cond", fn);
        auto* lpBody = llvm::BasicBlock::Create(ctx, "walk.body", fn);
        auto* keep   = llvm::BasicBlock::Create(ctx, "walk.keep", fn);
        auto* lpNext = llvm::BasicBlock::Create(ctx, "walk.next", fn);
        auto* fin    = llvm::BasicBlock::Create(ctx, "walk.done", fn);
        builder->SetInsertPoint(entry);
        llvm::Value* mx = fn->getArg(0);
        llvm::Value* mx64 = builder->CreateSExt(mx, i64);
        llvm::Value* buf = builder->CreateAlloca(i64, mx, "buf");

        // Platform-specific fill: write up to maxDepth return addresses into buf.
        // `n` = count; `skip` = leading buf entries to drop (the capture frame).
        llvm::Value* n;
        int64_t skip;
        if (windows) {
            // RtlCaptureStackBackTrace with skip=1 already drops this frame.
            llvm::Value* n16 = builder->CreateCall(getOrDeclareRtlCaptureStackBackTrace(),
                { llvm::ConstantInt::get(i32, 1), mx, buf, llvm::ConstantPointerNull::get(ptrTy) }, "n16");
            n = builder->CreateZExt(n16, i64, "n");
            skip = 0;
        } else {
            auto* stateTy = llvm::StructType::get(ctx, { ptrTy, i32, i32 });
            llvm::Value* state = builder->CreateAlloca(stateTy, nullptr, "state");
            builder->CreateStore(buf, builder->CreateStructGEP(stateTy, state, 0));
            builder->CreateStore(llvm::ConstantInt::get(i32, 0), builder->CreateStructGEP(stateTy, state, 1));
            builder->CreateStore(mx, builder->CreateStructGEP(stateTy, state, 2));
            builder->CreateCall(getOrDeclareUnwindBacktrace(),
                { module->getFunction("ens_unwind_cb"), state });
            n = builder->CreateSExt(
                builder->CreateLoad(i32, builder->CreateStructGEP(stateTy, state, 1)), i64, "n");
            skip = 1;   // _Unwind_Backtrace reports the capture frame first
        }

        // Common: over-allocate a long[] of maxDepth, keep resolvable frames, stop at
        // the program entry, set length to the kept count.
        llvm::Value* bytes = builder->CreateAdd(llvm::ConstantInt::get(i64, 8),
            builder->CreateMul(mx64, llvm::ConstantInt::get(i64, 8)));
        llvm::Value* arr = builder->CreateCall(getOrDefineEnsAlloc(),
            { bytes, llvm::ConstantPointerNull::get(ptrTy), llvm::ConstantPointerNull::get(ptrTy) }, "trace");
        llvm::Value* data = builder->CreateGEP(llvm::Type::getInt8Ty(ctx), arr,
            llvm::ConstantInt::get(i64, 8), "trace.data");
        llvm::Value* iA = builder->CreateAlloca(i64, nullptr, "i");
        llvm::Value* jA = builder->CreateAlloca(i64, nullptr, "j");
        builder->CreateStore(llvm::ConstantInt::get(i64, skip), iA);
        builder->CreateStore(llvm::ConstantInt::get(i64, 0), jA);
        builder->CreateBr(lpCond);

        builder->SetInsertPoint(lpCond);
        llvm::Value* i = builder->CreateLoad(i64, iA, "i.cur");
        builder->CreateCondBr(builder->CreateICmpSLT(i, n), lpBody, fin);

        builder->SetInsertPoint(lpBody);
        llvm::Value* addr = builder->CreateLoad(i64, builder->CreateGEP(i64, buf, i), "addr");
        llvm::Value* e = builder->CreateCall(module->getFunction("ens_resolve_addr"), { addr }, "e");
        builder->CreateCondBr(
            builder->CreateICmpNE(e, llvm::ConstantPointerNull::get(ptrTy)), keep, lpNext);

        builder->SetInsertPoint(keep);
        llvm::Value* j = builder->CreateLoad(i64, jA, "j.cur");
        builder->CreateStore(addr, builder->CreateGEP(i64, data, j));
        builder->CreateStore(builder->CreateAdd(j, llvm::ConstantInt::get(i64, 1)), jA);
        llvm::Value* isEntry = builder->CreateLoad(i32, builder->CreateStructGEP(entryTy, e, 4), "isEntry");
        builder->CreateCondBr(
            builder->CreateICmpNE(isEntry, llvm::ConstantInt::get(i32, 0)), fin, lpNext);

        builder->SetInsertPoint(lpNext);
        builder->CreateStore(builder->CreateAdd(i, llvm::ConstantInt::get(i64, 1)), iA);
        builder->CreateBr(lpCond);

        builder->SetInsertPoint(fin);
        builder->CreateStore(builder->CreateLoad(i64, jA), arr);   // length at +0
        builder->CreateRet(arr);
        builder->restoreIP(saved);
    }

    // ptr ens_format_trace(ptr frames): one "  at <fn> (<file>:<line>)\n" line per
    // captured frame, into a heap buffer. Capture already dropped non-user frames
    // and truncated at the program entry, so this iterates every stored frame.
    void defineFormatTrace() {
        auto* fn = formatTraceFn();
        if (!fn->empty()) return;
        auto* ptrTy = llvm::PointerType::get(ctx, 0);
        auto* i8 = llvm::Type::getInt8Ty(ctx);
        auto* i32 = llvm::Type::getInt32Ty(ctx);
        auto* i64 = llvm::Type::getInt64Ty(ctx);
        auto* entryTy = getSymEntryTy();
        const int64_t BUFSZ = 8192;
        fn->addFnAttr(llvm::Attribute::NoUnwind);
        auto saved = builder->saveIP();
        auto* entry = llvm::BasicBlock::Create(ctx, "entry", fn);
        auto* emptyBB = llvm::BasicBlock::Create(ctx, "empty", fn);
        auto* haveBB = llvm::BasicBlock::Create(ctx, "have", fn);
        auto* cond = llvm::BasicBlock::Create(ctx, "cond", fn);
        auto* body = llvm::BasicBlock::Create(ctx, "body", fn);
        auto* doFmt = llvm::BasicBlock::Create(ctx, "fmt", fn);
        auto* next = llvm::BasicBlock::Create(ctx, "next", fn);
        auto* after = llvm::BasicBlock::Create(ctx, "after", fn);
        auto* trunc = llvm::BasicBlock::Create(ctx, "trunc", fn);
        auto* retBB = llvm::BasicBlock::Create(ctx, "ret", fn);
        builder->SetInsertPoint(entry);
        llvm::Value* frames = fn->getArg(0);
        builder->CreateCondBr(
            builder->CreateICmpEQ(frames, llvm::ConstantPointerNull::get(ptrTy)), emptyBB, haveBB);

        builder->SetInsertPoint(emptyBB);
        builder->CreateRet(emitStringLiteralObject(""));

        builder->SetInsertPoint(haveBB);
        llvm::Value* n = builder->CreateLoad(i64, frames, "n");
        llvm::Value* data = builder->CreateGEP(i8, frames, llvm::ConstantInt::get(i64, 8), "data");
        llvm::Value* buf = builder->CreateCall(getOrDeclareMalloc(), { llvm::ConstantInt::get(i64, BUFSZ) }, "buf");
        builder->CreateStore(llvm::ConstantInt::get(i8, 0), buf);
        llvm::Value* lenA = builder->CreateAlloca(i64, nullptr, "len");
        llvm::Value* iA = builder->CreateAlloca(i64, nullptr, "i");
        llvm::Value* sawA = builder->CreateAlloca(llvm::Type::getInt1Ty(ctx), nullptr, "sawEntry");
        builder->CreateStore(llvm::ConstantInt::get(i64, 0), lenA);
        builder->CreateStore(llvm::ConstantInt::get(i64, 0), iA);
        builder->CreateStore(llvm::ConstantInt::getFalse(ctx), sawA);
        llvm::Value* fmt = builder->CreateGlobalString("  at %s (%s:%d)\n", ".trace.fmt");
        builder->CreateBr(cond);

        builder->SetInsertPoint(cond);
        llvm::Value* i = builder->CreateLoad(i64, iA, "i.cur");
        builder->CreateCondBr(builder->CreateICmpSLT(i, n), body, after);

        builder->SetInsertPoint(body);
        llvm::Value* addr = builder->CreateLoad(i64, builder->CreateGEP(i64, data, i), "addr");
        llvm::Value* e = builder->CreateCall(module->getFunction("ens_resolve_addr"), { addr }, "e");
        builder->CreateCondBr(
            builder->CreateICmpEQ(e, llvm::ConstantPointerNull::get(ptrTy)), next, doFmt);

        builder->SetInsertPoint(doFmt);
        llvm::Value* name = builder->CreateLoad(ptrTy, builder->CreateStructGEP(entryTy, e, 1), "name");
        llvm::Value* file = builder->CreateLoad(ptrTy, builder->CreateStructGEP(entryTy, e, 2), "file");
        llvm::Value* line = resolvePreciseLine(addr, e);
        llvm::Value* isEntry = builder->CreateLoad(i32, builder->CreateStructGEP(entryTy, e, 4), "isEntry");
        llvm::Value* len = builder->CreateLoad(i64, lenA, "len.cur");
        llvm::Value* cur = builder->CreateGEP(i8, buf, len);
        llvm::Value* rem = builder->CreateSub(llvm::ConstantInt::get(i64, BUFSZ), len);
        llvm::Value* w = builder->CreateCall(getOrDeclareSnprintf(), { cur, rem, fmt, name, file, line }, "w");
        llvm::Value* newlen = builder->CreateAdd(len, builder->CreateSExt(w, i64));
        llvm::Value* over = builder->CreateICmpSGT(newlen, llvm::ConstantInt::get(i64, BUFSZ - 1));
        builder->CreateStore(builder->CreateSelect(over, llvm::ConstantInt::get(i64, BUFSZ - 1), newlen), lenA);
        builder->CreateStore(
            builder->CreateOr(builder->CreateLoad(llvm::Type::getInt1Ty(ctx), sawA),
                              builder->CreateICmpNE(isEntry, llvm::ConstantInt::get(i32, 0))),
            sawA);
        builder->CreateBr(next);

        builder->SetInsertPoint(next);
        builder->CreateStore(builder->CreateAdd(i, llvm::ConstantInt::get(i64, 1)), iA);
        builder->CreateBr(cond);

        builder->SetInsertPoint(after);
        llvm::Value* nonEmpty = builder->CreateICmpSGT(n, llvm::ConstantInt::get(i64, 0));
        llvm::Value* sawEntry = builder->CreateLoad(llvm::Type::getInt1Ty(ctx), sawA);
        builder->CreateCondBr(builder->CreateAnd(nonEmpty, builder->CreateNot(sawEntry)), trunc, retBB);

        builder->SetInsertPoint(trunc);
        llvm::Value* tlen = builder->CreateLoad(i64, lenA);
        builder->CreateCall(getOrDeclareSnprintf(),
            { builder->CreateGEP(i8, buf, tlen),
              builder->CreateSub(llvm::ConstantInt::get(i64, BUFSZ), tlen),
              builder->CreateGlobalString("  ... (trace truncated)\n", ".trace.trunc") });
        builder->CreateBr(retBB);

        builder->SetInsertPoint(retBB);
        llvm::Value* traceStr = builder->CreateCall(getOrDefineEnsStringFromCStr(), { buf }, "trace.str");
        builder->CreateCall(getOrDeclareFree(), { buf });
        builder->CreateRet(traceStr);
        builder->restoreIP(saved);
    }

    // ptr ens_symbolicate(ptr frames): build a StackFrame[] from the captured
    // addresses. Every stored frame resolves (capture kept only resolved ones).
    void defineSymbolicate() {
        auto* fn = symbolicateFn();
        if (!fn->empty()) return;
        fn->addFnAttr(llvm::Attribute::NoUnwind);
        if (!stackFrameType || !stackFrameType->structInfo) {
            auto* ptrTy = llvm::PointerType::get(ctx, 0);
            auto saved = builder->saveIP();
            builder->SetInsertPoint(llvm::BasicBlock::Create(ctx, "entry", fn));
            builder->CreateRet(llvm::ConstantPointerNull::get(ptrTy));
            builder->restoreIP(saved);
            return;
        }
        emitSymbolicateBodyFinal(fn);
    }

    void emitSymbolicateBodyFinal(llvm::Function* fn) {
        auto* ptrTy = llvm::PointerType::get(ctx, 0);
        auto* i8 = llvm::Type::getInt8Ty(ctx);
        auto* i32 = llvm::Type::getInt32Ty(ctx);
        auto* i64 = llvm::Type::getInt64Ty(ctx);
        auto* entryTy = getSymEntryTy();
        auto* sfStructTy = mapStructType(stackFrameType);
        uint64_t sfSize = module->getDataLayout().getTypeAllocSize(sfStructTy);
        llvm::Value* sfDtor = getOrEmitClassDtor(stackFrameType);
        llvm::Value* sfDesc = getOrEmitTypeDescriptor(stackFrameType->structInfo);
        llvm::Value* arrDtor = getOrEmitArrayDtor(stackFrameType);
        if (!arrDtor) arrDtor = llvm::ConstantPointerNull::get(ptrTy);
        auto saved = builder->saveIP();
        auto* entry = llvm::BasicBlock::Create(ctx, "entry", fn);
        auto* getN = llvm::BasicBlock::Create(ctx, "getn", fn);
        auto* zeroN = llvm::BasicBlock::Create(ctx, "zeron", fn);
        auto* alloc = llvm::BasicBlock::Create(ctx, "alloc", fn);
        auto* cond = llvm::BasicBlock::Create(ctx, "cond", fn);
        auto* body = llvm::BasicBlock::Create(ctx, "body", fn);
        auto* retBB = llvm::BasicBlock::Create(ctx, "ret", fn);
        builder->SetInsertPoint(entry);
        llvm::Value* frames = fn->getArg(0);
        llvm::Value* nA = builder->CreateAlloca(i64, nullptr, "n");
        llvm::Value* arrA = builder->CreateAlloca(ptrTy, nullptr, "arr");
        llvm::Value* iA = builder->CreateAlloca(i64, nullptr, "i");
        builder->CreateCondBr(
            builder->CreateICmpEQ(frames, llvm::ConstantPointerNull::get(ptrTy)), zeroN, getN);
        builder->SetInsertPoint(zeroN);
        builder->CreateStore(llvm::ConstantInt::get(i64, 0), nA);
        builder->CreateBr(alloc);
        builder->SetInsertPoint(getN);
        builder->CreateStore(builder->CreateLoad(i64, frames), nA);
        builder->CreateBr(alloc);

        builder->SetInsertPoint(alloc);
        llvm::Value* n = builder->CreateLoad(i64, nA, "n.cur");
        llvm::Value* bytes = builder->CreateAdd(llvm::ConstantInt::get(i64, 8),
            builder->CreateMul(n, llvm::ConstantInt::get(i64, 8)));
        llvm::Value* arr = builder->CreateCall(getOrDefineEnsAlloc(), { bytes, arrDtor,
            llvm::ConstantPointerNull::get(ptrTy) }, "frames.arr");
        builder->CreateStore(n, arr);                 // length at +0
        builder->CreateStore(arr, arrA);
        builder->CreateStore(llvm::ConstantInt::get(i64, 0), iA);
        builder->CreateBr(cond);

        builder->SetInsertPoint(cond);
        llvm::Value* i = builder->CreateLoad(i64, iA, "i.cur");
        builder->CreateCondBr(builder->CreateICmpSLT(i, n), body, retBB);

        builder->SetInsertPoint(body);
        llvm::Value* data = builder->CreateGEP(i8, frames, llvm::ConstantInt::get(i64, 8));
        llvm::Value* addr = builder->CreateLoad(i64, builder->CreateGEP(i64, data, i));
        llvm::Value* e = builder->CreateCall(module->getFunction("ens_resolve_addr"), { addr }, "e");
        llvm::Value* name = builder->CreateLoad(ptrTy, builder->CreateStructGEP(entryTy, e, 1));
        llvm::Value* file = builder->CreateLoad(ptrTy, builder->CreateStructGEP(entryTy, e, 2));
        llvm::Value* line = resolvePreciseLine(addr, e);
        // The symbol table holds raw C strings; the StackFrame fields are Ens
        // strings, so wrap them into owned objects the frame's dtor can release.
        llvm::Value* nameStr = builder->CreateCall(getOrDefineEnsStringFromCStr(), { name });
        llvm::Value* fileStr = builder->CreateCall(getOrDefineEnsStringFromCStr(), { file });
        llvm::Value* sf = builder->CreateCall(getOrDefineEnsAlloc(),
            { llvm::ConstantInt::get(i64, sfSize), sfDtor, sfDesc }, "frame");
        builder->CreateStore(nameStr, builder->CreateStructGEP(sfStructTy, sf, 0));
        builder->CreateStore(fileStr, builder->CreateStructGEP(sfStructTy, sf, 1));
        builder->CreateStore(line, builder->CreateStructGEP(sfStructTy, sf, 2));
        llvm::Value* arrData = builder->CreateGEP(i8, builder->CreateLoad(ptrTy, arrA),
            llvm::ConstantInt::get(i64, 8));
        builder->CreateStore(sf, builder->CreateGEP(ptrTy, arrData, i));
        builder->CreateStore(builder->CreateAdd(i, llvm::ConstantInt::get(i64, 1)), iA);
        builder->CreateBr(cond);

        builder->SetInsertPoint(retBB);
        builder->CreateRet(builder->CreateLoad(ptrTy, arrA));
        builder->restoreIP(saved);
    }

    void definePreludeRuntime() {
        if (!module->getTargetTriple().isOSWindows()) defineUnwindCallback();
        defineResolveAddr();
        defineResolveLine();
        defineCaptureTrace();
        defineFormatTrace();
        defineSymbolicate();
        defineSymtabRegister();
    }

    llvm::Constant* makeCString(const std::string& s) {
        auto* init = llvm::ConstantDataArray::getString(ctx, s, /*AddNull=*/true);
        auto* gv = new llvm::GlobalVariable(*module, init->getType(), /*isConstant=*/true,
            llvm::GlobalValue::PrivateLinkage, init, ".trace.s");
        gv->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
        return gv;
    }

    // A callee that never appears as a user stack frame: our runtime and the libc
    // helpers we declare. Everything else (Ens functions, including cross-module
    // external references, and ens_capture_trace) is a frame-producing call.
    static bool isNonFrameCallee(llvm::StringRef n) {
        if (n == "ens_capture_trace") return false;
        if (n.starts_with("ens_") || n.starts_with("_ens") || n.starts_with("_dtor_") ||
            n.starts_with("_typedesc") || n.starts_with("_Unwind") || n.starts_with("llvm."))
            return true;
        static const char* kLibc[] = { "malloc", "calloc", "free", "memcpy", "memset",
            "snprintf", "puts", "fputs", "exit", "strlen", "__acrt_iob_func" };
        for (const char* c : kLibc) if (n == c) return true;
        return false;
    }

    // Split each frame-producing call into its own block so its start address is a
    // blockaddress constant, and record (address, call line). A captured return
    // address then resolves to the greatest marker <= it (its precise call site).
    void emitCallSiteLineTable() {
        for (auto& F : *module) {
            if (F.isDeclaration() || !F.getSubprogram()) continue;
            std::vector<llvm::CallInst*> calls;
            for (auto& BB : F)
                for (auto& I : BB)
                    if (auto* ci = llvm::dyn_cast<llvm::CallInst>(&I)) {
                        if (!ci->getDebugLoc() || ci->isInlineAsm()) continue;
                        llvm::Function* callee = ci->getCalledFunction();
                        if (!callee || !isNonFrameCallee(callee->getName())) calls.push_back(ci);
                    }
            for (auto* ci : calls) {
                // Always split: a new block starts at the call, so its address is a
                // legal blockaddress (never the entry block) and is unique per call.
                llvm::BasicBlock* callBB = ci->getParent()->splitBasicBlock(ci, "call.site");
                lineRecords.emplace_back(llvm::BlockAddress::get(&F, callBB),
                    static_cast<int>(ci->getDebugLoc().getLine()));
            }
        }
    }

    void recordSymtabEntry(llvm::Function* fn, const std::string& display, int line, bool isEntry) {
        std::string file = std::filesystem::path(sourceFilename).filename().string();
        if (file.empty()) file = sourceFilename;
        symtabRecords.push_back({ fn, display, file, line, isEntry });
    }

    // Emit this module's symbol table as a constant array, then a global ctor that
    // registers it into the runtime list at load (works for static and dlopen'd
    // modules alike).
    void emitSymtabRegistration() {
        if (symtabRecords.empty()) return;
        auto* ptrTy = llvm::PointerType::get(ctx, 0);
        auto* i32 = llvm::Type::getInt32Ty(ctx);
        auto* i64 = llvm::Type::getInt64Ty(ctx);
        auto* entryTy = getSymEntryTy();
        auto* chunkTy = getSymChunkTy();

        std::vector<llvm::Constant*> elems;
        elems.reserve(symtabRecords.size());
        for (auto& r : symtabRecords) {
            elems.push_back(llvm::ConstantStruct::get(entryTy, {
                r.fn, makeCString(r.name), makeCString(r.file),
                llvm::ConstantInt::get(i32, r.line),
                llvm::ConstantInt::get(i32, r.isEntry ? 1 : 0) }));
        }
        auto* arrTy = llvm::ArrayType::get(entryTy, elems.size());
        auto* entriesGV = new llvm::GlobalVariable(*module, arrTy, /*isConstant=*/true,
            llvm::GlobalValue::PrivateLinkage, llvm::ConstantArray::get(arrTy, elems), "_ens_symtab");

        auto* lineTy = getLineEntryTy();
        std::vector<llvm::Constant*> lineElems;
        lineElems.reserve(lineRecords.size());
        for (auto& lr : lineRecords)
            lineElems.push_back(llvm::ConstantStruct::get(lineTy,
                { lr.first, llvm::ConstantInt::get(i32, lr.second) }));
        auto* lineArrTy = llvm::ArrayType::get(lineTy, lineElems.size());
        auto* linesGV = new llvm::GlobalVariable(*module, lineArrTy, /*isConstant=*/true,
            llvm::GlobalValue::PrivateLinkage, llvm::ConstantArray::get(lineArrTy, lineElems), "_ens_linetab");

        auto* chunkInit = llvm::ConstantStruct::get(chunkTy, {
            llvm::ConstantPointerNull::get(ptrTy), entriesGV,
            llvm::ConstantInt::get(i64, elems.size()),
            linesGV, llvm::ConstantInt::get(i64, lineElems.size()) });
        auto* chunkGV = new llvm::GlobalVariable(*module, chunkTy, /*isConstant=*/false,
            llvm::GlobalValue::PrivateLinkage, chunkInit, "_ens_symtab_chunk");

        auto* ctor = llvm::Function::Create(
            llvm::FunctionType::get(llvm::Type::getVoidTy(ctx), false),
            llvm::Function::InternalLinkage, "_ens_register_symtab", module.get());
        ctor->addFnAttr(llvm::Attribute::NoUnwind);
        auto saved = builder->saveIP();
        builder->SetInsertPoint(llvm::BasicBlock::Create(ctx, "entry", ctor));
        builder->CreateCall(symtabRegisterFn(), { chunkGV });
        builder->CreateRetVoid();
        builder->restoreIP(saved);
        llvm::appendToGlobalCtors(*module, ctor, 65535);
    }

    llvm::Function* getOrDefineEnsAlloc() {
        if (auto* existing = module->getFunction("ens_alloc")) return existing;
        auto* ptrTy = llvm::PointerType::get(ctx, 0);
        auto* i64Ty = llvm::Type::getInt64Ty(ctx);
        auto* i8Ty = llvm::Type::getInt8Ty(ctx);
        auto* fnTy = llvm::FunctionType::get(ptrTy, { i64Ty, ptrTy, ptrTy }, false);
        auto* fn = llvm::Function::Create(fnTy, llvm::Function::InternalLinkage, "ens_alloc", module.get());
        fn->addFnAttr(llvm::Attribute::NoUnwind);

        auto savedIP = builder->saveIP();
        auto* entry  = llvm::BasicBlock::Create(ctx, "entry", fn);
        auto* nullBB = llvm::BasicBlock::Create(ctx, "alloc.null", fn);
        auto* initBB = llvm::BasicBlock::Create(ctx, "alloc.init", fn);

        builder->SetInsertPoint(entry);
        llvm::Value* total = builder->CreateAdd(fn->getArg(0), llvm::ConstantInt::get(i64Ty, 32));
        llvm::Value* header = builder->CreateCall(getOrDeclareCalloc(),
            { llvm::ConstantInt::get(i64Ty, 1), total });
        llvm::Value* isNull = builder->CreateICmpEQ(header, llvm::ConstantPointerNull::get(ptrTy));
        builder->CreateCondBr(isNull, nullBB, initBB);

        builder->SetInsertPoint(nullBB);
        builder->CreateRet(llvm::ConstantPointerNull::get(ptrTy));

        builder->SetInsertPoint(initBB);
        builder->CreateStore(fn->getArg(2), header);  // typeDescriptor at offset 0
        llvm::Value* rcSlot = builder->CreateGEP(i8Ty, header, llvm::ConstantInt::get(i64Ty, 8));
        builder->CreateStore(llvm::ConstantInt::get(i64Ty, 1), rcSlot);  // refcount at offset 8
        llvm::Value* dtorSlot = builder->CreateGEP(i8Ty, header, llvm::ConstantInt::get(i64Ty, 16));
        builder->CreateStore(fn->getArg(1), dtorSlot);  // dtor at offset 16
        // side_table slot at offset 24 stays null (calloc-zeroed); lazily set by ens_weak_init.
        llvm::Value* payload = builder->CreateGEP(i8Ty, header, llvm::ConstantInt::get(i64Ty, 32));
        builder->CreateRet(payload);

        builder->restoreIP(savedIP);
        return fn;
    }

    llvm::Function* getOrDefineEnsRetain() {
        if (auto* existing = module->getFunction("ens_retain")) return existing;
        auto* ptrTy = llvm::PointerType::get(ctx, 0);
        auto* i64Ty = llvm::Type::getInt64Ty(ctx);
        auto* fnTy = llvm::FunctionType::get(llvm::Type::getVoidTy(ctx), { ptrTy }, false);
        auto* fn = llvm::Function::Create(fnTy, llvm::Function::InternalLinkage, "ens_retain", module.get());
        fn->addFnAttr(llvm::Attribute::AlwaysInline);
        fn->addFnAttr(llvm::Attribute::NoUnwind);

        auto savedIP = builder->saveIP();
        auto* entry  = llvm::BasicBlock::Create(ctx, "entry", fn);
        auto* checkBB = llvm::BasicBlock::Create(ctx, "retain.check", fn);
        auto* bumpBB = llvm::BasicBlock::Create(ctx, "retain.bump", fn);
        auto* doneBB = llvm::BasicBlock::Create(ctx, "retain.done", fn);

        builder->SetInsertPoint(entry);
        llvm::Value* obj = fn->getArg(0);
        llvm::Value* isNull = builder->CreateICmpEQ(obj, llvm::ConstantPointerNull::get(ptrTy));
        builder->CreateCondBr(isNull, doneBB, checkBB);

        builder->SetInsertPoint(checkBB);
        llvm::Value* header = builder->CreateGEP(
            llvm::Type::getInt8Ty(ctx), obj, llvm::ConstantInt::getSigned(i64Ty, -24));
        llvm::LoadInst* rc = builder->CreateLoad(i64Ty, header, "refcount");
        rc->setAtomic(llvm::AtomicOrdering::Monotonic);
        rc->setAlignment(llvm::Align(8));
        llvm::Value* immortal = builder->CreateICmpEQ(
            rc, llvm::ConstantInt::get(i64Ty, kImmortalRefcount));
        builder->CreateCondBr(immortal, doneBB, bumpBB);

        builder->SetInsertPoint(bumpBB);
        builder->CreateAtomicRMW(
            llvm::AtomicRMWInst::Add, header,
            llvm::ConstantInt::get(i64Ty, 1),
            llvm::MaybeAlign(8),
            llvm::AtomicOrdering::Monotonic);
        builder->CreateBr(doneBB);

        builder->SetInsertPoint(doneBB);
        builder->CreateRetVoid();

        builder->restoreIP(savedIP);
        return fn;
    }

    llvm::Function* getOrDefineEnsRelease() {
        if (auto* existing = module->getFunction("ens_release")) return existing;
        auto* ptrTy = llvm::PointerType::get(ctx, 0);
        auto* i64Ty = llvm::Type::getInt64Ty(ctx);
        auto* i8Ty = llvm::Type::getInt8Ty(ctx);
        auto* fnTy = llvm::FunctionType::get(llvm::Type::getVoidTy(ctx), { ptrTy }, false);
        auto* fn = llvm::Function::Create(fnTy, llvm::Function::InternalLinkage, "ens_release", module.get());
        fn->addFnAttr(llvm::Attribute::NoUnwind);

        auto savedIP = builder->saveIP();
        auto* entry   = llvm::BasicBlock::Create(ctx, "entry", fn);
        auto* checkBB = llvm::BasicBlock::Create(ctx, "release.check", fn);
        auto* decBB   = llvm::BasicBlock::Create(ctx, "release.dec", fn);
        auto* dtorBB  = llvm::BasicBlock::Create(ctx, "release.dtor", fn);
        auto* callBB  = llvm::BasicBlock::Create(ctx, "release.dtor.call", fn);
        auto* stBB    = llvm::BasicBlock::Create(ctx, "release.sidetable", fn);
        auto* stHasBB = llvm::BasicBlock::Create(ctx, "release.sidetable.has", fn);
        auto* stFreeBB= llvm::BasicBlock::Create(ctx, "release.sidetable.free", fn);
        auto* freeBB  = llvm::BasicBlock::Create(ctx, "release.free", fn);
        auto* doneBB  = llvm::BasicBlock::Create(ctx, "release.done", fn);

        builder->SetInsertPoint(entry);
        llvm::Value* obj = fn->getArg(0);
        llvm::Value* isNull = builder->CreateICmpEQ(obj, llvm::ConstantPointerNull::get(ptrTy));
        builder->CreateCondBr(isNull, doneBB, checkBB);

        builder->SetInsertPoint(checkBB);
        llvm::Value* header = builder->CreateGEP(i8Ty, obj, llvm::ConstantInt::getSigned(i64Ty, -24));
        llvm::LoadInst* rc = builder->CreateLoad(i64Ty, header, "refcount");
        rc->setAtomic(llvm::AtomicOrdering::Monotonic);
        rc->setAlignment(llvm::Align(8));
        llvm::Value* immortal = builder->CreateICmpEQ(
            rc, llvm::ConstantInt::get(i64Ty, kImmortalRefcount));
        builder->CreateCondBr(immortal, doneBB, decBB);

        builder->SetInsertPoint(decBB);
        llvm::Value* prev = builder->CreateAtomicRMW(
            llvm::AtomicRMWInst::Sub, header,
            llvm::ConstantInt::get(i64Ty, 1),
            llvm::MaybeAlign(8),
            llvm::AtomicOrdering::AcquireRelease);
        llvm::Value* isLast = builder->CreateICmpEQ(prev, llvm::ConstantInt::get(i64Ty, 1));
        builder->CreateCondBr(isLast, dtorBB, doneBB);

        builder->SetInsertPoint(dtorBB);
        llvm::Value* dtorSlot = builder->CreateGEP(i8Ty, obj, llvm::ConstantInt::getSigned(i64Ty, -16));
        llvm::Value* dtorFn = builder->CreateLoad(ptrTy, dtorSlot);
        llvm::Value* dtorIsNull = builder->CreateICmpEQ(dtorFn, llvm::ConstantPointerNull::get(ptrTy));
        builder->CreateCondBr(dtorIsNull, stBB, callBB);

        builder->SetInsertPoint(callBB);
        auto* dtorTy = llvm::FunctionType::get(llvm::Type::getVoidTy(ctx), { ptrTy }, false);
        builder->CreateCall(dtorTy, dtorFn, { obj });
        builder->CreateBr(stBB);

        // Side-table handling: if non-null, null its object_ptr, conditionally free entry.
        builder->SetInsertPoint(stBB);
        llvm::Value* stSlot = builder->CreateGEP(i8Ty, obj, llvm::ConstantInt::getSigned(i64Ty, -8));
        llvm::Value* stPtr = builder->CreateLoad(ptrTy, stSlot);
        llvm::Value* stIsNull = builder->CreateICmpEQ(stPtr, llvm::ConstantPointerNull::get(ptrTy));
        builder->CreateCondBr(stIsNull, freeBB, stHasBB);

        builder->SetInsertPoint(stHasBB);
        // Null the entry's object_ptr (offset 0) with release ordering so weak readers see "dead".
        builder->CreateAtomicRMW(
            llvm::AtomicRMWInst::Xchg, stPtr,
            llvm::ConstantPointerNull::get(ptrTy),
            llvm::MaybeAlign(8),
            llvm::AtomicOrdering::Release);
        // Read entry's weak_count (offset 8). If 0, we own the entry's free.
        llvm::Value* wcSlot = builder->CreateGEP(i8Ty, stPtr, llvm::ConstantInt::get(i64Ty, 8));
        llvm::LoadInst* wc = builder->CreateLoad(i64Ty, wcSlot, "weak_count");
        wc->setAtomic(llvm::AtomicOrdering::Acquire);
        wc->setAlignment(llvm::Align(8));
        llvm::Value* wcZero = builder->CreateICmpEQ(wc, llvm::ConstantInt::get(i64Ty, 0));
        builder->CreateCondBr(wcZero, stFreeBB, freeBB);

        builder->SetInsertPoint(stFreeBB);
        builder->CreateCall(getOrDeclareFree(), { stPtr });
        builder->CreateBr(freeBB);

        builder->SetInsertPoint(freeBB);
        llvm::Value* allocBase = builder->CreateGEP(i8Ty, obj, llvm::ConstantInt::getSigned(i64Ty, -32));
        builder->CreateCall(getOrDeclareFree(), { allocBase });
        builder->CreateBr(doneBB);

        builder->SetInsertPoint(doneBB);
        builder->CreateRetVoid();

        builder->restoreIP(savedIP);
        return fn;
    }

    // Side-table entry layout: [obj_ptr (8) | weak_count (8)] = 16 bytes.
    // Lazily allocated; pointer stored in object header at offset -8 from the payload.

    llvm::Function* getOrDefineEnsWeakInit() {
        if (auto* existing = module->getFunction("ens_weak_init")) return existing;
        auto* ptrTy = llvm::PointerType::get(ctx, 0);
        auto* i64Ty = llvm::Type::getInt64Ty(ctx);
        auto* i8Ty = llvm::Type::getInt8Ty(ctx);
        auto* fnTy = llvm::FunctionType::get(ptrTy, { ptrTy }, false);
        auto* fn = llvm::Function::Create(fnTy, llvm::Function::InternalLinkage, "ens_weak_init", module.get());
        fn->addFnAttr(llvm::Attribute::NoUnwind);

        auto savedIP = builder->saveIP();
        auto* entry    = llvm::BasicBlock::Create(ctx, "entry", fn);
        auto* nullRet  = llvm::BasicBlock::Create(ctx, "init.null", fn);
        auto* haveSt   = llvm::BasicBlock::Create(ctx, "init.have", fn);
        auto* makeSt   = llvm::BasicBlock::Create(ctx, "init.make", fn);
        auto* afterCAS = llvm::BasicBlock::Create(ctx, "init.after_cas", fn);
        auto* freeOurs = llvm::BasicBlock::Create(ctx, "init.free_ours", fn);
        auto* bumpBB   = llvm::BasicBlock::Create(ctx, "init.bump", fn);

        builder->SetInsertPoint(entry);
        llvm::Value* obj = fn->getArg(0);
        llvm::Value* isNull = builder->CreateICmpEQ(obj, llvm::ConstantPointerNull::get(ptrTy));
        builder->CreateCondBr(isNull, nullRet, haveSt);

        builder->SetInsertPoint(nullRet);
        builder->CreateRet(llvm::ConstantPointerNull::get(ptrTy));

        builder->SetInsertPoint(haveSt);
        llvm::Value* stSlot = builder->CreateGEP(i8Ty, obj, llvm::ConstantInt::getSigned(i64Ty, -8));
        llvm::Value* existingSt = builder->CreateLoad(ptrTy, stSlot, "existing_st");
        llvm::Value* needMake = builder->CreateICmpEQ(existingSt, llvm::ConstantPointerNull::get(ptrTy));
        builder->CreateCondBr(needMake, makeSt, bumpBB);

        builder->SetInsertPoint(makeSt);
        llvm::Value* newSt = builder->CreateCall(getOrDeclareCalloc(),
            { llvm::ConstantInt::get(i64Ty, 1), llvm::ConstantInt::get(i64Ty, 16) }, "new_st");
        // Initialize object_ptr (offset 0).
        builder->CreateStore(obj, newSt);
        // CAS the header's side_table slot from null to newSt.
        llvm::Value* cas = builder->CreateAtomicCmpXchg(
            stSlot,
            llvm::ConstantPointerNull::get(ptrTy),
            newSt,
            llvm::MaybeAlign(8),
            llvm::AtomicOrdering::AcquireRelease,
            llvm::AtomicOrdering::Acquire);
        llvm::Value* prev = builder->CreateExtractValue(cas, 0);
        llvm::Value* success = builder->CreateExtractValue(cas, 1);
        builder->CreateCondBr(success, bumpBB, freeOurs);

        builder->SetInsertPoint(freeOurs);
        // Someone else won the race; free our allocation and use theirs.
        builder->CreateCall(getOrDeclareFree(), { newSt });
        builder->CreateBr(afterCAS);

        // PHI for the resolved side-table pointer.
        builder->SetInsertPoint(bumpBB);
        auto* finalSt = builder->CreatePHI(ptrTy, 2, "st");
        finalSt->addIncoming(existingSt, haveSt);
        finalSt->addIncoming(newSt, makeSt);
        builder->CreateBr(afterCAS);

        builder->SetInsertPoint(afterCAS);
        auto* finalSt2 = builder->CreatePHI(ptrTy, 2, "st2");
        finalSt2->addIncoming(prev, freeOurs);
        finalSt2->addIncoming(finalSt, bumpBB);
        // Atomic increment weak_count at offset 8.
        llvm::Value* wcSlot = builder->CreateGEP(i8Ty, finalSt2, llvm::ConstantInt::get(i64Ty, 8));
        builder->CreateAtomicRMW(
            llvm::AtomicRMWInst::Add, wcSlot,
            llvm::ConstantInt::get(i64Ty, 1),
            llvm::MaybeAlign(8),
            llvm::AtomicOrdering::Monotonic);
        builder->CreateRet(finalSt2);

        builder->restoreIP(savedIP);
        return fn;
    }

    llvm::Function* getOrDefineEnsWeakRelease() {
        if (auto* existing = module->getFunction("ens_weak_release")) return existing;
        auto* ptrTy = llvm::PointerType::get(ctx, 0);
        auto* i64Ty = llvm::Type::getInt64Ty(ctx);
        auto* i8Ty = llvm::Type::getInt8Ty(ctx);
        auto* fnTy = llvm::FunctionType::get(llvm::Type::getVoidTy(ctx), { ptrTy }, false);
        auto* fn = llvm::Function::Create(fnTy, llvm::Function::InternalLinkage, "ens_weak_release", module.get());
        fn->addFnAttr(llvm::Attribute::NoUnwind);

        auto savedIP = builder->saveIP();
        auto* entry  = llvm::BasicBlock::Create(ctx, "entry", fn);
        auto* decBB  = llvm::BasicBlock::Create(ctx, "wrel.dec", fn);
        auto* lastBB = llvm::BasicBlock::Create(ctx, "wrel.last", fn);
        auto* freeBB = llvm::BasicBlock::Create(ctx, "wrel.free", fn);
        auto* doneBB = llvm::BasicBlock::Create(ctx, "wrel.done", fn);

        builder->SetInsertPoint(entry);
        llvm::Value* st = fn->getArg(0);
        llvm::Value* isNull = builder->CreateICmpEQ(st, llvm::ConstantPointerNull::get(ptrTy));
        builder->CreateCondBr(isNull, doneBB, decBB);

        builder->SetInsertPoint(decBB);
        llvm::Value* wcSlot = builder->CreateGEP(i8Ty, st, llvm::ConstantInt::get(i64Ty, 8));
        llvm::Value* prev = builder->CreateAtomicRMW(
            llvm::AtomicRMWInst::Sub, wcSlot,
            llvm::ConstantInt::get(i64Ty, 1),
            llvm::MaybeAlign(8),
            llvm::AtomicOrdering::AcquireRelease);
        llvm::Value* isLast = builder->CreateICmpEQ(prev, llvm::ConstantInt::get(i64Ty, 1));
        builder->CreateCondBr(isLast, lastBB, doneBB);

        builder->SetInsertPoint(lastBB);
        // Load object_ptr; if null, object is dead and we can free the entry.
        llvm::LoadInst* objPtr = builder->CreateLoad(ptrTy, st, "obj_ptr");
        objPtr->setAtomic(llvm::AtomicOrdering::Acquire);
        objPtr->setAlignment(llvm::Align(8));
        llvm::Value* objNull = builder->CreateICmpEQ(objPtr, llvm::ConstantPointerNull::get(ptrTy));
        builder->CreateCondBr(objNull, freeBB, doneBB);

        builder->SetInsertPoint(freeBB);
        builder->CreateCall(getOrDeclareFree(), { st });
        builder->CreateBr(doneBB);

        builder->SetInsertPoint(doneBB);
        builder->CreateRetVoid();

        builder->restoreIP(savedIP);
        return fn;
    }

    llvm::Function* getOrDefineEnsWeakLoad() {
        if (auto* existing = module->getFunction("ens_weak_load")) return existing;
        auto* ptrTy = llvm::PointerType::get(ctx, 0);
        auto* i64Ty = llvm::Type::getInt64Ty(ctx);
        auto* i8Ty = llvm::Type::getInt8Ty(ctx);
        auto* fnTy = llvm::FunctionType::get(ptrTy, { ptrTy }, false);
        auto* fn = llvm::Function::Create(fnTy, llvm::Function::InternalLinkage, "ens_weak_load", module.get());
        fn->addFnAttr(llvm::Attribute::NoUnwind);

        auto savedIP = builder->saveIP();
        auto* entry    = llvm::BasicBlock::Create(ctx, "entry", fn);
        auto* loadObj  = llvm::BasicBlock::Create(ctx, "wload.obj", fn);
        auto* casLoop  = llvm::BasicBlock::Create(ctx, "wload.loop", fn);
        auto* casFail  = llvm::BasicBlock::Create(ctx, "wload.fail", fn);
        auto* retObj   = llvm::BasicBlock::Create(ctx, "wload.success", fn);
        auto* retNull  = llvm::BasicBlock::Create(ctx, "wload.null", fn);

        builder->SetInsertPoint(entry);
        llvm::Value* st = fn->getArg(0);
        llvm::Value* isNull = builder->CreateICmpEQ(st, llvm::ConstantPointerNull::get(ptrTy));
        builder->CreateCondBr(isNull, retNull, loadObj);

        builder->SetInsertPoint(loadObj);
        llvm::LoadInst* obj = builder->CreateLoad(ptrTy, st, "obj");
        obj->setAtomic(llvm::AtomicOrdering::Acquire);
        obj->setAlignment(llvm::Align(8));
        llvm::Value* objIsNull = builder->CreateICmpEQ(obj, llvm::ConstantPointerNull::get(ptrTy));
        auto* preLoop = llvm::BasicBlock::Create(ctx, "wload.preloop", fn);
        builder->CreateCondBr(objIsNull, retNull, preLoop);

        builder->SetInsertPoint(preLoop);
        llvm::Value* refSlot = builder->CreateGEP(i8Ty, obj, llvm::ConstantInt::getSigned(i64Ty, -24));
        llvm::LoadInst* initial = builder->CreateLoad(i64Ty, refSlot, "cur_initial");
        initial->setAtomic(llvm::AtomicOrdering::Monotonic);
        initial->setAlignment(llvm::Align(8));
        builder->CreateBr(casLoop);

        builder->SetInsertPoint(casLoop);
        auto* curPhi = builder->CreatePHI(i64Ty, 2, "cur");
        curPhi->addIncoming(initial, preLoop);
        // If cur == 0, object is being deallocated; return null.
        llvm::Value* curZero = builder->CreateICmpEQ(curPhi, llvm::ConstantInt::get(i64Ty, 0));
        auto* tryCAS = llvm::BasicBlock::Create(ctx, "wload.try_cas", fn);
        builder->CreateCondBr(curZero, retNull, tryCAS);

        builder->SetInsertPoint(tryCAS);
        llvm::Value* newCount = builder->CreateAdd(curPhi, llvm::ConstantInt::get(i64Ty, 1));
        llvm::Value* cas = builder->CreateAtomicCmpXchg(
            refSlot, curPhi, newCount,
            llvm::MaybeAlign(8),
            llvm::AtomicOrdering::AcquireRelease,
            llvm::AtomicOrdering::Monotonic);
        llvm::Value* prevVal = builder->CreateExtractValue(cas, 0);
        llvm::Value* success = builder->CreateExtractValue(cas, 1);
        builder->CreateCondBr(success, retObj, casFail);

        builder->SetInsertPoint(casFail);
        curPhi->addIncoming(prevVal, casFail);
        builder->CreateBr(casLoop);

        builder->SetInsertPoint(retObj);
        builder->CreateRet(obj);

        builder->SetInsertPoint(retNull);
        builder->CreateRet(llvm::ConstantPointerNull::get(ptrTy));

        builder->restoreIP(savedIP);
        return fn;
    }

    // TypeDescriptor { const char* name; TypeDescriptor* parent; uint32_t id; void** vtable; }
    llvm::StructType* getTypeDescriptorTy() {
        if (typeDescriptorTy) return typeDescriptorTy;
        auto* ptrTy = llvm::PointerType::get(ctx, 0);
        auto* i32 = llvm::Type::getInt32Ty(ctx);
        typeDescriptorTy = llvm::StructType::create(ctx, { ptrTy, ptrTy, i32, ptrTy }, "TypeDescriptor");
        return typeDescriptorTy;
    }

    // The vtable global for a class: slot i holds the most-derived implementation visible to
    // `si`. Returns null when the class introduces/inherits no virtual slots.
    llvm::Constant* emitVtable(StructInfo* si) {
        if (!si || si->vtableSize == 0) return nullptr;
        auto* ptrTy = llvm::PointerType::get(ctx, 0);
        std::vector<llvm::Constant*> slots(si->vtableSize, llvm::ConstantPointerNull::get(ptrTy));
        for (int slot = 0; slot < si->vtableSize; ++slot) {
            for (StructInfo* s = si; s; s = s->baseInfo) {
                bool found = false;
                for (auto& mi : s->methods) {
                    if (mi.vtableSlot != slot) continue;
                    found = true;
                    if (!mi.isAbstract && mi.symbol) {
                        if (llvm::Function* f = getOrDeclareExternalFunction(mi.symbol, nullptr))
                            slots[slot] = f;
                    }
                    break;
                }
                if (found) break;  // nearest (most-derived) declaration wins
            }
        }
        auto* arrTy = llvm::ArrayType::get(ptrTy, si->vtableSize);
        return new llvm::GlobalVariable(*module, arrTy, /*isConstant=*/true,
            llvm::GlobalValue::InternalLinkage, llvm::ConstantArray::get(arrTy, slots),
            "_vtable_" + asAscii(si->name));
    }

    llvm::GlobalVariable* getOrEmitTypeDescriptor(StructInfo* si) {
        if (!si) return nullptr;
        auto it = descriptorCache.find(si);
        if (it != descriptorCache.end()) return it->second;

        auto* ptrTy = llvm::PointerType::get(ctx, 0);
        auto* i32 = llvm::Type::getInt32Ty(ctx);
        auto* descTy = getTypeDescriptorTy();
        std::string symName = descriptorSymbolName(si);

        // Each class's descriptor is defined once, in its own module; other
        // modules reference it as an external so pointer identity holds globally.
        auto* gv = new llvm::GlobalVariable(*module, descTy, /*isConstant=*/true,
            llvm::GlobalValue::ExternalLinkage, nullptr, symName);
        descriptorCache[si] = gv;  // cache before recursing into parent
        if (si->modulePath != modulePath) return gv;  // external declaration only

        llvm::Constant* nameStr = builder->CreateGlobalString(asAscii(si->name),
            "_typename_" + symName);
        llvm::Constant* parent = si->baseInfo
            ? static_cast<llvm::Constant*>(getOrEmitTypeDescriptor(si->baseInfo))
            : static_cast<llvm::Constant*>(llvm::ConstantPointerNull::get(ptrTy));
        si->typeId = fnv1a32(symName);
        llvm::Constant* vtable = emitVtable(si);
        if (!vtable) vtable = llvm::ConstantPointerNull::get(ptrTy);
        gv->setInitializer(llvm::ConstantStruct::get(descTy,
            { nameStr, parent, llvm::ConstantInt::get(i32, si->typeId), vtable }));
        return gv;
    }

    // Load the function pointer for virtual slot `slot` from an object's TypeDescriptor.
    llvm::Value* loadVtableSlot(llvm::Value* obj, int slot) {
        auto* ptrTy = llvm::PointerType::get(ctx, 0);
        auto* i8Ty = llvm::Type::getInt8Ty(ctx);
        auto* i32 = llvm::Type::getInt32Ty(ctx);
        auto* i64 = llvm::Type::getInt64Ty(ctx);
        llvm::Value* descSlot = builder->CreateGEP(i8Ty, obj,
            llvm::ConstantInt::getSigned(i64, -32), "desc.slot");
        llvm::Value* desc = builder->CreateLoad(ptrTy, descSlot, "desc");
        llvm::Value* vtSlot = builder->CreateGEP(getTypeDescriptorTy(), desc,
            { llvm::ConstantInt::get(i32, 0), llvm::ConstantInt::get(i32, 3) }, "vtable.addr");
        llvm::Value* vtable = builder->CreateLoad(ptrTy, vtSlot, "vtable");
        llvm::Value* fnSlot = builder->CreateGEP(ptrTy, vtable,
            llvm::ConstantInt::get(i64, slot), "fn.addr");
        return builder->CreateLoad(ptrTy, fnSlot, "vfn");
    }

    llvm::Value* getOrEmitClassDtor(::Type* t) {
        auto* ptrTy = llvm::PointerType::get(ctx, 0);
        if (!t || !t->structInfo) return llvm::ConstantPointerNull::get(ptrTy);

        bool hasOwning = false;
        for (auto& f : t->structInfo->fields) {
            if (!f.type) continue;
            if (f.isWeak || isReferenceType(f.type) || structHasClassFields(f.type)) {
                hasOwning = true;
                break;
            }
        }
        if (!hasOwning) return llvm::ConstantPointerNull::get(ptrTy);

        std::string name = "_dtor_" + asAscii(t->structInfo->name);
        if (auto* existing = module->getFunction(name)) return existing;

        auto* fnTy = llvm::FunctionType::get(llvm::Type::getVoidTy(ctx), { ptrTy }, false);
        auto* fn = llvm::Function::Create(fnTy, llvm::Function::InternalLinkage, name, module.get());
        fn->addFnAttr(llvm::Attribute::NoUnwind);

        auto savedIP = builder->saveIP();
        auto* entry = llvm::BasicBlock::Create(ctx, "entry", fn);
        builder->SetInsertPoint(entry);
        emitStructFieldRelease(t, fn->getArg(0));
        builder->CreateRetVoid();

        builder->restoreIP(savedIP);
        return fn;
    }

    // ===== Arrays =====
    //
    // Layout (offsets from the array pointer, which is the value visible to Ens code):
    //   payload + 0  : i64 length
    //   payload + 8  : element data

    llvm::Function* getOrDeclareExit() {
        if (auto* existing = module->getFunction("exit")) return existing;
        auto* fnTy = llvm::FunctionType::get(
            llvm::Type::getVoidTy(ctx),
            { llvm::Type::getInt32Ty(ctx) }, false);
        auto* fn = llvm::Function::Create(fnTy, llvm::Function::ExternalLinkage, "exit", module.get());
        fn->addFnAttr(llvm::Attribute::NoReturn);
        return fn;
    }

    llvm::Constant* makeMessageString(const std::string& text) {
        return builder->CreateGlobalString(text, ".panicmsg");
    }

    void emitPanic(const std::string& message, int exitCode) {
        auto* i32Ty = llvm::Type::getInt32Ty(ctx);
        llvm::Value* stderrF = getStderr();
        auto* fputs = getOrDeclareFputs();
        builder->CreateCall(fputs, { builder->CreateGlobalString("panic: ", ".panic.pfx"), stderrF });
        builder->CreateCall(fputs, { makeMessageString(message), stderrF });
        builder->CreateCall(fputs, { builder->CreateGlobalString("\n", ".panic.nl"), stderrF });
        llvm::Value* trace = builder->CreateCall(captureTraceFn(), { llvm::ConstantInt::get(i32Ty, 64) }, "trace");
        llvm::Value* traceStr = builder->CreateCall(formatTraceFn(), { trace }, "trace.str");
        builder->CreateCall(fputs, { emitStringDataPtr(traceStr), stderrF });
        builder->CreateCall(getOrDeclareExit(), { llvm::ConstantInt::get(i32Ty, exitCode) });
        builder->CreateUnreachable();
    }

    // Builds an LLVM-symbol-safe name from Type::toString() by escaping the
    // characters toString() may emit that aren't valid in symbol names
    static std::string mangleTypeForName(::Type* t) {
        if (!t) return "_";
        std::string s = t->toString();
        std::string out;
        out.reserve(s.size());
        for (size_t i = 0; i < s.size(); ++i) {
            char c = s[i];
            if (c == '?') {
                out += "_opt";
            } else if (c == '[' && i + 1 < s.size() && s[i + 1] == ']') {
                out += "_arr";
                ++i;
            } else if (c == '<' || c == '>') {
                // skip
            } else {
                out += c;
            }
        }
        return out;
    }

    uint64_t elementSizeBytes(::Type* elem) {
        if (!elem) return 1;
        llvm::Type* lt = mapType(elem);
        if (!lt) return 1;
        return module->getDataLayout().getTypeAllocSize(lt);
    }

    llvm::Value* arrayLengthAddr(llvm::Value* arrPtr) {
        return arrPtr; // length lives at payload offset 0.
    }

    llvm::Value* emitArrayLength(llvm::Value* arrPtr) {
        return builder->CreateLoad(llvm::Type::getInt64Ty(ctx),
                                   arrayLengthAddr(arrPtr), "arr.length");
    }

    llvm::Value* emitArrayDataPtr(llvm::Value* arrPtr) {
        return builder->CreateGEP(
            llvm::Type::getInt8Ty(ctx), arrPtr,
            llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx), 8), "arr.data");
    }

    // Strings share the array payload layout: i64 length at +0, bytes at +8.
    llvm::Value* emitStringLength(llvm::Value* strPtr) {
        return builder->CreateLoad(llvm::Type::getInt64Ty(ctx), strPtr, "str.length");
    }

    llvm::Value* emitStringDataPtr(llvm::Value* strPtr) {
        return builder->CreateGEP(
            llvm::Type::getInt8Ty(ctx), strPtr,
            llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx), 8), "str.data");
    }

    // Emits (and caches) an immortal string object for a literal: a read-only
    // { typeDesc, refcount, dtor, sidetable, length, [N+1 x i8] } global whose
    // payload (length onward) matches the array layout, with a trailing NUL for
    // C interop. The refcount sentinel makes ens_retain/ens_release no-ops, so
    // the literal is never freed. Returns a constant pointer to the payload.
    llvm::Constant* emitStringLiteralObject(const std::string& utf8) {
        auto found = stringLiteralCache.find(utf8);
        if (found != stringLiteralCache.end()) return found->second;

        auto* ptrTy = llvm::PointerType::get(ctx, 0);
        auto* i64Ty = llvm::Type::getInt64Ty(ctx);
        auto* i32Ty = llvm::Type::getInt32Ty(ctx);
        auto* i8Ty = llvm::Type::getInt8Ty(ctx);

        size_t len = utf8.size();
        auto* bytesTy = llvm::ArrayType::get(i8Ty, len + 1);
        auto* objTy = llvm::StructType::get(ctx, { ptrTy, i64Ty, ptrTy, ptrTy, i64Ty, bytesTy });

        std::vector<llvm::Constant*> bytes;
        bytes.reserve(len + 1);
        for (char c : utf8) bytes.push_back(llvm::ConstantInt::get(i8Ty, static_cast<uint8_t>(c)));
        bytes.push_back(llvm::ConstantInt::get(i8Ty, 0));

        llvm::Constant* init = llvm::ConstantStruct::get(objTy, {
            llvm::ConstantPointerNull::get(ptrTy),
            llvm::ConstantInt::get(i64Ty, kImmortalRefcount),
            llvm::ConstantPointerNull::get(ptrTy),
            llvm::ConstantPointerNull::get(ptrTy),
            llvm::ConstantInt::get(i64Ty, static_cast<int64_t>(len)),
            llvm::ConstantArray::get(bytesTy, bytes),
        });

        auto* gv = new llvm::GlobalVariable(*module, objTy, /*isConstant=*/true,
            llvm::GlobalValue::PrivateLinkage, init, ".strobj");
        gv->setAlignment(llvm::Align(8));

        llvm::Constant* payload = llvm::ConstantExpr::getGetElementPtr(
            objTy, gv,
            llvm::ArrayRef<llvm::Constant*>{
                llvm::ConstantInt::get(i32Ty, 0),
                llvm::ConstantInt::get(i32Ty, 4) });
        stringLiteralCache[utf8] = payload;
        return payload;
    }

    // Wraps a NUL-terminated C string (possibly null) into a fresh Ens string
    // object. Returns null for a null input, so it composes with `string?`.
    llvm::Function* getOrDefineEnsStringFromCStr() {
        if (auto* existing = module->getFunction("ens_string_from_cstr")) return existing;
        auto* ptrTy = llvm::PointerType::get(ctx, 0);
        auto* i64Ty = llvm::Type::getInt64Ty(ctx);
        auto* i8Ty = llvm::Type::getInt8Ty(ctx);
        auto* fnTy = llvm::FunctionType::get(ptrTy, { ptrTy }, false);
        auto* fn = llvm::Function::Create(fnTy, llvm::Function::InternalLinkage,
            "ens_string_from_cstr", module.get());
        fn->addFnAttr(llvm::Attribute::NoUnwind);

        auto savedIP = builder->saveIP();
        auto* entry  = llvm::BasicBlock::Create(ctx, "entry", fn);
        auto* nullBB = llvm::BasicBlock::Create(ctx, "fromcstr.null", fn);
        auto* copyBB = llvm::BasicBlock::Create(ctx, "fromcstr.copy", fn);

        builder->SetInsertPoint(entry);
        llvm::Value* cstr = fn->getArg(0);
        llvm::Value* isNull = builder->CreateICmpEQ(cstr, llvm::ConstantPointerNull::get(ptrTy));
        builder->CreateCondBr(isNull, nullBB, copyBB);

        builder->SetInsertPoint(nullBB);
        builder->CreateRet(llvm::ConstantPointerNull::get(ptrTy));

        builder->SetInsertPoint(copyBB);
        llvm::Value* len = builder->CreateCall(getOrDeclareStrlen(), { cstr });
        llvm::Value* obj = emitStringAlloc(len);
        llvm::Value* data = emitStringDataPtr(obj);
        builder->CreateMemCpy(data, llvm::MaybeAlign(1), cstr, llvm::MaybeAlign(1), len);
        builder->CreateRet(obj);

        builder->restoreIP(savedIP);
        return fn;
    }

    // Allocates an uninitialized string object holding `len` bytes plus a NUL.
    // Stores the length and writes the trailing NUL; the caller fills the bytes.
    llvm::Value* emitStringAlloc(llvm::Value* len) {
        auto* ptrTy = llvm::PointerType::get(ctx, 0);
        auto* i64Ty = llvm::Type::getInt64Ty(ctx);
        auto* i8Ty = llvm::Type::getInt8Ty(ctx);
        llvm::Value* payloadBytes = builder->CreateAdd(
            len, llvm::ConstantInt::get(i64Ty, 9), "str.payloadbytes");
        llvm::Value* obj = builder->CreateCall(getOrDefineEnsAlloc(),
            { payloadBytes, llvm::ConstantPointerNull::get(ptrTy),
              llvm::ConstantPointerNull::get(ptrTy) }, "str.new");
        builder->CreateStore(len, obj);
        llvm::Value* nulSlot = builder->CreateGEP(i8Ty, emitStringDataPtr(obj), len, "str.nul");
        builder->CreateStore(llvm::ConstantInt::get(i8Ty, 0), nulSlot);
        return obj;
    }

    // i1 ens_string_eq(a, b): true when both strings have equal contents. A
    // pointer-identity check fast-paths shared/interned literals and both-null;
    // length and memcmp are the source of truth. Null-safe for `string?`.
    llvm::Function* getOrDefineEnsStringEq() {
        if (auto* existing = module->getFunction("ens_string_eq")) return existing;
        auto* ptrTy = llvm::PointerType::get(ctx, 0);
        auto* i32Ty = llvm::Type::getInt32Ty(ctx);
        auto* fnTy = llvm::FunctionType::get(llvm::Type::getInt1Ty(ctx), { ptrTy, ptrTy }, false);
        auto* fn = llvm::Function::Create(fnTy, llvm::Function::InternalLinkage,
            "ens_string_eq", module.get());
        fn->addFnAttr(llvm::Attribute::NoUnwind);

        auto savedIP = builder->saveIP();
        auto* entry   = llvm::BasicBlock::Create(ctx, "entry", fn);
        auto* notSame = llvm::BasicBlock::Create(ctx, "streq.notsame", fn);
        auto* cmpLen  = llvm::BasicBlock::Create(ctx, "streq.cmplen", fn);
        auto* cmpData = llvm::BasicBlock::Create(ctx, "streq.cmpdata", fn);
        auto* trueBB  = llvm::BasicBlock::Create(ctx, "streq.true", fn);
        auto* falseBB = llvm::BasicBlock::Create(ctx, "streq.false", fn);

        builder->SetInsertPoint(entry);
        llvm::Value* a = fn->getArg(0);
        llvm::Value* b = fn->getArg(1);
        builder->CreateCondBr(builder->CreateICmpEQ(a, b), trueBB, notSame);

        builder->SetInsertPoint(notSame);
        llvm::Value* aNull = builder->CreateICmpEQ(a, llvm::ConstantPointerNull::get(ptrTy));
        llvm::Value* bNull = builder->CreateICmpEQ(b, llvm::ConstantPointerNull::get(ptrTy));
        builder->CreateCondBr(builder->CreateOr(aNull, bNull), falseBB, cmpLen);

        builder->SetInsertPoint(cmpLen);
        llvm::Value* la = emitStringLength(a);
        llvm::Value* lb = emitStringLength(b);
        builder->CreateCondBr(builder->CreateICmpEQ(la, lb), cmpData, falseBB);

        builder->SetInsertPoint(cmpData);
        llvm::Value* cmp = builder->CreateCall(getOrDeclareMemcmp(),
            { emitStringDataPtr(a), emitStringDataPtr(b), la }, "streq.memcmp");
        builder->CreateCondBr(
            builder->CreateICmpEQ(cmp, llvm::ConstantInt::get(i32Ty, 0)), trueBB, falseBB);

        builder->SetInsertPoint(trueBB);
        builder->CreateRet(llvm::ConstantInt::getTrue(ctx));
        builder->SetInsertPoint(falseBB);
        builder->CreateRet(llvm::ConstantInt::getFalse(ctx));

        builder->restoreIP(savedIP);
        return fn;
    }

    static bool isStringLike(::Type* t) {
        if (!t) return false;
        if (t->isString()) return true;
        return t->isOptional() && t->inner && t->inner->isString();
    }

    // string ens_string_concat(a, b): a fresh string holding a's bytes followed
    // by b's, with a trailing NUL. Owned (+1) by the caller.
    llvm::Function* getOrDefineEnsStringConcat() {
        if (auto* existing = module->getFunction("ens_string_concat")) return existing;
        auto* ptrTy = llvm::PointerType::get(ctx, 0);
        auto* i8Ty = llvm::Type::getInt8Ty(ctx);
        auto* fnTy = llvm::FunctionType::get(ptrTy, { ptrTy, ptrTy }, false);
        auto* fn = llvm::Function::Create(fnTy, llvm::Function::InternalLinkage,
            "ens_string_concat", module.get());
        fn->addFnAttr(llvm::Attribute::NoUnwind);

        auto savedIP = builder->saveIP();
        builder->SetInsertPoint(llvm::BasicBlock::Create(ctx, "entry", fn));
        llvm::Value* a = fn->getArg(0);
        llvm::Value* b = fn->getArg(1);
        llvm::Value* la = emitStringLength(a);
        llvm::Value* lb = emitStringLength(b);
        llvm::Value* obj = emitStringAlloc(builder->CreateAdd(la, lb));
        llvm::Value* data = emitStringDataPtr(obj);
        builder->CreateMemCpy(data, llvm::MaybeAlign(1), emitStringDataPtr(a), llvm::MaybeAlign(1), la);
        llvm::Value* tail = builder->CreateGEP(i8Ty, data, la, "str.concat.tail");
        builder->CreateMemCpy(tail, llvm::MaybeAlign(1), emitStringDataPtr(b), llvm::MaybeAlign(1), lb);
        builder->CreateRet(obj);

        builder->restoreIP(savedIP);
        return fn;
    }

    // Lazily emits `_dtor_<elem>_array` which walks the element data and
    // releases per-slot ownership where required. Returns null if the element
    // type needs no per-slot work (primitives / externals / class-free structs).
    llvm::Value* getOrEmitArrayDtor(::Type* elem) {
        auto* ptrTy = llvm::PointerType::get(ctx, 0);
        if (!elem) return llvm::ConstantPointerNull::get(ptrTy);

        bool needsWalk = isReferenceType(elem) || structHasClassFields(elem);
        if (!needsWalk) return llvm::ConstantPointerNull::get(ptrTy);

        std::string name = "_dtor_" + mangleTypeForName(elem) + "_array";
        if (auto* existing = module->getFunction(name)) return existing;

        auto* fnTy = llvm::FunctionType::get(llvm::Type::getVoidTy(ctx), { ptrTy }, false);
        auto* fn = llvm::Function::Create(fnTy, llvm::Function::InternalLinkage, name, module.get());
        fn->addFnAttr(llvm::Attribute::NoUnwind);

        auto savedIP = builder->saveIP();
        auto* entry    = llvm::BasicBlock::Create(ctx, "entry",    fn);
        auto* loopCond = llvm::BasicBlock::Create(ctx, "loop.cond", fn);
        auto* loopBody = llvm::BasicBlock::Create(ctx, "loop.body", fn);
        auto* loopEnd  = llvm::BasicBlock::Create(ctx, "loop.end",  fn);

        builder->SetInsertPoint(entry);
        llvm::Value* arrPtr = fn->getArg(0);
        llvm::Type* i64 = llvm::Type::getInt64Ty(ctx);
        llvm::Value* length = builder->CreateLoad(i64, arrPtr, "arr.length");
        llvm::Value* data = builder->CreateGEP(
            llvm::Type::getInt8Ty(ctx), arrPtr,
            llvm::ConstantInt::get(i64, 8), "arr.data");
        llvm::Value* idxAlloca = builder->CreateAlloca(i64, nullptr, "i");
        builder->CreateStore(llvm::ConstantInt::get(i64, 0), idxAlloca);
        builder->CreateBr(loopCond);

        builder->SetInsertPoint(loopCond);
        llvm::Value* idx = builder->CreateLoad(i64, idxAlloca, "i.load");
        llvm::Value* cond = builder->CreateICmpSLT(idx, length, "i.lt.len");
        builder->CreateCondBr(cond, loopBody, loopEnd);

        builder->SetInsertPoint(loopBody);
        llvm::Type* elemTy = mapType(elem);
        llvm::Value* slot = builder->CreateGEP(elemTy, data, idx, "slot");
        if (isReferenceType(elem)) {
            llvm::Value* val = builder->CreateLoad(ptrTy, slot, "slot.val");
            builder->CreateCall(getOrDefineEnsRelease(), { val });
        } else if (structHasClassFields(elem)) {
            emitStructFieldRelease(elem, slot);
        }
        llvm::Value* next = builder->CreateAdd(idx, llvm::ConstantInt::get(i64, 1));
        builder->CreateStore(next, idxAlloca);
        builder->CreateBr(loopCond);

        builder->SetInsertPoint(loopEnd);
        builder->CreateRetVoid();

        builder->restoreIP(savedIP);
        return fn;
    }

    bool structHasFieldDefaults(::Type* t) {
        if (!t || !t->isStruct() || !t->structInfo) return false;
        for (auto& f : t->structInfo->fields) {
            if (!f.declaration) continue;
            auto fieldNode = SyntaxNode::makeRoot(f.declaration);
            auto fd = ast::FieldDecl::cast(*fieldNode);
            if (fd && fd->defaultValue()) return true;
        }
        return false;
    }

    // Extract a compile-time integer literal from an array-size or array-literal-element initializer
    int64_t stackArrayCountFromInit(const ast::Expression& init) {
        if (auto n = init.asNew()) {
            auto sizes = n->arraySizeExpressions();
            if (sizes.size() != 1) return 0;
            auto lit = sizes[0].asLiteral();
            if (!lit) return 0;
            auto tok = lit->token();
            if (!tok) return 0;
            return static_cast<int64_t>(parseIntText(tok->tokenText()));
        }
        if (auto al = init.asArrayLiteral()) {
            return static_cast<int64_t>(al->elements().size());
        }
        if (auto p = init.asParen()) {
            if (auto inner = p->inner()) return stackArrayCountFromInit(*inner);
        }
        return 0;
    }

    void emitStackArrayInit(Symbol* sym, ::Type* arrayType,
                            const ast::Expression& init,
                            llvm::Value* slotAlloca) {
        ::Type* elem = arrayType ? arrayType->inner : nullptr;
        if (!elem) return;

        int64_t count = stackArrayCountFromInit(init);
        if (count < 0) count = 0;
        uint64_t elemBytes = elementSizeBytes(elem);
        uint64_t totalBytes = 8 + static_cast<uint64_t>(count) * elemBytes;

        auto* i8Ty  = llvm::Type::getInt8Ty(ctx);
        auto* i64Ty = llvm::Type::getInt64Ty(ctx);
        // Use a fixed-size i8 array so the alloca claims the exact byte width.
        auto* storageTy = llvm::ArrayType::get(i8Ty, totalBytes);
        llvm::Value* arrPtr = createEntryAlloca(currentFunction, storageTy,
            asAscii(sym->name) + ".stack");

        // make sure to zero out slots for each loop tieration
        builder->CreateMemSet(arrPtr, llvm::ConstantInt::get(i8Ty, 0),
                              llvm::ConstantInt::get(i64Ty, totalBytes),
                              llvm::Align(8));

        builder->CreateStore(llvm::ConstantInt::get(i64Ty, count), arrPtr);

        if (auto al = init.asArrayLiteral()) {
            llvm::Value* data = builder->CreateGEP(
                i8Ty, arrPtr, llvm::ConstantInt::get(i64Ty, 8), "arr.data");
            llvm::Type* elemTy = mapType(elem);
            bool elemIsClass = isReferenceType(elem);
            bool elemIsStructWithClass = structHasClassFields(elem);
            auto elements = al->elements();
            for (size_t i = 0; i < elements.size(); ++i) {
                const ast::Expression& src = elements[i];
                bool borrowed = !expressionProducesOwnedRef(src);
                llvm::Value* v = emitExprConverted(src, elem);
                if (!v) return;
                llvm::Value* slot = builder->CreateGEP(
                    elemTy, data, llvm::ConstantInt::get(i64Ty, static_cast<int64_t>(i)),
                    "lit.slot");
                if (elemIsClass) {
                    if (borrowed) emitRetain(v);
                    builder->CreateStore(v, slot);
                } else if (elemIsStructWithClass) {
                    builder->CreateStore(v, slot);
                    if (borrowed) emitStructFieldRetain(elem, slot);
                } else {
                    builder->CreateStore(v, slot);
                }
            }
        } else if (init.asNew() && structHasFieldDefaults(elem)) {
            llvm::Value* data = builder->CreateGEP(
                i8Ty, arrPtr, llvm::ConstantInt::get(i64Ty, 8), "arr.data");
            llvm::Type* elemTy = mapType(elem);
            for (int64_t i = 0; i < count; ++i) {
                llvm::Value* slot = builder->CreateGEP(
                    elemTy, data, llvm::ConstantInt::get(i64Ty, i), "arr.init.slot");
                initStructFieldDefaults(elem, slot);
            }
        }

        builder->CreateStore(arrPtr, slotAlloca);

        if ((isReferenceType(elem) || structHasClassFields(elem)) && !cleanupStack.empty()) {
            cleanupStack.back().push_back({ slotAlloca, arrayType, /*isStackArray*/ true });
        }
    }

    llvm::Value* emitArrayNew(::Type* elem, llvm::Value* sizeI64,
                              uint32_t diagOffset) {
        if (!elem) return nullptr;
        auto* ptrTy = llvm::PointerType::get(ctx, 0);
        llvm::Type* i64 = llvm::Type::getInt64Ty(ctx);

        // Bounds check: size must be non-negative.
        auto* badBB = llvm::BasicBlock::Create(ctx, "arr.new.neg", currentFunction);
        auto* okBB  = llvm::BasicBlock::Create(ctx, "arr.new.ok",  currentFunction);
        llvm::Value* neg = builder->CreateICmpSLT(
            sizeI64, llvm::ConstantInt::get(i64, 0), "arr.new.neg.cmp");
        builder->CreateCondBr(neg, badBB, okBB);

        builder->SetInsertPoint(badBB);
        emitPanic("array size cannot be negative", 134);

        builder->SetInsertPoint(okBB);
        uint64_t elemBytes = elementSizeBytes(elem);
        llvm::Value* dataBytes = builder->CreateMul(
            sizeI64, llvm::ConstantInt::get(i64, static_cast<int64_t>(elemBytes)), "arr.databytes");
        llvm::Value* payloadBytes = builder->CreateAdd(
            dataBytes, llvm::ConstantInt::get(i64, 8), "arr.payloadbytes");

        llvm::Value* dtor = getOrEmitArrayDtor(elem);
        if (!dtor) dtor = llvm::ConstantPointerNull::get(ptrTy);
        // Arrays carry no TypeDescriptor (no RTTI), so pass null.
        llvm::Value* arrPtr = builder->CreateCall(
            getOrDefineEnsAlloc(), { payloadBytes, dtor, llvm::ConstantPointerNull::get(ptrTy) }, "arr.new");
        builder->CreateStore(sizeI64, arrayLengthAddr(arrPtr));

        // Run per-slot struct field defaults when the element type declares
        // any. (calloc already produced zero-initialized slots, so structs
        // with only-zero defaults / no defaults at all are already done.)
        if (structHasFieldDefaults(elem)) {
            auto* loopCond = llvm::BasicBlock::Create(ctx, "arr.init.cond", currentFunction);
            auto* loopBody = llvm::BasicBlock::Create(ctx, "arr.init.body", currentFunction);
            auto* loopEnd  = llvm::BasicBlock::Create(ctx, "arr.init.end",  currentFunction);

            llvm::Value* data = emitArrayDataPtr(arrPtr);
            llvm::Value* idxAlloca = createEntryAlloca(currentFunction, i64, "arr.init.i");
            builder->CreateStore(llvm::ConstantInt::get(i64, 0), idxAlloca);
            builder->CreateBr(loopCond);

            builder->SetInsertPoint(loopCond);
            llvm::Value* idx = builder->CreateLoad(i64, idxAlloca, "arr.init.i.load");
            llvm::Value* cond = builder->CreateICmpSLT(idx, sizeI64, "arr.init.i.lt");
            builder->CreateCondBr(cond, loopBody, loopEnd);

            builder->SetInsertPoint(loopBody);
            llvm::Type* elemTy = mapType(elem);
            llvm::Value* slot = builder->CreateGEP(elemTy, data, idx, "arr.init.slot");
            initStructFieldDefaults(elem, slot);
            llvm::Value* next = builder->CreateAdd(idx, llvm::ConstantInt::get(i64, 1));
            builder->CreateStore(next, idxAlloca);
            builder->CreateBr(loopCond);

            builder->SetInsertPoint(loopEnd);
        }
        return arrPtr;
    }

    llvm::Value* emitArrayLiteral(const ast::ArrayLiteralExpression& e) {
        ::Type* arrT = typeOf(e.node);
        if (!arrT || !arrT->isArray() || !arrT->inner) {
            error(e.node.startOffset(), "Internal: array literal has no resolved array type");
            return nullptr;
        }
        ::Type* elemT = arrT->inner;
        auto elems = e.elements();
        llvm::Type* i64 = llvm::Type::getInt64Ty(ctx);
        llvm::Value* sizeConst = llvm::ConstantInt::get(i64, static_cast<int64_t>(elems.size()));
        llvm::Value* arrPtr = emitArrayNew(elemT, sizeConst, e.node.startOffset());
        if (!arrPtr || elems.empty()) return arrPtr;

        llvm::Value* data = emitArrayDataPtr(arrPtr);
        llvm::Type* elemTy = mapType(elemT);
        bool elemIsClass = isReferenceType(elemT);
        bool elemIsStructWithClass = structHasClassFields(elemT);

        for (size_t i = 0; i < elems.size(); ++i) {
            const ast::Expression& src = elems[i];
            bool borrowed = !expressionProducesOwnedRef(src);
            llvm::Value* v = emitExprConverted(src, elemT);
            if (!v) return nullptr;
            llvm::Value* slot = builder->CreateGEP(
                elemTy, data, llvm::ConstantInt::get(i64, static_cast<int64_t>(i)),
                "lit.slot");
            if (elemIsClass) {
                if (borrowed) emitRetain(v);
                builder->CreateStore(v, slot);
            } else if (elemIsStructWithClass) {
                // Slot may have struct field defaults written by emitArrayNew;
                // those copies are owning +1 against the source pool, release
                // them before overwriting with the literal value.
                if (structHasFieldDefaults(elemT)) emitStructFieldRelease(elemT, slot);
                builder->CreateStore(v, slot);
                if (borrowed) emitStructFieldRetain(elemT, slot);
            } else {
                builder->CreateStore(v, slot);
            }
        }
        return arrPtr;
    }

    // Emits a bounds check then returns the address of element `index`.
    llvm::Value* emitArraySubscriptAddr(llvm::Value* arrPtr, llvm::Value* indexAny,
                                        ::Type* elem) {
        llvm::Type* i64 = llvm::Type::getInt64Ty(ctx);
        llvm::Value* index = indexAny;
        if (index->getType() != i64) {
            index = builder->CreateSExtOrTrunc(index, i64, "idx.i64");
        }
        llvm::Value* length = emitArrayLength(arrPtr);

        auto* outOfBoundsBB = llvm::BasicBlock::Create(ctx, "arr.oob", currentFunction);
        auto* okBB          = llvm::BasicBlock::Create(ctx, "arr.idx.ok", currentFunction);

        llvm::Value* tooLow  = builder->CreateICmpSLT(index, llvm::ConstantInt::get(i64, 0), "idx.neg");
        llvm::Value* tooHigh = builder->CreateICmpSGE(index, length, "idx.ge.len");
        llvm::Value* oob     = builder->CreateOr(tooLow, tooHigh, "idx.oob");
        builder->CreateCondBr(oob, outOfBoundsBB, okBB);

        builder->SetInsertPoint(outOfBoundsBB);
        emitPanic("array index out of bounds", 134);

        builder->SetInsertPoint(okBB);
        llvm::Value* data = emitArrayDataPtr(arrPtr);
        llvm::Type* elemTy = mapType(elem);
        return builder->CreateGEP(elemTy, data, index, "arr.slot");
    }

    // Multi-dim `new T[a][b][c]`: allocate one array per level, and for each
    // non-innermost level fill every slot with a freshly-allocated next-level
    // array. The +1 from each inner `new` is transferred straight into the
    // outer's slot (no retain). Innermost slots stay zero-initialized.
    //
    // `slotElemType` is the element type for THIS level, i.e. for level 0
    // it's the next-deeper array type, and for the deepest level it's the
    // user-written T.
    llvm::Value* emitMultiDimNew(::Type* slotElemType,
                                  const std::vector<llvm::Value*>& sizes,
                                  size_t levelIdx,
                                  uint32_t diagOffset) {
        if (levelIdx >= sizes.size()) return nullptr;
        llvm::Value* arr = emitArrayNew(slotElemType, sizes[levelIdx], diagOffset);
        if (!arr) return nullptr;
        if (levelIdx + 1 >= sizes.size()) return arr;

        // We have more sizes to consume; fill each slot with a recursive alloc.
        ::Type* nextElem = slotElemType->inner;
        if (!nextElem) {
            error(diagOffset, "Internal: multi-dim 'new' level has no inner element type");
            return arr;
        }
        llvm::Type* i64 = llvm::Type::getInt64Ty(ctx);
        auto* loopCond = llvm::BasicBlock::Create(ctx, "mdim.cond", currentFunction);
        auto* loopBody = llvm::BasicBlock::Create(ctx, "mdim.body", currentFunction);
        auto* loopEnd  = llvm::BasicBlock::Create(ctx, "mdim.end",  currentFunction);

        llvm::Value* idxAlloca = createEntryAlloca(currentFunction, i64, "mdim.i");
        builder->CreateStore(llvm::ConstantInt::get(i64, 0), idxAlloca);
        builder->CreateBr(loopCond);

        builder->SetInsertPoint(loopCond);
        llvm::Value* idx = builder->CreateLoad(i64, idxAlloca);
        llvm::Value* cond = builder->CreateICmpSLT(idx, sizes[levelIdx], "mdim.cmp");
        builder->CreateCondBr(cond, loopBody, loopEnd);

        builder->SetInsertPoint(loopBody);
        llvm::Value* inner = emitMultiDimNew(nextElem, sizes, levelIdx + 1, diagOffset);
        if (!inner) return arr;
        llvm::Value* data = emitArrayDataPtr(arr);
        llvm::Type* slotTy = mapType(slotElemType);
        llvm::Value* slot = builder->CreateGEP(slotTy, data, idx, "mdim.slot");
        builder->CreateStore(inner, slot);
        llvm::Value* next = builder->CreateAdd(idx, llvm::ConstantInt::get(i64, 1));
        builder->CreateStore(next, idxAlloca);
        builder->CreateBr(loopCond);

        builder->SetInsertPoint(loopEnd);
        return arr;
    }

    const FieldInfo* memberFieldInfo(const ast::MemberExpression& m) const {
        auto obj = m.object();
        if (!obj) return nullptr;
        ::Type* objType = typeOf(obj->node);
        if (!objType || !objType->structInfo) return nullptr;
        auto name = m.memberText();
        if (!name) return nullptr;
        int idx = objType->structInfo->findFieldIndex(*name);
        if (idx < 0) return nullptr;
        return &objType->structInfo->fields[static_cast<size_t>(idx)];
    }

    bool expressionProducesOwnedRef(const ast::Expression& e) {
        if (e.asNew() || e.asCall()) return true;
        if (e.asArrayLiteral()) return true;
        if (e.asSafeMember()) return true;
        if (e.asSafeSubscript()) return true;
        if (auto m = e.asMember()) {
            // Weak field reads return +1 via ens_weak_load's CAS-upgrade.
            const FieldInfo* fi = memberFieldInfo(*m);
            return fi && fi->isWeak;
        }
        if (auto p = e.asParen()) {
            if (auto inner = p->inner()) return expressionProducesOwnedRef(*inner);
        }
        if (auto tr = e.asTry()) {
            if (auto operand = tr->operand()) return expressionProducesOwnedRef(*operand);
        }
        if (auto bin = e.asBinary()) {
            // String concatenation builds a fresh owned string.
            auto opTok = bin->operatorToken();
            if (opTok && opTok->kind() == SyntaxKind::Plus) {
                ::Type* t = typeOf(e.node);
                if (t && t->isString()) return true;
            }
        }
        return false;
    }

    // Releases a value that an expression produced as a fresh +1 temporary,
    // once it has been consumed (e.g. a concat operand in a chain). No-op for
    // borrowed sources (variables, fields) and immortal literals.
    void releaseIfOwnedTemp(llvm::Value* v, const ast::Expression& e) {
        if (v && expressionProducesOwnedRef(e)) {
            builder->CreateCall(getOrDefineEnsRelease(), { v });
        }
    }

    bool structHasClassFields(::Type* t) {
        if (!t || !t->isStruct() || !t->structInfo) return false;
        for (auto& f : t->structInfo->fields) {
            if (isReferenceType(f.type)) return true;
        }
        return false;
    }

    void emitStructFieldRetain(::Type* t, llvm::Value* base) {
        if (!t || !t->structInfo) return;
        auto* ptrTy = llvm::PointerType::get(ctx, 0);
        llvm::StructType* layout = mapStructType(t);
        auto* retainFn = getOrDefineEnsRetain();
        for (size_t i = 0; i < t->structInfo->fields.size(); ++i) {
            auto& f = t->structInfo->fields[i];
            if (!f.type) continue;
            if (isReferenceType(f.type)) {
                llvm::Value* fieldAddr = builder->CreateStructGEP(layout, base, static_cast<unsigned>(i),
                                                                  asAscii(f.name) + ".addr");
                llvm::Value* fieldVal = builder->CreateLoad(ptrTy, fieldAddr);
                builder->CreateCall(retainFn, { fieldVal });
            } else if (structHasClassFields(f.type)) {
                llvm::Value* fieldAddr = builder->CreateStructGEP(layout, base, static_cast<unsigned>(i),
                                                                  asAscii(f.name) + ".addr");
                emitStructFieldRetain(f.type, fieldAddr);
            }
        }
    }

    void emitStructFieldRetainOnValue(::Type* t, llvm::Value* aggVal) {
        if (!t || !t->structInfo) return;
        auto* retainFn = getOrDefineEnsRetain();
        for (size_t i = 0; i < t->structInfo->fields.size(); ++i) {
            auto& f = t->structInfo->fields[i];
            if (!f.type) continue;
            if (isReferenceType(f.type)) {
                llvm::Value* fieldVal = builder->CreateExtractValue(aggVal, { static_cast<unsigned>(i) });
                builder->CreateCall(retainFn, { fieldVal });
            } else if (structHasClassFields(f.type)) {
                llvm::Value* sub = builder->CreateExtractValue(aggVal, { static_cast<unsigned>(i) });
                emitStructFieldRetainOnValue(f.type, sub);
            }
        }
    }

    void emitStructFieldRelease(::Type* t, llvm::Value* base) {
        if (!t || !t->structInfo) return;
        auto* ptrTy = llvm::PointerType::get(ctx, 0);
        llvm::StructType* layout = mapStructType(t);
        auto* releaseFn = getOrDefineEnsRelease();
        auto* weakReleaseFn = getOrDefineEnsWeakRelease();
        for (size_t i = 0; i < t->structInfo->fields.size(); ++i) {
            auto& f = t->structInfo->fields[i];
            if (!f.type) continue;
            if (f.isWeak) {
                llvm::Value* fieldAddr = builder->CreateStructGEP(layout, base, static_cast<unsigned>(i),
                                                                  asAscii(f.name) + ".addr");
                llvm::Value* fieldVal = builder->CreateLoad(ptrTy, fieldAddr);
                builder->CreateCall(weakReleaseFn, { fieldVal });
            } else if (isReferenceType(f.type)) {
                llvm::Value* fieldAddr = builder->CreateStructGEP(layout, base, static_cast<unsigned>(i),
                                                                  asAscii(f.name) + ".addr");
                llvm::Value* fieldVal = builder->CreateLoad(ptrTy, fieldAddr);
                builder->CreateCall(releaseFn, { fieldVal });
            } else if (structHasClassFields(f.type)) {
                llvm::Value* fieldAddr = builder->CreateStructGEP(layout, base, static_cast<unsigned>(i),
                                                                  asAscii(f.name) + ".addr");
                emitStructFieldRelease(f.type, fieldAddr);
            }
        }
    }

    void registerOwnedLocal(llvm::Value* alloca, ::Type* type) {
        if (cleanupStack.empty()) return;
        if (isReferenceType(type) || structHasClassFields(type)) {
            // In a function with an error path, an initializer that throws would
            // unwind before this slot is stored; pre-null so its release no-ops.
            if (throwTargetSlot) {
                if (isReferenceType(type)) {
                    builder->CreateStore(
                        llvm::ConstantPointerNull::get(llvm::PointerType::get(ctx, 0)), alloca);
                } else {
                    builder->CreateStore(llvm::Constant::getNullValue(mapType(type)), alloca);
                }
            }
            cleanupStack.back().push_back({ alloca, type });
        }
    }

    // Release frame entries [from, end) in reverse order.
    void emitFrameCleanupFrom(const std::vector<OwnedLocal>& frame, size_t from) {
        if (from >= frame.size()) return;
        auto* ptrTy = llvm::PointerType::get(ctx, 0);
        auto* releaseFn = getOrDefineEnsRelease();
        for (size_t i = frame.size(); i > from; --i) {
            const OwnedLocal& ol = frame[i - 1];
            if (ol.isStackArray) {
                if (!ol.type || !ol.type->isArray() || !ol.type->inner) continue;
                ::Type* elem = ol.type->inner;
                if (!isReferenceType(elem) && !structHasClassFields(elem)) continue;
                llvm::Value* arrPtr = builder->CreateLoad(ptrTy, ol.alloca);
                llvm::Value* dtor = getOrEmitArrayDtor(elem);
                if (auto* dtorFn = llvm::dyn_cast<llvm::Function>(dtor)) {
                    builder->CreateCall(dtorFn, { arrPtr });
                }
            } else if (isReferenceType(ol.type)) {
                llvm::Value* val = builder->CreateLoad(ptrTy, ol.alloca);
                builder->CreateCall(releaseFn, { val });
            } else if (structHasClassFields(ol.type)) {
                emitStructFieldRelease(ol.type, ol.alloca);
            }
        }
    }

    void emitFrameCleanup(const std::vector<OwnedLocal>& frame) {
        emitFrameCleanupFrom(frame, 0);
    }

    void emitFullCleanup() {
        for (auto it = cleanupStack.rbegin(); it != cleanupStack.rend(); ++it) {
            emitFrameCleanup(*it);
        }
    }

    // Release every owned local live at a throw/propagation site except the
    // function's parameter copies (kept alive for catch clauses).
    void emitCleanupToWatermark() {
        for (size_t fi = cleanupStack.size(); fi > 0; --fi) {
            const auto& frame = cleanupStack[fi - 1];
            if (fi == 1) emitFrameCleanupFrom(frame, paramCleanupWatermark);
            else emitFrameCleanup(frame);
        }
    }

    void emitReturnZero() {
        llvm::Type* rt = currentFunction->getReturnType();
        if (rt->isVoidTy()) builder->CreateRetVoid();
        else builder->CreateRet(llvm::Constant::getNullValue(rt));
    }

    // Emit the error path for the current insert point: either branch to this
    // function's catch dispatch, or run full cleanup and return zero (propagate).
    void emitErrorUnwind() {
        if (unwindToDispatch && catchDispatchBB) {
            emitCleanupToWatermark();
            builder->CreateBr(catchDispatchBB);
        } else {
            emitFullCleanup();
            emitReturnZero();
        }
    }

    // After a call to a function that may throw (its slot was passed as the
    // trailing arg), branch to the error path if the slot is now non-null.
    void emitThrowsCheck(const Symbol* callee) {
        if (!callee || !callee->abiThrows || !throwTargetSlot) return;
        auto* ptrTy = llvm::PointerType::get(ctx, 0);
        llvm::Value* err = builder->CreateLoad(ptrTy, throwTargetSlot, "err.check");
        llvm::Value* thrown = builder->CreateICmpNE(
            err, llvm::ConstantPointerNull::get(ptrTy), "thrown");
        auto* unwindBB = llvm::BasicBlock::Create(ctx, "unwind", currentFunction);
        auto* okBB = llvm::BasicBlock::Create(ctx, "cont", currentFunction);
        builder->CreateCondBr(thrown, unwindBB, okBB);
        builder->SetInsertPoint(unwindBB);
        emitErrorUnwind();
        builder->SetInsertPoint(okBB);
    }

    // Internal helper: walk a TypeDescriptor parent chain; true if `desc` is
    // `target` or descends from it.
    llvm::Function* getOrDefineEnsTypeIs() {
        if (auto* existing = module->getFunction("ens_type_is")) return existing;
        auto* ptrTy = llvm::PointerType::get(ctx, 0);
        auto* i1 = llvm::Type::getInt1Ty(ctx);
        auto* i32 = llvm::Type::getInt32Ty(ctx);
        auto* fnTy = llvm::FunctionType::get(i1, { ptrTy, ptrTy }, false);
        auto* fn = llvm::Function::Create(fnTy, llvm::Function::InternalLinkage, "ens_type_is", module.get());
        fn->addFnAttr(llvm::Attribute::NoUnwind);

        llvm::IRBuilder<> b(ctx);
        auto* entry = llvm::BasicBlock::Create(ctx, "entry", fn);
        auto* loop  = llvm::BasicBlock::Create(ctx, "loop", fn);
        auto* next  = llvm::BasicBlock::Create(ctx, "next", fn);
        auto* yes   = llvm::BasicBlock::Create(ctx, "yes", fn);
        auto* no    = llvm::BasicBlock::Create(ctx, "no", fn);
        llvm::Value* descArg = fn->getArg(0);
        llvm::Value* target = fn->getArg(1);

        b.SetInsertPoint(entry);
        b.CreateBr(loop);
        b.SetInsertPoint(loop);
        auto* cur = b.CreatePHI(ptrTy, 2, "cur");
        cur->addIncoming(descArg, entry);
        llvm::Value* isNull = b.CreateICmpEQ(cur, llvm::ConstantPointerNull::get(ptrTy));
        auto* body = llvm::BasicBlock::Create(ctx, "body", fn);
        b.CreateCondBr(isNull, no, body);
        b.SetInsertPoint(body);
        llvm::Value* eq = b.CreateICmpEQ(cur, target);
        b.CreateCondBr(eq, yes, next);
        b.SetInsertPoint(next);
        llvm::Value* parentAddr = b.CreateGEP(getTypeDescriptorTy(), cur,
            { llvm::ConstantInt::get(i32, 0), llvm::ConstantInt::get(i32, 1) }, "parent.addr");
        llvm::Value* parent = b.CreateLoad(ptrTy, parentAddr, "parent");
        cur->addIncoming(parent, next);
        b.CreateBr(loop);
        b.SetInsertPoint(yes);
        b.CreateRet(llvm::ConstantInt::getTrue(ctx));
        b.SetInsertPoint(no);
        b.CreateRet(llvm::ConstantInt::getFalse(ctx));
        return fn;
    }

    // Load the TypeDescriptor pointer from a heap object's header (offset -32).
    llvm::Value* loadDescriptor(llvm::Value* obj) {
        auto* ptrTy = llvm::PointerType::get(ctx, 0);
        auto* i8Ty = llvm::Type::getInt8Ty(ctx);
        auto* i64 = llvm::Type::getInt64Ty(ctx);
        llvm::Value* slot = builder->CreateGEP(i8Ty, obj,
            llvm::ConstantInt::getSigned(i64, -32), "desc.slot");
        return builder->CreateLoad(ptrTy, slot, "desc");
    }

    llvm::Value* emitAddressForByPointerArg(const ast::Expression& e, ::Type* paramType) {
        if (e.asIdent() || e.asMember()) {
            return emitLValue(e);
        }
        if (auto p = e.asParen()) {
            if (auto inner = p->inner()) {
                return emitAddressForByPointerArg(*inner, paramType);
            }
        }
        llvm::Type* lt = mapType(paramType);
        auto* temp = createEntryAlloca(currentFunction, lt, "byptr.tmp");
        llvm::Value* val = emitExpr(e);
        if (!val) return nullptr;
        builder->CreateStore(val, temp);
        if (structHasClassFields(paramType) && !cleanupStack.empty()) {
            cleanupStack.back().push_back({ temp, paramType });
        }
        return temp;
    }

    // An owned temporary passed by value (a `new`/call result, not bound to a
    // local) is borrowed by the callee; the caller must release it. Track it so
    // both normal scope exit and an exception unwind free it exactly once.
    void trackOwnedArgTemp(llvm::Value* v, const ast::Expression& a, ::Type* paramT) {
        if (!paramT || !isReferenceType(paramT) || cleanupStack.empty()) return;
        if (!expressionProducesOwnedRef(a)) return;
        auto* slot = createEntryAlloca(currentFunction, llvm::PointerType::get(ctx, 0), "arg.tmp");
        builder->CreateStore(v, slot);
        cleanupStack.back().push_back({ slot, paramT });
    }

    bool appendCallArgs(Symbol* sym, const std::vector<ast::Expression>& userArgs,
                        std::vector<llvm::Value*>& out) {
        for (size_t i = 0; i < userArgs.size(); ++i) {
            auto& a = userArgs[i];
            ::Type* paramT = (sym && i < sym->paramTypes.size()) ? sym->paramTypes[i] : nullptr;
            if (sym && paramIsByPointer(sym, i)) {
                llvm::Value* v = emitAddressForByPointerArg(a, paramT);
                if (!v) return false;
                out.push_back(v);
            } else {
                llvm::Value* v = emitExprConverted(a, paramT);
                if (!v) return false;
                trackOwnedArgTemp(v, a, paramT);
                out.push_back(v);
            }
        }
        if (!sym || !sym->funcDeclCst) return true;
        auto rootNode = SyntaxNode::makeRoot(sym->funcDeclCst);
        auto fn = ast::FuncDecl::cast(*rootNode);
        if (!fn) return true;
        auto params = fn->parameters();
        for (size_t i = userArgs.size(); i < params.size(); ++i) {
            auto dv = params[i].defaultValue();
            if (!dv) return false;
            auto expr = dv->expression();
            if (!expr) return false;
            ::Type* paramT = (i < sym->paramTypes.size()) ? sym->paramTypes[i] : nullptr;
            if (sym && paramIsByPointer(sym, i)) {
                llvm::Value* v = emitAddressForByPointerArg(*expr, paramT);
                if (!v) return false;
                out.push_back(v);
            } else {
                llvm::Value* v = emitExprConverted(*expr, paramT);
                if (!v) return false;
                out.push_back(v);
            }
        }
        return true;
    }

    llvm::Value* emitNew(const ast::NewExpression& e) {
        ::Type* t = typeOf(e.node);
        if (e.isArrayNew()) {
            if (!t || !t->isArray() || !t->inner) {
                error(e.node.startOffset(), "Internal: array-'new' has no resolved array type");
                return nullptr;
            }
            auto sizes = e.arraySizeExpressions();
            if (sizes.empty()) {
                error(e.node.startOffset(), "Internal: array-'new' missing size expression");
                return nullptr;
            }
            llvm::Type* i64 = llvm::Type::getInt64Ty(ctx);
            std::vector<llvm::Value*> sizeVals;
            sizeVals.reserve(sizes.size());
            for (auto& sz : sizes) {
                llvm::Value* v = emitExpr(sz);
                if (!v) return nullptr;
                if (v->getType() != i64) {
                    v = builder->CreateSExtOrTrunc(v, i64, "arr.size.i64");
                }
                sizeVals.push_back(v);
            }
            // The top-level array type wraps the element once per size bracket.
            // Each level's slot-element-type is the inner-of-this-level.
            return emitMultiDimNew(t->inner, sizeVals, 0, e.node.startOffset());
        }
        if (!t || !t->structInfo) {
            error(e.node.startOffset(), "Internal: 'new' has no resolved class type");
            return nullptr;
        }
        llvm::StructType* layout = mapStructType(t);
        const llvm::DataLayout& dl = module->getDataLayout();
        uint64_t sizeBytes = dl.getTypeAllocSize(layout);

        auto* allocFn = getOrDefineEnsAlloc();
        llvm::Value* sizeArg = llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx), sizeBytes);
        llvm::Value* dtorArg = getOrEmitClassDtor(t);
        llvm::Value* descArg = getOrEmitTypeDescriptor(t->structInfo);
        llvm::Value* heapPtr = builder->CreateCall(allocFn, {sizeArg, dtorArg, descArg},
                                                    "new." + asAscii(t->structInfo->name));

        int ctorIdx = t->structInfo->findMethodIndex(t->structInfo->name);
        if (ctorIdx >= 0) {
            Symbol* ctorSym = t->structInfo->methods[ctorIdx].symbol;
            llvm::Function* fn = getOrDeclareExternalFunction(ctorSym, t);
            if (fn) {
                std::vector<llvm::Value*> args;
                args.reserve(e.arguments().size() + 1);
                args.push_back(heapPtr);
                if (!appendCallArgs(ctorSym, e.arguments(), args)) return nullptr;
                builder->CreateCall(fn, args);
            }
        } else {
            // No own constructor: run the nearest inherited zero-argument constructor.
            for (StructInfo* base = t->structInfo->baseInfo; base; base = base->baseInfo) {
                int bidx = base->findMethodIndex(base->name);
                if (bidx < 0) continue;
                Symbol* baseCtor = base->methods[bidx].symbol;
                if (baseCtor && baseCtor->paramTypes.empty()) {
                    if (llvm::Function* bfn = getOrDeclareExternalFunction(baseCtor, nullptr))
                        builder->CreateCall(bfn, { heapPtr });
                }
                break;
            }
        }
        return heapPtr;
    }

    llvm::Value* emitBuiltinCall(Symbol* sym, const ast::CallExpression& e) {
        std::string name = asAscii(sym->name);
        auto args = e.arguments();
        if (name == "print") {
            if (args.size() != 1) {
                error(e.node.startOffset(), "print expects exactly 1 argument");
                return nullptr;
            }
            llvm::Value* arg = emitExpr(args[0]);
            if (!arg) return nullptr;
            auto* puts = getOrDeclarePuts();
            builder->CreateCall(puts, {emitStringDataPtr(arg)});
            releaseIfOwnedTemp(arg, args[0]);
            return nullptr;
        }
        error(e.node.startOffset(), "Unknown builtin '" + name + "'");
        return nullptr;
    }

    llvm::Value* emitCall(const ast::CallExpression& e) {
        auto callee = e.callee();

        // Base-constructor chaining: super(args).
        if (callee && callee->asSuper()) {
            Symbol* ctorSym = methodSymbolOf(callee->node);
            if (!ctorSym) return nullptr;  // base has no constructor: nothing to call
            llvm::Value* thisPtr = emitSuper(*callee->asSuper());
            llvm::Function* fn = getOrDeclareExternalFunction(ctorSym, nullptr);
            if (!thisPtr || !fn) return nullptr;
            std::vector<llvm::Value*> args;
            args.push_back(thisPtr);
            if (!appendCallArgs(ctorSym, e.arguments(), args)) return nullptr;
            builder->CreateCall(fn, args);
            return nullptr;
        }

        if (callee && callee->asMember()) {
            auto member = *callee->asMember();
            Symbol* methodSym = methodSymbolOf(member.node);
            if (methodSym && isInterceptedTraceMethod(methodSym)) {
                auto obj = member.object();
                if (!obj) return nullptr;
                ::Type* objType = typeOf(obj->node);
                llvm::Value* recv = isReferenceType(objType) ? emitExpr(*obj) : emitLValue(*obj);
                if (!recv) return nullptr;
                trackOwnedArgTemp(recv, *obj, objType);
                auto* ptrTy = llvm::PointerType::get(ctx, 0);
                llvm::Value* frames = builder->CreateLoad(ptrTy,
                    builder->CreateGEP(llvm::Type::getInt8Ty(ctx), recv,
                        llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx), 8)), "frames");
                return builder->CreateCall(
                    methodSym->name == u"getStackTrace" ? formatTraceFn() : symbolicateFn(),
                    { frames }, "trace.result");
            }
            if (methodSym) {
                auto obj = member.object();
                if (!obj) return nullptr;
                ::Type* objType = typeOf(obj->node);
                bool isSuper = obj->asSuper().has_value();
                // A virtual call dispatches through the vtable, except `super.m()` which is
                // always a direct call to the inherited implementation.
                int vslot = -1;
                if (!isSuper && objType && objType->structInfo) {
                    if (StructInfo* decl = objType->structInfo->classDeclaringMethod(methodSym->name))
                        vslot = decl->methods[decl->findMethodIndex(methodSym->name)].vtableSlot;
                }
                llvm::Function* fn = getOrDeclareExternalFunction(methodSym, objType);
                if (!fn) {
                    error(e.node.startOffset(), "Internal: method has no LLVM function");
                    return nullptr;
                }
                llvm::Value* receiver = isReferenceType(objType)
                    ? emitExpr(*obj)
                    : emitLValue(*obj);
                if (!receiver) return nullptr;
                trackOwnedArgTemp(receiver, *obj, objType);
                std::vector<llvm::Value*> args;
                args.push_back(receiver);
                if (!appendCallArgs(methodSym, e.arguments(), args)) return nullptr;
                if (methodSym->abiThrows && throwTargetSlot) args.push_back(throwTargetSlot);
                llvm::Value* result = (vslot >= 0)
                    ? builder->CreateCall(fn->getFunctionType(), loadVtableSlot(receiver, vslot), args)
                    : builder->CreateCall(fn, args);
                emitThrowsCheck(methodSym);
                return result;
            }

            // Namespace-qualified free-function call: ns.func(args), lowered as a direct
            // call with no receiver (the namespace symbol carries no runtime value).
            if (Symbol* fnSym = symbolOf(member.node)) {
                if (fnSym->kind == SymbolKind::Function) {
                    if (fnSym->isBuiltin) return emitBuiltinCall(fnSym, e);
                    if (fnSym->isExternal) return emitForeignCall(fnSym, e);
                    llvm::Function* fn = getOrDeclareExternalFunction(fnSym, /*receiver*/ nullptr);
                    if (!fn) {
                        error(e.node.startOffset(), "Internal: namespace callee has no LLVM function");
                        return nullptr;
                    }
                    std::vector<llvm::Value*> args;
                    if (!appendCallArgs(fnSym, e.arguments(), args)) return nullptr;
                    if (fnSym->abiThrows && throwTargetSlot) args.push_back(throwTargetSlot);
                    llvm::Value* result = builder->CreateCall(fn, args);
                    emitThrowsCheck(fnSym);
                    return result;
                }
            }
        }

        if (callee && callee->asSafeMember()) {
            auto member = *callee->asSafeMember();
            Symbol* methodSym = methodSymbolOf(member.node);
            if (methodSym) {
                auto obj = member.object();
                if (!obj) return nullptr;
                ::Type* recvType = typeOf(obj->node);
                if (!recvType || !recvType->isOptional() || !recvType->inner) {
                    error(e.node.startOffset(), "Internal: safe-call receiver type is malformed");
                    return nullptr;
                }
                ::Type* innerType = recvType->inner;
                int vslot = -1;
                if (innerType && innerType->structInfo) {
                    if (StructInfo* decl = innerType->structInfo->classDeclaringMethod(methodSym->name))
                        vslot = decl->methods[decl->findMethodIndex(methodSym->name)].vtableSlot;
                }
                llvm::Function* fn = getOrDeclareExternalFunction(methodSym, innerType);
                if (!fn) {
                    error(e.node.startOffset(), "Internal: method has no LLVM function");
                    return nullptr;
                }
                llvm::Value* recv = emitExpr(*obj);
                if (!recv) return nullptr;
                auto* ptrTy = llvm::PointerType::get(ctx, 0);
                llvm::Value* isNull = builder->CreateICmpEQ(
                    recv, llvm::ConstantPointerNull::get(ptrTy), "safecall.isnull");

                auto* nullBB    = llvm::BasicBlock::Create(ctx, "safecall.null",    currentFunction);
                auto* nonnullBB = llvm::BasicBlock::Create(ctx, "safecall.nonnull", currentFunction);
                auto* endBB     = llvm::BasicBlock::Create(ctx, "safecall.end",     currentFunction);

                builder->CreateCondBr(isNull, nullBB, nonnullBB);

                builder->SetInsertPoint(nonnullBB);
                std::vector<llvm::Value*> args;
                args.push_back(recv);
                if (!appendCallArgs(methodSym, e.arguments(), args)) return nullptr;
                if (methodSym->abiThrows && throwTargetSlot) args.push_back(throwTargetSlot);
                llvm::Value* callRes = (vslot >= 0)
                    ? builder->CreateCall(fn->getFunctionType(), loadVtableSlot(recv, vslot), args)
                    : builder->CreateCall(fn, args);
                emitThrowsCheck(methodSym);
                llvm::BasicBlock* nonnullEnd = builder->GetInsertBlock();
                builder->CreateBr(endBB);

                builder->SetInsertPoint(nullBB);
                llvm::Value* nullVal = llvm::ConstantPointerNull::get(ptrTy);
                llvm::BasicBlock* nullEnd = builder->GetInsertBlock();
                builder->CreateBr(endBB);

                builder->SetInsertPoint(endBB);
                auto* phi = builder->CreatePHI(ptrTy, 2, "safecall.result");
                phi->addIncoming(callRes, nonnullEnd);
                phi->addIncoming(nullVal, nullEnd);
                return phi;
            }
        }

        auto idCallee = callee ? callee->asIdent() : std::nullopt;
        if (!idCallee) {
            error(e.node.startOffset(), "Only direct function calls are supported");
            return nullptr;
        }
        Symbol* sym = symbolOf(idCallee->node);
        if (!sym) {
            error(e.node.startOffset(), "Internal: callee has no resolved symbol");
            return nullptr;
        }
        if (sym->isBuiltin) return emitBuiltinCall(sym, e);
        if (sym->isExternal) return emitForeignCall(sym, e);
        llvm::Function* fn = getOrDeclareExternalFunction(sym, /*receiver*/ nullptr);
        if (!fn) {
            error(e.node.startOffset(), "Internal: callee has no LLVM function");
            return nullptr;
        }
        std::vector<llvm::Value*> args;
        if (!appendCallArgs(sym, e.arguments(), args)) return nullptr;
        if (sym->abiThrows && throwTargetSlot) args.push_back(throwTargetSlot);
        llvm::Value* result = builder->CreateCall(fn, args);
        emitThrowsCheck(sym);
        return result;
    }

    llvm::Value* emitForeignCall(Symbol* sym, const ast::CallExpression& e) {
        llvm::Function* fn = getOrDeclareExternalFunction(sym, /*receiver*/ nullptr);
        if (!fn) {
            error(e.node.startOffset(), "Internal: external callee has no LLVM function");
            return nullptr;
        }
        auto userArgs = e.arguments();
        std::vector<llvm::Value*> args;
        args.reserve(userArgs.size());
        size_t n = std::min(userArgs.size(), sym->paramTypes.size());
        for (size_t i = 0; i < n; ++i) {
            bool isOut = i < sym->paramIsOut.size() && sym->paramIsOut[i];
            auto& userArg = userArgs[i];
            if (isOut) {
                auto outArg = userArg.asOutArgument();
                if (!outArg) {
                    error(userArg.node.startOffset(), "Internal: expected 'out' argument");
                    return nullptr;
                }
                auto identTok = outArg->identifier();
                if (!identTok) {
                    error(userArg.node.startOffset(), "Internal: 'out' argument missing identifier");
                    return nullptr;
                }
                Symbol* local = symbolOf(*identTok);
                if (!local) {
                    error(userArg.node.startOffset(), "Internal: 'out' identifier has no symbol");
                    return nullptr;
                }
                auto it = values.find(local);
                if (it == values.end()) {
                    error(userArg.node.startOffset(),
                          "'out' local '" + asAscii(local->name) + "' is not initialized before this call.");
                    return nullptr;
                }
                args.push_back(it->second);
            } else {
                ::Type* paramT = sym->paramTypes[i];
                ::Type* convertTo = (paramT && (paramT->isInteger() || paramT->isFloat()))
                    ? paramT : nullptr;
                llvm::Value* v = convertTo ? emitExprConverted(userArg, convertTo)
                                           : emitExpr(userArg);
                if (!v) return nullptr;
                ::Type* baseT = (paramT && paramT->isOptional()) ? paramT->inner : paramT;
                if (baseT && baseT->isArray()) {
                    v = emitArrayDataPtr(v);
                } else if (baseT && baseT->isString()) {
                    v = emitStringDataPtr(v);
                }
                args.push_back(v);
            }
        }
        llvm::Value* result = builder->CreateCall(fn, args);
        // A C function that returns a string hands back a raw char*; wrap it in
        // an Ens string object so ARC and string operations see a real object.
        ::Type* retT = sym->returnType;
        ::Type* retBase = (retT && retT->isOptional()) ? retT->inner : retT;
        if (retBase && retBase->isString()) {
            return builder->CreateCall(getOrDefineEnsStringFromCStr(), { result });
        }
        return result;
    }

    const FieldInfo* targetFieldInfo(const ast::Expression& target) const {
        auto m = target.asMember();
        if (!m) return nullptr;
        auto obj = m->object();
        if (!obj) return nullptr;
        ::Type* objType = typeOf(obj->node);
        if (!objType || !objType->structInfo) return nullptr;
        auto memberName = m->memberText();
        if (!memberName) return nullptr;
        int idx = objType->structInfo->findFieldIndex(*memberName);
        if (idx < 0) return nullptr;
        return &objType->structInfo->fields[static_cast<size_t>(idx)];
    }

    llvm::Value* emitAssign(const ast::AssignExpression& e) {
        auto opTok = e.operatorToken();
        if (!opTok || opTok->kind() != SyntaxKind::Eq) {
            error(e.node.startOffset(), "Compound assignment not yet supported in codegen");
            return nullptr;
        }
        auto target = e.target();
        auto value = e.value();
        if (!target || !value) return nullptr;

        ::Type* targetType = typeOf(target->node);
        bool isClass = isReferenceType(targetType);
        bool isStructWithClass = structHasClassFields(targetType);
        bool borrowedSource = !expressionProducesOwnedRef(*value);

        const FieldInfo* fi = targetFieldInfo(*target);
        bool isWeakField = fi && fi->isWeak;

        Symbol* targetIdentSym = nullptr;
        if (auto id = target->asIdent()) targetIdentSym = symbolOf(id->node);
        bool isBorrowModeTarget = isClass && isClassBorrowMode(targetIdentSym);

        Symbol* moveSrc = (isClass && !isBorrowModeTarget && !isWeakField)
            ? moveSourceSymbol(*value) : nullptr;

        llvm::Value* lv = emitLValue(*target);
        if (!lv) return nullptr;
        llvm::Value* val = emitExprConverted(*value, targetType);
        if (!val) return nullptr;

        if (isWeakField) {
            auto* ptrTy = llvm::PointerType::get(ctx, 0);
            llvm::Value* newSt = emitWeakInit(val);
            llvm::Value* oldSt = builder->CreateLoad(ptrTy, lv);
            emitWeakRelease(oldSt);
            builder->CreateStore(newSt, lv);
            // If RHS produced a fresh +1 (e.g., new T() or a call), release it. Weak doesn't retain strong ownership.
            if (!borrowedSource) {
                emitRelease(val);
            }
        } else if (isBorrowModeTarget) {
            builder->CreateStore(val, lv);
        } else if (moveSrc) {
            auto* ptrTy = llvm::PointerType::get(ctx, 0);
            auto it = values.find(moveSrc);
            if (it != values.end()) {
                builder->CreateStore(llvm::ConstantPointerNull::get(ptrTy), it->second);
            }
            llvm::Value* old = builder->CreateLoad(ptrTy, lv);
            emitRelease(old);
            builder->CreateStore(val, lv);
        } else if (isClass) {
            if (borrowedSource) {
                emitRetain(val);
            }
            auto* ptrTy = llvm::PointerType::get(ctx, 0);
            llvm::Value* old = builder->CreateLoad(ptrTy, lv);
            emitRelease(old);
            builder->CreateStore(val, lv);
        } else if (isStructWithClass) {
            emitStructFieldRelease(targetType, lv);
            builder->CreateStore(val, lv);
            if (borrowedSource) {
                emitStructFieldRetain(targetType, lv);
            }
        } else {
            builder->CreateStore(val, lv);
        }
        return val;
    }

    llvm::Value* emitLValue(const ast::Expression& e) {
        if (auto id = e.asIdent()) {
            Symbol* sym = symbolOf(id->node);
            if (!sym) return nullptr;
            auto it = values.find(sym);
            if (it == values.end()) return nullptr;
            if (byPointerParams.count(sym)) {
                return builder->CreateLoad(llvm::PointerType::get(ctx, 0), it->second,
                                           asAscii(sym->name) + ".byptr");
            }
            return it->second;
        }
        if (auto th = e.asThis()) {
            Symbol* sym = symbolOf(th->node);
            if (!sym) return nullptr;
            auto it = values.find(sym);
            if (it == values.end()) return nullptr;
            return builder->CreateLoad(llvm::PointerType::get(ctx, 0), it->second, "this");
        }
        if (auto m = e.asMember()) {
            auto obj = m->object();
            if (!obj) return nullptr;
            ::Type* objType = typeOf(obj->node);
            if (!objType || !objType->hasRecordLayout() || !objType->structInfo) {
                error(e.node.startOffset(), "Cannot take address of member on non-record type");
                return nullptr;
            }
            llvm::Value* objAddr = isReferenceType(objType)
                ? emitExpr(*obj)
                : emitLValue(*obj);
            if (!objAddr) return nullptr;
            auto memberName = m->memberText();
            if (!memberName) return nullptr;
            int idx = objType->structInfo->findFieldIndex(*memberName);
            if (idx < 0) {
                error(e.node.startOffset(), "Internal: field not found in struct");
                return nullptr;
            }
            llvm::StructType* st = mapStructType(objType);
            return builder->CreateStructGEP(st, objAddr, static_cast<unsigned>(idx),
                                            asAscii(*memberName) + ".addr");
        }
        if (auto su = e.asSubscript()) {
            auto obj = su->object();
            auto idx = su->index();
            if (!obj || !idx) return nullptr;
            ::Type* objType = typeOf(obj->node);
            if (!objType || !objType->isArray() || !objType->inner) {
                error(e.node.startOffset(), "Internal: subscript receiver is not an array");
                return nullptr;
            }
            llvm::Value* arrPtr = emitExpr(*obj);
            llvm::Value* idxVal = emitExpr(*idx);
            if (!arrPtr || !idxVal) return nullptr;
            return emitArraySubscriptAddr(arrPtr, idxVal, objType->inner);
        }
        error(e.node.startOffset(), "Cannot get address of this expression");
        return nullptr;
    }

    llvm::Value* emitMember(const ast::MemberExpression& e) {
        auto obj = e.object();
        ::Type* objType = obj ? typeOf(obj->node) : nullptr;
        if (objType && objType->isArray()) {
            auto memberName = e.memberText().value_or(std::u16string{});
            if (memberName == u"length") {
                llvm::Value* arrPtr = emitExpr(*obj);
                if (!arrPtr) return nullptr;
                return emitArrayLength(arrPtr);
            }
            error(e.node.startOffset(), "Internal: unsupported array member '" +
                  asAscii(memberName) + "'");
            return nullptr;
        }
        if (objType && objType->isString()) {
            auto memberName = e.memberText().value_or(std::u16string{});
            if (memberName == u"length") {
                llvm::Value* strPtr = emitExpr(*obj);
                if (!strPtr) return nullptr;
                return emitStringLength(strPtr);
            }
            error(e.node.startOffset(), "Internal: unsupported string member '" +
                  asAscii(memberName) + "'");
            return nullptr;
        }
        ast::Expression wrapper{e.node};
        llvm::Value* addr = emitLValue(wrapper);
        if (!addr) return nullptr;
        const FieldInfo* fi = memberFieldInfo(e);
        auto memberName = e.memberText().value_or(std::u16string{});
        if (fi && fi->isWeak) {
            auto* ptrTy = llvm::PointerType::get(ctx, 0);
            llvm::Value* stPtr = builder->CreateLoad(ptrTy, addr, asAscii(memberName) + ".st");
            return builder->CreateCall(getOrDefineEnsWeakLoad(), { stPtr });
        }
        ::Type* fieldType = typeOf(e.node);
        return builder->CreateLoad(mapType(fieldType), addr, asAscii(memberName) + ".load");
    }

    llvm::Value* emitSubscript(const ast::SubscriptExpression& e) {
        auto obj = e.object();
        auto idx = e.index();
        if (!obj || !idx) return nullptr;
        ::Type* objType = typeOf(obj->node);
        if (!objType || !objType->isArray() || !objType->inner) {
            error(e.node.startOffset(), "Internal: subscript receiver is not an array");
            return nullptr;
        }
        llvm::Value* arrPtr = emitExpr(*obj);
        llvm::Value* idxVal = emitExpr(*idx);
        if (!arrPtr || !idxVal) return nullptr;
        llvm::Value* slot = emitArraySubscriptAddr(arrPtr, idxVal, objType->inner);
        return builder->CreateLoad(mapType(objType->inner), slot, "arr.elem");
    }

    llvm::Value* emitSafeSubscript(const ast::SafeSubscriptExpression& e) {
        auto obj = e.object();
        auto idx = e.index();
        if (!obj || !idx) return nullptr;
        ::Type* recvType = typeOf(obj->node);
        if (!recvType || !recvType->isOptional() || !recvType->inner ||
            !recvType->inner->isArray() || !recvType->inner->inner) {
            error(e.node.startOffset(), "Internal: safe subscript receiver is not a nullable array");
            return nullptr;
        }
        ::Type* arrType = recvType->inner;
        ::Type* elemType = arrType->inner;

        llvm::Value* recv = emitExpr(*obj);
        if (!recv) return nullptr;
        auto* ptrTy = llvm::PointerType::get(ctx, 0);
        llvm::Value* isNull = builder->CreateICmpEQ(
            recv, llvm::ConstantPointerNull::get(ptrTy), "safesub.isnull");

        auto* nullBB    = llvm::BasicBlock::Create(ctx, "safesub.null",    currentFunction);
        auto* nonnullBB = llvm::BasicBlock::Create(ctx, "safesub.nonnull", currentFunction);
        auto* endBB     = llvm::BasicBlock::Create(ctx, "safesub.end",     currentFunction);

        builder->CreateCondBr(isNull, nullBB, nonnullBB);

        builder->SetInsertPoint(nonnullBB);
        llvm::Value* idxVal = emitExpr(*idx);
        if (!idxVal) return nullptr;
        llvm::Value* slot = emitArraySubscriptAddr(recv, idxVal, elemType);
        llvm::Value* loaded = builder->CreateLoad(mapType(elemType), slot, "safesub.elem");
        if (isReferenceType(elemType)) emitRetain(loaded);
        llvm::BasicBlock* nonnullEnd = builder->GetInsertBlock();
        builder->CreateBr(endBB);

        builder->SetInsertPoint(nullBB);
        llvm::Value* nullVal = llvm::ConstantPointerNull::get(ptrTy);
        llvm::BasicBlock* nullEnd = builder->GetInsertBlock();
        builder->CreateBr(endBB);

        builder->SetInsertPoint(endBB);
        auto* phi = builder->CreatePHI(ptrTy, 2, "safesub.result");
        phi->addIncoming(loaded, nonnullEnd);
        phi->addIncoming(nullVal, nullEnd);
        return phi;
    }

    llvm::Value* emitSafeMember(const ast::SafeMemberExpression& e) {
        auto obj = e.object();
        if (!obj) return nullptr;
        ::Type* recvType = typeOf(obj->node);
        if (!recvType || !recvType->isOptional() || !recvType->inner ||
            !recvType->inner->structInfo) {
            error(e.node.startOffset(), "Internal: safe member receiver type is malformed");
            return nullptr;
        }
        ::Type* innerType = recvType->inner;
        auto memberName = e.memberText();
        if (!memberName) return nullptr;
        int idx = innerType->structInfo->findFieldIndex(*memberName);
        if (idx < 0) {
            error(e.node.startOffset(), "Internal: safe member field not found");
            return nullptr;
        }
        ::Type* fieldType = innerType->structInfo->fields[static_cast<size_t>(idx)].type;

        llvm::Value* recv = emitExpr(*obj);
        if (!recv) return nullptr;
        auto* ptrTy = llvm::PointerType::get(ctx, 0);
        llvm::Value* isNull = builder->CreateICmpEQ(
            recv, llvm::ConstantPointerNull::get(ptrTy), "safe.isnull");

        auto* nullBB    = llvm::BasicBlock::Create(ctx, "safe.null",    currentFunction);
        auto* nonnullBB = llvm::BasicBlock::Create(ctx, "safe.nonnull", currentFunction);
        auto* endBB     = llvm::BasicBlock::Create(ctx, "safe.end",     currentFunction);

        builder->CreateCondBr(isNull, nullBB, nonnullBB);

        builder->SetInsertPoint(nonnullBB);
        llvm::StructType* layout = mapStructType(innerType);
        llvm::Value* fieldAddr = builder->CreateStructGEP(
            layout, recv, static_cast<unsigned>(idx), asAscii(*memberName) + ".addr");
        llvm::Value* loaded = builder->CreateLoad(
            mapType(fieldType), fieldAddr, asAscii(*memberName) + ".load");
        if (isReferenceType(fieldType)) emitRetain(loaded);
        llvm::BasicBlock* nonnullEnd = builder->GetInsertBlock();
        builder->CreateBr(endBB);

        builder->SetInsertPoint(nullBB);
        llvm::Value* nullVal = llvm::ConstantPointerNull::get(ptrTy);
        llvm::BasicBlock* nullEnd = builder->GetInsertBlock();
        builder->CreateBr(endBB);

        builder->SetInsertPoint(endBB);
        auto* phi = builder->CreatePHI(ptrTy, 2, "safe.result");
        phi->addIncoming(loaded, nonnullEnd);
        phi->addIncoming(nullVal, nullEnd);
        return phi;
    }

    llvm::Value* emitTernary(const ast::TernaryExpression& e) {
        auto cond = e.condition();
        auto thenE = e.thenBranch();
        auto elseE = e.elseBranch();
        if (!cond || !thenE || !elseE) return nullptr;
        llvm::Value* condV = emitExpr(*cond);
        if (!condV) return nullptr;

        ::Type* resultType = typeOf(e.node);

        auto* thenBB  = llvm::BasicBlock::Create(ctx, "tern.then", currentFunction);
        auto* elseBB  = llvm::BasicBlock::Create(ctx, "tern.else", currentFunction);
        auto* mergeBB = llvm::BasicBlock::Create(ctx, "tern.end",  currentFunction);
        builder->CreateCondBr(condV, thenBB, elseBB);

        builder->SetInsertPoint(thenBB);
        llvm::Value* thenV = emitExprConverted(*thenE, resultType);
        llvm::BasicBlock* thenEnd = builder->GetInsertBlock();
        if (thenV && !thenEnd->getTerminator()) builder->CreateBr(mergeBB);

        builder->SetInsertPoint(elseBB);
        llvm::Value* elseV = emitExprConverted(*elseE, resultType);
        llvm::BasicBlock* elseEnd = builder->GetInsertBlock();
        if (elseV && !elseEnd->getTerminator()) builder->CreateBr(mergeBB);

        builder->SetInsertPoint(mergeBB);
        if (!thenV || !elseV) return nullptr;
        if (thenV->getType() != elseV->getType()) {
            error(e.node.startOffset(), "Internal: ternary branches did not unify to a common type");
            return nullptr;
        }
        auto* phi = builder->CreatePHI(thenV->getType(), 2);
        phi->addIncoming(thenV, thenEnd);
        phi->addIncoming(elseV, elseEnd);
        return phi;
    }

    bool generate(const SyntaxNode& root) {
        if (!initializeTargetMachine()) return false;
        auto sf = ast::SourceFile::cast(root);
        if (!sf) {
            error(0, "Internal: root is not a SourceFile node");
            return false;
        }

        auto eachDecl = [&](auto&& visit) {
            for (auto& fn : sf->functions()) visit(fn);
            for (auto& sd : sf->structs())   for (auto& m : sd.methods()) visit(m);
            for (auto& cd : sf->classes())   for (auto& m : cd.methods()) visit(m);
        };
        eachDecl([&](const ast::FuncDecl& fn) { declareFunction(fn); });
        eachDecl([&](const ast::FuncDecl& fn) { emitFunction(fn); });

        // Define a descriptor for every class declared here, even one only ever
        // thrown or caught (e.g. the prelude's Error), so its definition exists.
        for (auto& cd : sf->classes()) {
            if (Type* t = analysis.typeOf(cd.node.greenNode())) {
                if (t->structInfo) getOrEmitTypeDescriptor(t->structInfo);
                if (t->structInfo && t->structInfo->name == u"StackFrame" && isPreludeModule())
                    stackFrameType = t;
            }
        }

        // A throwing `main` was renamed to `ens.main`; emit the real entry point.
        for (auto& fn : sf->functions()) {
            if (fn.nameText().value_or(std::u16string{}) != u"main") continue;
            Symbol* msym = symbolOf(fn.node);
            if (msym && msym->abiThrows) emitMainWrapper(msym);
            break;
        }

        builder->SetCurrentDebugLocation(llvm::DebugLoc());
        emitCallSiteLineTable();
        if (isPreludeModule()) definePreludeRuntime();
        emitSymtabRegistration();

        // The unwinder reads .eh_frame; force it even on the nounwind runtime so a
        // capture can walk through every frame.
        for (auto& F : *module)
            if (!F.isDeclaration()) F.setUWTableKind(llvm::UWTableKind::Async);

        if (debugEnabled && diBuilder) diBuilder->finalize();
        if (!diagnostics.empty()) return false;

        std::string verifyErr;
        llvm::raw_string_ostream rso(verifyErr);
        if (llvm::verifyModule(*module, &rso)) {
            error(0, "Internal: generated module failed verification: " + verifyErr);
            return false;
        }
        return true;
    }

    void print(std::ostream& os) const {
        std::string text;
        llvm::raw_string_ostream rso(text);
        module->print(rso, nullptr);
        os << text;
    }

    bool emitObjectFile(const std::string& path) {
        if (!targetMachine && !initializeTargetMachine()) return false;
        std::error_code ec;
        llvm::raw_fd_ostream dest(path, ec, llvm::sys::fs::OF_None);
        if (ec) {
            error(0, "Could not open '" + path + "' for writing: " + ec.message());
            return false;
        }
        llvm::legacy::PassManager pass;
        if (targetMachine->addPassesToEmitFile(pass, dest, nullptr, llvm::CodeGenFileType::ObjectFile)) {
            error(0, "Target does not support object-file emission");
            return false;
        }
        pass.run(*module);
        dest.flush();
        return true;
    }
};

CodeGenerator::CodeGenerator(std::string moduleName,
                                   std::string sourceFilename,
                                   const SourceFile& src,
                                   const AnalysisResult& analysis,
                                   std::u16string modulePath,
                                   std::string targetTriple)
    : impl(std::make_unique<Impl>(std::move(moduleName), std::move(sourceFilename), src, analysis,
                                  std::move(modulePath), std::move(targetTriple))) {}

CodeGenerator::~CodeGenerator() = default;

bool CodeGenerator::generate(const SyntaxNode& root) { return impl->generate(root); }
void CodeGenerator::print(std::ostream& os) const    { impl->print(os); }
bool CodeGenerator::emitObjectFile(const std::string& path) { return impl->emitObjectFile(path); }
bool CodeGenerator::hasErrors() const                { return !impl->diagnostics.empty(); }
const std::vector<Diagnostic>& CodeGenerator::getDiagnostics() const { return impl->diagnostics; }
