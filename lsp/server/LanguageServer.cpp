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
    auto [start, length] = node.contentRange();
    return toLspRange(source, start, start + length);
}

lsp::Range nameRangeAt(int line1, int col1, size_t nameLength) {
    lsp::Range r;
    r.start.line = line1 > 0 ? line1 - 1 : 0;
    r.start.character = col1 > 0 ? col1 - 1 : 0;
    r.end.line = r.start.line;
    r.end.character = r.start.character + static_cast<int>(nameLength);
    return r;
}

// A caret sitting immediately after an identifier (before `;`, `)`, a space, ...)
// should resolve that identifier, matching how editors treat word boundaries.
uint32_t preferIdentifierToTheLeft(const SyntaxNode& root, uint32_t offset) {
    auto at = root.tokenAtOffset(offset);
    if ((at && at->kind() == SyntaxKind::Identifier) || offset == 0) return offset;
    auto left = root.tokenAtOffset(offset - 1);
    if (left && left->kind() == SyntaxKind::Identifier) return offset - 1;
    return offset;
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

// Node kinds that name or evaluate to something with a declaration. Hover and
// go-to-definition only act on these; walking further up the chain would
// misattribute the enclosing statement or declaration to the cursor position.
bool isReferenceKind(SyntaxKind k) {
    return k == SyntaxKind::IdentExpr || k == SyntaxKind::ThisExpr ||
           k == SyntaxKind::SuperExpr || k == SyntaxKind::MemberExpr ||
           k == SyntaxKind::SafeMemberExpr || k == SyntaxKind::TypeRef ||
           k == SyntaxKind::NewExpr;
}

StructInfo* structInfoOf(Type* t) {
    if (!t) return nullptr;
    if (t->isOptional() && t->inner) t = t->inner;
    return t->structInfo;
}

// The named type a type reference denotes, peeling array and optional suffixes so
// go-to-definition and rename on `T[]`, `T?`, or `T[]?` all target T's declaration.
// Distinct from structInfoOf, which keeps a receiver's own type (e.g. `array.length`).
StructInfo* namedTypeStruct(Type* t) {
    while (t && (t->isArray() || t->isOptional()) && t->inner) t = t->inner;
    return t ? t->structInfo : nullptr;
}

std::string formatFunctionSignature(const Symbol& s) {
    std::string r = (s.methodOwner ? "method " : "function ") + utf16To8(s.name) + "(";
    for (size_t i = 0; i < s.paramTypes.size(); ++i) {
        if (i) r += ", ";
        r += s.paramTypes[i]->toString();
    }
    r += ") -> ";
    r += s.returnType ? s.returnType->toString() : "void";
    return r;
}

std::string formatHoverFor(const SyntaxNode& node, const ResolutionInfo& info) {
    SyntaxKind k = node.kind();
    bool identLike = k == SyntaxKind::IdentExpr || k == SyntaxKind::ThisExpr || node.isToken();
    if (identLike && info.resolvedSymbol) {
        const Symbol& s = *info.resolvedSymbol;
        if (s.kind == SymbolKind::Namespace) {
            return "(namespace) " + utf16To8(s.namespaceModulePath);
        }
        if (s.kind == SymbolKind::Function) {
            return formatFunctionSignature(s);
        }
        if (s.type) {
            const char* label = k == SyntaxKind::ThisExpr ? "this"
                : s.kind == SymbolKind::Parameter ? "parameter" : "value";
            return "(" + std::string(label) + ") : " + s.type->toString();
        }
    }
    if ((k == SyntaxKind::MemberExpr || k == SyntaxKind::SafeMemberExpr) &&
        info.resolvedMethodSymbol) {
        return formatFunctionSignature(*info.resolvedMethodSymbol);
    }
    if (info.resolvedType && !info.resolvedType->isError()) {
        return "type: " + info.resolvedType->toString();
    }
    return {};
}

// Hover on the name inside a declaration: the declaration node carries the
// symbol (or type) it introduces, keyed by matching the token text.
std::string declarationHoverText(const AnalysisResult& analysis,
                                 const std::vector<SyntaxNode>& chain) {
    if (chain.empty() || !chain.back().isToken()) return {};
    std::u16string_view name = chain.back().tokenText();
    if (name.empty()) return {};
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        if (it->isToken()) continue;
        const ResolutionInfo* info = lookupResolution(analysis, *it);
        if (info && info->resolvedSymbol && info->resolvedSymbol->name == name) {
            const Symbol& s = *info->resolvedSymbol;
            if (s.kind == SymbolKind::Function) return formatFunctionSignature(s);
            if (s.type) {
                const char* label = s.kind == SymbolKind::Parameter ? "parameter" : "value";
                return "(" + std::string(label) + ") : " + s.type->toString();
            }
        }
        if (it->kind() == SyntaxKind::FieldDecl) {
            for (auto up = it; up != chain.rend(); ++up) {
                const ResolutionInfo* ownerInfo = lookupResolution(analysis, *up);
                StructInfo* si = ownerInfo ? structInfoOf(ownerInfo->resolvedType) : nullptr;
                if (!si) continue;
                int fieldIndex = si->findFieldIndex(std::u16string(name));
                if (fieldIndex >= 0 && si->fields[fieldIndex].type) {
                    return "(field) : " + si->fields[fieldIndex].type->toString();
                }
                break;
            }
        }
        if (info && info->resolvedType) {
            StructInfo* si = structInfoOf(info->resolvedType);
            if (si && si->name == name) {
                return "type: " + info->resolvedType->toString();
            }
        }
    }
    return {};
}

