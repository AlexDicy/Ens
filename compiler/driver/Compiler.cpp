#include "Compiler.h"
#include "CodeGenerator.h"
#include "Linker.h"
#include "ast/Declaration.h"
#include "cst/SyntaxNode.h"
#include "diagnostics/Diagnostic.h"
#include "diagnostics/DiagnosticSink.h"
#include "diagnostics/SourceFile.h"
#include "parser/Parser.h"
#include "semantic/Analyzer.h"
#include "semantic/EscapeAnalyzer.h"
#include "semantic/Prelude.h"
#include "semantic/ThrowsAnalyzer.h"
#include "semantic/Symbol.h"
#include "semantic/Type.h"
#include "semantic/TypeContext.h"

#include <algorithm>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace fs = std::filesystem;

namespace {

struct Module {
    std::u16string modulePath;
    fs::path absolutePath;
    fs::path relativePath; // relative to source root
    std::unique_ptr<SourceFile> source;
    std::unique_ptr<DiagnosticSink> sink;
    GreenElementPtr cstRoot;
    std::unique_ptr<SyntaxNode> rootNode;
    std::unique_ptr<Analyzer> analyzer;
};

std::u16string toU16(std::string_view s) {
    std::u16string out;
    out.reserve(s.size());
    for (char c : s) out.push_back(static_cast<unsigned char>(c));
    return out;
}

std::string asAscii(std::u16string_view s)
{
    std::string r;
    r.reserve(s.size());
    for (char16_t c : s) r.push_back(c < 128 ? static_cast<char>(c) : '?');
    return r;
}

std::u16string modulePathOfRelative(const fs::path& relative) {
    fs::path no_ext = relative;
    no_ext.replace_extension();
    std::u16string out;
    bool first = true;
    for (const auto& part : no_ext) {
        std::string s = part.string();
        if (s.empty() || s == ".") continue;
        if (!first) out.push_back(u'.');
        out += toU16(s);
        first = false;
    }
    return out;
}

fs::path relativeFromModulePath(const std::u16string& modulePath) {
    fs::path rel;
    std::string segment;
    for (char16_t c : modulePath) {
        if (c == u'.') {
            if (!segment.empty()) { rel /= segment; segment.clear(); }
        } else {
            segment.push_back(c < 128 ? static_cast<char>(c) : '?');
        }
    }
    if (!segment.empty()) rel /= segment;
    if (!rel.empty()) rel.replace_extension(".ens");
    return rel;
}

std::string sanitizeForFilename(std::u16string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char16_t c : s) {
        if ((c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z') ||
            (c >= u'0' && c <= u'9') || c == u'_' || c == u'.') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('_');
        }
    }
    if (out.empty()) out = "module";
    return out;
}

