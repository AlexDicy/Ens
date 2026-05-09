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

#include "lld/Common/Driver.h"

#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

LLD_HAS_DRIVER(coff)
LLD_HAS_DRIVER(elf)
LLD_HAS_DRIVER(macho)

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
    std::unordered_map<Symbol*, llvm::Value*> values;
    std::unordered_map<int, llvm::DIType*> diTypeCache;  // keyed by TypeKind
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
            default:                return nullptr;
        }
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

    llvm::DISubroutineType* createDISubroutineType(Symbol* fn) {
        if (!debugEnabled || !diBuilder) return nullptr;
        std::vector<llvm::Metadata*> elems;
        elems.push_back(mapDIType(fn->returnType));  // index 0 is the return type (null = void)
        for (auto* pt : fn->paramTypes) elems.push_back(mapDIType(pt));
        return diBuilder->createSubroutineType(diBuilder->getOrCreateTypeArray(elems));
    }

    bool generate(const std::vector<StmtPtr>& program) {
        for (auto& s : program) {
            if (auto* fn = dynamic_cast<FuncDecl*>(s.get())) {
                declareFunction(fn);
            }
        }
        for (auto& s : program) {
            if (auto* fn = dynamic_cast<FuncDecl*>(s.get())) {
                emitFunction(fn);
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
        auto* func = llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, asAscii(fn->name), module.get());
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

        size_t i = 0;
        for (auto& arg : currentFunction->args()) {
            auto& param = fn->parameters[i];
            std::string pname = asAscii(param.name);
            arg.setName(pname);
            llvm::Type* lt = mapType(param.resolvedSymbol->type);
            auto* alloca = createEntryAlloca(currentFunction, lt, pname);
            builder->CreateStore(&arg, alloca);
            values[param.resolvedSymbol] = alloca;

            if (debugEnabled && diBuilder && sp) {
                int pline = param.type ? param.type->line : fn->line;
                int pcol  = param.type ? param.type->column : fn->column;
                auto* diVar = diBuilder->createParameterVariable(
                    sp, pname, static_cast<unsigned>(i + 1), diFile,
                    static_cast<unsigned>(pline),
                    mapDIType(param.resolvedSymbol->type),
                    /*AlwaysPreserve*/ true);
                diBuilder->insertDeclare(
                    alloca, diVar, diBuilder->createExpression(),
                    llvm::DILocation::get(ctx, static_cast<unsigned>(pline),
                                          static_cast<unsigned>(pcol), sp),
                    builder->GetInsertBlock());
            }
            i++;
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
            auto* diVar = diBuilder->createAutoVariable(
                currentDIScope, asAscii(s->name), diFile,
                static_cast<unsigned>(s->line),
                mapDIType(sym->type));
            diBuilder->insertDeclare(
                alloca, diVar, diBuilder->createExpression(),
                llvm::DILocation::get(ctx, static_cast<unsigned>(s->line),
                                      static_cast<unsigned>(s->column), currentDIScope),
                builder->GetInsertBlock());
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
        if (auto* b = dynamic_cast<BinaryExpr*>(e))   return emitBinary(b);
        if (auto* u = dynamic_cast<UnaryExpr*>(e))    return emitUnary(u);
        if (auto* c = dynamic_cast<CallExpr*>(e))     return emitCall(c);
        if (auto* a = dynamic_cast<AssignExpr*>(e))   return emitAssign(a);
        if (dynamic_cast<MemberExpr*>(e))    { error(e->line, e->column, "member access codegen not yet supported"); return nullptr; }
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
        error(e->line, e->column, "Cannot get address of this expression");
        return nullptr;
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

    bool emitObjectFile(const std::string& path) {
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
        auto* machine = target->createTargetMachine(llvm::Triple(triple), "generic", "", opts, rm);
        if (!machine) {
            error(0, 0, "Failed to create TargetMachine for '" + triple + "'");
            return false;
        }

        module->setDataLayout(machine->createDataLayout());
        module->setTargetTriple(llvm::Triple(triple));

        std::error_code ec;
        llvm::raw_fd_ostream dest(path, ec, llvm::sys::fs::OF_None);
        if (ec) {
            error(0, 0, "Could not open '" + path + "' for writing: " + ec.message());
            return false;
        }

        llvm::legacy::PassManager pass;
        if (machine->addPassesToEmitFile(pass, dest, nullptr, llvm::CodeGenFileType::ObjectFile)) {
            error(0, 0, "Target does not support object-file emission");
            return false;
        }
        pass.run(*module);
        dest.flush();
        return true;
    }

    bool linkExecutable(const std::string& objectPath, const std::string& exePath) {
        // Build a platform-appropriate driver argv. For now we support COFF (Windows).
        // ELF and Mach-O drivers are linked in too via LLD_HAS_DRIVER above so the
        // build remains portable; we just need to choose the right driver per host.
        const std::string triple = llvm::sys::getDefaultTargetTriple();
        const bool isWindowsCoff = triple.find("windows") != std::string::npos
                                || triple.find("win32") != std::string::npos
                                || triple.find("msvc") != std::string::npos;

        std::vector<std::string> argv;
        if (isWindowsCoff) {
            argv = {
                "lld-link",
                "/nologo",
                "/subsystem:console",
                objectPath,
                "/out:" + exePath,
                "/defaultlib:libcmt",      // static C runtime
                "/defaultlib:oldnames",
            };
        } else {
            // Best-effort ELF default; users on non-Windows can iterate.
            argv = {"ld.lld", objectPath, "-o", exePath};
        }

        std::vector<const char*> args;
        args.reserve(argv.size());
        for (auto& s : argv) args.push_back(s.c_str());

        std::string outBuf, errBuf;
        llvm::raw_string_ostream outStream(outBuf);
        llvm::raw_string_ostream errStream(errBuf);

        bool ok = false;
        if (isWindowsCoff) {
            ok = lld::coff::link(args, outStream, errStream, /*exitEarly*/ false, /*disableOutput*/ false);
        } else if (triple.find("darwin") != std::string::npos || triple.find("apple") != std::string::npos) {
            ok = lld::macho::link(args, outStream, errStream, false, false);
        } else {
            ok = lld::elf::link(args, outStream, errStream, false, false);
        }
        outStream.flush();
        errStream.flush();
        if (!outBuf.empty()) std::cout << outBuf;
        if (!ok && !errBuf.empty()) {
            error(0, 0, "Linker failed:\n" + errBuf);
        } else if (!errBuf.empty()) {
            std::cerr << errBuf;
        }
        return ok;
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

bool CodeGenerator::linkExecutable(const std::string& objectPath, const std::string& exePath) {
    return impl->linkExecutable(objectPath, exePath);
}

bool CodeGenerator::hasErrors() const {
    return !impl->diagnostics.empty();
}

const std::vector<Diagnostic>& CodeGenerator::getDiagnostics() const {
    return impl->diagnostics;
}