struct DefinitionTarget {
    std::u16string modulePath;  // empty = the open document's own module
    int line = 0;               // 1-based; 0 = start of the target file
    int column = 0;
    size_t nameLength = 0;      // UTF-16 length of the declared name at (line, column)
};

DefinitionTarget targetForSymbol(const Symbol& s) {
    DefinitionTarget t;
    if (s.methodOwner) t.modulePath = s.methodOwner->modulePath;
    t.line = s.line;
    t.column = s.column;
    t.nameLength = s.name.size();
    return t;
}

// Field or method declaration position, searched by member name on the
// receiver's type (base fields are flattened in; methods walk the base chain).
std::optional<DefinitionTarget> memberTarget(StructInfo* si, const std::u16string& name) {
    if (!si) return std::nullopt;
    int fieldIndex = si->findFieldIndex(name);
    if (fieldIndex >= 0) {
        const FieldInfo& f = si->fields[fieldIndex];
        DefinitionTarget t;
        t.modulePath = f.definingClass ? f.definingClass->modulePath : si->modulePath;
        t.line = f.line;
        t.column = f.column;
        t.nameLength = f.name.size();
        return t;
    }
    if (StructInfo* declaring = si->classDeclaringMethod(name)) {
        const MethodInfo& m = declaring->methods[declaring->findMethodIndex(name)];
        DefinitionTarget t;
        t.modulePath = m.definingClass ? m.definingClass->modulePath : declaring->modulePath;
        if (m.symbol) {
            t.line = m.symbol->line;
            t.column = m.symbol->column;
        }
        t.nameLength = m.name.size();
        return t;
    }
    return std::nullopt;
}

std::optional<DefinitionTarget> resolveDefinitionTarget(const AnalysisResult& analysis,
                                                        const SyntaxNode& node) {
    const ResolutionInfo* info = lookupResolution(analysis, node);
    SyntaxKind k = node.kind();

    if (k == SyntaxKind::MemberExpr || k == SyntaxKind::SafeMemberExpr) {
        std::optional<ast::Expression> object;
        std::optional<std::u16string> memberName;
        if (auto member = ast::MemberExpression::cast(node)) {
            object = member->object();
            memberName = member->memberText();
        } else if (auto safeMember = ast::SafeMemberExpression::cast(node)) {
            object = safeMember->object();
            memberName = safeMember->memberText();
        }
        if (object) {
            // Namespace-qualified member `ns.X`: jump into the imported module.
            if (auto idObj = object->asIdent()) {
                if (auto* objInfo = lookupResolution(analysis, idObj->node)) {
                    Symbol* nsSym = objInfo->resolvedSymbol;
                    if (nsSym && nsSym->kind == SymbolKind::Namespace) {
                        DefinitionTarget t;
                        t.modulePath = nsSym->namespaceModulePath;
                        if (info) {
                            Symbol* s = info->resolvedMethodSymbol ? info->resolvedMethodSymbol
                                                                   : info->resolvedSymbol;
                            StructInfo* si = info->resolvedType ? structInfoOf(info->resolvedType) : nullptr;
                            if (s) {
                                t.line = s->line;
                                t.column = s->column;
                                t.nameLength = s->name.size();
                            } else if (si) {
                                t.line = si->line;
                                t.column = si->column;
                                t.nameLength = si->name.size();
                            }
                        }
                        return t;
                    }
                }
            }
            if (memberName) {
                StructInfo* receiver = structInfoOf(analysis.typeOf(object->node.greenNode()));
                if (auto t = memberTarget(receiver, *memberName)) return t;
            }
        }
    }

    if (!info) return std::nullopt;

    // `this` and `super` resolve to the synthetic this-symbol; the useful
    // definition is the class (respectively base class) declaration instead.
    if (k == SyntaxKind::ThisExpr || k == SyntaxKind::SuperExpr) {
        if (StructInfo* si = structInfoOf(info->resolvedType)) {
            DefinitionTarget t;
            t.modulePath = si->modulePath;
            t.line = si->line;
            t.column = si->column;
            t.nameLength = si->name.size();
            return t;
        }
    }

    if (info->resolvedMethodSymbol) return targetForSymbol(*info->resolvedMethodSymbol);
    if (info->resolvedSymbol) {
        const Symbol& s = *info->resolvedSymbol;
        if (s.kind == SymbolKind::Namespace) {
            DefinitionTarget t;
            t.modulePath = s.namespaceModulePath;
            return t;
        }
        return targetForSymbol(s);
    }
    if (StructInfo* si = namedTypeStruct(info->resolvedType)) {
        DefinitionTarget t;
        t.modulePath = si->modulePath;
        t.line = si->line;
        t.column = si->column;
        t.nameLength = si->name.size();
        return t;
    }
    return std::nullopt;
}

