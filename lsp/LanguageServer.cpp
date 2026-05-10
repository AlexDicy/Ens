#include "LanguageServer.h"

#include <utility>
#include <vector>

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
        if (info.resolvedSymbol && info.resolvedSymbol->type) {
            return "(" + std::string(k == SyntaxKind::ThisExpr ? "this" : "value") +
                   ") : " + info.resolvedSymbol->type->toString();
        }
        if (info.resolvedType) {
            return "type: " + info.resolvedType->toString();
        }
    }
    if (k == SyntaxKind::MemberExpr) {
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

lsp::InitializeResult LanguageServer::onInitialize(lsp::InitializeParams&&) {
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
    Symbol* target = nullptr;
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        if (auto* info = lookupResolution(analysis, *it)) {
            if (info->resolvedMethodSymbol) { target = info->resolvedMethodSymbol; break; }
            if (info->resolvedSymbol)       { target = info->resolvedSymbol; break; }
        }
    }
    if (!target) return nullptr;

    lsp::Location loc;
    loc.uri = lsp::Uri::parse(doc->uri());
    loc.range = zeroWidthRangeAt(target->line, target->column);
    return lsp::Definition{std::move(loc)};
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
