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
#include "llvm/Support/raw_ostream.h"

#include <memory>
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
    std::unordered_map<Symbol*, llvm::Value*> values;
    llvm::Function* currentFunction = nullptr;
    std::vector<Diagnostic> diagnostics;

    Impl(const std::string& moduleName, const std::string& filename) {
        module = std::make_unique<llvm::Module>(moduleName, ctx);
        module->setSourceFileName(filename);
        builder = std::make_unique<llvm::IRBuilder<>>(ctx);
    }

    // Hook for future debug info: would call builder->SetCurrentDebugLocation here.
    void setLocation(Node* /*n*/) {}

    void error(int line, int col, std::string msg) {
        diagnostics.emplace_back(DiagnosticLevel::Error, SourceSpan{line, col, 1}, std::move(msg));
    }

    bool isUnsupportedType(::Type* t) {
        if (!t) return true;
        switch (t->kind) {
            case TypeKind::Decimal:
            case TypeKind::String:
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
        for (auto& child : s->statements) {
            emitStmt(child.get());
            if (builder->GetInsertBlock()->getTerminator()) break;
        }
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
        if (s->init) {
            llvm::Value* v = emitExpr(s->init.get());
            if (v) builder->CreateStore(v, alloca);
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
        if (dynamic_cast<StringLitExpr*>(e)) { error(e->line, e->column, "string codegen not yet supported"); return nullptr; }
        if (auto* id = dynamic_cast<IdentExpr*>(e))   return emitIdent(id);
        if (auto* b = dynamic_cast<BinaryExpr*>(e))   return emitBinary(b);
        if (auto* u = dynamic_cast<UnaryExpr*>(e))    return emitUnary(u);
        if (auto* c = dynamic_cast<CallExpr*>(e))     return emitCall(c);
        if (auto* a = dynamic_cast<AssignExpr*>(e))   return emitAssign(a);
        if (dynamic_cast<MemberExpr*>(e))    { error(e->line, e->column, "member access codegen not yet supported"); return nullptr; }
        if (dynamic_cast<SubscriptExpr*>(e)) { error(e->line, e->column, "subscript codegen not yet supported"); return nullptr; }
        return nullptr;
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

    llvm::Value* emitCall(CallExpr* e) {
        auto* idCallee = dynamic_cast<IdentExpr*>(e->callee.get());
        if (!idCallee || !idCallee->resolvedSymbol) {
            error(e->line, e->column, "Only direct function calls are supported");
            return nullptr;
        }
        auto it = values.find(idCallee->resolvedSymbol);
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

bool CodeGenerator::hasErrors() const {
    return !impl->diagnostics.empty();
}

const std::vector<Diagnostic>& CodeGenerator::getDiagnostics() const {
    return impl->diagnostics;
}