// lsp::FileUri::fromPath normalizes to native separators, which produces
// percent-encoded backslashes on Windows; build the URI from the generic
// (forward-slash) form instead.
lsp::DocumentUri uriForFilePath(const std::filesystem::path& path) {
    auto generic = path.generic_u8string();
    static const char* hex = "0123456789ABCDEF";
    std::string encoded = "file:///";
    for (char8_t unit : generic) {
        auto c = static_cast<unsigned char>(unit);
        bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                          (c >= '0' && c <= '9') || c == '-' || c == '.' ||
                          c == '_' || c == '~' || c == '/';
        if (unreserved) {
            encoded += static_cast<char>(c);
        } else {
            encoded += '%';
            encoded += hex[c >> 4];
            encoded += hex[c & 0xF];
        }
    }
    return lsp::Uri::parse(encoded);
}

// Definition invoked on the name inside a declaration: a this-field parameter
// navigates to the field it initializes; other declarations resolve to themselves.
std::optional<DefinitionTarget> resolveDeclarationTarget(const AnalysisResult& analysis,
                                                         const std::vector<SyntaxNode>& chain) {
    if (chain.empty() || !chain.back().isToken()) return std::nullopt;
    std::u16string_view name = chain.back().tokenText();
    if (name.empty()) return std::nullopt;
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        if (it->isToken()) continue;
        const ResolutionInfo* info = lookupResolution(analysis, *it);
        SyntaxKind k = it->kind();
        if (info && info->resolvedSymbol && info->resolvedSymbol->name == name) {
            const Symbol& s = *info->resolvedSymbol;
            if (s.thisFieldOwner) {
                if (auto t = memberTarget(s.thisFieldOwner, std::u16string(name))) return t;
            }
            if (s.isTypeName) {
                if (StructInfo* si = structInfoOf(s.type)) {
                    DefinitionTarget t;
                    t.modulePath = si->modulePath;
                    t.line = si->line;
                    t.column = si->column;
                    t.nameLength = si->name.size();
                    return t;
                }
            }
            return targetForSymbol(s);
        }
        if (k == SyntaxKind::FieldDecl) {
            for (auto up = it; up != chain.rend(); ++up) {
                const ResolutionInfo* ownerInfo = lookupResolution(analysis, *up);
                StructInfo* si = ownerInfo ? structInfoOf(ownerInfo->resolvedType) : nullptr;
                if (!si) continue;
                if (auto t = memberTarget(si, std::u16string(name))) return t;
                break;
            }
        }
        if (info && info->resolvedType) {
            StructInfo* si = structInfoOf(info->resolvedType);
            if (si && si->name == name) {
                DefinitionTarget t;
                t.modulePath = si->modulePath;
                t.line = si->line;
                t.column = si->column;
                t.nameLength = si->name.size();
                return t;
            }
        }
    }
    return std::nullopt;
}

lsp::DocumentUri uriForModuleFile(const Document& doc, const std::filesystem::path& absolute) {
    if (absolute.empty()) return lsp::Uri::parse(doc.uri());
    std::error_code ec;
    bool sameFile = absolute == doc.path() ||
                    std::filesystem::equivalent(absolute, doc.path(), ec);
    if (sameFile) return lsp::Uri::parse(doc.uri());
    return uriForFilePath(absolute);
}

lsp::DocumentUri definitionUri(const Document& doc, const std::u16string& modulePath) {
    if (!modulePath.empty()) {
        const auto& files = doc.moduleFiles();
        auto it = files.find(modulePath);
        if (it != files.end()) {
            std::error_code ec;
            bool sameFile = it->second == doc.path() ||
                            std::filesystem::equivalent(it->second, doc.path(), ec);
            if (!sameFile) return uriForFilePath(it->second);
        }
    }
    return lsp::Uri::parse(doc.uri());
}

// ===== References and rename =====
//
// A reference search resolves the position to the declared entity (a symbol, a
// type, or a field), then finds every identifier in the module graph whose own
// resolution reaches the same entity.

struct Entity {
    Symbol* symbol = nullptr;
    StructInfo* type = nullptr;
    StructInfo* fieldOwner = nullptr;
    std::u16string fieldName;

    bool valid() const { return symbol || type || fieldOwner; }
    bool operator==(const Entity& other) const {
        return symbol == other.symbol && type == other.type &&
               fieldOwner == other.fieldOwner && fieldName == other.fieldName;
    }
};

Entity typeEntity(StructInfo* si) {
    Entity e;
    e.type = si && si->templateOf ? si->templateOf : si;
    return e;
}

std::optional<Entity> memberEntity(StructInfo* si, const std::u16string& name);

// An override chain is one entity: root a method symbol at the base-most class
// (or interface) declaration with the same signature, so renaming any link
// renames them all.
Symbol* baseMostMethodSymbol(Symbol* s) {
    StructInfo* owner = s->methodOwner;
    if (!owner) return s;
    for (StructInfo* base = owner->baseInfo; base; base = base->baseInfo) {
        if (base->findMethodIndexBySignature(s->name, s) >= 0) owner = base;
    }
    for (StructInfo* c = owner; c; c = c->baseInfo) {
        for (::Type* interfaceType : c->implementedInterfaces) {
            StructInfo* interfaceInfo = interfaceType ? interfaceType->structInfo : nullptr;
            if (!interfaceInfo) continue;
            int index = interfaceInfo->findMethodIndexBySignature(s->name, s);
            if (index >= 0 && interfaceInfo->methods[index].symbol) {
                return interfaceInfo->methods[index].symbol;
            }
        }
    }
    int index = owner->findMethodIndexBySignature(s->name, s);
    if (index >= 0 && owner->methods[index].symbol) return owner->methods[index].symbol;
    return s;
}

