#include "LanguageServer.h"

#include <filesystem>
#include <optional>
#include <utility>
#include <vector>

#include <lsp/fileuri.h>

#include "DiagnosticBridge.h"
#include "Encoding.h"
#include "ast/Declaration.h"
#include "ast/Expression.h"
#include "ast/Statement.h"
#include "ast/TypeReference.h"
#include "cst/SyntaxKind.h"
#include "cst/SyntaxNode.h"
#include "semantic/AnalysisResult.h"
#include "semantic/Symbol.h"
#include "semantic/Type.h"

namespace {

lsp::Position toLspPosition(const SourceFile& source, uint32_t offset) {
    auto [line, col] = source.offsetToPosition(offset);
    lsp::Position p;
    p.line = line - 1;
    p.character = col - 1;
    return p;
}

lsp::Range toLspRange(const SourceFile& source, uint32_t startOffset, uint32_t endOffset) {
    lsp::Range r;
    r.start = toLspPosition(source, startOffset);
    r.end = toLspPosition(source, endOffset);
    return r;
}

lsp::Range toLspRange(const SourceFile& source, const SyntaxNode& node) {
    return toLspRange(source, node.startOffset(), node.endOffset());
}

lsp::Range zeroWidthRangeAt(int line1, int col1) {
    lsp::Range r;
    r.start.line = line1 > 0 ? line1 - 1 : 0;
    r.start.character = col1 > 0 ? col1 - 1 : 0;
    r.end = r.start;
    return r;
}

// Walks from `root` down to the deepest descendant whose extent contains `offset`.
// Returns root → ... → deepest. Empty if offset is outside root.
std::vector<SyntaxNode> ancestorChainAt(const SyntaxNode& root, uint32_t offset) {
    std::vector<SyntaxNode> chain;
    if (offset < root.startOffset() || offset >= root.endOffset()) return chain;
    chain.push_back(root);
    while (!chain.back().isToken()) {
        bool descended = false;
        for (auto& c : chain.back().children()) {
            if (offset >= c.startOffset() && offset < c.endOffset()) {
                chain.push_back(c);
                descended = true;
                break;
            }
        }
        if (!descended) break;
    }
    return chain;
}

const ResolutionInfo* lookupResolution(const AnalysisResult& analysis, const SyntaxNode& node) {
    return analysis.find(node.greenNode());
}

std::string formatHoverFor(const SyntaxNode& node, const ResolutionInfo& info) {
    SyntaxKind k = node.kind();
    if (k == SyntaxKind::IdentExpr || k == SyntaxKind::ThisExpr) {
        if (info.resolvedSymbol && info.resolvedSymbol->kind == SymbolKind::Namespace) {
            return "(namespace) " + utf16To8(info.resolvedSymbol->namespaceModulePath);
        }
        if (info.resolvedSymbol && info.resolvedSymbol->type) {
            return "(" + std::string(k == SyntaxKind::ThisExpr ? "this" : "value") +
                   ") : " + info.resolvedSymbol->type->toString();
        }
        if (info.resolvedType) {
            return "type: " + info.resolvedType->toString();
        }
    }
    if (k == SyntaxKind::MemberExpr || k == SyntaxKind::SafeMemberExpr) {
        if (info.resolvedMethodSymbol) {
            std::string r = "method " + utf16To8(info.resolvedMethodSymbol->name);
            r += "(";
            for (size_t i = 0; i < info.resolvedMethodSymbol->paramTypes.size(); ++i) {
                if (i) r += ", ";
                r += info.resolvedMethodSymbol->paramTypes[i]->toString();
            }
            r += ") -> ";
            r += info.resolvedMethodSymbol->returnType
                     ? info.resolvedMethodSymbol->returnType->toString()
                     : "void";
            return r;
        }
        if (info.resolvedType) {
            return "type: " + info.resolvedType->toString();
        }
    }
    if (info.resolvedType) {
        return "type: " + info.resolvedType->toString();
    }
    return {};
}

lsp::SymbolKind kindForFunction(bool isConstructor) {
    return isConstructor ? lsp::SymbolKind::Constructor : lsp::SymbolKind::Function;
}

std::string utf16ToUtf8AsString(const std::optional<std::u16string>& s) {
    return s ? utf16To8(*s) : std::string("<missing>");
}

lsp::DocumentSymbol buildFunctionSymbol(const SourceFile& source, const ast::FuncDecl& fn,
                                        bool isMember = false, bool isConstructor = false) {
    lsp::DocumentSymbol sym;
    sym.name = utf16ToUtf8AsString(fn.nameText());
    sym.kind = lsp::SymbolKindEnum(
        isMember ? (isConstructor ? lsp::SymbolKind::Constructor : lsp::SymbolKind::Method)
                 : kindForFunction(false));
    sym.range = toLspRange(source, fn.node);
    if (auto t = fn.nameToken()) {
        sym.selectionRange = toLspRange(source, *t);
    } else {
        sym.selectionRange = sym.range;
    }
    return sym;
}

lsp::DocumentSymbol buildFieldSymbol(const SourceFile& source, const ast::FieldDecl& f) {
    lsp::DocumentSymbol sym;
    sym.name = utf16ToUtf8AsString(f.nameText());
    sym.kind = lsp::SymbolKindEnum(lsp::SymbolKind::Field);
    sym.range = toLspRange(source, f.node);
    if (auto t = f.nameToken()) {
        sym.selectionRange = toLspRange(source, *t);
    } else {
        sym.selectionRange = sym.range;
    }
    return sym;
}

lsp::DocumentSymbol buildRecordSymbol(const SourceFile& source,
                                      const std::optional<std::u16string>& name,
                                      const std::optional<SyntaxNode>& nameToken,
                                      const SyntaxNode& wholeNode,
                                      lsp::SymbolKind kind,
                                      const std::vector<ast::FieldDecl>& fields,
                                      const std::vector<ast::FuncDecl>& methods) {
    lsp::DocumentSymbol sym;
    sym.name = utf16ToUtf8AsString(name);
    sym.kind = lsp::SymbolKindEnum(kind);
    sym.range = toLspRange(source, wholeNode);
    sym.selectionRange = nameToken ? toLspRange(source, *nameToken) : sym.range;

    std::vector<lsp::DocumentSymbol> children;
    children.reserve(fields.size() + methods.size());
    std::u16string typeName = name.value_or(std::u16string{});
    for (const auto& f : fields) {
        children.push_back(buildFieldSymbol(source, f));
    }
    for (const auto& m : methods) {
        bool isCtor = m.nameText().has_value() && *m.nameText() == typeName;
        children.push_back(buildFunctionSymbol(source, m, /*isMember*/ true, /*isConstructor*/ isCtor));
    }
    sym.children = std::move(children);
    return sym;
}

}  // namespace

