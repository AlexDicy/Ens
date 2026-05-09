#include "CodeGenerator.h"
#include "../ast/Expr.h"
#include "../semantic/Type.h"
#include "../semantic/Symbol.h"
#include "../tokenizer/TokenType.h"

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
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

static std::string asAscii(const std::u16string& s) {
    std::string r;
    r.reserve(s.size());
    for (char16_t c : s) r.push_back(static_cast<char>(c));
    return r;
}

struct CodeGenerator::Impl {
    llvm::LLVMContext ctx;
    std::unique_ptr<llvm::Module> module;
    std::unique_ptr<llvm::IRBuilder<>> builder;
    std::unique_ptr<llvm::DIBuilder> diBuilder;
    std::unique_ptr<llvm::TargetMachine> targetMachine;
    std::unordered_map<Symbol*, llvm::Value*> values;
    std::unordered_map<int, llvm::DIType*> diTypeCache;  // keyed by TypeKind
    std::unordered_map<::Type*, llvm::StructType*> structTypeCache;
    llvm::Function* currentFunction = nullptr;
    llvm::DIScope* currentDIScope = nullptr;
    llvm::DICompileUnit* diCU = nullptr;
    llvm::DIFile* diFile = nullptr;
    bool debugEnabled = true;
    std::vector<Diagnostic> diagnostics;

    Impl(const std::string& moduleName, const std::string& filename) {
        module = std::make_unique<llvm::Module>(moduleName, ctx);
        module->setSourceFileName(filename);
        builder = std::make_unique<llvm::IRBuilder<>>(ctx);

        if (debugEnabled) {
            module->addModuleFlag(llvm::Module::Warning, "Debug Info Version", llvm::DEBUG_METADATA_VERSION);
            module->addModuleFlag(llvm::Module::Warning, "Dwarf Version", 4);

            diBuilder = std::make_unique<llvm::DIBuilder>(*module);
            std::filesystem::path p(filename);
            std::string fname = p.filename().string();
            std::string dir   = p.parent_path().string();
            if (fname.empty()) fname = filename;
            diFile = diBuilder->createFile(fname, dir);
            diCU = diBuilder->createCompileUnit(
                llvm::dwarf::DW_LANG_C,
                diFile,
                "Ens compiler",
                /*isOptimized*/ false,
                /*flags*/ "",
                /*runtimeVersion*/ 0);
            currentDIScope = diCU;
        }
    }

    // Attach !dbg metadata to instructions emitted hereafter.
    void setLocation(Node* n) {
        if (!debugEnabled || !n || !currentDIScope) return;
        builder->SetCurrentDebugLocation(
            llvm::DILocation::get(ctx, static_cast<unsigned>(n->line),
                                  static_cast<unsigned>(n->column), currentDIScope));
    }

    void clearLocation() {
        builder->SetCurrentDebugLocation(llvm::DebugLoc());
    }

    void error(int line, int col, std::string msg) {
        diagnostics.emplace_back(DiagnosticLevel::Error, SourceSpan{line, col, 1}, std::move(msg));
    }