// Constructors, imported type aliases, this-field parameters, and overrides all
// stand for another declaration; fold them so every spelling matches the same entity.
Entity entityForSymbol(Symbol* s) {
    if (!s) return {};
    if (s->isTypeName) {
        if (StructInfo* si = structInfoOf(s->type)) return typeEntity(si);
    }
    if (s->kind == SymbolKind::Function && s->methodOwner) {
        if (s->isConstructor) return typeEntity(s->methodOwner);
        s = baseMostMethodSymbol(s);
    }
    if (s->thisFieldOwner) {
        if (auto e = memberEntity(s->thisFieldOwner, s->name)) return *e;
    }
    Entity e;
    e.symbol = s;
    return e;
}

std::optional<Entity> memberEntity(StructInfo* si, const std::u16string& name) {
    if (!si) return std::nullopt;
    int fieldIndex = si->findFieldIndex(name);
    if (fieldIndex >= 0) {
        const FieldInfo& f = si->fields[fieldIndex];
        Entity e;
        e.fieldOwner = f.definingClass ? f.definingClass : si;
        e.fieldName = name;
        return e;
    }
    if (StructInfo* declaring = si->classDeclaringMethod(name)) {
        const MethodInfo& m = declaring->methods[declaring->findMethodIndex(name)];
        if (m.symbol) return entityForSymbol(m.symbol);
    }
    return std::nullopt;
}

Entity entityForNode(const AnalysisResult& analysis, const SyntaxNode& node) {
    const ResolutionInfo* info = lookupResolution(analysis, node);
    SyntaxKind k = node.kind();

    if (k == SyntaxKind::MemberExpr || k == SyntaxKind::SafeMemberExpr) {
        std::optional<ast::Expression> object;
        std::optional<std::u16string> memberName;
        if (auto member = ast::MemberExpression::cast(node)) {
            object = member->object();
            memberName = member->memberText();
        } else if (auto safeMember = ast::SafeMemberExpression::cast(node)) {
            object = safeMember->object();
            memberName = safeMember->memberText();
        }
        if (object) {
            if (auto idObj = object->asIdent()) {
                if (auto* objInfo = lookupResolution(analysis, idObj->node)) {
                    Symbol* nsSym = objInfo->resolvedSymbol;
                    if (nsSym && nsSym->kind == SymbolKind::Namespace) {
                        if (info) {
                            Symbol* s = info->resolvedMethodSymbol ? info->resolvedMethodSymbol
                                                                   : info->resolvedSymbol;
                            if (s) return entityForSymbol(s);
                            if (StructInfo* si = structInfoOf(info->resolvedType)) return typeEntity(si);
                        }
                        return {};
                    }
                }
            }
            if (memberName) {
                StructInfo* receiver = structInfoOf(analysis.typeOf(object->node.greenNode()));
                if (auto e = memberEntity(receiver, *memberName)) return *e;
            }
        }
    }

    if (!info) return {};
    if (info->resolvedMethodSymbol) return entityForSymbol(info->resolvedMethodSymbol);
    if (info->resolvedSymbol) return entityForSymbol(info->resolvedSymbol);
    // Bare tokens cover resolutions attached directly to name tokens (extends bases).
    if (node.isToken() || k == SyntaxKind::IdentExpr || k == SyntaxKind::TypeRef ||
        k == SyntaxKind::NewExpr) {
        if (StructInfo* si = namedTypeStruct(info->resolvedType)) return typeEntity(si);
    }
    return {};
}

Entity declarationEntityAt(const AnalysisResult& analysis, const std::vector<SyntaxNode>& chain) {
    if (chain.empty() || !chain.back().isToken()) return {};
    std::u16string_view name = chain.back().tokenText();
    if (name.empty()) return {};
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        if (it->isToken()) continue;
        const ResolutionInfo* info = lookupResolution(analysis, *it);
        if (info && info->resolvedSymbol && info->resolvedSymbol->name == name) {
            return entityForSymbol(info->resolvedSymbol);
        }
        if (it->kind() == SyntaxKind::FieldDecl) {
            for (auto up = it; up != chain.rend(); ++up) {
                const ResolutionInfo* ownerInfo = lookupResolution(analysis, *up);
                StructInfo* si = ownerInfo ? structInfoOf(ownerInfo->resolvedType) : nullptr;
                if (!si) continue;
                if (auto e = memberEntity(si, std::u16string(name))) return *e;
                break;
            }
        }
        if (info && info->resolvedType) {
            StructInfo* si = structInfoOf(info->resolvedType);
            if (si && si->name == name) return typeEntity(si);
        }
    }
    return {};
}

struct EntityAtResult {
    Entity entity;
    bool atDeclaration = false;
};

EntityAtResult entityAt(const SyntaxNode& root, const AnalysisResult& analysis, uint32_t offset) {
    auto chain = ancestorChainAt(root, offset);
    if (chain.empty() || !chain.back().isToken() || isTrivia(chain.back().kind())) return {};
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        if (!it->isToken() && !isReferenceKind(it->kind())) break;
        Entity e = entityForNode(analysis, *it);
        if (e.valid()) return {e, false};
    }
    Entity declared = declarationEntityAt(analysis, chain);
    return {declared, declared.valid()};
}