LanguageServer::LanguageServer(lsp::MessageHandler& mh) : messages(mh) {}

lsp::InitializeResult LanguageServer::onInitialize(lsp::InitializeParams&& params) {
    // Resolve imports relative to the workspace root: prefer a workspace folder, fall
    // back to the (deprecated) rootUri. Without one, each file uses its own directory.
    std::optional<std::filesystem::path> root;
    if (params.workspaceFolders.has_value() && !params.workspaceFolders->isNull()) {
        const auto& folders = params.workspaceFolders->value();
        if (!folders.empty()) {
            auto path = lsp::FileUri(folders.front().uri).path();
            if (!path.empty()) root = std::filesystem::path(std::string(path));
        }
    }
    if (!root && !params.rootUri.isNull()) {
        auto path = params.rootUri.value().path();
        if (!path.empty()) root = std::filesystem::path(std::string(path));
    }
    if (root) documents.setWorkspaceRoot(std::move(*root));

    lsp::InitializeResult r;

    lsp::InitializeResultServerInfo info;
    info.name = "ens-lsp";
    info.version = std::string("0.1");
    r.serverInfo = std::move(info);

    lsp::ServerCapabilities caps;
    caps.positionEncoding =
        lsp::PositionEncodingKindEnum(lsp::PositionEncodingKind::UTF16);

    lsp::TextDocumentSyncOptions sync;
    sync.openClose = true;
    sync.change = lsp::TextDocumentSyncKindEnum(lsp::TextDocumentSyncKind::Full);
    caps.textDocumentSync = std::move(sync);

    caps.hoverProvider = true;
    caps.definitionProvider = true;
    caps.documentSymbolProvider = true;

    lsp::SemanticTokensOptions stOpts;
    stOpts.legend.tokenTypes = {
        "function", "method", "parameter", "variable",
        "property", "class", "struct", "type", "namespace", "enum"
    };
    stOpts.legend.tokenModifiers = {};
    stOpts.full = true;
    caps.semanticTokensProvider = std::move(stOpts);

    lsp::CompletionOptions cOpts;
    cOpts.triggerCharacters = std::vector<std::string>{"."};
    caps.completionProvider = std::move(cOpts);

    r.capabilities = std::move(caps);
    return r;
}