    bool isUnsupportedType(::Type* t) {
        if (!t) return true;
        switch (t->kind) {
            case TypeKind::Decimal:
            case TypeKind::Optional:
            case TypeKind::Null:
            case TypeKind::Error:
                return true;
            default:
                return false;
        }
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
            default:                return nullptr;
        }
    }

    llvm::StructType* mapStructType(::Type* t) {
        auto it = structTypeCache.find(t);
        if (it != structTypeCache.end()) return it->second;
        std::string sname;
        if (t->structInfo) {
            for (char16_t c : t->structInfo->name)
                sname.push_back(c < 128 ? static_cast<char>(c) : '?');
        } else {
            sname = "struct.anon";
        }
        auto* st = llvm::StructType::create(ctx, sname);  // opaque first
        structTypeCache[t] = st;  // cache before recursion to handle self-refs
        std::vector<llvm::Type*> fieldTypes;
        if (t->structInfo) {
            fieldTypes.reserve(t->structInfo->fields.size());
            for (const auto& f : t->structInfo->fields) {
                fieldTypes.push_back(mapType(f.type));
            }
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

    std::unordered_map<::Type*, llvm::DIType*> diStructTypeCache;

    llvm::DIType* mapDIType(::Type* t) {
        if (!debugEnabled || !diBuilder || !t) return nullptr;
        if (t->kind == TypeKind::Struct) return mapDIStructType(t);
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
            default:               break;  // void and unsupported types stay null
        }
        if (result) diTypeCache[key] = result;
        return result;
    }

    llvm::DIType* mapDIStructType(::Type* t) {
        auto it = diStructTypeCache.find(t);
        if (it != diStructTypeCache.end()) return it->second;
        if (!t->structInfo) return nullptr;

        std::string sname;
        for (char16_t c : t->structInfo->name)
            sname.push_back(c < 128 ? static_cast<char>(c) : '?');

        // Use the LLVM struct layout (driven by the module's DataLayout) for
        // accurate field offsets and total size — matches what mapStructType
        // produces and what codegen actually accesses via GEP.
        auto* st = mapStructType(t);
        const llvm::DataLayout& dl = module->getDataLayout();
        const llvm::StructLayout* sl = dl.getStructLayout(st);

        unsigned structLine = static_cast<unsigned>(t->structInfo->line);
        std::vector<llvm::Metadata*> members;
        members.reserve(t->structInfo->fields.size());
        for (size_t i = 0; i < t->structInfo->fields.size(); ++i) {
            const auto& f = t->structInfo->fields[i];
            std::string fname;
            for (char16_t c : f.name)
                fname.push_back(c < 128 ? static_cast<char>(c) : '?');
            auto* memberType = mapDIType(f.type);
            uint64_t offsetBits = sl->getElementOffsetInBits(static_cast<unsigned>(i));
            uint64_t sizeBits = memberType
                ? memberType->getSizeInBits()
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
        elems.push_back(mapDIType(fn->returnType));  // index 0 is the return type (null = void)
        for (auto* pt : fn->paramTypes) elems.push_back(mapDIType(pt));
        return diBuilder->createSubroutineType(diBuilder->getOrCreateTypeArray(elems));
    }

    bool generate(const std::vector<StmtPtr>& program) {
        // Set DataLayout up front so DI emission can compute struct offsets.
        if (!initializeTargetMachine()) return false;

        for (auto& s : program) {
            if (auto* fn = dynamic_cast<FuncDecl*>(s.get())) {
                declareFunction(fn);
            } else if (auto* sd = dynamic_cast<StructDecl*>(s.get())) {
                for (auto& m : sd->methods) declareFunction(m.get());
            }
        }
        for (auto& s : program) {
            if (auto* fn = dynamic_cast<FuncDecl*>(s.get())) {
                emitFunction(fn);
            } else if (auto* sd = dynamic_cast<StructDecl*>(s.get())) {
                for (auto& m : sd->methods) emitFunction(m.get());
            }
        }
        if (debugEnabled && diBuilder) diBuilder->finalize();
        if (!diagnostics.empty()) return false;

        std::string verifyErr;
        llvm::raw_string_ostream rso(verifyErr);
        if (llvm::verifyModule(*module, &rso)) {
            error(0, 0, "LLVM module verification failed: " + verifyErr);
            return false;
        }
        return true;
    }

    llvm::AllocaInst* createEntryAlloca(llvm::Function* fn, llvm::Type* t, const std::string& name) {
        llvm::IRBuilder<> tmpBuilder(&fn->getEntryBlock(), fn->getEntryBlock().begin());
        return tmpBuilder.CreateAlloca(t, nullptr, name);
    }

    void declareFunction(FuncDecl* fn) {
        Symbol* sym = fn->resolvedSymbol;
        if (!sym) return;

        std::vector<llvm::Type*> paramTypes;

        // Methods get an implicit leading `this` pointer parameter.
        if (fn->receiverType) {
            paramTypes.push_back(llvm::PointerType::get(ctx, 0));
        }

        for (auto* pt : sym->paramTypes) {
            if (isUnsupportedType(pt)) {
                error(fn->line, fn->column,
                      "Function '" + asAscii(fn->name) + "' has unsupported parameter type '" + (pt ? pt->toString() : "<null>") + "'");
                return;
            }
            paramTypes.push_back(mapType(pt));
        }
        if (sym->returnType && !sym->returnType->isVoid() && isUnsupportedType(sym->returnType)) {
            error(fn->line, fn->column,
                  "Function '" + asAscii(fn->name) + "' has unsupported return type '" + sym->returnType->toString() + "'");
            return;
        }
        llvm::Type* retType = mapType(sym->returnType);
        auto* fnType = llvm::FunctionType::get(retType, paramTypes, false);

        std::string mangled = asAscii(fn->name);
        if (fn->receiverType && fn->receiverType->structInfo) {
            mangled = asAscii(fn->receiverType->structInfo->name) + "_" + mangled;
        }

        auto* func = llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, mangled, module.get());
        values[sym] = func;
    }

    void emitFunction(FuncDecl* fn) {
        Symbol* sym = fn->resolvedSymbol;
        auto it = values.find(sym);
        if (it == values.end()) return;

        currentFunction = llvm::cast<llvm::Function>(it->second);
        auto* entry = llvm::BasicBlock::Create(ctx, "entry", currentFunction);
        builder->SetInsertPoint(entry);

        llvm::DISubprogram* sp = nullptr;
        llvm::DIScope* prevScope = currentDIScope;
        if (debugEnabled && diBuilder) {
            sp = diBuilder->createFunction(
                diCU,
                asAscii(fn->name),
                /*linkageName*/ asAscii(fn->name),
                diFile,
                static_cast<unsigned>(fn->line),
                createDISubroutineType(sym),
                /*scopeLine*/ static_cast<unsigned>(fn->line),
                llvm::DINode::FlagPrototyped,
                llvm::DISubprogram::SPFlagDefinition);
            currentFunction->setSubprogram(sp);
            currentDIScope = sp;
        }

        // Prologue: parameter allocas and stores. Use the function's location so
        // verifyModule is happy with !dbg on every instruction.
        setLocation(fn);

        auto argIter = currentFunction->args().begin();
        auto argEnd  = currentFunction->args().end();

        if (fn->receiverType && fn->thisSymbol && argIter != argEnd) {
            argIter->setName("this");
            llvm::Type* ptrTy = llvm::PointerType::get(ctx, 0);
            auto* thisAlloca = createEntryAlloca(currentFunction, ptrTy, "this.addr");
            builder->CreateStore(&*argIter, thisAlloca);
            values[fn->thisSymbol] = thisAlloca;
            ++argIter;
        }

        for (size_t i = 0; i < fn->parameters.size() && argIter != argEnd; ++i, ++argIter) {
            auto& param = fn->parameters[i];
            std::string pname = asAscii(param.name);
            argIter->setName(pname);
            llvm::Type* lt = mapType(param.resolvedSymbol->type);
            auto* alloca = createEntryAlloca(currentFunction, lt, pname);
            builder->CreateStore(&*argIter, alloca);
            values[param.resolvedSymbol] = alloca;

            if (debugEnabled && diBuilder && sp) {
                llvm::DIType* paramDIType = mapDIType(param.resolvedSymbol->type);
                if (paramDIType) {
                    int pline = param.type ? param.type->line : fn->line;
                    int pcol  = param.type ? param.type->column : fn->column;
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

        if (fn->body) {
            for (auto& s : fn->body->statements) {
                emitStmt(s.get());
                if (builder->GetInsertBlock()->getTerminator()) break;
            }
        }
        if (!builder->GetInsertBlock()->getTerminator()) {
            if (currentFunction->getReturnType()->isVoidTy()) {
                builder->CreateRetVoid();
            } else {
                builder->CreateUnreachable();
            }
        }

        if (debugEnabled && diBuilder && sp) diBuilder->finalizeSubprogram(sp);
        currentDIScope = prevScope;
        currentFunction = nullptr;
    }

    void emitStmt(Stmt* s) {
        setLocation(s);
        if (auto* b = dynamic_cast<BlockStmt*>(s))    { emitBlock(b); return; }
        if (auto* v = dynamic_cast<VarDeclStmt*>(s))  { emitVarDecl(v); return; }
        if (auto* i = dynamic_cast<IfStmt*>(s))       { emitIf(i); return; }
        if (auto* w = dynamic_cast<WhileStmt*>(s))    { emitWhile(w); return; }
        if (auto* r = dynamic_cast<ReturnStmt*>(s))   { emitReturn(r); return; }
        if (auto* e = dynamic_cast<ExprStmt*>(s))     { emitExpr(e->expr.get()); return; }
    }

    void emitBlock(BlockStmt* s) {
        llvm::DIScope* prev = currentDIScope;
        if (debugEnabled && diBuilder && currentDIScope) {
            currentDIScope = diBuilder->createLexicalBlock(
                currentDIScope, diFile,
                static_cast<unsigned>(s->line),
                static_cast<unsigned>(s->column));
        }
        for (auto& child : s->statements) {
            emitStmt(child.get());
            if (builder->GetInsertBlock()->getTerminator()) break;
        }
        currentDIScope = prev;
    }

    void emitVarDecl(VarDeclStmt* s) {
        Symbol* sym = s->resolvedSymbol;
        if (!sym) return;
        if (isUnsupportedType(sym->type)) {
            error(s->line, s->column,
                  "Variable '" + asAscii(s->name) + "' has unsupported type '" + (sym->type ? sym->type->toString() : "<null>") + "'");
            return;
        }
        llvm::Type* lt = mapType(sym->type);
        auto* alloca = createEntryAlloca(currentFunction, lt, asAscii(s->name));
        values[sym] = alloca;

        if (debugEnabled && diBuilder && currentDIScope) {
            llvm::DIType* diVarType = mapDIType(sym->type);
            if (diVarType) {
                auto* diVar = diBuilder->createAutoVariable(
                    currentDIScope, asAscii(s->name), diFile,
                    static_cast<unsigned>(s->line), diVarType);
                diBuilder->insertDeclare(
                    alloca, diVar, diBuilder->createExpression(),
                    llvm::DILocation::get(ctx, static_cast<unsigned>(s->line),
                                          static_cast<unsigned>(s->column), currentDIScope),
                    builder->GetInsertBlock());
            }
        }

        if (s->init) {
            setLocation(s);
            llvm::Value* v = emitExpr(s->init.get());
            if (v) {
                setLocation(s);
                builder->CreateStore(v, alloca);
            }
        }
    }

    void emitIf(IfStmt* s) {
        llvm::Value* cond = emitExpr(s->condition.get());
        if (!cond) return;
        auto* thenBB = llvm::BasicBlock::Create(ctx, "if.then", currentFunction);
        auto* elseBB = s->elseBranch ? llvm::BasicBlock::Create(ctx, "if.else", currentFunction) : nullptr;
        auto* mergeBB = llvm::BasicBlock::Create(ctx, "if.end", currentFunction);

        builder->CreateCondBr(cond, thenBB, elseBB ? elseBB : mergeBB);

        builder->SetInsertPoint(thenBB);
        emitStmt(s->thenBranch.get());
        if (!builder->GetInsertBlock()->getTerminator()) {
            builder->CreateBr(mergeBB);
        }

        if (elseBB) {
            builder->SetInsertPoint(elseBB);
            emitStmt(s->elseBranch.get());
            if (!builder->GetInsertBlock()->getTerminator()) {
                builder->CreateBr(mergeBB);
            }
        }
        builder->SetInsertPoint(mergeBB);
    }

    void emitWhile(WhileStmt* s) {
        auto* condBB = llvm::BasicBlock::Create(ctx, "while.cond", currentFunction);
        auto* bodyBB = llvm::BasicBlock::Create(ctx, "while.body", currentFunction);
        auto* endBB  = llvm::BasicBlock::Create(ctx, "while.end",  currentFunction);

        builder->CreateBr(condBB);
        builder->SetInsertPoint(condBB);
        llvm::Value* cond = emitExpr(s->condition.get());
        if (cond) builder->CreateCondBr(cond, bodyBB, endBB);

        builder->SetInsertPoint(bodyBB);
        emitStmt(s->body.get());
        if (!builder->GetInsertBlock()->getTerminator()) {
            builder->CreateBr(condBB);
        }
        builder->SetInsertPoint(endBB);
    }

    void emitReturn(ReturnStmt* s) {
        if (s->expr) {
            llvm::Value* v = emitExpr(s->expr.get());
            if (v) builder->CreateRet(v);
            else builder->CreateUnreachable();
        } else {
            builder->CreateRetVoid();
        }
    }

    llvm::Value* emitExpr(Expr* e) {
        setLocation(e);
        if (auto* lit = dynamic_cast<IntLitExpr*>(e)) {
            llvm::Type* t = mapType(e->resolvedType);
            return llvm::ConstantInt::get(t, static_cast<uint64_t>(lit->value), isSigned(e->resolvedType));
        }
        if (auto* lit = dynamic_cast<DoubleLitExpr*>(e)) {
            return llvm::ConstantFP::get(mapType(e->resolvedType), lit->value);
        }
        if (auto* lit = dynamic_cast<BoolLitExpr*>(e)) {
            return builder->getInt1(lit->value);
        }
        if (dynamic_cast<NullLitExpr*>(e))   { error(e->line, e->column, "null codegen not yet supported"); return nullptr; }
        if (auto* lit = dynamic_cast<StringLitExpr*>(e)) {
            std::string utf8;
            for (char16_t c : lit->value) {
                if (c < 0x80) {
                    utf8.push_back(static_cast<char>(c));
                } else if (c < 0x800) {
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
        if (auto* id = dynamic_cast<IdentExpr*>(e))   return emitIdent(id);
        if (auto* th = dynamic_cast<ThisExpr*>(e))    return emitThis(th);
        if (auto* b = dynamic_cast<BinaryExpr*>(e))   return emitBinary(b);
        if (auto* u = dynamic_cast<UnaryExpr*>(e))    return emitUnary(u);
        if (auto* c = dynamic_cast<CallExpr*>(e))     return emitCall(c);
        if (auto* a = dynamic_cast<AssignExpr*>(e))   return emitAssign(a);
        if (auto* m = dynamic_cast<MemberExpr*>(e)) return emitMember(m);
        if (dynamic_cast<SubscriptExpr*>(e)) { error(e->line, e->column, "subscript codegen not yet supported"); return nullptr; }
        if (auto* tern = dynamic_cast<TernaryExpr*>(e)) return emitTernary(tern);
        return nullptr;
    }

    llvm::Value* emitTernary(TernaryExpr* e) {
        llvm::Value* cond = emitExpr(e->cond.get());
        if (!cond) return nullptr;

        auto* thenBB  = llvm::BasicBlock::Create(ctx, "tern.then", currentFunction);
        auto* elseBB  = llvm::BasicBlock::Create(ctx, "tern.else", currentFunction);
        auto* mergeBB = llvm::BasicBlock::Create(ctx, "tern.end",  currentFunction);

        builder->CreateCondBr(cond, thenBB, elseBB);

        builder->SetInsertPoint(thenBB);
        llvm::Value* thenV = emitExpr(e->thenExpr.get());
        llvm::BasicBlock* thenEnd = builder->GetInsertBlock();
        if (thenV && !thenEnd->getTerminator()) builder->CreateBr(mergeBB);

        builder->SetInsertPoint(elseBB);
        llvm::Value* elseV = emitExpr(e->elseExpr.get());
        llvm::BasicBlock* elseEnd = builder->GetInsertBlock();
        if (elseV && !elseEnd->getTerminator()) builder->CreateBr(mergeBB);

        builder->SetInsertPoint(mergeBB);
        if (!thenV || !elseV) return nullptr;
        if (thenV->getType() != elseV->getType()) {
            error(e->line, e->column, "Ternary branches produce different LLVM types");
            return nullptr;
        }
        auto* phi = builder->CreatePHI(thenV->getType(), 2);
        phi->addIncoming(thenV, thenEnd);
        phi->addIncoming(elseV, elseEnd);
        return phi;
    }

    llvm::Value* emitIdent(IdentExpr* e) {
        Symbol* sym = e->resolvedSymbol;
        if (!sym) return nullptr;
        auto it = values.find(sym);
        if (it == values.end()) {
            error(e->line, e->column, "Internal: symbol has no LLVM value");
            return nullptr;
        }
        if (sym->kind == SymbolKind::Function) {
            error(e->line, e->column, "Function values are not yet first-class");
            return nullptr;
        }
        return builder->CreateLoad(mapType(sym->type), it->second, asAscii(e->name) + ".load");
    }

    llvm::Value* emitThis(ThisExpr* e) {
        Symbol* sym = e->resolvedSymbol;
        if (!sym) {
            error(e->line, e->column, "Internal: `this` not bound");
            return nullptr;
        }
        auto it = values.find(sym);
        if (it == values.end()) {
            error(e->line, e->column, "Internal: `this` has no LLVM value");
            return nullptr;
        }
        // `this` is stored at an alloca holding a pointer to the receiver. Load
        // the pointer; member access GEPs from this address directly.
        return builder->CreateLoad(llvm::PointerType::get(ctx, 0), it->second, "this");
    }

    llvm::Value* emitBinary(BinaryExpr* e) {
        llvm::Value* L = emitExpr(e->left.get());
        llvm::Value* R = emitExpr(e->right.get());
        if (!L || !R) return nullptr;

        ::Type* leftType = e->left->resolvedType;
        bool flt = leftType && leftType->isFloat();
        bool sgn = isSigned(leftType);

        switch (e->op) {
            case TokenType::PLUS:    return flt ? builder->CreateFAdd(L, R) : builder->CreateAdd(L, R);
            case TokenType::SUB:     return flt ? builder->CreateFSub(L, R) : builder->CreateSub(L, R);
            case TokenType::STAR:    return flt ? builder->CreateFMul(L, R) : builder->CreateMul(L, R);
            case TokenType::SLASH:   return flt ? builder->CreateFDiv(L, R) : (sgn ? builder->CreateSDiv(L, R) : builder->CreateUDiv(L, R));
            case TokenType::PERCENT: return flt ? builder->CreateFRem(L, R) : (sgn ? builder->CreateSRem(L, R) : builder->CreateURem(L, R));
            case TokenType::EQ_EQ:   return flt ? builder->CreateFCmpOEQ(L, R) : builder->CreateICmpEQ(L, R);
            case TokenType::NOT_EQ:  return flt ? builder->CreateFCmpONE(L, R) : builder->CreateICmpNE(L, R);
            case TokenType::LT:      return flt ? builder->CreateFCmpOLT(L, R) : (sgn ? builder->CreateICmpSLT(L, R) : builder->CreateICmpULT(L, R));
            case TokenType::GT:      return flt ? builder->CreateFCmpOGT(L, R) : (sgn ? builder->CreateICmpSGT(L, R) : builder->CreateICmpUGT(L, R));
            case TokenType::LT_EQ:   return flt ? builder->CreateFCmpOLE(L, R) : (sgn ? builder->CreateICmpSLE(L, R) : builder->CreateICmpULE(L, R));
            case TokenType::GT_EQ:   return flt ? builder->CreateFCmpOGE(L, R) : (sgn ? builder->CreateICmpSGE(L, R) : builder->CreateICmpUGE(L, R));
            case TokenType::AND:
            case TokenType::BIT_AND: return builder->CreateAnd(L, R);
            case TokenType::OR:
            case TokenType::BIT_OR:  return builder->CreateOr(L, R);
            case TokenType::CARET:   return builder->CreateXor(L, R);
            case TokenType::LT_LT:   return builder->CreateShl(L, R);
            case TokenType::GT_GT:   return sgn ? builder->CreateAShr(L, R) : builder->CreateLShr(L, R);
            case TokenType::GT_GT_GT: return builder->CreateLShr(L, R);
            default:
                error(e->line, e->column, "Unsupported binary operator in codegen");
                return nullptr;
        }
    }

    llvm::Value* emitUnary(UnaryExpr* e) {
        ::Type* t = e->operand->resolvedType;
        bool flt = t && t->isFloat();
        switch (e->op) {
            case TokenType::SUB: {
                llvm::Value* v = emitExpr(e->operand.get());
                if (!v) return nullptr;
                return flt ? builder->CreateFNeg(v) : builder->CreateNeg(v);
            }
            case TokenType::NOT: {
                llvm::Value* v = emitExpr(e->operand.get());
                if (!v) return nullptr;
                return builder->CreateNot(v);
            }
            case TokenType::PLUS_PLUS:
            case TokenType::SUB_SUB: {
                llvm::Value* lv = emitLValue(e->operand.get());
                if (!lv) return nullptr;
                llvm::Type* lt = mapType(t);
                llvm::Value* v = builder->CreateLoad(lt, lv);
                llvm::Value* one = flt
                    ? static_cast<llvm::Value*>(llvm::ConstantFP::get(lt, 1.0))
                    : static_cast<llvm::Value*>(llvm::ConstantInt::get(lt, 1));
                llvm::Value* nv = (e->op == TokenType::PLUS_PLUS)
                    ? (flt ? builder->CreateFAdd(v, one) : builder->CreateAdd(v, one))
                    : (flt ? builder->CreateFSub(v, one) : builder->CreateSub(v, one));
                builder->CreateStore(nv, lv);
                return nv;
            }
            default:
                error(e->line, e->column, "Unsupported unary operator in codegen");
                return nullptr;
        }
    }

    llvm::Function* getOrDeclarePuts() {
        if (auto* existing = module->getFunction("puts")) return existing;
        auto* ty = llvm::FunctionType::get(
            llvm::Type::getInt32Ty(ctx),
            { llvm::PointerType::get(ctx, 0) },
            /*isVarArg*/ false);
        return llvm::Function::Create(ty, llvm::Function::ExternalLinkage, "puts", module.get());
    }

    llvm::Value* emitBuiltinCall(Symbol* sym, CallExpr* e) {
        std::string name = asAscii(sym->name);
        if (name == "print") {
            if (e->args.size() != 1) {
                error(e->line, e->column, "print expects exactly 1 argument");
                return nullptr;
            }
            llvm::Value* arg = emitExpr(e->args[0].get());
            if (!arg) return nullptr;
            auto* puts = getOrDeclarePuts();
            builder->CreateCall(puts, {arg});
            return nullptr;  // void
        }
        error(e->line, e->column, "Unknown builtin '" + name + "'");
        return nullptr;
    }

    llvm::Value* emitCall(CallExpr* e) {
        // Method call: `obj.method(args)`
        if (auto* member = dynamic_cast<MemberExpr*>(e->callee.get())) {
            if (member->resolvedMethodSymbol) {
                Symbol* methodSym = member->resolvedMethodSymbol;
                auto fnIt = values.find(methodSym);
                if (fnIt == values.end()) {
                    error(e->line, e->column, "Internal: method has no LLVM function");
                    return nullptr;
                }
                llvm::Value* receiverAddr = emitLValue(member->object.get());
                if (!receiverAddr) return nullptr;

                std::vector<llvm::Value*> args;
                args.reserve(e->args.size() + 1);
                args.push_back(receiverAddr);
                for (auto& a : e->args) {
                    llvm::Value* v = emitExpr(a.get());
                    if (!v) return nullptr;
                    args.push_back(v);
                }
                auto* fn = llvm::cast<llvm::Function>(fnIt->second);
                return builder->CreateCall(fn, args);
            }
        }

        auto* idCallee = dynamic_cast<IdentExpr*>(e->callee.get());
        if (!idCallee || !idCallee->resolvedSymbol) {
            error(e->line, e->column, "Only direct function calls are supported");
            return nullptr;
        }
        Symbol* sym = idCallee->resolvedSymbol;
        if (sym->isBuiltin) return emitBuiltinCall(sym, e);

        auto it = values.find(sym);
        if (it == values.end()) {
            error(e->line, e->column, "Internal: callee has no LLVM function");
            return nullptr;
        }
        auto* fn = llvm::cast<llvm::Function>(it->second);
        std::vector<llvm::Value*> args;
        args.reserve(e->args.size());
        for (auto& a : e->args) {
            llvm::Value* v = emitExpr(a.get());
            if (!v) return nullptr;
            args.push_back(v);
        }
        return builder->CreateCall(fn, args);
    }

    llvm::Value* emitAssign(AssignExpr* e) {
        if (e->op != TokenType::EQ) {
            error(e->line, e->column, "Compound assignment not yet supported in codegen");
            return nullptr;
        }
        llvm::Value* lv = emitLValue(e->target.get());
        if (!lv) return nullptr;
        llvm::Value* val = emitExpr(e->value.get());
        if (!val) return nullptr;
        builder->CreateStore(val, lv);
        return val;
    }

    llvm::Value* emitLValue(Expr* e) {
        if (auto* id = dynamic_cast<IdentExpr*>(e)) {
            Symbol* sym = id->resolvedSymbol;
            if (!sym) return nullptr;
            auto it = values.find(sym);
            return it == values.end() ? nullptr : it->second;
        }
        if (auto* th = dynamic_cast<ThisExpr*>(e)) {
            // `this` already represents the address of the receiver — load the
            // pointer from its alloca and use it as the lvalue address.
            Symbol* sym = th->resolvedSymbol;
            if (!sym) return nullptr;
            auto it = values.find(sym);
            if (it == values.end()) return nullptr;
            return builder->CreateLoad(llvm::PointerType::get(ctx, 0), it->second, "this");
        }
        if (auto* m = dynamic_cast<MemberExpr*>(e)) {
            ::Type* objType = m->object->resolvedType;
            if (!objType || !objType->isStruct() || !objType->structInfo) {
                error(e->line, e->column, "Cannot take address of member on non-struct");
                return nullptr;
            }
            llvm::Value* objAddr = emitLValue(m->object.get());
            if (!objAddr) return nullptr;
            int idx = objType->structInfo->findFieldIndex(m->member);
            if (idx < 0) {
                error(e->line, e->column, "Internal: field not found in struct");
                return nullptr;
            }
            llvm::StructType* st = mapStructType(objType);
            return builder->CreateStructGEP(st, objAddr, static_cast<unsigned>(idx),
                                            asAscii(m->member) + ".addr");
        }
        error(e->line, e->column, "Cannot get address of this expression");
        return nullptr;
    }

    llvm::Value* emitMember(MemberExpr* e) {
        llvm::Value* addr = emitLValue(e);
        if (!addr) return nullptr;
        ::Type* fieldType = e->resolvedType;
        return builder->CreateLoad(mapType(fieldType), addr, asAscii(e->member) + ".load");
    }

    void print(std::ostream& os) const {
        std::string text;
        llvm::raw_string_ostream rso(text);
        module->print(rso, nullptr);
        os << text;
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

    // Create a persistent TargetMachine for the host triple and stamp its
    // DataLayout onto the module. Idempotent. Done up front so DI emission
    // can compute correct struct field offsets via getStructLayout.
    bool initializeTargetMachine() {
        if (targetMachine) return true;
        initializeNativeTargetOnce();

        std::string triple = llvm::sys::getDefaultTargetTriple();
        std::string lookupErr;
        const llvm::Target* target = llvm::TargetRegistry::lookupTarget(triple, lookupErr);
        if (!target) {
            error(0, 0, "Failed to find target '" + triple + "': " + lookupErr);
            return false;
        }

        llvm::TargetOptions opts;
        std::optional<llvm::Reloc::Model> rm = llvm::Reloc::PIC_;
        targetMachine.reset(target->createTargetMachine(
            llvm::Triple(triple), "generic", "", opts, rm));
        if (!targetMachine) {
            error(0, 0, "Failed to create TargetMachine for '" + triple + "'");
            return false;
        }
        module->setDataLayout(targetMachine->createDataLayout());
        module->setTargetTriple(llvm::Triple(triple));
        return true;
    }

    bool emitObjectFile(const std::string& path) {
        if (!targetMachine && !initializeTargetMachine()) return false;

        std::error_code ec;
        llvm::raw_fd_ostream dest(path, ec, llvm::sys::fs::OF_None);
        if (ec) {
            error(0, 0, "Could not open '" + path + "' for writing: " + ec.message());
            return false;
        }

        llvm::legacy::PassManager pass;
        if (targetMachine->addPassesToEmitFile(pass, dest, nullptr, llvm::CodeGenFileType::ObjectFile)) {
            error(0, 0, "Target does not support object-file emission");
            return false;
        }
        pass.run(*module);
        dest.flush();
        return true;
    }

};

CodeGenerator::CodeGenerator(std::string moduleName, std::string sourceFilename)
    : impl(std::make_unique<Impl>(moduleName, sourceFilename)) {}

CodeGenerator::~CodeGenerator() = default;

bool CodeGenerator::generate(const std::vector<StmtPtr>& program) {
    return impl->generate(program);
}

void CodeGenerator::print(std::ostream& os) const {
    impl->print(os);
}

bool CodeGenerator::emitObjectFile(const std::string& path) {
    return impl->emitObjectFile(path);
}

bool CodeGenerator::hasErrors() const {
    return !impl->diagnostics.empty();
}

const std::vector<Diagnostic>& CodeGenerator::getDiagnostics() const {
    return impl->diagnostics;
}