std::u16string entityName(const Entity& e) {
    if (e.symbol) return e.symbol->name;
    if (e.type) return e.type->name;
    return e.fieldName;
}

void collectIdentifierTokens(const SyntaxNode& node, std::u16string_view name,
                             std::vector<SyntaxNode>& out) {
    if (node.isToken()) {
        if (node.kind() == SyntaxKind::Identifier && node.tokenText() == name) out.push_back(node);
        return;
    }
    for (auto& c : node.children()) collectIdentifierTokens(c, name, out);
}

struct Occurrence {
    const ens::modules::Module* module = nullptr;
    lsp::Range range;
    bool atDeclaration = false;
};

std::vector<Occurrence> findOccurrencesInModules(
        const std::vector<std::unique_ptr<ens::modules::Module>>& modules, const Entity& target) {
    std::vector<Occurrence> out;
    std::u16string name = entityName(target);
    if (name.empty()) return out;
    for (const auto& modulePtr : modules) {
        const ens::modules::Module& m = *modulePtr;
        if (!m.rootNode || !m.analyzer || !m.source) continue;
        const AnalysisResult& analysis = m.analyzer->result();
        std::vector<SyntaxNode> tokens;
        collectIdentifierTokens(*m.rootNode, name, tokens);
        for (const auto& token : tokens) {
            auto resolved = entityAt(*m.rootNode, analysis, token.startOffset());
            if (resolved.entity == target) {
                out.push_back({&m, toLspRange(*m.source, token), resolved.atDeclaration});
            }
        }
    }
    return out;
}

const ens::modules::Module* moduleForPath(
        const std::vector<std::unique_ptr<ens::modules::Module>>& modules,
        const std::filesystem::path& path) {
    if (path.empty()) return nullptr;
    std::string key = ens::modules::overrideKey(path);
    for (const auto& m : modules) {
        if (m->rootNode && m->analyzer && m->source && !m->absolutePath.empty() &&
            ens::modules::overrideKey(m->absolutePath) == key) {
            return m.get();
        }
    }
    return nullptr;
}

bool isRenameableEntity(const Entity& e) {
    if (!e.valid()) return false;
    if (e.symbol) {
        const Symbol& s = *e.symbol;
        if (s.isBuiltin || s.isExternal || s.kind == SymbolKind::Namespace) return false;
    }
    return true;
}