void LanguageServer::onInitialized() {}

void LanguageServer::onDidOpen(lsp::notifications::TextDocument_DidOpen::Params&& p) {
    auto& doc = documents.upsert(p.textDocument.uri.toString(),
                                  utf8To16(p.textDocument.text),
                                  p.textDocument.version);
    publishDiagnostics(doc);
}

void LanguageServer::onDidChange(lsp::notifications::TextDocument_DidChange::Params&& p) {
    if (p.contentChanges.empty()) return;
    const auto& last = p.contentChanges.back();
    if (auto* full = std::get_if<lsp::TextDocumentContentChangeEvent_Text>(&last)) {
        auto& doc = documents.upsert(p.textDocument.uri.toString(),
                                      utf8To16(full->text),
                                      p.textDocument.version);
        publishDiagnostics(doc);
    }
}

void LanguageServer::onDidClose(lsp::notifications::TextDocument_DidClose::Params&& p) {
    documents.erase(p.textDocument.uri.toString());
}

void LanguageServer::publishDiagnostics(const Document& doc) {
    lsp::notifications::TextDocument_PublishDiagnostics::Params p;
    p.uri = lsp::Uri::parse(doc.uri());
    p.version = doc.version();
    p.diagnostics = toLspDiagnostics(doc.sink().list());
    messages.sendNotification<lsp::notifications::TextDocument_PublishDiagnostics>(std::move(p));
}

lsp::TextDocument_HoverResult LanguageServer::onHover(lsp::HoverParams&& p) {
    auto* doc = documents.find(p.textDocument.uri.toString());
    if (!doc) return nullptr;

    int line1 = p.position.line + 1;
    int col1 = p.position.character + 1;
    uint32_t offset = doc->sourceFile().positionToOffset(line1, col1);
    auto chain = ancestorChainAt(doc->root(), offset);

    const auto& analysis = doc->analyzer().result();
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        if (auto* info = lookupResolution(analysis, *it)) {
            std::string text = formatHoverFor(*it, *info);
            if (!text.empty()) {
                lsp::Hover h;
                lsp::MarkupContent mc;
                mc.kind = lsp::MarkupKindEnum(lsp::MarkupKind::PlainText);
                mc.value = std::move(text);
                h.contents = std::move(mc);
                h.range = toLspRange(doc->sourceFile(), *it);
                return h;
            }
        }
    }
    return nullptr;
}

