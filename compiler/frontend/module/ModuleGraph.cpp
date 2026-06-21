#include "module/ModuleGraph.h"

#include "ast/Declaration.h"
#include "parser/Parser.h"
#include "semantic/Prelude.h"
#include "semantic/Symbol.h"
#include "semantic/ThrowsAnalyzer.h"
#include "semantic/Type.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <unordered_set>

namespace ens::modules {

namespace {

std::u16string toU16(std::string_view s) {
    std::u16string out;
    out.reserve(s.size());
    for (char c : s) out.push_back(static_cast<unsigned char>(c));
    return out;
}

std::string asAscii(std::u16string_view s) {
    std::string r;
    r.reserve(s.size());
    for (char16_t c : s) r.push_back(c < 128 ? static_cast<char>(c) : '?');
    return r;
}

bool readFileToU16(const fs::path& path, std::u16string& out) {
    std::ifstream f(path);
    if (!f) return false;
    std::string code((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    out.assign(code.begin(), code.end());
    return true;
}

}  // namespace

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

std::string overrideKey(const fs::path& absolute) {
    std::error_code ec;
    fs::path norm = fs::weakly_canonical(absolute, ec);
    if (ec) norm = absolute.lexically_normal();
    return norm.string();
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

// Build a module from its in-memory override if present, else read it from disk.
static std::unique_ptr<Module> loadOrOverride(const fs::path& base, const fs::path& rel,
                                              const std::u16string& mp,
                                              const SourceOverrides* overrides) {
    if (overrides) {
        fs::path absolute = base / rel;
        auto it = overrides->find(overrideKey(absolute));
        if (it != overrides->end()) {
            auto m = makeInMemoryModule(mp, absolute.string(), it->second);
            m->absolutePath = absolute;
            m->relativePath = rel;
            return m;
        }
    }
    return loadModule(base, rel, mp);
}

bool buildModuleGraph(const fs::path& sourceRoot,
                      const fs::path& stdlibRoot,
                      std::deque<fs::path>& seedRelatives,
                      std::vector<std::unique_ptr<Module>>& modulesOut,
                      std::unordered_map<std::u16string, Module*>& byPath,
                      const SourceOverrides* overrides) {
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

        auto module = loadOrOverride(item.base, item.rel, mp, overrides);
        if (!module) return false;

        Module* raw = module.get();
        modulesOut.push_back(std::move(module));
        byPath.emplace(mp, raw);

        auto sf = ast::SourceFile::cast(*raw->rootNode);
        if (!sf) continue;
        for (auto& imp : sf->imports()) {
            std::u16string targetPath = imp.modulePath();
            if (queued.count(targetPath)) continue;
            fs::path targetRel = relativeFromModulePath(targetPath);
            if (targetRel.empty()) continue;

            auto& sink = *raw->sink;
            auto reportAt = [&](const std::string& message) {
                auto [line, col] = raw->source->offsetToPosition(imp.node.startOffset());
                sink.error({line, col, 1}, message);
            };

            // `@pkg...` selects an external package root; a bare path is local to the
            // source root. Only the standard library (`@std`) is available as a package.
            bool isStd = false;
            fs::path base;
            if (imp.isPackage()) {
                auto segs = imp.pathSegments();
                if (!segs.empty() && segs.front() == u"std") {
                    isStd = true;
                    base = stdlibRoot;
                } else {
                    reportAt("External package '" + asAscii(targetPath) +
                        "' is not available yet; only the standard library (@std) is supported.");
                    continue;
                }
            } else if (isStdlibPath(targetPath)) {
                reportAt("Import the standard library as a package: write '@" +
                    asAscii(targetPath) + "'.");
                continue;
            } else {
                base = sourceRoot;
            }

            fs::path absolute = base.empty() ? fs::path() : base / targetRel;
            bool haveOverride = overrides && !absolute.empty() &&
                overrides->count(overrideKey(absolute)) > 0;
            if (base.empty() || (!haveOverride && !fs::exists(absolute))) {
                if (isStd) {
                    reportAt("Cannot find standard library module '" + asAscii(targetPath) +
                        "'. Set ENS_STDLIB to the directory containing 'std/' (normally <repo>/libs).");
                } else {
                    reportAt("Cannot find module '" + asAscii(targetPath) +
                        "' (looked for " + absolute.string() + ")");
                }
                continue;
            }
            enqueue(base, targetRel);
        }
    }
    return true;
}

void insertPreludeModule(std::vector<std::unique_ptr<Module>>& modules,
                         std::unordered_map<std::u16string, Module*>& byPath) {
    auto prelude = loadPreludeModule();
    Module* raw = prelude.get();
    modules.insert(modules.begin(), std::move(prelude));
    byPath.emplace(std::u16string(kPreludeModulePath), raw);
}

bool analyzeModuleGraph(std::vector<std::unique_ptr<Module>>& modules,
                        std::unordered_map<std::u16string, Module*>& byPath,
                        TypeContext& sharedCtx) {
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
    for (auto& m : modules) m->analyzer->bindTypeImports(resolver);

    for (auto& m : modules) m->analyzer->resolveSignatures();

    for (auto& m : modules) m->analyzer->bindValueImports(resolver);

    // Lay out classes whole-program, bases before derived, so a class can extend a class
    // in another module (inherited fields + virtual slots resolve across module
    // boundaries). The prelude is modules[0], so it sorts first among roots.
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

    // Checked-exception throws-set fixpoint. Sets propagate cross-module via shared Symbol*.
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

    bool anyErrors = false;
    for (auto& m : modules) if (m->sink->hasErrors()) anyErrors = true;
    return !anyErrors;
}

}  // namespace ens::modules