bool isValidIdentifierName(const std::u16string& name) {
    auto isNameStart = [](char16_t c) {
        return (c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z') || c == u'_' || c == u'$';
    };
    auto isDigit = [](char16_t c) { return c >= u'0' && c <= u'9'; };
    if (name.empty() || !isNameStart(name.front())) return false;
    for (char16_t c : name) {
        if (!isNameStart(c) && !isDigit(c)) return false;
    }
    return keywordKindFromText(name) == SyntaxKind::Identifier;
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
    for (const auto& f : fields) {
        children.push_back(buildFieldSymbol(source, f));
    }
    for (const auto& m : methods) {
        bool isCtor = m.isConstructor();
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
    stOpts.legend.tokenModifiers = {"declaration"};
    stOpts.full = true;
    caps.semanticTokensProvider = std::move(stOpts);

    lsp::CompletionOptions cOpts;
    cOpts.triggerCharacters = std::vector<std::string>{"."};
    caps.completionProvider = std::move(cOpts);

    caps.referencesProvider = true;
    lsp::RenameOptions renameOptions;
    renameOptions.prepareProvider = true;
    caps.renameProvider = std::move(renameOptions);

    r.capabilities = std::move(caps);
    return r;
}

void LanguageServer::onInitialized() {
    // Watch the workspace's Ens files so edits that land on disk without an open
    // buffer (rename edits in closed files, external tools) refresh diagnostics.
    lsp::FileSystemWatcher watcher;
    watcher.globPattern = std::string("**/*.ens");
    lsp::DidChangeWatchedFilesRegistrationOptions watchOptions;
    watchOptions.watchers = {std::move(watcher)};
    lsp::Registration registration;
    registration.id = "ens-watched-files";
    registration.method = "workspace/didChangeWatchedFiles";
    registration.registerOptions = lsp::toJson(std::move(watchOptions));
    lsp::RegistrationParams params;
    params.registrations = {std::move(registration)};
    messages.sendRequest<lsp::requests::Client_RegisterCapability>(
        std::move(params), [](auto&&...) {}, [](const lsp::ResponseError&) {});
}

// Every open document analyzes its own module graph, so a change to one file can
// invalidate the diagnostics of any open document whose graph includes it.
void LanguageServer::refreshDocumentsDependingOn(
        const std::vector<std::filesystem::path>& changedPaths, const Document* skip) {
    std::vector<std::string> changedKeys;
    changedKeys.reserve(changedPaths.size());
    for (const auto& path : changedPaths) changedKeys.push_back(ens::modules::overrideKey(path));
    documents.forEachDocument([&](Document& doc) {
        if (&doc == skip) return;
        // Documents without a module graph (fallback analysis) are cheap; refresh them too.
        bool depends = doc.moduleFiles().empty();
        for (const auto& [modulePath, file] : doc.moduleFiles()) {
            if (depends) break;
            std::string key = ens::modules::overrideKey(file);
            depends = std::find(changedKeys.begin(), changedKeys.end(), key) != changedKeys.end();
        }
        if (!depends) return;
        doc.analyze();
        publishDiagnostics(doc);
    });
}

void LanguageServer::onDidOpen(lsp::notifications::TextDocument_DidOpen::Params&& p) {
    std::string uri = p.textDocument.uri.toString();
    documents.clearTransientOverride(Document::pathForUri(uri));
    auto& doc = documents.upsert(std::move(uri),
                                  utf8To16(p.textDocument.text),
                                  p.textDocument.version);
    publishDiagnostics(doc);
    refreshDocumentsDependingOn({doc.path()}, &doc);
}

void LanguageServer::onDidChange(lsp::notifications::TextDocument_DidChange::Params&& p) {
    if (p.contentChanges.empty()) return;
    const auto& last = p.contentChanges.back();
    if (auto* full = std::get_if<lsp::TextDocumentContentChangeEvent_Text>(&last)) {
        std::string uri = p.textDocument.uri.toString();
        documents.clearTransientOverride(Document::pathForUri(uri));
        auto& doc = documents.upsert(std::move(uri),
                                      utf8To16(full->text),
                                      p.textDocument.version);
        publishDiagnostics(doc);
        refreshDocumentsDependingOn({doc.path()}, &doc);
    }
}

void LanguageServer::onDidClose(lsp::notifications::TextDocument_DidClose::Params&& p) {
    std::string uri = p.textDocument.uri.toString();
    std::filesystem::path closedPath = Document::pathForUri(uri);
    documents.erase(uri);
    refreshDocumentsDependingOn({closedPath}, nullptr);
}

void LanguageServer::onDidChangeWatchedFiles(
        lsp::notifications::Workspace_DidChangeWatchedFiles::Params&& p) {
    if (p.changes.empty()) return;
    std::vector<std::filesystem::path> changedPaths;
    changedPaths.reserve(p.changes.size());
    for (const auto& change : p.changes) {
        std::filesystem::path path = Document::pathForUri(change.uri.toString());
        if (path.empty()) continue;
        documents.clearTransientOverride(path);
        changedPaths.push_back(std::move(path));
    }
    if (!changedPaths.empty()) refreshDocumentsDependingOn(changedPaths, nullptr);
}

// A companion editor plugin can report edits the IDE applied to documents that are
// not open in the LSP sense (rename workspace edits, undo in files without a tab);
// the content stays a transient override until a real buffer or the disk takes over.
void LanguageServer::onBackgroundDocumentChanged(lsp::json::Value&& params) {
    if (!params.isObject()) return;
    const lsp::json::Value* uri = params.object().find("uri");
    const lsp::json::Value* text = params.object().find("text");
    if (!uri || !uri->isString() || !text || !text->isString()) return;
    std::filesystem::path path = Document::pathForUri(uri->string());
    if (path.empty()) return;
    // Open buffers are authoritative; compare by path identity because the
    // reporting plugin and the LSP client may spell the same URI differently.
    std::string key = ens::modules::overrideKey(path);
    bool isOpenDocument = false;
    documents.forEachDocument([&](Document& doc) {
        if (!isOpenDocument && ens::modules::overrideKey(doc.path()) == key) isOpenDocument = true;
    });
    if (isOpenDocument) return;
    documents.setTransientOverride(path, utf8To16(text->string()));
    refreshDocumentsDependingOn({path}, nullptr);
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
    uint32_t offset = preferIdentifierToTheLeft(
        doc->root(), doc->sourceFile().positionToOffset(line1, col1));
    auto chain = ancestorChainAt(doc->root(), offset);

    if (!chain.empty() && chain.back().isToken() && isTrivia(chain.back().kind())) return nullptr;

    const auto& analysis = doc->analyzer().result();
    auto makeHover = [&](std::string text, const SyntaxNode& node) {
        lsp::Hover h;
        lsp::MarkupContent mc;
        mc.kind = lsp::MarkupKindEnum(lsp::MarkupKind::PlainText);
        mc.value = std::move(text);
        h.contents = std::move(mc);
        h.range = toLspRange(doc->sourceFile(), node);
        return h;
    };

    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        if (!it->isToken() && !isReferenceKind(it->kind())) break;
        if (auto* info = lookupResolution(analysis, *it)) {
            std::string text = formatHoverFor(*it, *info);
            if (!text.empty()) return makeHover(std::move(text), *it);
        }
    }
    std::string declText = declarationHoverText(analysis, chain);
    if (!declText.empty()) return makeHover(std::move(declText), chain.back());
    return nullptr;
}

lsp::TextDocument_DefinitionResult LanguageServer::onDefinition(lsp::DefinitionParams&& p) {
    auto* doc = documents.find(p.textDocument.uri.toString());
    if (!doc) return nullptr;

    int line1 = p.position.line + 1;
    int col1 = p.position.character + 1;
    uint32_t offset = preferIdentifierToTheLeft(
        doc->root(), doc->sourceFile().positionToOffset(line1, col1));
    auto chain = ancestorChainAt(doc->root(), offset);

    if (!chain.empty() && chain.back().isToken() && isTrivia(chain.back().kind())) return nullptr;

    const auto& analysis = doc->analyzer().result();
    auto makeLocation = [&](const DefinitionTarget& target) {
        lsp::Location loc;
        loc.uri = definitionUri(*doc, target.modulePath);
        loc.range = nameRangeAt(target.line, target.column, target.nameLength);
        return lsp::Definition{std::move(loc)};
    };

    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        if (!it->isToken() && !isReferenceKind(it->kind())) break;
        if (auto target = resolveDefinitionTarget(analysis, *it)) return makeLocation(*target);
    }
    if (auto target = resolveDeclarationTarget(analysis, chain)) return makeLocation(*target);
    return nullptr;
}