bool readFileToU16(const fs::path& path, std::u16string& out) {
    std::ifstream f(path);
    if (!f) return false;
    std::string code((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    out.assign(code.begin(), code.end());
    return true;
}

std::unique_ptr<Module> loadModule(const fs::path& sourceRoot,
                                   const fs::path& relativePath,
                                   const std::u16string& modulePath) {
    fs::path absolute = sourceRoot / relativePath;
    std::u16string code;
    if (!readFileToU16(absolute, code)) {
        std::cerr << "ERROR: Couldn't read " << absolute << '\n';
        return nullptr;
    }
    auto m = std::make_unique<Module>();
    m->modulePath = modulePath;
    m->absolutePath = absolute;
    m->relativePath = relativePath;
    m->source = std::make_unique<SourceFile>(absolute.string(), std::move(code));
    m->sink = std::make_unique<DiagnosticSink>();
    Parser parser(m->source->getSource(), *m->sink);
    m->cstRoot = parser.parseSourceFile();
    m->rootNode = SyntaxNode::makeRoot(m->cstRoot.get());
    return m;
}

std::unique_ptr<Module> makeInMemoryModule(const std::u16string& modulePath,
                                           const std::string& filename, std::u16string code) {
    auto m = std::make_unique<Module>();
    m->modulePath = modulePath;
    m->absolutePath = filename;
    m->relativePath = filename;
    m->source = std::make_unique<SourceFile>(filename, std::move(code));
    m->sink = std::make_unique<DiagnosticSink>();
    Parser parser(m->source->getSource(), *m->sink);
    m->cstRoot = parser.parseSourceFile();
    m->rootNode = SyntaxNode::makeRoot(m->cstRoot.get());
    return m;
}

std::unique_ptr<Module> loadPreludeModule() {
    return makeInMemoryModule(std::u16string(kPreludeModulePath), "<prelude>",
                              std::u16string(kPreludeSource));
}

bool isStdlibPath(const std::u16string& modulePath) {
    static const std::u16string prefix = u"std.";
    return modulePath == u"std" ||
           (modulePath.size() >= prefix.size() &&
            modulePath.compare(0, prefix.size(), prefix) == 0);
}

// Locate the standard library root (the directory containing `std/`): an explicit
// ENS_STDLIB override, otherwise the nearest `libs/` walking up from the working
// directory. Empty if none is found.
fs::path findStdlibRoot() {
    std::error_code ec;
    if (const char* env = std::getenv("ENS_STDLIB")) {
        fs::path root(env);
        if (!root.empty() && fs::exists(root, ec)) return root;
    }
    fs::path dir = fs::current_path(ec);
    if (ec) return fs::path();
    for (fs::path d = dir;; d = d.parent_path()) {
        fs::path libs = d / "libs";
        if (fs::exists(libs / "std" / "system.ens", ec)) return libs;
        if (d == d.parent_path()) break;
    }
    return fs::path();
}

bool buildModuleGraph(const fs::path& sourceRoot,
                      const fs::path& stdlibRoot,
                      std::deque<fs::path>& seedRelatives,
                      std::vector<std::unique_ptr<Module>>& modulesOut,
                      std::unordered_map<std::u16string, Module*>& byPath) {
    struct WorkItem { fs::path base; fs::path rel; };
    std::unordered_set<std::u16string> queued;
    std::deque<WorkItem> work;

    auto enqueue = [&](const fs::path& base, const fs::path& rel) {
        std::u16string mp = modulePathOfRelative(rel);
        if (queued.count(mp)) return;
        queued.insert(mp);
        work.push_back({base, rel});
    };

    for (auto& r : seedRelatives) enqueue(sourceRoot, r);
    seedRelatives.clear();

    while (!work.empty()) {
        WorkItem item = work.front();
        work.pop_front();
        std::u16string mp = modulePathOfRelative(item.rel);

        auto module = loadModule(item.base, item.rel, mp);
        if (!module) return false;

        Module* raw = module.get();
        modulesOut.push_back(std::move(module));
        byPath.emplace(mp, raw);

        auto sf = ast::SourceFile::cast(*raw->rootNode);
        if (!sf) continue;
        for (auto& imp : sf->imports()) {
            if (imp.isPackage()) continue;  // diagnosed later by the analyzer
            std::u16string targetPath = imp.modulePath();
            if (queued.count(targetPath)) continue;
            fs::path targetRel = relativeFromModulePath(targetPath);
            if (targetRel.empty()) continue;
            const bool isStd = isStdlibPath(targetPath);
            const fs::path& base = isStd ? stdlibRoot : sourceRoot;
            fs::path absolute = base.empty() ? fs::path() : base / targetRel;
            if (base.empty() || !fs::exists(absolute)) {
                auto& sink = *raw->sink;
                auto [line, col] = raw->source->offsetToPosition(imp.node.startOffset());
                if (isStd) {
                    sink.error({line, col, 1},
                        "Cannot find standard library module '" + asAscii(targetPath) +
                        "'. Set ENS_STDLIB to the directory containing 'std/' (normally <repo>/libs).");
                } else {
                    sink.error({line, col, 1},
                        "Cannot find module '" + asAscii(targetPath) +
                        "' (looked for " + absolute.string() + ")");
                }
                continue;
            }
            enqueue(base, targetRel);
        }
    }
    return true;
}

std::string escapeKindStr(EscapeKind k) {
    switch (k) {
        case EscapeKind::Unknown:  return "Unknown";
        case EscapeKind::NoEscape: return "NoEscape";
        case EscapeKind::Escape:   return "Escape";
    }
    return "?";
}

std::string asciiOfU16(const std::u16string& s) {
    std::string r;
    r.reserve(s.size());
    for (char16_t c : s) r.push_back(c < 128 ? static_cast<char>(c) : '?');
    return r;
}

void printEscapeFactsForFunction(Symbol* sym, std::ostream& os) {
    if (!sym || !sym->escapeInfo.analyzed) return;
    os << asciiOfU16(sym->name) << "(";
    for (size_t i = 0; i < sym->paramTypes.size(); ++i) {
        if (i > 0) os << ", ";
        os << (sym->paramTypes[i] ? sym->paramTypes[i]->toString() : "<?>");
    }
    os << ")";
    const auto& ei = sym->escapeInfo;
    if (!ei.params.empty()) {
        os << " - ";
        for (size_t i = 0; i < ei.params.size(); ++i) {
            if (i > 0) os << ", ";
            os << "p" << i << "=" << escapeKindStr(ei.params[i]);
            if (i < ei.paramMutated.size() && ei.paramMutated[i]) os << "(mut)";
        }
    }
    os << "\n";
}

static void printPromotionsForStmt(const ast::Statement& s,
                                   const AnalysisResult& analysis,
                                   std::ostream& os);

static void printPromotionsForLocal(Symbol* sym, std::ostream& os) {
    if (!sym || !sym->stackPromoted) return;
    std::string typeStr = sym->type ? sym->type->toString() : std::string("?");
    os << "  stack-promoted: " << asciiOfU16(sym->name)
       << " : " << typeStr << "\n";
}

static void printPromotionsForStmt(const ast::Statement& s,
                                   const AnalysisResult& analysis,
                                   std::ostream& os) {
    if (auto b = s.asBlock()) {
        for (auto& child : b->statements()) printPromotionsForStmt(child, analysis, os);
        return;
    }
    if (auto l = s.asLet()) {
        if (auto* info = analysis.find(l->node.greenNode())) {
            printPromotionsForLocal(info->resolvedSymbol, os);
        }
        return;
    }
    if (auto v = s.asTypedVarDecl()) {
        if (auto* info = analysis.find(v->node.greenNode())) {
            printPromotionsForLocal(info->resolvedSymbol, os);
        }
        return;
    }
    if (auto i = s.asIf()) {
        if (auto t = i->thenBlock()) {
            for (auto& child : t->statements()) printPromotionsForStmt(child, analysis, os);
        }
        if (auto ec = i->elseClause()) {
            if (auto innerIf = ec->ifStatement()) {
                ast::Statement asStmt{innerIf->node};
                printPromotionsForStmt(asStmt, analysis, os);
            } else if (auto bb = ec->block()) {
                for (auto& child : bb->statements()) printPromotionsForStmt(child, analysis, os);
            }
        }
        return;
    }
    if (auto w = s.asWhile()) {
        if (auto body = w->body()) {
            for (auto& child : body->statements()) printPromotionsForStmt(child, analysis, os);
        }
        return;
    }
}

static void printPromotionsForFunction(const ast::FuncDecl& fn,
                                       const AnalysisResult& analysis,
                                       std::ostream& os) {
    if (auto body = fn.body()) {
        for (auto& s : body->statements()) printPromotionsForStmt(s, analysis, os);
    }
}

void printEscapeFacts(const std::vector<std::unique_ptr<Module>>& modules, std::ostream& os) {
    os << "=== escape analysis ===\n";
    for (auto& m : modules) {
        auto sf = ast::SourceFile::cast(*m->rootNode);
        if (!sf) continue;
        const auto& analysis = m->analyzer->result();
        auto emitFn = [&](const ast::FuncDecl& fn) {
            auto* info = analysis.find(fn.node.greenNode());
            if (info && info->resolvedSymbol) printEscapeFactsForFunction(info->resolvedSymbol, os);
            printPromotionsForFunction(fn, analysis, os);
        };
        for (auto& fn : sf->functions()) emitFn(fn);
        for (auto& sd : sf->structs()) for (auto& mm : sd.methods()) emitFn(mm);
        for (auto& cd : sf->classes()) for (auto& mm : cd.methods()) emitFn(mm);
    }
}

bool runMultiModuleAnalysis(std::vector<std::unique_ptr<Module>>& modules,
                            std::unordered_map<std::u16string, Module*>& byPath,
                            TypeContext& sharedCtx,
                            bool explainArc) {
    for (auto& m : modules) {
        m->analyzer = std::make_unique<Analyzer>(*m->source, *m->sink, sharedCtx, m->modulePath);
    }

    for (auto& m : modules) m->analyzer->registerNames(*m->rootNode);
    for (auto& m : modules) m->analyzer->importPrelude();

    ModuleResolver resolver = [&](const std::u16string& path) -> const Analyzer* {
        auto it = byPath.find(path);
        if (it == byPath.end()) return nullptr;
        return it->second->analyzer.get();
    };
    for (auto& m : modules) m->analyzer->bindImports(resolver);

    for (auto& m : modules) m->analyzer->resolveSignatures();

    // Lay out classes whole-program, bases before derived, so a class can extend
    // a class in another module (inherited fields + virtual slots resolve across
    // module boundaries). The prelude is modules[0], so it sorts first among roots.
    {
        struct ClassItem { Analyzer* owner; ast::ClassDecl decl; StructInfo* info; };
        std::vector<ClassItem> items;
        for (auto& m : modules) {
            auto sf = ast::SourceFile::cast(*m->rootNode);
            if (!sf) continue;
            for (auto& cd : sf->classes()) {
                Type* t = m->analyzer->result().typeOf(cd.node.greenNode());
                if (t && t->structInfo) items.push_back({ m->analyzer.get(), cd, t->structInfo });
            }
        }
        auto depthOf = [](StructInfo* si) {
            int d = 0;
            for (StructInfo* s = si->baseInfo; s; s = s->baseInfo) ++d;
            return d;
        };
        std::stable_sort(items.begin(), items.end(),
            [&](const ClassItem& a, const ClassItem& b) { return depthOf(a.info) < depthOf(b.info); });
        for (auto& it : items) it.owner->layoutOneClass(it.decl);
        std::vector<StructInfo*> infos;
        infos.reserve(items.size());
        for (auto& it : items) infos.push_back(it.info);
        Analyzer::finalizeClassHierarchy(infos);
    }

    for (auto& m : modules) m->analyzer->analyzeBodies();

    // Checked-exception throws-set fixpoint (a correctness pass: runs before the
    // error gate). Sets propagate cross-module via shared Symbol*.
    StructInfo* errorClass = nullptr;
    for (auto& m : modules) { errorClass = m->analyzer->errorClass(); if (errorClass) break; }
    std::vector<std::optional<ast::SourceFile>> throwsSourceFiles;
    std::vector<Module*> throwsModules;
    throwsSourceFiles.reserve(modules.size());
    for (auto& m : modules) {
        auto sf = ast::SourceFile::cast(*m->rootNode);
        if (!sf) continue;
        throwsSourceFiles.push_back(*sf);
        throwsModules.push_back(m.get());
    }
    std::vector<ThrowsAnalyzer> throwsAnalyzers;
    throwsAnalyzers.reserve(throwsSourceFiles.size());
    for (size_t i = 0; i < throwsSourceFiles.size(); ++i) {
        throwsAnalyzers.emplace_back(*throwsSourceFiles[i],
                                     throwsModules[i]->analyzer->result(), errorClass);
    }
    bool throwsChanged;
    do {
        throwsChanged = false;
        for (auto& ta : throwsAnalyzers) if (ta.runOnce()) throwsChanged = true;
    } while (throwsChanged);
    for (size_t i = 0; i < throwsAnalyzers.size(); ++i) {
        throwsAnalyzers[i].validate(*throwsModules[i]->sink, *throwsModules[i]->source);
    }

    bool ok = true;
    for (auto& m : modules) {
        if (m->sink->hasErrors()) {
            m->sink->printAll(*m->source, std::cerr);
            ok = false;
        }
    }
    if (!ok) return false;

    // Escape analysis across all modules. Cross-module calls propagate facts
    // via shared Symbol*; loop until no module's facts changed in a pass.
    std::vector<std::optional<ast::SourceFile>> moduleSourceFiles;
    moduleSourceFiles.reserve(modules.size());
    std::vector<EscapeAnalyzer> escapeAnalyzers;
    escapeAnalyzers.reserve(modules.size());
    for (auto& m : modules) {
        auto sf = ast::SourceFile::cast(*m->rootNode);
        if (!sf) continue;
        moduleSourceFiles.push_back(*sf);
        escapeAnalyzers.emplace_back(*moduleSourceFiles.back(), m->analyzer->result());
    }
    bool anyChanged;
    do {
        anyChanged = false;
        for (auto& ea : escapeAnalyzers) {
            if (ea.runOnce()) anyChanged = true;
        }
    } while (anyChanged);
    for (auto& ea : escapeAnalyzers) ea.finalize();
    for (auto& ea : escapeAnalyzers) ea.decideStackPromotions();

    if (explainArc) {
        printEscapeFacts(modules, std::cerr);
    }

    return ok;
}

bool emitModule(Module& module,
                const std::string& moduleName,
                const fs::path& objectPath,
                const std::string& targetTriple) {
    CodeGenerator codegen(moduleName, module.source->getFilename(),
                          *module.source, module.analyzer->result(), module.modulePath, targetTriple);
    if (!codegen.generate(*module.rootNode)) {
        for (const auto& d : codegen.getDiagnostics()) d.print(*module.source, std::cerr);
        return false;
    }
    if (!codegen.emitObjectFile(objectPath.string())) {
        for (const auto& d : codegen.getDiagnostics()) d.print(*module.source, std::cerr);
        return false;
    }
    return true;
}

bool linkModulesToExe(std::vector<std::unique_ptr<Module>>& modules,
                      const fs::path& outputFile,
                      const std::string& targetTriple) {
    fs::path outDir = outputFile.parent_path();
    if (outDir.empty()) outDir = fs::current_path();
    std::string baseStem = outputFile.stem().string();
    if (baseStem.empty()) baseStem = "ens";

    std::vector<std::string> objectPaths;
    objectPaths.reserve(modules.size());
    std::vector<std::string> libraries;
    auto addLibrary = [&](const std::u16string& lib) {
        std::string asciiLib = asAscii(lib);
        for (auto& l : libraries) if (l == asciiLib) return;
        libraries.push_back(std::move(asciiLib));
    };
    for (auto& m : modules) {
        std::string name = baseStem + "." + sanitizeForFilename(m->modulePath) + ".obj";
        fs::path objPath = outDir / name;
        if (!emitModule(*m, "ens_" + sanitizeForFilename(m->modulePath), objPath, targetTriple)) return false;
        objectPaths.push_back(objPath.string());
        for (auto& lib : m->analyzer->linkLibraries()) addLibrary(lib);
    }
    return Linker::link(objectPaths, libraries, outputFile.string(), std::cerr, targetTriple);
}

}  // namespace

bool Compiler::compile(const fs::path& source,
                       const fs::path& outputFolder,
                       const fs::path& /*sourcePath*/,
                       bool explainArc,
                       const std::string& targetTriple) {
    fs::path sourceRoot = fs::is_directory(source) ? source : source.parent_path();

    std::deque<fs::path> seeds;
    if (fs::is_directory(source)) {
        auto files = getFileTree(source, sourceRoot);
        for (auto& f : files) seeds.push_back(f);
    } else {
        if (!fs::exists(source)) {
            std::cerr << "ERROR: " << source << " does not exist\n";
            return false;
        }
        fs::path rel = fs::relative(source, sourceRoot);
        seeds.push_back(rel);
    }

    if (seeds.empty()) {
        std::cerr << "ERROR: no .ens files found in " << sourceRoot << '\n';
        return false;
    }

    std::vector<std::unique_ptr<Module>> modules;
    std::unordered_map<std::u16string, Module*> byPath;
    fs::path stdlibRoot = findStdlibRoot();
    if (!buildModuleGraph(sourceRoot, stdlibRoot, seeds, modules, byPath)) return false;

    {
        auto prelude = loadPreludeModule();
        Module* raw = prelude.get();
        modules.insert(modules.begin(), std::move(prelude));
        byPath.emplace(std::u16string(kPreludeModulePath), raw);
    }

    TypeContext sharedCtx;
    if (!runMultiModuleAnalysis(modules, byPath, sharedCtx, explainArc)) return false;

    std::string ext = outputFolder.extension().string();
    for (auto& c : ext) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    const bool linkToExe = !outputFolder.empty() && (ext == ".exe" || ext.empty());

    if (outputFolder.empty()) {
        for (auto& m : modules) {
            CodeGenerator codegen("ens_" + sanitizeForFilename(m->modulePath),
                                  m->source->getFilename(),
                                  *m->source, m->analyzer->result(), m->modulePath, targetTriple);
            if (!codegen.generate(*m->rootNode)) {
                for (const auto& d : codegen.getDiagnostics()) d.print(*m->source, std::cerr);
                return false;
            }
            std::cout << "--- LLVM IR (" << asAscii(m->modulePath) << ") ---\n";
            codegen.print(std::cout);
        }
        return true;
    }
    if (!linkToExe) {
        std::cerr << "Multi-file compilation only supports linking to an executable; got '"
                  << ext << "'\n";
        return false;
    }

    return linkModulesToExe(modules, outputFolder, targetTriple);
}

std::vector<fs::path> Compiler::getFileTree(const fs::path& root, const fs::path& rootPath) {
    std::vector<fs::path> files;

    auto rel = fs::relative(root, rootPath);
    bool hasSubfolder = !rel.empty() && rel != fs::path(".");

    for (const auto& entry : fs::directory_iterator(root)) {
        const auto& path = entry.path();
        if (fs::is_directory(path)) {
            auto sub = getFileTree(path, rootPath);
            files.insert(files.end(), sub.begin(), sub.end());
        } else if (fs::is_regular_file(path) && path.extension() == ".ens") {
            files.push_back(hasSubfolder ? rel / path.filename() : path.filename());
        }
    }

    return files;
}

static std::string asAscii16(std::u16string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char16_t c : s) out.push_back(c < 128 ? static_cast<char>(c) : '?');
    return out;
}

