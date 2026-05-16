#include "CodeGenerator.h"
#include "ast/Declaration.h"
#include "ast/Expression.h"
#include "ast/Statement.h"
#include "ast/TypeReference.h"
#include "semantic/Symbol.h"
#include "semantic/Type.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IR/DIBuilder.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/LegacyPassManager.h"
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
    llvm::Function* currentFunction = nullptr;
    llvm::DIScope* currentDIScope = nullptr;
    llvm::DICompileUnit* diCU = nullptr;
    llvm::DIFile* diFile = nullptr;
    bool debugEnabled = true;
    std::vector<Diagnostic> diagnostics;

    struct OwnedLocal {
        llvm::Value* alloca;
        ::Type* type;
    };
    std::vector<std::vector<OwnedLocal>> cleanupStack;
    std::unordered_set<Symbol*> byPointerParams;

    Impl(std::string mn, std::string sf, const SourceFile& src, const AnalysisResult& an)
        : moduleName(std::move(mn)), sourceFilename(std::move(sf)),
          sourceFile(src), analysis(an) {
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
                // Nullable class types are supported (represented as a pointer).
                return !(t->inner && t->inner->isClass());
            default:
                return false;
        }
    }

    bool isReferenceType(::Type* t) {
        if (!t) return false;
        if (t->kind == TypeKind::Class) return true;
        if (t->kind == TypeKind::Optional && t->inner && t->inner->isClass()) return true;
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
            case TypeKind::Optional:
                if (t->inner && t->inner->isClass()) return llvm::PointerType::get(ctx, 0);
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

    bool isSigned(::Type* t) {
        if (!t) return false;
        switch (t->kind) {
            case TypeKind::Byte:
            case TypeKind::Short:
            case TypeKind::Int:
            case TypeKind::Long:
                return true;
            default:
                return false;
        }
    }

    llvm::DIType* mapDIType(::Type* t) {
        if (!debugEnabled || !diBuilder || !t) return nullptr;
        if (t->kind == TypeKind::Optional && t->inner && t->inner->isClass()) {
            return mapDIType(t->inner);
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
            case TypeKind::Byte:   result = diBuilder->createBasicType("byte",   8,  llvm::dwarf::DW_ATE_signed); break;
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

    bool initializeNativeTargetOnce() {
        static const bool ok = []() {
            llvm::InitializeNativeTarget();
            llvm::InitializeNativeTargetAsmPrinter();
            llvm::InitializeNativeTargetAsmParser();
            return true;
        }();
        return ok;
    }

    bool initializeTargetMachine() {
        if (targetMachine) return true;
        initializeNativeTargetOnce();
        std::string triple = llvm::sys::getDefaultTargetTriple();
        std::string lookupErr;
        const llvm::Target* target = llvm::TargetRegistry::lookupTarget(triple, lookupErr);
        if (!target) {
            error(0, "Failed to find target '" + triple + "': " + lookupErr);
            return false;
        }
        llvm::TargetOptions opts;
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

    llvm::Function* getOrDeclareExternalFunction(Symbol* sym, ::Type* receiver) {
        auto it = values.find(sym);
        if (it != values.end()) return llvm::cast<llvm::Function>(it->second);
        if (!sym) return nullptr;

        std::vector<llvm::Type*> paramTypes;
        if (receiver) paramTypes.push_back(llvm::PointerType::get(ctx, 0));
        for (size_t i = 0; i < sym->paramTypes.size(); ++i) {
            auto* pt = sym->paramTypes[i];
            if (isUnsupportedType(pt)) return nullptr;
            if (paramIsByPointer(sym, i)) {
                paramTypes.push_back(llvm::PointerType::get(ctx, 0));
            } else {
                paramTypes.push_back(mapType(pt));
            }
        }
        if (sym->returnType && !sym->returnType->isVoid() && isUnsupportedType(sym->returnType)) {
            return nullptr;
        }
        llvm::Type* retType = mapType(sym->returnType);
        auto* fnType = llvm::FunctionType::get(retType, paramTypes, false);

        std::string mangled = asAscii(sym->name);
        if (receiver && receiver->structInfo) {
            mangled = asAscii(receiver->structInfo->name) + "_" + mangled;
        }

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
        llvm::Type* retType = mapType(sym->returnType);
        auto* fnType = llvm::FunctionType::get(retType, paramTypes, false);

        std::string mangled = asAscii(fname);
        if (receiver && receiver->structInfo) {
            mangled = asAscii(receiver->structInfo->name) + "_" + mangled;
        }
        auto* func = llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, mangled, module.get());
        values[sym] = func;
    }

    void emitFunction(const ast::FuncDecl& fn) {
        Symbol* sym = symbolOf(fn.node);
        if (!sym) return;
        auto it = values.find(sym);
        if (it == values.end()) return;

        byPointerParams.clear();
        currentFunction = llvm::cast<llvm::Function>(it->second);
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
                    builder->CreateCall(getOrDefineEnsRetain(), { paramVal });
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
            else builder->CreateUnreachable();
        }
        cleanupStack.pop_back();

        if (debugEnabled && diBuilder && sp) diBuilder->finalizeSubprogram(sp);
        currentDIScope = prevScope;
        currentFunction = nullptr;
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

        bool elideClassRetain = false;
        if (auto init = s.initializer()) {
            elideClassRetain = classLetCanBorrow(sym, *init);
        }
        if (!elideClassRetain && isClassBorrowMode(sym)) {
            elideClassRetain = true;
        }
        if (!elideClassRetain) {
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
            bool needsStructRetain = structHasClassFields(sym->type) && borrowedSource;
            llvm::Value* v = emitExpr(*init);
            if (v) {
                setLocationFromNode(s.node);
                if (needsClassRetain) {
                    builder->CreateCall(getOrDefineEnsRetain(), { v });
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

        bool elideClassRetain = false;
        if (auto init = s.initializer()) {
            elideClassRetain = classLetCanBorrow(sym, *init);
        }
        if (!elideClassRetain) {
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
            bool needsStructRetain = structHasClassFields(sym->type) && borrowedSource;
            llvm::Value* v = emitExpr(*init);
            if (v) {
                if (needsClassRetain) {
                    builder->CreateCall(getOrDefineEnsRetain(), { v });
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
                                builder->CreateStore(v, fieldAddr);
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

            llvm::Value* val = emitExpr(*v);
            if (!val) { builder->CreateUnreachable(); return; }

            if (needsClassRetain) {
                builder->CreateCall(getOrDefineEnsRetain(), { val });
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
        if (auto b = e.asBinary()) return emitBinary(*b);
        if (auto p = e.asPrefix()) return emitPrefix(*p);
        if (auto c = e.asCall()) return emitCall(*c);
        if (auto m = e.asMember()) return emitMember(*m);
        if (auto a = e.asAssign()) return emitAssign(*a);
        if (auto t = e.asTernary()) return emitTernary(*t);
        if (auto n = e.asNew()) return emitNew(*n);
        if (auto pr = e.asParen()) {
            if (auto inner = pr->inner()) return emitExpr(*inner);
            return nullptr;
        }
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
                return llvm::ConstantInt::get(lt, static_cast<uint64_t>(v), isSigned(t));
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
                return builder->CreateGlobalString(utf8, ".str");
            }
            default:
                error(e.node.startOffset(), "Unsupported literal");
                return nullptr;
        }
    }

    llvm::Value* emitIdent(const ast::IdentExpression& e) {
        Symbol* sym = symbolOf(e.node);
        if (!sym) return nullptr;
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

    llvm::Value* emitBinary(const ast::BinaryExpression& e) {
        auto leftE = e.left();
        auto rightE = e.right();
        if (!leftE || !rightE) return nullptr;
        llvm::Value* L = emitExpr(*leftE);
        llvm::Value* R = emitExpr(*rightE);
        if (!L || !R) return nullptr;
        ::Type* leftType = typeOf(leftE->node);
        bool flt = leftType && leftType->isFloat();
        bool sgn = isSigned(leftType);
        auto opTok = e.operatorToken();
        SyntaxKind op = opTok ? opTok->kind() : SyntaxKind::Invalid;
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

    llvm::Function* getOrDeclarePuts() {
        if (auto* existing = module->getFunction("puts")) return existing;
        auto* ty = llvm::FunctionType::get(
            llvm::Type::getInt32Ty(ctx),
            { llvm::PointerType::get(ctx, 0) }, false);
        return llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "puts", module.get());
    }

    llvm::Function* getOrDeclareMalloc() {
        if (auto* existing = module->getFunction("malloc")) return existing;
        auto* ty = llvm::FunctionType::get(
            llvm::PointerType::get(ctx, 0),
            { llvm::Type::getInt64Ty(ctx) }, false);
        return llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "malloc", module.get());
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

    llvm::Function* getOrDefineEnsAlloc() {
        if (auto* existing = module->getFunction("ens_alloc")) return existing;
        auto* ptrTy = llvm::PointerType::get(ctx, 0);
        auto* i64Ty = llvm::Type::getInt64Ty(ctx);
        auto* fnTy = llvm::FunctionType::get(ptrTy, { i64Ty, ptrTy }, false);
        auto* fn = llvm::Function::Create(fnTy, llvm::Function::InternalLinkage, "ens_alloc", module.get());
        fn->addFnAttr(llvm::Attribute::NoUnwind);

        auto savedIP = builder->saveIP();
        auto* entry  = llvm::BasicBlock::Create(ctx, "entry", fn);
        auto* nullBB = llvm::BasicBlock::Create(ctx, "alloc.null", fn);
        auto* initBB = llvm::BasicBlock::Create(ctx, "alloc.init", fn);

        builder->SetInsertPoint(entry);
        llvm::Value* total = builder->CreateAdd(fn->getArg(0), llvm::ConstantInt::get(i64Ty, 24));
        llvm::Value* header = builder->CreateCall(getOrDeclareCalloc(),
            { llvm::ConstantInt::get(i64Ty, 1), total });
        llvm::Value* isNull = builder->CreateICmpEQ(header, llvm::ConstantPointerNull::get(ptrTy));
        builder->CreateCondBr(isNull, nullBB, initBB);

        builder->SetInsertPoint(nullBB);
        builder->CreateRet(llvm::ConstantPointerNull::get(ptrTy));

        builder->SetInsertPoint(initBB);
        builder->CreateStore(llvm::ConstantInt::get(i64Ty, 1), header);
        llvm::Value* dtorSlot = builder->CreateGEP(
            llvm::Type::getInt8Ty(ctx), header, llvm::ConstantInt::get(i64Ty, 8));
        builder->CreateStore(fn->getArg(1), dtorSlot);
        // side_table slot at offset 16 stays null (calloc-zeroed); lazily set by ens_weak_init.
        llvm::Value* payload = builder->CreateGEP(
            llvm::Type::getInt8Ty(ctx), header, llvm::ConstantInt::get(i64Ty, 24));
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
        auto* bumpBB = llvm::BasicBlock::Create(ctx, "retain.bump", fn);
        auto* doneBB = llvm::BasicBlock::Create(ctx, "retain.done", fn);

        builder->SetInsertPoint(entry);
        llvm::Value* obj = fn->getArg(0);
        llvm::Value* isNull = builder->CreateICmpEQ(obj, llvm::ConstantPointerNull::get(ptrTy));
        builder->CreateCondBr(isNull, doneBB, bumpBB);

        builder->SetInsertPoint(bumpBB);
        llvm::Value* header = builder->CreateGEP(
            llvm::Type::getInt8Ty(ctx), obj, llvm::ConstantInt::getSigned(i64Ty, -24));
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
        builder->CreateCondBr(isNull, doneBB, decBB);

        builder->SetInsertPoint(decBB);
        llvm::Value* header = builder->CreateGEP(i8Ty, obj, llvm::ConstantInt::getSigned(i64Ty, -24));
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
        builder->CreateCall(getOrDeclareFree(), { header });
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
        if (auto m = e.asMember()) {
            // Weak field reads return +1 via ens_weak_load's CAS-upgrade.
            const FieldInfo* fi = memberFieldInfo(*m);
            return fi && fi->isWeak;
        }
        if (auto p = e.asParen()) {
            if (auto inner = p->inner()) return expressionProducesOwnedRef(*inner);
        }
        return false;
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
            cleanupStack.back().push_back({ alloca, type });
        }
    }

    void emitFrameCleanup(const std::vector<OwnedLocal>& frame) {
        if (frame.empty()) return;
        auto* ptrTy = llvm::PointerType::get(ctx, 0);
        auto* releaseFn = getOrDefineEnsRelease();
        for (auto it = frame.rbegin(); it != frame.rend(); ++it) {
            if (isReferenceType(it->type)) {
                llvm::Value* val = builder->CreateLoad(ptrTy, it->alloca);
                builder->CreateCall(releaseFn, { val });
            } else if (structHasClassFields(it->type)) {
                emitStructFieldRelease(it->type, it->alloca);
            }
        }
    }

    void emitFullCleanup() {
        for (auto it = cleanupStack.rbegin(); it != cleanupStack.rend(); ++it) {
            emitFrameCleanup(*it);
        }
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

    bool appendCallArgs(Symbol* sym, const std::vector<ast::Expression>& userArgs,
                        std::vector<llvm::Value*>& out) {
        for (size_t i = 0; i < userArgs.size(); ++i) {
            auto& a = userArgs[i];
            if (sym && paramIsByPointer(sym, i)) {
                llvm::Value* v = emitAddressForByPointerArg(a, sym->paramTypes[i]);
                if (!v) return false;
                out.push_back(v);
            } else {
                llvm::Value* v = emitExpr(a);
                if (!v) return false;
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
            if (sym && paramIsByPointer(sym, i)) {
                llvm::Value* v = emitAddressForByPointerArg(*expr, sym->paramTypes[i]);
                if (!v) return false;
                out.push_back(v);
            } else {
                llvm::Value* v = emitExpr(*expr);
                if (!v) return false;
                out.push_back(v);
            }
        }
        return true;
    }

    llvm::Value* emitNew(const ast::NewExpression& e) {
        ::Type* t = typeOf(e.node);
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
        llvm::Value* heapPtr = builder->CreateCall(allocFn, {sizeArg, dtorArg},
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
            builder->CreateCall(puts, {arg});
            return nullptr;
        }
        error(e.node.startOffset(), "Unknown builtin '" + name + "'");
        return nullptr;
    }

    llvm::Value* emitCall(const ast::CallExpression& e) {
        auto callee = e.callee();
        if (callee && callee->asMember()) {
            auto member = *callee->asMember();
            Symbol* methodSym = methodSymbolOf(member.node);
            if (methodSym) {
                auto obj = member.object();
                if (!obj) return nullptr;
                ::Type* objType = typeOf(obj->node);
                llvm::Function* fn = getOrDeclareExternalFunction(methodSym, objType);
                if (!fn) {
                    error(e.node.startOffset(), "Internal: method has no LLVM function");
                    return nullptr;
                }
                llvm::Value* receiver = isReferenceType(objType)
                    ? emitExpr(*obj)
                    : emitLValue(*obj);
                if (!receiver) return nullptr;
                std::vector<llvm::Value*> args;
                args.push_back(receiver);
                if (!appendCallArgs(methodSym, e.arguments(), args)) return nullptr;
                return builder->CreateCall(fn, args);
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
        llvm::Function* fn = getOrDeclareExternalFunction(sym, /*receiver*/ nullptr);
        if (!fn) {
            error(e.node.startOffset(), "Internal: callee has no LLVM function");
            return nullptr;
        }
        std::vector<llvm::Value*> args;
        if (!appendCallArgs(sym, e.arguments(), args)) return nullptr;
        return builder->CreateCall(fn, args);
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
        llvm::Value* val = emitExpr(*value);
        if (!val) return nullptr;

        if (isWeakField) {
            auto* ptrTy = llvm::PointerType::get(ctx, 0);
            llvm::Value* newSt = builder->CreateCall(getOrDefineEnsWeakInit(), { val });
            llvm::Value* oldSt = builder->CreateLoad(ptrTy, lv);
            builder->CreateCall(getOrDefineEnsWeakRelease(), { oldSt });
            builder->CreateStore(newSt, lv);
            // If RHS produced a fresh +1 (e.g., new T() or a call), release it. Weak doesn't retain strong ownership.
            if (!borrowedSource) {
                builder->CreateCall(getOrDefineEnsRelease(), { val });
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
            builder->CreateCall(getOrDefineEnsRelease(), { old });
            builder->CreateStore(val, lv);
        } else if (isClass) {
            if (borrowedSource) {
                builder->CreateCall(getOrDefineEnsRetain(), { val });
            }
            auto* ptrTy = llvm::PointerType::get(ctx, 0);
            llvm::Value* old = builder->CreateLoad(ptrTy, lv);
            builder->CreateCall(getOrDefineEnsRelease(), { old });
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
        error(e.node.startOffset(), "Cannot get address of this expression");
        return nullptr;
    }

    llvm::Value* emitMember(const ast::MemberExpression& e) {
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

    llvm::Value* emitTernary(const ast::TernaryExpression& e) {
        auto cond = e.condition();
        auto thenE = e.thenBranch();
        auto elseE = e.elseBranch();
        if (!cond || !thenE || !elseE) return nullptr;
        llvm::Value* condV = emitExpr(*cond);
        if (!condV) return nullptr;

        auto* thenBB  = llvm::BasicBlock::Create(ctx, "tern.then", currentFunction);
        auto* elseBB  = llvm::BasicBlock::Create(ctx, "tern.else", currentFunction);
        auto* mergeBB = llvm::BasicBlock::Create(ctx, "tern.end",  currentFunction);
        builder->CreateCondBr(condV, thenBB, elseBB);

        builder->SetInsertPoint(thenBB);
        llvm::Value* thenV = emitExpr(*thenE);
        llvm::BasicBlock* thenEnd = builder->GetInsertBlock();
        if (thenV && !thenEnd->getTerminator()) builder->CreateBr(mergeBB);

        builder->SetInsertPoint(elseBB);
        llvm::Value* elseV = emitExpr(*elseE);
        llvm::BasicBlock* elseEnd = builder->GetInsertBlock();
        if (elseV && !elseEnd->getTerminator()) builder->CreateBr(mergeBB);

        builder->SetInsertPoint(mergeBB);
        if (!thenV || !elseV) return nullptr;
        if (thenV->getType() != elseV->getType()) {
            error(e.node.startOffset(), "Ternary branches produce different LLVM types");
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

        if (debugEnabled && diBuilder) diBuilder->finalize();
        if (!diagnostics.empty()) return false;

        std::string verifyErr;
        llvm::raw_string_ostream rso(verifyErr);
        if (llvm::verifyModule(*module, &rso)) {
            error(0, "LLVM module verification failed: " + verifyErr);
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
                                   const AnalysisResult& analysis)
    : impl(std::make_unique<Impl>(std::move(moduleName), std::move(sourceFilename), src, analysis)) {}

CodeGenerator::~CodeGenerator() = default;

bool CodeGenerator::generate(const SyntaxNode& root) { return impl->generate(root); }
void CodeGenerator::print(std::ostream& os) const    { impl->print(os); }
bool CodeGenerator::emitObjectFile(const std::string& path) { return impl->emitObjectFile(path); }
bool CodeGenerator::hasErrors() const                { return !impl->diagnostics.empty(); }
const std::vector<Diagnostic>& CodeGenerator::getDiagnostics() const { return impl->diagnostics; }