lsp::TextDocument_ReferencesResult LanguageServer::onReferences(lsp::ReferenceParams&& p) {
    auto* doc = documents.find(p.textDocument.uri.toString());
    if (!doc) return nullptr;

    int line1 = p.position.line + 1;
    int col1 = p.position.character + 1;
    uint32_t offset = preferIdentifierToTheLeft(
        doc->root(), doc->sourceFile().positionToOffset(line1, col1));

    // Search the file's workspace so reverse dependencies (files importing this
    // one) are covered; the document's own forward graph is the fallback.
    WorkspaceModules workspace = documents.buildWorkspaceModules(doc->path());
    const ens::modules::Module* requestModule = moduleForPath(workspace.modules, doc->path());

    Entity target;
    std::vector<Occurrence> occurrences;
    if (requestModule) {
        target = entityAt(*requestModule->rootNode, requestModule->analyzer->result(), offset).entity;
        if (!target.valid()) return nullptr;
        occurrences = findOccurrencesInModules(workspace.modules, target);
    } else {
        target = entityAt(doc->root(), doc->analyzer().result(), offset).entity;
        if (!target.valid()) return nullptr;
        occurrences = findOccurrencesInModules(doc->moduleList(), target);
    }

    lsp::Array<lsp::Location> locations;
    for (const auto& occurrence : occurrences) {
        if (!p.context.includeDeclaration && occurrence.atDeclaration) continue;
        lsp::Location loc;
        loc.uri = uriForModuleFile(*doc, occurrence.module->absolutePath);
        loc.range = occurrence.range;
        locations.push_back(std::move(loc));
    }
    return locations;
}

lsp::TextDocument_PrepareRenameResult LanguageServer::onPrepareRename(lsp::PrepareRenameParams&& p) {
    auto* doc = documents.find(p.textDocument.uri.toString());
    if (!doc) return nullptr;

    int line1 = p.position.line + 1;
    int col1 = p.position.character + 1;
    uint32_t offset = preferIdentifierToTheLeft(
        doc->root(), doc->sourceFile().positionToOffset(line1, col1));
    auto token = doc->root().tokenAtOffset(offset);
    if (!token || token->kind() != SyntaxKind::Identifier) return nullptr;
    Entity target = entityAt(doc->root(), doc->analyzer().result(), offset).entity;
    if (!isRenameableEntity(target)) return nullptr;

    lsp::PrepareRenameResult_Range_Placeholder result;
    result.range = toLspRange(doc->sourceFile(), *token);
    result.placeholder = utf16To8(std::u16string(token->tokenText()));
    return lsp::PrepareRenameResult(std::move(result));
}