lsp::TextDocument_DefinitionResult LanguageServer::onDefinition(lsp::DefinitionParams&& p) {
    auto* doc = documents.find(p.textDocument.uri.toString());
    if (!doc) return nullptr;

    int line1 = p.position.line + 1;
    int col1 = p.position.character + 1;
    uint32_t offset = doc->sourceFile().positionToOffset(line1, col1);
    auto chain = ancestorChainAt(doc->root(), offset);

    const auto& analysis = doc->analyzer().result();
    const ResolutionInfo* hit = nullptr;
    const SyntaxNode* hitNode = nullptr;
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        if (auto* info = lookupResolution(analysis, *it)) {
            if (info->resolvedMethodSymbol || info->resolvedSymbol ||
                (info->resolvedType && info->resolvedType->structInfo)) {
                hit = info;
                hitNode = &*it;
                break;
            }
        }
    }
    if (!hit) return nullptr;

    Symbol* sym = hit->resolvedMethodSymbol ? hit->resolvedMethodSymbol : hit->resolvedSymbol;
    std::u16string modulePath;  // empty = same file
    int line = 0, col = 0;
    bool resolved = false;

    // Namespace-qualified member `ns.X`: jump into the imported module. The receiver
    // identifier resolves to the Namespace symbol, which names the target module.
    if (auto member = ast::MemberExpression::cast(*hitNode)) {
        if (auto obj = member->object()) {
            if (auto idObj = obj->asIdent()) {
                if (auto* objInfo = lookupResolution(analysis, idObj->node)) {
                    Symbol* nsSym = objInfo->resolvedSymbol;
                    if (nsSym && nsSym->kind == SymbolKind::Namespace) {
                        modulePath = nsSym->namespaceModulePath;
                        if (hit->resolvedType && hit->resolvedType->structInfo) {
                            line = hit->resolvedType->structInfo->line;
                            col = hit->resolvedType->structInfo->column;
                        } else if (sym) {
                            line = sym->line;
                            col = sym->column;
                        }
                        resolved = true;
                    }
                }
            }
        }
    }

    if (!resolved) {
        if (sym) {
            line = sym->line;
            col = sym->column;
        } else if (hit->resolvedType && hit->resolvedType->structInfo) {
            // A type reference: jump to the type's declaration (in its own module).
            StructInfo* si = hit->resolvedType->structInfo;
            modulePath = si->modulePath;
            line = si->line;
            col = si->column;
        } else {
            return nullptr;
        }
    }

    lsp::Location loc;
    const auto& files = doc->moduleFiles();
    auto fit = modulePath.empty() ? files.end() : files.find(modulePath);
    if (fit != files.end()) {
        loc.uri = lsp::FileUri::fromPath(fit->second.string());
    } else {
        loc.uri = lsp::Uri::parse(doc->uri());  // same file
    }
    loc.range = zeroWidthRangeAt(line, col);
    return lsp::Definition{std::move(loc)};
}

