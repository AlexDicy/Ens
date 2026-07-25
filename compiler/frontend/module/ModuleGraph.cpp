#include "module/ModuleGraph.h"

#include "ast/Declaration.h"
#include "parser/Parser.h"
#include "semantic/Literals.h"
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

// Reads `path` and decodes its UTF-8 bytes into UTF-16. Returns false on
// failure: an empty `error` means the file could not be opened, a non-empty
// `error` describes invalid UTF-8 content.
bool readFileToU16(const fs::path& path, std::u16string& out, std::string& error) {
    error.clear();
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::string code((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    Utf8DecodeError decodeError;
    if (!decodeUtf8ToUtf16(code, out, decodeError)) {
        error = describeUtf8DecodeError(decodeError);
        return false;
    }
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
    std::string readError;
    if (!readFileToU16(absolute, code, readError)) {
        if (readError.empty()) {
            std::cerr << "ERROR: Couldn't read " << absolute.string() << '\n';
        } else {
            std::cerr << "ERROR: " << absolute.string() << ": " << readError << '\n';
        }
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
        if (fs::exists(libs / "std" / "ens.package", ec)) return libs;
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

namespace {

std::u16string joinSegments(const std::vector<std::u16string>& segs, size_t from, size_t to) {
    std::u16string out;
    for (size_t i = from; i < to; ++i) {
        if (i > from) out.push_back(u'.');
        out += segs[i];
    }
    return out;
}

std::u16string canonicalModulePath(const Workspace& ws, const fs::path& rel) {
    std::u16string mp = modulePathOfRelative(rel);
    if (ws.packagePrefix.empty()) return mp;
    return ws.packagePrefix + u"." + mp;
}

// Build the graph from explicit (base, rel) seeds, all belonging to `root`.
bool buildModuleGraphFromSeeds(Workspace& root,
                               WorkspaceRegistry& registry,
                               const fs::path& stdlibRoot,
                               const std::vector<std::pair<fs::path, fs::path>>& seeds,
                               std::vector<std::unique_ptr<Module>>& modulesOut,
                               std::unordered_map<std::u16string, Module*>& byPath,
                               const SourceOverrides* overrides) {
    struct WorkItem {
        Workspace* ws;
        fs::path base;             // folder the relative path is under
        fs::path rel;              // relative to base
        std::u16string canonical;  // canonical (package-qualified) module path
    };
    std::unordered_set<std::u16string> queued;
    std::deque<WorkItem> work;

    auto enqueue = [&](Workspace* ws, const fs::path& base, const fs::path& rel,
                       const std::u16string& canonical) {
        if (canonical.empty() || queued.count(canonical)) return;
        queued.insert(canonical);
        work.push_back({ws, base, rel, canonical});
    };

    for (const auto& seed : seeds) {
        enqueue(&root, seed.first, seed.second, canonicalModulePath(root, seed.second));
    }

    while (!work.empty()) {
        WorkItem item = work.front();
        work.pop_front();

        auto module = loadOrOverride(item.base, item.rel, item.canonical, overrides);
        if (!module) return false;
        module->packagePrefix = item.ws->packagePrefix;
        module->restrictNatives = item.ws->hasPackageManifest;
        module->declaredNatives = item.ws->nativeNames;
        module->manifestPath = item.ws->manifestPath;

        Module* raw = module.get();
        modulesOut.push_back(std::move(module));
        byPath.emplace(item.canonical, raw);

        auto sf = ast::SourceFile::cast(*raw->rootNode);
        if (!sf) continue;
        Workspace& ws = *item.ws;

        for (auto& imp : sf->imports()) {
            auto& sink = *raw->sink;
            auto reportAt = [&](const std::string& message) {
                auto [line, col] = raw->source->offsetToPosition(imp.node.startOffset());
                sink.error({line, col, 1}, message);
            };

            std::u16string modPath = imp.modulePath();
            std::vector<std::u16string> segs = imp.pathSegments();
            if (segs.empty()) continue;

            // Resolve the import to (targetWs, base folder, relative path within it, and the
            // canonical package-qualified module path). `@std` is built in; `@package`
            // follows the owning workspace's dependencies; a bare path stays local.
            Workspace* targetWs = nullptr;
            fs::path base;
            fs::path targetRel;
            std::u16string canonical;

            if (imp.isPackage()) {
                if (segs.front() == u"std") {
                    // `@std` is implicitly available to every package; the standard library
                    // itself is an ordinary package under the stdlib root.
                    if (stdlibRoot.empty()) {
                        reportAt("Cannot find the standard library. Set ENS_STDLIB to the "
                            "directory containing 'std/' (normally <repo>/libs).");
                        continue;
                    }
                    if (segs.size() == 1) {
                        reportAt("Import a module within package 'std', not the package "
                            "itself.");
                        continue;
                    }
                    targetWs = registry.getOrLoad(stdlibRoot / "std", u"std");
                    base = targetWs->srcRoot;
                    targetRel = relativeFromModulePath(joinSegments(segs, 1, segs.size()));
                    canonical = modPath;
                } else {
                    auto match = matchPackage(ws.deps, segs);
                    if (!match) {
                        if (ws.hasPackageManifest) {
                            reportAt("Package '" + asAscii(modPath) + "' is not declared as a "
                                "dependency in " + ws.manifestPath + ".");
                        } else if (ws.isWorkspaceRoot) {
                            reportAt("Package '" + asAscii(modPath) + "' is not a member of "
                                "the workspace at " + ws.manifestPath + ".");
                        } else {
                            reportAt("External package '" + asAscii(modPath) + "' is not "
                                "available; these sources have no ens.package manifest "
                                "(only '@std' is built in).");
                        }
                        continue;
                    }
                    if (match->segmentCount == segs.size()) {
                        reportAt("Import a module within package '" + asAscii(modPath) +
                            "', not the package itself.");
                        continue;
                    }
                    std::u16string prefix = joinSegments(segs, 0, match->segmentCount);
                    targetWs = registry.getOrLoad(match->folder, prefix);
                    base = targetWs->srcRoot;
                    targetRel = relativeFromModulePath(joinSegments(segs, match->segmentCount, segs.size()));
                    canonical = modPath;  // == package prefix + remaining path
                }
            } else if (isStdlibPath(modPath)) {
                reportAt("Import the standard library as a package: write '@" +
                    asAscii(modPath) + "'.");
                continue;
            } else {
                targetWs = &ws;
                base = ws.srcRoot;
                targetRel = relativeFromModulePath(modPath);
                canonical = canonicalModulePath(ws, targetRel);
                // The root workspace's tests fall back to the tests root after the source
                // root; a module present under both is ambiguous.
                if (!ws.testsRoot.empty()) {
                    auto present = [&](const fs::path& r) {
                        fs::path candidate = r / targetRel;
                        return fs::exists(candidate) ||
                            (overrides && overrides->count(overrideKey(candidate)) > 0);
                    };
                    bool underSource = present(ws.srcRoot);
                    bool underTests = present(ws.testsRoot);
                    if (underSource && underTests) {
                        reportAt("Module '" + asAscii(modPath) +
                            "' exists under both the source folder and the tests folder; rename one of the files.");
                        continue;
                    }
                    if (underTests) base = ws.testsRoot;
                }
            }

            if (canonical.empty() || queued.count(canonical)) continue;

            fs::path absolute = base.empty() ? fs::path() : base / targetRel;
            bool haveOverride = overrides && !absolute.empty() &&
                overrides->count(overrideKey(absolute)) > 0;
            if (base.empty() || (!haveOverride && !fs::exists(absolute))) {
                if (targetWs && targetWs->packagePrefix == u"std") {
                    reportAt("Cannot find standard library module '" + asAscii(modPath) +
                        "'. Set ENS_STDLIB to the directory containing 'std/' (normally <repo>/libs).");
                } else {
                    std::string lookedFor = absolute.string();
                    if (targetWs == &ws && !ws.testsRoot.empty()) {
                        lookedFor += " and " + (ws.testsRoot / targetRel).string();
                    }
                    reportAt("Cannot find module '" + asAscii(modPath) +
                        "' (looked for " + lookedFor + ")");
                }
                continue;
            }
            enqueue(targetWs, base, targetRel, canonical);
        }
    }
    return true;
}

}  // namespace

bool buildModuleGraph(Workspace& root,
                      WorkspaceRegistry& registry,
                      const fs::path& stdlibRoot,
                      std::deque<fs::path>& seedRelatives,
                      std::vector<std::unique_ptr<Module>>& modulesOut,
                      std::unordered_map<std::u16string, Module*>& byPath,
                      const SourceOverrides* overrides) {
    // Seeds belong to the root workspace. When a tests root is set they resolve against it
    // (`ens test --tests`); otherwise against the source root.
    const fs::path seedBase = root.testsRoot.empty() ? root.srcRoot : root.testsRoot;
    std::vector<std::pair<fs::path, fs::path>> seeds;
    seeds.reserve(seedRelatives.size());
    for (auto& r : seedRelatives) seeds.emplace_back(seedBase, r);
    seedRelatives.clear();
    return buildModuleGraphFromSeeds(root, registry, stdlibRoot, seeds, modulesOut, byPath, overrides);
}

bool buildModuleGraph(Workspace& root,
                      WorkspaceRegistry& registry,
                      const fs::path& stdlibRoot,
                      const std::vector<std::pair<fs::path, fs::path>>& seeds,
                      std::vector<std::unique_ptr<Module>>& modulesOut,
                      std::unordered_map<std::u16string, Module*>& byPath,
                      const SourceOverrides* overrides) {
    return buildModuleGraphFromSeeds(root, registry, stdlibRoot, seeds, modulesOut, byPath, overrides);
}

bool buildModuleGraph(const fs::path& sourceRoot,
                      const fs::path& stdlibRoot,
                      std::deque<fs::path>& seedRelatives,
                      std::vector<std::unique_ptr<Module>>& modulesOut,
                      std::unordered_map<std::u16string, Module*>& byPath,
                      const SourceOverrides* overrides,
                      const fs::path& testsRoot) {
    WorkspaceRegistry registry;
    Workspace& root = registry.defineRoot(sourceRoot, sourceRoot, testsRoot,
                                          /*withDependencies=*/false);
    return buildModuleGraph(root, registry, stdlibRoot, seedRelatives, modulesOut, byPath, overrides);
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
        m->analyzer = std::make_unique<Analyzer>(*m->source, *m->sink, sharedCtx,
                                                 m->modulePath, m->packagePrefix);
        m->analyzer->setNativePolicy(m->restrictNatives, m->declaredNatives, m->manifestPath);
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
            std::unordered_set<StructInfo*> seen;
            // A generic base may be an unfilled instantiation; its chain
            // continues through the template.
            for (StructInfo* s = si->baseInfo; s && seen.insert(s).second; ) {
                ++d;
                StructInfo* authority = s->templateOf ? s->templateOf : s;
                s = authority->baseInfo;
            }
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

    // Fill generic instantiations whose template was laid out after the use site.
    sharedCtx.materializeInstantiations();
    sharedCtx.refreshInstantiationInheritance();

    // Every module's struct fields are resolved now, so a by-value containment
    // cycle can be detected across module boundaries.
    for (auto& m : modules) m->analyzer->checkStructValueCycles();

    for (auto& m : modules) m->analyzer->analyzeBodies();

    // Runs after bodies so declared throws clauses are resolved.
    for (auto& m : modules) m->analyzer->checkSignatureVisibility();

    // A generic instantiation that never terminates was capped; report it against
    // the module that declares the recursive template so codegen never sees it.
    for (const auto& o : sharedCtx.takeInstantiationOverflows()) {
        DiagnosticSink* target = nullptr;
        for (auto& m : modules) {
            if (o.templ && m->modulePath == o.templ->modulePath) {
                target = m->sink.get();
                break;
            }
        }
        if (!target && !modules.empty()) target = modules.back()->sink.get();
        if (target) target->error({o.line, o.column, o.length}, o.message);
    }

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