static void dumpTypedOutline(const SyntaxNode& root, std::ostream& os) {
    auto sf = ast::SourceFile::cast(root);
    if (!sf) return;
    os << "\n--- Typed outline ---\n";
    for (auto& fn : sf->functions()) {
        os << "fn " << (fn.nameText() ? asAscii16(*fn.nameText()) : std::string("<missing>")) << "(";
        bool first = true;
        for (auto& p : fn.parameters()) {
            if (!first) os << ", ";
            first = false;
            if (p.isThisField()) os << "this.";
            os << (p.nameText() ? asAscii16(*p.nameText()) : std::string("<?>"));
            if (auto tr = p.typeReference()) {
                os << ":" << (tr->nameText() ? asAscii16(*tr->nameText()) : std::string("<?>"));
                if (tr->isOptional()) os << "?";
            }
            if (p.defaultValue()) os << "=...";
        }
        os << ")";
        if (auto rt = fn.returnType()) {
            if (auto tr = rt->typeReference()) {
                os << " -> " << (tr->nameText() ? asAscii16(*tr->nameText()) : std::string("<?>"));
            }
        }
        if (fn.isShorthand()) os << " ;";
        os << "\n";
    }
    for (auto& sd : sf->structs()) {
        os << "struct " << (sd.nameText() ? asAscii16(*sd.nameText()) : std::string("<missing>")) << " {\n";
        for (auto& f : sd.fields()) {
            os << "  field " << (f.nameText() ? asAscii16(*f.nameText()) : std::string("<?>"));
            if (auto tr = f.typeReference()) os << " : " << (tr->nameText() ? asAscii16(*tr->nameText()) : std::string("<?>"));
            os << "\n";
        }
        for (auto& m : sd.methods()) {
            os << "  method " << (m.nameText() ? asAscii16(*m.nameText()) : std::string("<?>"))
               << "/" << m.parameters().size() << "\n";
        }
        os << "}\n";
    }
    for (auto& cd : sf->classes()) {
        os << "class " << (cd.nameText() ? asAscii16(*cd.nameText()) : std::string("<missing>")) << " {\n";
        for (auto& f : cd.fields()) {
            os << "  field " << (f.nameText() ? asAscii16(*f.nameText()) : std::string("<?>"));
            if (auto tr = f.typeReference()) os << " : " << (tr->nameText() ? asAscii16(*tr->nameText()) : std::string("<?>"));
            os << "\n";
        }
        for (auto& m : cd.methods()) {
            os << "  method " << (m.nameText() ? asAscii16(*m.nameText()) : std::string("<?>"))
               << "/" << m.parameters().size();
            if (m.isShorthand()) os << " (shorthand)";
            os << "\n";
        }
        os << "}\n";
    }
}