namespace {

// Indices into the SemanticTokensLegend.tokenTypes array declared in onInitialize.
enum SemanticTokenType : uint32_t {
    StFunction = 0, StMethod, StParameter, StVariable,
    StProperty, StClass, StStruct, StType, StNamespace, StEnum
};

struct SemanticTokenEntry {
    uint32_t line;
    uint32_t startChar;
    uint32_t length;
    uint32_t tokenType;
    uint32_t tokenModifiers;
};

void emitTokenAt(std::vector<SemanticTokenEntry>& out, const SourceFile& source,
                 const SyntaxNode& tokenNode, uint32_t tokenType) {
    if (tokenNode.length() == 0) return;
    auto [line, col] = source.offsetToPosition(tokenNode.startOffset());
    SemanticTokenEntry e;
    e.line = static_cast<uint32_t>(line - 1);
    e.startChar = static_cast<uint32_t>(col - 1);
    e.length = tokenNode.length();
    e.tokenType = tokenType;
    e.tokenModifiers = 0;
    out.push_back(e);
}

uint32_t typeForSymbol(const Symbol& sym, bool isMember) {
    switch (sym.kind) {
        case SymbolKind::Function:     return isMember ? StMethod : StFunction;
        case SymbolKind::Parameter:    return StParameter;
        case SymbolKind::Variable:     return StVariable;
        case SymbolKind::Namespace:    return StNamespace;
        case SymbolKind::SiblingField: return StProperty;
    }
    return StVariable;
}

uint32_t typeForType(const ::Type* t) {
    if (!t) return StType;
    if (t->kind == TypeKind::Class)  return StClass;
    if (t->kind == TypeKind::Struct) return StStruct;
    if (t->kind == TypeKind::Enum)   return StEnum;
    return StType;
}

void collectFromExpression(const SyntaxNode& node, const SourceFile& source,
                           const AnalysisResult& analysis,
                           std::vector<SemanticTokenEntry>& out);

void collectFromStatement(const SyntaxNode& node, const SourceFile& source,
                          const AnalysisResult& analysis,
                          std::vector<SemanticTokenEntry>& out);

void collectFromTypeReference(const ast::TypeReference& tr, const SourceFile& source,
                              const AnalysisResult& analysis,
                              std::vector<SemanticTokenEntry>& out);

void collectFromExpression(const SyntaxNode& node, const SourceFile& source,
                           const AnalysisResult& analysis,
                           std::vector<SemanticTokenEntry>& out) {
    auto e = ast::Expression::cast(node);
    if (!e) {
        for (auto& c : node.children()) collectFromExpression(c, source, analysis, out);
        return;
    }

    if (auto id = e->asIdent()) {
        if (auto* info = analysis.find(id->node.greenNode())) {
            if (info->resolvedSymbol) {
                if (auto t = id->identifier()) {
                    emitTokenAt(out, source, *t, typeForSymbol(*info->resolvedSymbol, false));
                }
            }
        }
    } else if (auto m = e->asMember()) {
        if (auto obj = m->object()) {
            collectFromExpression(obj->node, source, analysis, out);
        }
        if (auto* info = analysis.find(m->node.greenNode())) {
            if (auto nameTok = m->memberName()) {
                if (info->resolvedMethodSymbol) {
                    emitTokenAt(out, source, *nameTok, StMethod);
                } else if (info->resolvedType) {
                    emitTokenAt(out, source, *nameTok, StProperty);
                }
            }
        }
    } else if (auto nw = e->asNew()) {
        if (auto t = nw->typeName()) {
            ::Type* resolved = analysis.typeOf(nw->node.greenNode());
            emitTokenAt(out, source, *t, typeForType(resolved));
        }
        if (auto tr = nw->typeReference()) {
            for (auto& arg : tr->typeArguments()) collectFromTypeReference(arg, source, analysis, out);
        }
        for (auto& arg : nw->arguments()) {
            collectFromExpression(arg.node, source, analysis, out);
        }
    } else if (auto c = e->asCall()) {
        if (auto callee = c->callee()) {
            collectFromExpression(callee->node, source, analysis, out);
        }
        for (auto& arg : c->typeArguments()) collectFromTypeReference(arg, source, analysis, out);
        for (auto& arg : c->arguments()) {
            collectFromExpression(arg.node, source, analysis, out);
        }
    } else if (auto ca = e->asCast()) {
        if (auto src = ca->source()) collectFromExpression(src->node, source, analysis, out);
        if (auto tr = ca->targetType()) collectFromTypeReference(*tr, source, analysis, out);
    } else {
        for (auto& child : node.children()) {
            collectFromExpression(child, source, analysis, out);
        }
    }
}

void collectFromTypeReference(const ast::TypeReference& tr, const SourceFile& source,
                              const AnalysisResult& analysis,
                              std::vector<SemanticTokenEntry>& out) {
    auto nameTok = tr.nameToken();
    if (!nameTok) return;
    if (nameTok->kind() != SyntaxKind::Identifier) return;  // primitive keywords handled by TextMate
    ::Type* resolved = analysis.typeOf(tr.node.greenNode());
    emitTokenAt(out, source, *nameTok, typeForType(resolved));
    for (auto& arg : tr.typeArguments()) collectFromTypeReference(arg, source, analysis, out);
}

void collectFromParameter(const ast::Parameter& p, const SourceFile& source,
                          const AnalysisResult& analysis,
                          std::vector<SemanticTokenEntry>& out) {
    if (auto tr = p.typeReference()) {
        collectFromTypeReference(*tr, source, analysis, out);
    }
    if (auto nameTok = p.nameToken()) {
        emitTokenAt(out, source, *nameTok, StParameter);
    }
    if (auto dv = p.defaultValue()) {
        if (auto expr = dv->expression()) collectFromExpression(expr->node, source, analysis, out);
    }
}

void collectFromStatement(const SyntaxNode& node, const SourceFile& source,
                          const AnalysisResult& analysis,
                          std::vector<SemanticTokenEntry>& out) {
    auto stmt = ast::Statement::cast(node);
    if (!stmt) {
        for (auto& c : node.children()) collectFromStatement(c, source, analysis, out);
        return;
    }
    if (auto b = stmt->asBlock()) {
        for (auto& s : b->statements()) collectFromStatement(s.node, source, analysis, out);
        return;
    }
    if (auto l = stmt->asLet()) {
        if (auto nameTok = l->nameToken()) emitTokenAt(out, source, *nameTok, StVariable);
        if (auto init = l->initializer()) collectFromExpression(init->node, source, analysis, out);
        return;
    }
    if (auto v = stmt->asTypedVarDecl()) {
        if (auto tr = v->typeReference()) collectFromTypeReference(*tr, source, analysis, out);
        if (auto nameTok = v->nameToken()) emitTokenAt(out, source, *nameTok, StVariable);
        if (auto init = v->initializer()) collectFromExpression(init->node, source, analysis, out);
        return;
    }
    if (auto i = stmt->asIf()) {
        if (auto cond = i->condition()) collectFromExpression(cond->node, source, analysis, out);
        if (auto b = i->thenBlock())     collectFromStatement(b->node, source, analysis, out);
        if (auto ec = i->elseClause()) {
            if (auto innerIf = ec->ifStatement()) collectFromStatement(innerIf->node, source, analysis, out);
            else if (auto b = ec->block())        collectFromStatement(b->node, source, analysis, out);
        }
        return;
    }
    if (auto w = stmt->asWhile()) {
        if (auto cond = w->condition()) collectFromExpression(cond->node, source, analysis, out);
        if (auto b = w->body())         collectFromStatement(b->node, source, analysis, out);
        return;
    }
    if (auto r = stmt->asReturn()) {
        if (auto v = r->value()) collectFromExpression(v->node, source, analysis, out);
        return;
    }
    if (auto e = stmt->asExpressionStmt()) {
        if (auto x = e->expression()) collectFromExpression(x->node, source, analysis, out);
        return;
    }
}

void collectFromFunction(const ast::FuncDecl& fn, bool isMember, const SourceFile& source,
                         const AnalysisResult& analysis,
                         std::vector<SemanticTokenEntry>& out) {
    if (auto nameTok = fn.nameToken()) {
        emitTokenAt(out, source, *nameTok, isMember ? StMethod : StFunction);
    }
    for (auto& p : fn.parameters()) {
        collectFromParameter(p, source, analysis, out);
    }
    if (auto rt = fn.returnType()) {
        if (auto tr = rt->typeReference()) collectFromTypeReference(*tr, source, analysis, out);
    }
    if (auto body = fn.body()) {
        collectFromStatement(body->node, source, analysis, out);
    }
}

void collectFromField(const ast::FieldDecl& f, const SourceFile& source,
                      const AnalysisResult& analysis,
                      std::vector<SemanticTokenEntry>& out) {
    if (auto tr = f.typeReference()) collectFromTypeReference(*tr, source, analysis, out);
    if (auto nameTok = f.nameToken()) emitTokenAt(out, source, *nameTok, StProperty);
}

std::vector<uint32_t> encodeAsLspData(std::vector<SemanticTokenEntry> entries) {
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
        if (a.line != b.line) return a.line < b.line;
        return a.startChar < b.startChar;
    });
    std::vector<uint32_t> data;
    data.reserve(entries.size() * 5);
    uint32_t prevLine = 0, prevChar = 0;
    for (const auto& e : entries) {
        uint32_t deltaLine = e.line - prevLine;
        uint32_t deltaChar = (deltaLine == 0) ? e.startChar - prevChar : e.startChar;
        data.push_back(deltaLine);
        data.push_back(deltaChar);
        data.push_back(e.length);
        data.push_back(e.tokenType);
        data.push_back(e.tokenModifiers);
        prevLine = e.line;
        prevChar = e.startChar;
    }
    return data;
}

}  // namespace