lsp::TextDocument_RenameResult LanguageServer::onRename(lsp::RenameParams&& p) {
    auto* doc = documents.find(p.textDocument.uri.toString());
    if (!doc) return nullptr;

    std::u16string newName = utf8To16(p.newName);
    if (!isValidIdentifierName(newName)) {
        throw lsp::RequestError(-32602, "'" + p.newName + "' is not a valid Ens identifier");
    }

    int line1 = p.position.line + 1;
    int col1 = p.position.character + 1;
    uint32_t offset = preferIdentifierToTheLeft(
        doc->root(), doc->sourceFile().positionToOffset(line1, col1));

    WorkspaceModules workspace = documents.buildWorkspaceModules(doc->path());
    const ens::modules::Module* requestModule = moduleForPath(workspace.modules, doc->path());

    Entity target;
    std::vector<Occurrence> occurrences;
    if (requestModule) {
        target = entityAt(*requestModule->rootNode, requestModule->analyzer->result(), offset).entity;
        if (!isRenameableEntity(target)) return nullptr;
        occurrences = findOccurrencesInModules(workspace.modules, target);
    } else {
        target = entityAt(doc->root(), doc->analyzer().result(), offset).entity;
        if (!isRenameableEntity(target)) return nullptr;
        occurrences = findOccurrencesInModules(doc->moduleList(), target);
    }
    if (occurrences.empty()) return nullptr;

    lsp::Map<lsp::DocumentUri, lsp::Array<lsp::TextEdit>> changes;
    std::unordered_map<const ens::modules::Module*, std::vector<const Occurrence*>> occurrencesByModule;
    for (const auto& occurrence : occurrences) {
        lsp::TextEdit edit;
        edit.range = occurrence.range;
        edit.newText = p.newName;
        changes[uriForModuleFile(*doc, occurrence.module->absolutePath)].push_back(std::move(edit));
        occurrencesByModule[occurrence.module].push_back(&occurrence);
    }

    // The client applies the edit asynchronously and may keep the results in
    // unsaved buffers; install the post-edit contents as transient overrides so
    // analysis never sees a half-renamed module graph. The overrides drop out as
    // the client's buffers and the disk catch up.
    std::vector<std::filesystem::path> editedPaths;
    for (const auto& [module, moduleOccurrences] : occurrencesByModule) {
        if (module->absolutePath.empty()) continue;
        const SourceFile& source = *module->source;
        std::vector<std::pair<uint32_t, uint32_t>> spans;
        spans.reserve(moduleOccurrences.size());
        for (const Occurrence* occurrence : moduleOccurrences) {
            const lsp::Range& r = occurrence->range;
            spans.emplace_back(source.positionToOffset(r.start.line + 1, r.start.character + 1),
                               source.positionToOffset(r.end.line + 1, r.end.character + 1));
        }
        std::sort(spans.begin(), spans.end(),
                  [](const auto& a, const auto& b) { return a.first > b.first; });
        std::u16string text = source.getSource();
        for (const auto& [start, end] : spans) {
            text.replace(start, end - start, newName);
        }
        documents.setTransientOverride(module->absolutePath, std::move(text));
        editedPaths.push_back(module->absolutePath);
    }
    refreshDocumentsDependingOn(editedPaths, doc);

    lsp::WorkspaceEdit edit;
    edit.changes = std::move(changes);
    return edit;
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

// Indices into the SemanticTokensLegend.tokenModifiers array declared in onInitialize.
constexpr uint32_t StModifierDeclaration = 1u << 0;

void emitTokenAt(std::vector<SemanticTokenEntry>& out, const SourceFile& source,
                 const SyntaxNode& tokenNode, uint32_t tokenType, uint32_t tokenModifiers = 0) {
    if (tokenNode.length() == 0) return;
    auto [line, col] = source.offsetToPosition(tokenNode.startOffset());
    SemanticTokenEntry e;
    e.line = static_cast<uint32_t>(line - 1);
    e.startChar = static_cast<uint32_t>(col - 1);
    e.length = tokenNode.length();
    e.tokenType = tokenType;
    e.tokenModifiers = tokenModifiers;
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
    // Peel array/optional suffixes so `T[]` and `T?` color their name by T's kind,
    // matching a plain `T` reference.
    while (t && (t->isArray() || t->isOptional()) && t->inner) t = t->inner;
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
    } else if (auto lm = e->asLambda()) {
        for (auto& p : lm->parameters()) {
            if (auto tr = p.typeReference()) collectFromTypeReference(*tr, source, analysis, out);
            if (auto nameTok = p.nameToken()) {
                emitTokenAt(out, source, *nameTok, StParameter, StModifierDeclaration);
            }
        }
        if (auto body = lm->bodyExpr()) collectFromExpression(body->node, source, analysis, out);
        else if (auto block = lm->bodyBlockNode()) {
            collectFromStatement(*block, source, analysis, out);
        }
    } else {
        for (auto& child : node.children()) {
            collectFromExpression(child, source, analysis, out);
        }
    }
}

void collectFromTypeReference(const ast::TypeReference& tr, const SourceFile& source,
                              const AnalysisResult& analysis,
                              std::vector<SemanticTokenEntry>& out) {
    // A function type and a type in parentheses name nothing themselves; the types they
    // are written over do.
    if (tr.isFunctionType()) {
        for (auto& p : tr.parameterTypes()) collectFromTypeReference(p, source, analysis, out);
        if (auto returned = tr.returnedType()) {
            collectFromTypeReference(*returned, source, analysis, out);
        }
        return;
    }
    if (tr.isParenthesized()) {
        if (auto inner = tr.innerType()) collectFromTypeReference(*inner, source, analysis, out);
        return;
    }
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
        emitTokenAt(out, source, *nameTok, StParameter, StModifierDeclaration);
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
        if (auto nameTok = l->nameToken()) emitTokenAt(out, source, *nameTok, StVariable, StModifierDeclaration);
        if (auto init = l->initializer()) collectFromExpression(init->node, source, analysis, out);
        return;
    }
    if (auto v = stmt->asTypedVarDecl()) {
        if (auto tr = v->typeReference()) collectFromTypeReference(*tr, source, analysis, out);
        if (auto nameTok = v->nameToken()) emitTokenAt(out, source, *nameTok, StVariable, StModifierDeclaration);
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
        emitTokenAt(out, source, *nameTok, isMember ? StMethod : StFunction, StModifierDeclaration);
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
    if (auto nameTok = f.nameToken()) emitTokenAt(out, source, *nameTok, StProperty, StModifierDeclaration);
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
        if (m.isConstructor || m.isDestructor) continue;  // not callable directly
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
        if (auto t = sd.nameToken()) emitTokenAt(entries, source, *t, StStruct, StModifierDeclaration);
        for (auto& f : sd.fields())  collectFromField(f, source, analysis, entries);
        for (auto& m : sd.methods()) collectFromFunction(m, /*isMember*/ true, source, analysis, entries);
    }
    for (auto& cd : sf->classes()) {
        if (auto t = cd.nameToken()) emitTokenAt(entries, source, *t, StClass, StModifierDeclaration);
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