bool Compiler::dumpCst(std::istream& source, const std::string& filename) {
    std::string code((std::istreambuf_iterator<char>(source)), std::istreambuf_iterator<char>());
    std::u16string u16code(code.begin(), code.end());
    SourceFile sourceFile(filename, std::move(u16code));

    DiagnosticSink sink;
    Parser parser(sourceFile.getSource(), sink);
    auto root = parser.parseSourceFile();

    auto rootNode = SyntaxNode::makeRoot(root.get());
    rootNode->dump(std::cout, 0);
    dumpTypedOutline(*rootNode, std::cout);

    if (!sink.empty()) {
        std::cerr << "\n--- Diagnostics ---\n";
        sink.printAll(sourceFile, std::cerr);
    }
    return !sink.hasErrors();
}

bool Compiler::analyzeCst(std::istream& source, const std::string& filename) {
    std::string code((std::istreambuf_iterator<char>(source)), std::istreambuf_iterator<char>());
    std::u16string u16code(code.begin(), code.end());
    SourceFile sourceFile(filename, std::move(u16code));

    DiagnosticSink sink;
    Parser parser(sourceFile.getSource(), sink);
    auto root = parser.parseSourceFile();
    auto rootNode = SyntaxNode::makeRoot(root.get());

    Analyzer analyzer(sourceFile, sink);
    analyzer.analyze(*rootNode);

    if (!sink.empty()) {
        sink.printAll(sourceFile, std::cerr);
    }
    return !sink.hasErrors();
}