namespace {

std::string formatMethodSignature(const Symbol& sym) {
    std::string s = "(";
    for (size_t i = 0; i < sym.paramTypes.size(); ++i) {
        if (i) s += ", ";
        s += sym.paramTypes[i] ? sym.paramTypes[i]->toString() : std::string("?");
    }
    s += ") -> ";
    s += sym.returnType ? sym.returnType->toString() : std::string("void");
    return s;
}

struct MemberContext {
    ::Type* receiverType = nullptr;
    bool receiverIsThis = false;
};

MemberContext findMemberContext(const SyntaxNode& root, uint32_t offset,
                                 const AnalysisResult& analysis) {
    auto chainAt = [&](uint32_t pos) -> MemberContext {
        MemberContext ctx;
        auto chain = ancestorChainAt(root, pos);
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            SyntaxKind ck = it->kind();
            std::optional<ast::Expression> obj;
            if (ck == SyntaxKind::MemberExpr) {
                if (auto m = ast::MemberExpression::cast(*it)) obj = m->object();
            } else if (ck == SyntaxKind::SafeMemberExpr) {
                if (auto sm = ast::SafeMemberExpression::cast(*it)) obj = sm->object();
            } else {
                continue;
            }
            if (!obj) continue;
            ::Type* recvType = analysis.typeOf(obj->node.greenNode());
            if (ck == SyntaxKind::SafeMemberExpr && recvType && recvType->isOptional()) {
                recvType = recvType->inner;
            }
            ctx.receiverType = recvType;
            ctx.receiverIsThis = (obj->kind() == SyntaxKind::ThisExpr);
            return ctx;
        }
        return ctx;
    };
    auto ctx = chainAt(offset);
    if (ctx.receiverType) return ctx;
    if (offset > 0) return chainAt(offset - 1);
    return ctx;
}

}  // namespace

