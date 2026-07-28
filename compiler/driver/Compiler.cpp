#include "Compiler.h"
#include "Artifacts.h"
#include "CodeGenerator.h"
#include "Linker.h"
#include "TargetPlatform.h"
#include "module/ModuleGraph.h"
#include "ast/Declaration.h"
#include "cst/SyntaxNode.h"
#include "diagnostics/Diagnostic.h"
#include "diagnostics/DiagnosticSink.h"
#include "diagnostics/SourceFile.h"
#include "parser/Parser.h"
#include "semantic/Analyzer.h"
#include "semantic/EscapeAnalyzer.h"
#include "semantic/Literals.h"
#include "semantic/ThrowsAnalyzer.h"
#include "semantic/Symbol.h"
#include "semantic/Type.h"
#include "semantic/TypeContext.h"

#include <algorithm>
#include <cstdio>
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

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Program.h"

namespace fs = std::filesystem;

namespace {

using namespace ens::modules;

bool isTestFile(const fs::path& relativePath) {
    static const std::string suffix = "_test";
    std::string stem = relativePath.stem().string();
    return stem.size() >= suffix.size() &&
           stem.compare(stem.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::vector<fs::path> getFileTree(const fs::path& root, const fs::path& rootPath) {
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

void appendAscii(std::u16string& out, std::string_view ascii) {
    for (char c : ascii) out.push_back(static_cast<char16_t>(c));
}

// Reads a whole stream and decodes its UTF-8 bytes into UTF-16. On invalid
// UTF-8 prints a diagnostic naming `filename` and returns false.
bool readStreamToU16(std::istream& source, const std::string& filename, std::u16string& out) {
    std::string code((std::istreambuf_iterator<char>(source)), std::istreambuf_iterator<char>());
    Utf8DecodeError error;
    if (!decodeUtf8ToUtf16(code, out, error)) {
        std::cerr << "ERROR: " << filename << ": " << describeUtf8DecodeError(error) << '\n';
        return false;
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
        for (auto& td : sf->tests()) {
            auto* info = analysis.find(td.node.greenNode());
            if (info && info->resolvedSymbol) printEscapeFactsForFunction(info->resolvedSymbol, os);
            if (auto body = td.body()) {
                for (auto& s : body->statements()) printPromotionsForStmt(s, analysis, os);
            }
        }
    }
}


// Analyze the whole graph, then (driver only) print errors to stderr and, when clean,
// run escape analysis for codegen. Returns false if any module reported errors.
bool runDriverAnalysis(std::vector<std::unique_ptr<Module>>& modules,
                       std::unordered_map<std::u16string, Module*>& byPath,
                       TypeContext& sharedCtx, bool explainArc) {
    analyzeModuleGraph(modules, byPath, sharedCtx);

    bool ok = true;
    for (auto& m : modules) {
        if (m->sink->hasErrors()) {
            m->sink->printAll(*m->source, std::cerr);
            ok = false;
        }
    }
    if (!ok) return false;

    // Escape analysis across all modules (codegen only). Cross-module calls propagate
    // facts via shared Symbol*; loop until no module's facts changed in a pass.
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
    return true;
}

// A resolver that maps any module path in the compilation to its analysis and
// source, so codegen can emit a node against the module that owns it (e.g. a
// struct's field defaults reached through a generic instantiation in another
// module). The pointers stay valid for the lifetime of `modules`.
CodeGenerator::ModuleResolver makeModuleResolver(
        const std::vector<std::unique_ptr<Module>>& modules) {
    auto table = std::make_shared<
        std::unordered_map<std::u16string, CodeGenerator::ModuleAnalysis>>();
    for (const auto& m : modules) {
        if (!m || !m->source) continue;
        (*table)[m->modulePath] =
            CodeGenerator::ModuleAnalysis{&m->analyzer->result(), m->source.get()};
    }
    return [table](const std::u16string& path) -> CodeGenerator::ModuleAnalysis {
        auto it = table->find(path);
        return it == table->end() ? CodeGenerator::ModuleAnalysis{} : it->second;
    };
}

bool emitModule(Module& module,
                const std::string& moduleName,
                const fs::path& objectPath,
                const std::string& targetTriple,
                TypeContext& sharedCtx,
                const CodeGenerator::ModuleResolver& resolver) {
    CodeGenerator codegen(moduleName, module.source->getFilename(),
                          *module.source, module.analyzer->result(), module.modulePath, targetTriple,
                          &sharedCtx, resolver);
    if (!codegen.generate(*module.rootNode)) {
        codegen.printDiagnostics(std::cerr);
        return false;
    }
    if (!codegen.emitObjectFile(objectPath.string())) {
        codegen.printDiagnostics(std::cerr);
        return false;
    }
    return true;
}

// The platform name native bindings select on, for the resolved target triple.
std::string platformOfTriple(const std::string& triple) {
    if (triple.find("windows") != std::string::npos ||
        triple.find("win32") != std::string::npos ||
        triple.find("msvc") != std::string::npos) {
        return "windows";
    }
    if (triple.find("darwin") != std::string::npos ||
        triple.find("apple") != std::string::npos) {
        return "macos";
    }
    return "linux";
}

bool sameNativeBinding(const ManifestNative& a, const ManifestNative& b) {
    if (a.isSystem != b.isSystem || a.hasBindingBlock != b.hasBindingBlock) return false;
    if (a.bindings.size() != b.bindings.size()) return false;
    for (size_t i = 0; i < a.bindings.size(); ++i) {
        const ManifestNativeBinding& left = a.bindings[i];
        const ManifestNativeBinding& right = b.bindings[i];
        if (left.platform != right.platform || left.isArtifact != right.isArtifact ||
            left.artifactUrl != right.artifactUrl ||
            left.artifactChecksum != right.artifactChecksum ||
            left.baseNames != right.baseNames) {
            return false;
        }
    }
    return true;
}

// Map the build's native-library declarations to linker inputs for the target: the natives of
// every loaded manifest plus, for modules no manifest governs, their `external from` names bound
// by convention (`libc` is the C runtime, linked by platform defaults; any other name passes as
// a conventional library name). The same name declared twice with different bindings is an
// error; identical declarations dedupe. Artifact bindings for the target platform resolve to
// files through the artifact cache and land in `libraryFiles`.
bool collectLinkLibraries(const WorkspaceRegistry* registry,
                          const std::vector<std::unique_ptr<Module>>& modules,
                          const std::string& targetTriple,
                          bool offline,
                          std::vector<std::string>& libraries,
                          std::vector<std::string>& libraryFiles) {
    std::vector<CollectedNative> natives;
    if (registry) natives = registry->collectNatives();
    for (auto& m : modules) {
        if (m->restrictNatives || !m->analyzer) continue;
        for (auto& lib : m->analyzer->linkLibraries()) {
            CollectedNative implicit;
            implicit.native.name = asciiOfU16(lib);
            implicit.native.isSystem = implicit.native.name == "libc";
            implicit.declaredIn = m->source ? m->source->getFilename() : "<module>";
            natives.push_back(std::move(implicit));
        }
    }

    bool ok = true;
    std::vector<CollectedNative> unique;
    for (auto& n : natives) {
        CollectedNative* existing = nullptr;
        for (auto& u : unique) {
            if (u.native.name == n.native.name) { existing = &u; break; }
        }
        if (!existing) {
            unique.push_back(n);
            continue;
        }
        if (!sameNativeBinding(existing->native, n.native)) {
            std::cerr << "ERROR: Native library '" << n.native.name << "' is declared with "
                      << "different bindings by " << existing->declaredIn << " and "
                      << n.declaredIn << "; every package must agree on a native library's "
                      << "binding.\n";
            ok = false;
        }
    }
    if (!ok) return false;

    const std::string platform = platformOfTriple(ens::resolveTargetTriple(targetTriple));
    auto add = [&](const std::string& baseName) {
        for (auto& l : libraries) if (l == baseName) return;
        libraries.push_back(baseName);
    };
    auto addFile = [&](const std::string& file) {
        for (auto& f : libraryFiles) if (f == file) return;
        libraryFiles.push_back(file);
    };
    for (auto& u : unique) {
        if (u.native.isSystem) continue;
        if (!u.native.hasBindingBlock) {
            add(u.native.name);
            continue;
        }
        for (auto& binding : u.native.bindings) {
            if (binding.platform != platform) continue;
            if (binding.isArtifact) {
                ens::packages::ArtifactResult artifact = ens::packages::fetchArtifact(
                    binding.artifactUrl, binding.artifactChecksum, offline);
                if (!artifact.ok) {
                    std::cerr << "ERROR: " << artifact.error << '\n';
                    return false;
                }
                addFile(artifact.file.string());
                continue;
            }
            for (auto& baseName : binding.baseNames) add(baseName);
        }
    }
    return true;
}

// Emits every module's object file into `outDir` (fixed-point over generic instantiations)
// and appends the object paths to `objectPaths`.
bool emitModulesToObjects(std::vector<std::unique_ptr<Module>>& modules,
                          const fs::path& outDir,
                          const std::string& baseStem,
                          const std::string& targetTriple,
                          TypeContext& sharedCtx,
                          std::vector<std::string>& objectPaths) {
    objectPaths.reserve(modules.size());
    CodeGenerator::ModuleResolver resolver = makeModuleResolver(modules);

    // Each module's object path is deterministic, so re-emitting overwrites it in place.
    for (auto& m : modules) {
        std::string name = baseStem + "." + sanitizeForFilename(m->modulePath) + ".obj";
        objectPaths.push_back((outDir / name).string());
    }

    // How many concrete generic instances (free functions and classes/structs) are
    // recorded so far whose declaring module is `path`. A generic body is monomorphized
    // during codegen (emitGenericCall / a `new` over a type parameter), so an instance
    // can turn concrete only while some other module is emitted - possibly one already
    // written out - and each module emits only the instances declared in it
    // (emitInstantiations). A free function's declaring module is Symbol::modulePath and
    // a template's is StructInfo::modulePath, both the module's own canonical path.
    auto anyOpen = [](const std::vector<Type*>& args) {
        for (Type* a : args)
            if (TypeContext::containsTypeParam(a)) return true;
        return false;
    };
    auto concreteInstancesIn = [&](const std::u16string& path) {
        size_t n = 0;
        for (auto& fi : sharedCtx.functionInstantiations()) {
            if (!fi.function || fi.function->modulePath != path) continue;
            if (!anyOpen(fi.args)) ++n;
        }
        for (Type* t : sharedCtx.classInstantiations()) {
            StructInfo* inst = t ? t->structInfo : nullptr;
            StructInfo* templ = inst ? inst->templateOf : nullptr;
            if (!templ || templ->modulePath != path) continue;
            if (!anyOpen(inst->typeArgs)) ++n;
        }
        return n;
    };

    // Emit modules to a fixed point: after a pass, any module that gained a concrete
    // instance since it was last written out is emitted again (its cascade may in turn
    // reveal instances in further modules). The recorded set only grows and is bounded
    // by the monomorphization depth guard, so the loop terminates; the resulting object
    // files are independent of module discovery order.
    std::unordered_map<std::u16string, size_t> emittedCount;
    std::vector<bool> pending(modules.size(), true);
    for (bool anyPending = true; anyPending; ) {
        for (size_t i = 0; i < modules.size(); ++i) {
            if (!pending[i]) continue;
            Module& m = *modules[i];
            if (!emitModule(m, "ens_" + sanitizeForFilename(m.modulePath),
                            objectPaths[i], targetTriple, sharedCtx, resolver))
                return false;
            emittedCount[m.modulePath] = concreteInstancesIn(m.modulePath);
        }
        anyPending = false;
        for (size_t i = 0; i < modules.size(); ++i) {
            pending[i] =
                concreteInstancesIn(modules[i]->modulePath) != emittedCount[modules[i]->modulePath];
            if (pending[i]) anyPending = true;
        }
    }
    return true;
}

bool linkModulesToExe(std::vector<std::unique_ptr<Module>>& modules,
                      const fs::path& outputFile,
                      const std::string& targetTriple,
                      TypeContext& sharedCtx,
                      const std::vector<std::string>& libraries,
                      const std::vector<std::string>& libraryFiles) {
    fs::path outDir = outputFile.parent_path();
    if (outDir.empty()) outDir = fs::current_path();
    std::string baseStem = outputFile.stem().string();
    if (baseStem.empty()) baseStem = "ens";

    std::vector<std::string> objectPaths;
    if (!emitModulesToObjects(modules, outDir, baseStem, targetTriple, sharedCtx, objectPaths)) {
        return false;
    }
    return Linker::link(objectPaths, libraries, libraryFiles, outputFile.string(), std::cerr,
                        targetTriple);
}

// Print any manifest problems collected while loading workspaces. Returns true when there
// were none, so callers can bail on a malformed manifest.
bool printWorkspaceErrors(const WorkspaceRegistry& registry) {
    if (registry.errors().empty()) return true;
    for (const auto& e : registry.errors()) std::cerr << "ERROR: " << e << '\n';
    return false;
}

void printWorkspaceNotices(const WorkspaceRegistry& registry) {
    for (const auto& n : registry.notices()) std::cerr << "NOTE: " << n << '\n';
}

// A loaded compilation: the module graph plus the workspace registry that resolved it.
struct LoadedProgram {
    std::vector<std::unique_ptr<Module>> modules;
    std::unordered_map<std::u16string, Module*> byPath;
    WorkspaceRegistry registry;
};

// Seeds and loads the module graph for `source`, a single .ens file or a folder of sources.
bool loadProgram(const fs::path& source, const fs::path& overridesRoot,
                 const ens::packages::ResolvedPackages* packages, LoadedProgram& out) {
    fs::path sourceRoot = fs::is_directory(source) ? source : source.parent_path();
    // A bare file name has an empty parent; resolve it against the current
    // folder so the module paths below stay non-empty.
    if (sourceRoot.empty()) sourceRoot = ".";

    std::deque<fs::path> seeds;
    if (fs::is_directory(source)) {
        // *_test.ens files hold test declarations; they are compiled by `ens test`.
        auto files = getFileTree(source, sourceRoot);
        for (auto& f : files) {
            if (!isTestFile(f)) seeds.push_back(f);
        }
    } else {
        if (!fs::exists(source)) {
            std::cerr << "ERROR: " << fs::absolute(source).string() << " does not exist\n";
            return false;
        }
        fs::path rel = fs::relative(source, sourceRoot);
        seeds.push_back(rel);
    }

    if (seeds.empty()) {
        std::cerr << "ERROR: no .ens files found in " << sourceRoot << '\n';
        return false;
    }

    fs::path stdlibRoot = findStdlibRoot();

    // The governing workspace (nearest ens.package walking up from the source) supplies
    // `@package` dependencies; its src root stays whatever was compiled here.
    fs::path workspaceRoot = discoverWorkspaceRoot(sourceRoot);
    if (packages) out.registry.setResolvedGitPackages(packages->folders);
    Workspace& root = workspaceRoot.empty()
        ? out.registry.defineRoot(sourceRoot, sourceRoot, {}, /*withManifest=*/false)
        : out.registry.defineRoot(workspaceRoot, sourceRoot, {}, /*withManifest=*/true,
                                  overridesRoot);
    if (!printWorkspaceErrors(out.registry)) return false;
    if (!buildModuleGraph(root, out.registry, stdlibRoot, seeds, out.modules, out.byPath)) {
        return false;
    }
    printWorkspaceNotices(out.registry);
    if (!printWorkspaceErrors(out.registry)) return false;

    // A single .ens file compiles as a single-file program: the file is the program's main
    // module regardless of its name. The natural path stays as an alias so an import chain
    // that leads back to the entry file resolves to the same module.
    if (!fs::is_directory(source)) {
        std::u16string natural = modulePathOfRelative(fs::relative(source, sourceRoot));
        if (natural != u"main") {
            if (out.byPath.count(u"main")) {
                std::cerr << "ERROR: " << fs::absolute(source).string() << " is compiled as "
                          << "the module 'main', but the program also loads a different "
                          << "module named 'main'; rename one of the files.\n";
                return false;
            }
            Module* entry = out.byPath.at(natural);
            entry->modulePath = u"main";
            out.byPath.emplace(u"main", entry);
        }
    }
    return true;
}

bool sourceFileDefinesMain(const SyntaxNode& rootNode) {
    auto sf = ast::SourceFile::cast(rootNode);
    if (!sf) return false;
    for (auto& fn : sf->functions()) {
        if (fn.nameText().value_or(std::u16string{}) == u"main") return true;
    }
    return false;
}

}  // namespace

Compiler::BuildResult Compiler::build(const fs::path& source,
                                      const fs::path& outputFile,
                                      const std::string& defaultName,
                                      bool explainArc,
                                      const std::string& targetTriple,
                                      const fs::path& overridesRoot,
                                      const ens::packages::ResolvedPackages* packages) {
    BuildResult result;
    LoadedProgram program;
    if (!loadProgram(source, overridesRoot, packages, program)) return result;

    bool isApplication = false;
    if (auto it = program.byPath.find(u"main"); it != program.byPath.end()) {
        isApplication = sourceFileDefinesMain(*it->second->rootNode);
    }

    TypeContext sharedCtx;
    if (!runDriverAnalysis(program.modules, program.byPath, sharedCtx, explainArc)) {
        return result;
    }

    if (!isApplication) {
        if (!outputFile.empty()) {
            std::cerr << "ERROR: " << source.string() << " does not define main() in its main "
                      << "module, so there is no executable to produce; it builds as a "
                      << "library.\n";
            return result;
        }
        // A library validates through the full pipeline: every object file is emitted into a
        // temporary folder and nothing is kept.
        llvm::SmallString<128> tempDir;
        if (llvm::sys::fs::createUniqueDirectory("ens-build", tempDir)) {
            std::cerr << "ERROR: could not create a temporary folder for the build\n";
            return result;
        }
        fs::path tempFolder(tempDir.str().str());
        std::vector<std::string> objectPaths;
        bool emitted = emitModulesToObjects(program.modules, tempFolder, "lib", targetTriple,
                                            sharedCtx, objectPaths);
        std::error_code ec;
        fs::remove_all(tempFolder, ec);
        if (!emitted) return result;
        result.outcome = BuildOutcome::ValidatedLibrary;
        return result;
    }

    fs::path exePath = outputFile;
    if (exePath.empty()) {
        std::string name = defaultName.empty() ? std::string("program") : defaultName;
        if (platformOfTriple(ens::resolveTargetTriple(targetTriple)) == "windows") {
            name += ".exe";
        }
        exePath = fs::current_path() / name;
    }
    std::string ext = exePath.extension().string();
    for (auto& c : ext) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    if (ext != ".exe" && !ext.empty()) {
        std::cerr << "ERROR: the compiler produces executables; use an output name with no "
                  << "extension or '.exe', not '" << ext << "'\n";
        return result;
    }

    std::vector<std::string> libraries;
    std::vector<std::string> libraryFiles;
    if (!collectLinkLibraries(&program.registry, program.modules, targetTriple,
                              packages && packages->offline, libraries, libraryFiles)) {
        return result;
    }
    if (!linkModulesToExe(program.modules, exePath, targetTriple, sharedCtx, libraries,
                          libraryFiles)) {
        return result;
    }
    result.outcome = BuildOutcome::BuiltExecutable;
    result.executable = exePath;
    return result;
}

bool Compiler::check(const fs::path& source, const fs::path& overridesRoot,
                     const ens::packages::ResolvedPackages* packages) {
    LoadedProgram program;
    if (!loadProgram(source, overridesRoot, packages, program)) return false;

    TypeContext sharedCtx;
    analyzeModuleGraph(program.modules, program.byPath, sharedCtx);

    bool ok = true;
    for (auto& m : program.modules) {
        if (m->sink->hasErrors()) {
            m->sink->printAll(*m->source, std::cerr);
            ok = false;
        }
    }
    return ok;
}

bool Compiler::definesEntryPoint(const fs::path& source) {
    std::error_code ec;
    fs::path file = fs::is_directory(source, ec) ? source / "main.ens" : source;
    if (!fs::exists(file, ec)) return false;
    fs::path parent = file.parent_path();
    if (parent.empty()) parent = ".";
    auto module = loadModule(parent, file.filename(), u"main");
    if (!module || !module->rootNode) return false;
    return sourceFileDefinesMain(*module->rootNode);
}

namespace {

struct DiscoveredTest {
    std::u16string modulePath;
    std::string file;              // source file, relative to the discovery root
    int index = 0;                 // source order within the module: $test<index>
    std::u16string rawLiteral;     // the description literal exactly as written
    std::string description;       // decoded, UTF-8; used for --filter and messages
    bool selected = true;
};

// The runner is ordinary Ens source: it imports each test module, wraps each
// test in a bool-returning function whose catch (Error) prints the failure,
// and counts passes in main(). Descriptions are spliced as their original
// string literals, so no re-escaping is needed. Failure traces are printed
// frame by frame and stop at the runner's own frames, which are internal.
std::u16string buildRunnerSource(const std::vector<DiscoveredTest>& tests) {
    std::u16string out;
    std::u16string lastImported;
    for (auto& t : tests) {
        if (!t.selected || t.modulePath == lastImported) continue;
        lastImported = t.modulePath;
        appendAscii(out, "import ");
        out += t.modulePath;
        appendAscii(out, ";\n");
    }
    // Flush stdout after each result so completed results survive a crash in a
    // later test. 'fflush(null)' flushes every stream; the FILE handle is an
    // opaque external type here since the runner never inspects it.
    appendAscii(out, "\nexternal type CStdioFile;\n\n"
                     "external from libc {\n"
                     "    fflush(CStdioFile? stream) -> int;\n"
                     "}\n\n");

    std::u16string alias;
    auto aliasOf = [](const std::u16string& modulePath) {
        auto dot = modulePath.rfind(u'.');
        return dot == std::u16string::npos ? modulePath : modulePath.substr(dot + 1);
    };

    int runIndex = 0;
    for (auto& t : tests) {
        if (!t.selected) continue;
        std::string run = "$run" + std::to_string(runIndex++);
        std::string target = "$test" + std::to_string(t.index);
        appendAscii(out, run + "() -> bool {\n    try ");
        out += aliasOf(t.modulePath);
        appendAscii(out, "." + target + "();\n    print(\"PASS \" + ");
        out += t.rawLiteral;
        appendAscii(out, ");\n    fflush(null);\n    return true;\n} catch (Error e) {\n    print(\"FAIL \" + ");
        out += t.rawLiteral;
        appendAscii(out, " + \": \" + e.message);\n"
                         "    for (let frame in e.stackFrames()) {\n"
                         "        if (frame.file == \"$ens_test_runner.ens\") { break; }\n"
                         "        print(\"  at \" + frame.function + \" (\" + frame.file + \":\" + frame.line + \")\");\n"
                         "    }\n"
                         "    fflush(null);\n    return false;\n}\n\n");
    }

    std::string total = std::to_string(runIndex);
    appendAscii(out, "main() -> int {\n    int passed = 0;\n");
    for (int i = 0; i < runIndex; ++i) {
        appendAscii(out, "    if ($run" + std::to_string(i) + "()) { passed = passed + 1; }\n");
    }
    appendAscii(out, "    print(\"{passed}/" + total + " tests passed\");\n"
                     "    if (passed == " + total + ") { return 0; }\n"
                     "    return 1;\n}\n");
    return out;
}

// Write the runner's captured stdout back to our own stdout exactly as it was
// produced. On Windows this must go out in binary mode: the bytes already carry
// the runner's own line endings, so a text-mode write would translate them again.
void echoCapturedOutput(const std::string& bytes) {
    if (bytes.empty()) return;
    std::cout.flush();
#ifdef _WIN32
    std::fflush(stdout);
    int previousMode = _setmode(_fileno(stdout), _O_BINARY);
#endif
    std::fwrite(bytes.data(), 1, bytes.size(), stdout);
    std::fflush(stdout);
#ifdef _WIN32
    if (previousMode != -1) _setmode(_fileno(stdout), previousMode);
#endif
}

// Count the result lines the runner flushed before it died. Each completed test
// prints exactly one line beginning with "PASS " or "FAIL ".
size_t countReportedResults(const std::string& output) {
    size_t reported = 0;
    size_t pos = 0;
    while (pos < output.size()) {
        size_t eol = output.find('\n', pos);
        size_t end = (eol == std::string::npos) ? output.size() : eol;
        std::string_view line(output.data() + pos, end - pos);
        if (line.substr(0, 5) == "PASS " || line.substr(0, 5) == "FAIL ") reported++;
        if (eol == std::string::npos) break;
        pos = eol + 1;
    }
    return reported;
}

// A runner that exits with any code other than 0 (all passed) or 1 (some tests
// failed) aborted partway - a panic, an unhandled exception, or a hard crash.
// The buffered stdout is gone, so name the test that was running by counting how
// many results the runner reported before dying: the crasher is the next one.
void reportAbortedRun(const std::vector<DiscoveredTest>& tests,
                      const std::string& runnerOutput, int exitCode, std::ostream& os) {
    std::vector<const DiscoveredTest*> selected;
    for (auto& t : tests) if (t.selected) selected.push_back(&t);

    size_t reported = countReportedResults(runnerOutput);
    os << "The test run ended unexpectedly (exit code " << exitCode << ")";
    if (reported < selected.size()) {
        const DiscoveredTest* culprit = selected[reported];
        os << " while running test '" << culprit->description << "' (" << culprit->file << ")";
    }
    os << ". " << reported << " of " << selected.size()
       << " tests had reported a result.\n";
}

}  // namespace

int Compiler::test(const fs::path& sourceDir, const fs::path& testsDir,
                   const std::string& filter, bool explainArc,
                   const fs::path& overridesRoot,
                   const ens::packages::ResolvedPackages* packages) {
    std::error_code ec;

    // With no explicit source, discover the workspace from the current folder and run its
    // `src/` against its `tests/`. An explicit source is kept as-is but still discovers a
    // governing workspace (walking up) so `@package` imports resolve.
    fs::path sourceFolder = sourceDir;
    fs::path testsFolder = testsDir;
    fs::path workspaceRoot;
    if (sourceFolder.empty()) {
        workspaceRoot = discoverWorkspaceRoot(fs::current_path());
        if (!workspaceRoot.empty()) {
            sourceFolder = workspaceRoot / "src";
            fs::path wsTests = workspaceRoot / "tests";
            if (testsFolder.empty() && fs::is_directory(wsTests, ec)) testsFolder = wsTests;
        } else {
            sourceFolder = ".";
        }
    } else {
        workspaceRoot = discoverWorkspaceRoot(sourceFolder);
    }

    if (!fs::is_directory(sourceFolder, ec)) {
        std::cerr << "ERROR: '" << sourceFolder.string() << "' is not a folder\n";
        return 2;
    }
    const fs::path& sourceRoot = sourceFolder;
    if (!testsFolder.empty() && !fs::is_directory(testsFolder, ec)) {
        std::cerr << "ERROR: '" << testsFolder.string() << "' is not a folder\n";
        return 2;
    }
    // Tests are discovered under (and their module paths are relative to) the
    // tests folder when one is given, otherwise the source folder.
    const fs::path discoveryRoot = testsFolder.empty() ? sourceRoot : testsFolder;

    std::vector<fs::path> testFiles;
    for (auto& rel : getFileTree(discoveryRoot, discoveryRoot)) {
        if (isTestFile(rel)) testFiles.push_back(rel);
    }
    std::sort(testFiles.begin(), testFiles.end(), [](const fs::path& a, const fs::path& b) {
        return a.generic_string() < b.generic_string();
    });
    if (testFiles.empty()) {
        std::cout << "no tests found under " << discoveryRoot.string()
                  << " (tests live in files ending '_test.ens')\n";
        return 0;
    }

    // Two test files with the same name would collide on the runner's imports.
    std::unordered_map<std::string, fs::path> byStem;
    for (auto& rel : testFiles) {
        auto [existing, inserted] = byStem.emplace(rel.stem().string(), rel);
        if (!inserted) {
            std::cerr << "ERROR: test files must have unique names: '"
                      << existing->second.generic_string() << "' and '" << rel.generic_string()
                      << "' would both be imported as '" << existing->first << "'\n";
            return 2;
        }
    }

    // Discovery parses each test file; real diagnostics surface in the graph build below.
    std::vector<DiscoveredTest> tests;
    for (auto& rel : testFiles) {
        std::u16string modulePath = modulePathOfRelative(rel);
        auto module = loadModule(discoveryRoot, rel, modulePath);
        if (!module || !module->rootNode) return 2;
        auto sf = ast::SourceFile::cast(*module->rootNode);
        if (!sf) continue;
        for (auto& fn : sf->functions()) {
            if (fn.nameText().value_or(std::u16string{}) == u"main") {
                std::cerr << "ERROR: '" << rel.generic_string()
                          << "' defines main(); a test file cannot define the program entry point\n";
                return 2;
            }
        }
        int index = 0;
        for (auto& td : sf->tests()) {
            DiscoveredTest t;
            t.modulePath = modulePath;
            t.file = rel.generic_string();
            t.index = index++;
            t.rawLiteral = td.rawDescriptionLiteral().value_or(u"\"\"");
            t.description = utf16ToUtf8(td.descriptionText().value_or(std::u16string{}));
            tests.push_back(std::move(t));
        }
    }
    if (tests.empty()) {
        std::cout << "no tests found under " << discoveryRoot.string() << "\n";
        return 0;
    }

    size_t selectedCount = tests.size();
    if (!filter.empty()) {
        selectedCount = 0;
        for (auto& t : tests) {
            t.selected = t.description.find(filter) != std::string::npos;
            if (t.selected) selectedCount++;
        }
        if (selectedCount == 0) {
            std::cout << "no tests matched filter '" << filter << "'\n";
            return 0;
        }
    }

    // The runner enters the graph as a virtual seed via a source override.
    const fs::path runnerRelative = "$ens_test_runner.ens";
    SourceOverrides overrides;
    overrides[overrideKey(discoveryRoot / runnerRelative)] = buildRunnerSource(tests);
    const std::u16string runnerModulePath = modulePathOfRelative(runnerRelative);

    std::deque<fs::path> seeds;
    for (auto& rel : testFiles) seeds.push_back(rel);
    seeds.push_back(runnerRelative);

    std::vector<std::unique_ptr<Module>> modules;
    std::unordered_map<std::u16string, Module*> byPath;
    WorkspaceRegistry registry;
    if (packages) registry.setResolvedGitPackages(packages->folders);
    Workspace& root = workspaceRoot.empty()
        ? registry.defineRoot(sourceRoot, sourceRoot, testsFolder, /*withManifest=*/false)
        : registry.defineRoot(workspaceRoot, sourceRoot, testsFolder, /*withManifest=*/true,
                              overridesRoot);
    if (!printWorkspaceErrors(registry)) return 2;
    if (!buildModuleGraph(root, registry, findStdlibRoot(), seeds, modules, byPath, &overrides)) {
        return 2;
    }
    printWorkspaceNotices(registry);
    if (!printWorkspaceErrors(registry)) return 2;

    // The generated runner is compiler-owned source, not part of the workspace's package; its
    // one external block (libc) is exempt from the manifest's native declarations.
    if (auto runner = byPath.find(runnerModulePath); runner != byPath.end()) {
        runner->second->restrictNatives = false;
    }

    // Only the runner may define main(): a second entry point cannot be linked.
    for (auto& m : modules) {
        if (m->modulePath == runnerModulePath) continue;
        auto sf = ast::SourceFile::cast(*m->rootNode);
        if (!sf) continue;
        for (auto& fn : sf->functions()) {
            if (fn.nameText().value_or(std::u16string{}) == u"main") {
                std::cerr << "ERROR: module '" << asciiOfU16(m->modulePath)
                          << "' defines main(); it cannot be linked into a test binary. "
                             "Keep program entry points out of modules that tests import.\n";
                return 2;
            }
        }
    }

    TypeContext sharedCtx;
    if (!runDriverAnalysis(modules, byPath, sharedCtx, explainArc)) {
        std::cerr << "ens test: the test build failed to compile\n";
        return 2;
    }

    llvm::SmallString<128> tempDir;
    if (llvm::sys::fs::createUniqueDirectory("ens-test", tempDir)) {
        std::cerr << "ERROR: could not create a temporary folder for the test binary\n";
        return 2;
    }
#ifdef _WIN32
    fs::path exePath = fs::path(tempDir.str().str()) / "runner.exe";
#else
    fs::path exePath = fs::path(tempDir.str().str()) / "runner";
#endif
    std::vector<std::string> linkLibraries;
    std::vector<std::string> linkLibraryFiles;
    if (!collectLinkLibraries(&registry, modules, /*targetTriple*/ "",
                              packages && packages->offline, linkLibraries,
                              linkLibraryFiles)) {
        fs::remove_all(fs::path(tempDir.str().str()), ec);
        return 2;
    }
    if (!linkModulesToExe(modules, exePath, /*targetTriple*/ "", sharedCtx, linkLibraries,
                          linkLibraryFiles)) {
        fs::remove_all(fs::path(tempDir.str().str()), ec);
        return 2;
    }

    // Capture the runner's stdout to a file rather than letting it stream to the
    // console. Each result line is flushed as it is printed (see buildRunnerSource),
    // so a crash in a later test leaves the completed results on disk. We echo the
    // file back verbatim afterward, so a normal run prints exactly what it did before,
    // and count the results to name the test that was running if the run aborts.
    fs::path capturePath = fs::path(tempDir.str().str()) / "runner.stdout";
    std::string captureString = capturePath.string();

    std::string exeString = exePath.string();
    std::string errorMessage;
    bool executionFailed = false;
    llvm::StringRef argv[1] = { exeString };
    std::optional<llvm::StringRef> redirects[3] = {
        std::nullopt, llvm::StringRef(captureString), std::nullopt };
    int exitCode = llvm::sys::ExecuteAndWait(exeString, argv, /*Env*/ std::nullopt,
                                             /*Redirects*/ redirects, /*SecondsToWait*/ 0,
                                             /*MemoryLimit*/ 0, &errorMessage, &executionFailed);

    std::string runnerOutput;
    {
        std::ifstream in(capturePath, std::ios::binary);
        if (in) {
            runnerOutput.assign((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
        }
    }
    fs::remove_all(fs::path(tempDir.str().str()), ec);
    echoCapturedOutput(runnerOutput);

    if (executionFailed) {
        std::cerr << "ERROR: could not run the test binary"
                  << (errorMessage.empty() ? "" : ": " + errorMessage) << "\n";
        return 2;
    }
    if (exitCode == 0) return 0;
    if (exitCode != 1) {
        reportAbortedRun(tests, runnerOutput, exitCode, std::cerr);
        // Keep the exit-code contract: a negative code (a hard crash, e.g. a
        // segfault) stays a 2 as before; any other abnormal code reports a
        // failure with 1, the same as a run that merely had failing tests.
        return exitCode < 0 ? 2 : 1;
    }
    return 1;
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
    for (auto& td : sf->tests()) {
        os << "test \"" << (td.descriptionText() ? asAscii16(*td.descriptionText()) : std::string("<missing>"))
           << "\"\n";
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
    std::u16string u16code;
    if (!readStreamToU16(source, filename, u16code)) return false;
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
    std::u16string u16code;
    if (!readStreamToU16(source, filename, u16code)) return false;

    std::vector<std::unique_ptr<Module>> modules;
    std::unordered_map<std::u16string, Module*> byPath;
    TypeContext sharedCtx;
    Module* buffer = analyzeStandaloneSource(u"main", filename, std::move(u16code),
                                             findStdlibRoot(), modules, byPath, sharedCtx);

    if (!buffer->sink->empty()) {
        buffer->sink->printAll(*buffer->source, std::cerr);
    }
    return !buffer->sink->hasErrors();
}