bool Compiler::compileSingle(std::istream& source, const fs::path& outputFile, const std::string& filename, bool explainArc, const std::string& targetTriple) {
    std::string code((std::istreambuf_iterator<char>(source)), std::istreambuf_iterator<char>());
    std::u16string u16code(code.begin(), code.end());

    const std::u16string userPath = u"main";
    std::vector<std::unique_ptr<Module>> modules;
    std::unordered_map<std::u16string, Module*> byPath;

    auto prelude = loadPreludeModule();
    byPath.emplace(std::u16string(kPreludeModulePath), prelude.get());
    modules.push_back(std::move(prelude));

    auto userModule = makeInMemoryModule(userPath, filename, std::move(u16code));
    Module* user = userModule.get();
    byPath.emplace(userPath, user);
    modules.push_back(std::move(userModule));

    TypeContext sharedCtx;
    if (!runMultiModuleAnalysis(modules, byPath, sharedCtx, explainArc)) return false;

    std::string ext = outputFile.extension().string();
    for (auto& c : ext) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    const bool linkToExe = !outputFile.empty() && (ext == ".exe" || ext.empty());

    if (linkToExe) return linkModulesToExe(modules, outputFile, targetTriple);

    CodeGenerator codegen("ens_" + sanitizeForFilename(user->modulePath),
                          user->source->getFilename(),
                          *user->source, user->analyzer->result(), user->modulePath, targetTriple);
    if (!codegen.generate(*user->rootNode)) {
        for (const auto& d : codegen.getDiagnostics()) d.print(*user->source, std::cerr);
        return false;
    }
    if (outputFile.empty()) {
        std::cout << "--- LLVM IR ---\n";
        codegen.print(std::cout);
        return true;
    }
    if (ext == ".ll") {
        std::ofstream out(outputFile);
        if (!out) {
            std::cerr << "Could not open '" << outputFile << "' for writing\n";
            return false;
        }
        codegen.print(out);
        return true;
    }
    if (ext == ".obj" || ext == ".o") {
        if (!codegen.emitObjectFile(outputFile.string())) {
            for (const auto& d : codegen.getDiagnostics()) d.print(*user->source, std::cerr);
            return false;
        }
        return true;
    }
    std::cerr << "Unsupported --output extension: '" << ext << "'\n";
    return false;
}