lsp::TextDocument_CompletionResult LanguageServer::onCompletion(lsp::CompletionParams&& p) {
    auto* doc = documents.find(p.textDocument.uri.toString());
    if (!doc) return nullptr;

    int line1 = p.position.line + 1;
    int col1 = p.position.character + 1;
    uint32_t offset = doc->sourceFile().positionToOffset(line1, col1);

    const auto& analysis = doc->analyzer().result();
    MemberContext mctx = findMemberContext(doc->root(), offset, analysis);
    if (!mctx.receiverType || mctx.receiverType->isError() || !mctx.receiverType->structInfo) {
        return nullptr;
    }

    std::vector<lsp::CompletionItem> items;
    const auto& info = *mctx.receiverType->structInfo;
    items.reserve(info.fields.size() + info.methods.size());

    if (mctx.receiverType->isEnum()) {
        for (const auto& m : info.enumMembers) {
            lsp::CompletionItem item;
            item.label = utf16To8(m.name);
            item.kind = lsp::CompletionItemKindEnum(lsp::CompletionItemKind::EnumMember);
            item.detail = mctx.receiverType->toString();
            items.push_back(std::move(item));
        }
        return std::move(items);
    }

    auto isVisible = [&](Visibility v) {
        if (v == Visibility::Private) return mctx.receiverIsThis;
        return true;
    };

    for (const auto& f : info.fields) {
        if (!isVisible(f.visibility)) continue;
        lsp::CompletionItem item;
        item.label = utf16To8(f.name);
        item.kind = lsp::CompletionItemKindEnum(lsp::CompletionItemKind::Field);
        if (f.type) item.detail = f.type->toString();
        items.push_back(std::move(item));
    }
    for (const auto& m : info.methods) {
        if (m.name == info.name) continue;  // constructor - only callable via `new`
        if (!isVisible(m.visibility)) continue;
        lsp::CompletionItem item;
        item.label = utf16To8(m.name);
        item.kind = lsp::CompletionItemKindEnum(lsp::CompletionItemKind::Method);
        if (m.symbol) item.detail = formatMethodSignature(*m.symbol);
        items.push_back(std::move(item));
    }
    return std::move(items);
}

lsp::TextDocument_SemanticTokens_FullResult LanguageServer::onSemanticTokensFull(
        lsp::SemanticTokensParams&& p) {
    auto* doc = documents.find(p.textDocument.uri.toString());
    if (!doc) return nullptr;

    auto sf = ast::SourceFile::cast(doc->root());
    if (!sf) return nullptr;

    std::vector<SemanticTokenEntry> entries;
    const auto& analysis = doc->analyzer().result();
    const SourceFile& source = doc->sourceFile();

    for (auto& fn : sf->functions()) {
        collectFromFunction(fn, /*isMember*/ false, source, analysis, entries);
    }
    for (auto& td : sf->tests()) {
        if (auto body = td.body()) {
            collectFromStatement(body->node, source, analysis, entries);
        }
    }
    for (auto& sd : sf->structs()) {
        if (auto t = sd.nameToken()) emitTokenAt(entries, source, *t, StStruct);
        for (auto& f : sd.fields())  collectFromField(f, source, analysis, entries);
        for (auto& m : sd.methods()) collectFromFunction(m, /*isMember*/ true, source, analysis, entries);
    }
    for (auto& cd : sf->classes()) {
        if (auto t = cd.nameToken()) emitTokenAt(entries, source, *t, StClass);
        for (auto& f : cd.fields())  collectFromField(f, source, analysis, entries);
        for (auto& m : cd.methods()) collectFromFunction(m, /*isMember*/ true, source, analysis, entries);
    }

    lsp::SemanticTokens result;
    result.data = encodeAsLspData(std::move(entries));
    return result;
}

lsp::TextDocument_DocumentSymbolResult LanguageServer::onDocumentSymbol(
        lsp::DocumentSymbolParams&& p) {
    auto* doc = documents.find(p.textDocument.uri.toString());
    if (!doc) return nullptr;

    auto sf = ast::SourceFile::cast(doc->root());
    if (!sf) return nullptr;

    std::vector<lsp::DocumentSymbol> symbols;
    for (const auto& fn : sf->functions()) {
        symbols.push_back(buildFunctionSymbol(doc->sourceFile(), fn));
    }
    for (const auto& td : sf->tests()) {
        lsp::DocumentSymbol sym;
        auto desc = td.descriptionText();
        sym.name = "test \"" + (desc ? utf16To8(*desc) : std::string("<missing>")) + "\"";
        sym.kind = lsp::SymbolKindEnum(lsp::SymbolKind::Function);
        sym.range = toLspRange(doc->sourceFile(), td.node);
        if (auto t = td.descriptionToken()) {
            sym.selectionRange = toLspRange(doc->sourceFile(), *t);
        } else {
            sym.selectionRange = sym.range;
        }
        symbols.push_back(std::move(sym));
    }
    for (const auto& sd : sf->structs()) {
        symbols.push_back(buildRecordSymbol(doc->sourceFile(),
                                            sd.nameText(), sd.nameToken(), sd.node,
                                            lsp::SymbolKind::Struct,
                                            sd.fields(), sd.methods()));
    }
    for (const auto& cd : sf->classes()) {
        symbols.push_back(buildRecordSymbol(doc->sourceFile(),
                                            cd.nameText(), cd.nameToken(), cd.node,
                                            lsp::SymbolKind::Class,
                                            cd.fields(), cd.methods()));
    }
    return std::move(symbols);
}
