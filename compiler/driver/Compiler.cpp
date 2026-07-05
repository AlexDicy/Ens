#include "Compiler.h"
#include "CodeGenerator.h"
#include "Linker.h"
#include "module/ModuleGraph.h"
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

std::string utf8OfU16(const std::u16string& s) {
    std::string out;
    out.reserve(s.size());
    for (char16_t c : s) {
        if (c < 0x80) {
            out.push_back(static_cast<char>(c));
        } else if (c < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (c >> 6)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xE0 | (c >> 12)));
            out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        }
    }
    return out;
}

void appendAscii(std::u16string& out, std::string_view ascii) {
    for (char c : ascii) out.push_back(static_cast<char16_t>(c));
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

bool emitModule(Module& module,
                const std::string& moduleName,
                const fs::path& objectPath,
                const std::string& targetTriple,
                TypeContext& sharedCtx) {
    CodeGenerator codegen(moduleName, module.source->getFilename(),
                          *module.source, module.analyzer->result(), module.modulePath, targetTriple,
                          &sharedCtx);
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
                      const std::string& targetTriple,
                      TypeContext& sharedCtx) {
    fs::path outDir = outputFile.parent_path();
    if (outDir.empty()) outDir = fs::current_path();
    std::string baseStem = outputFile.stem().string();
    if (baseStem.empty()) baseStem = "ens";

    std::vector<std::string> objectPaths;
    objectPaths.reserve(modules.size());
    std::vector<std::string> libraries;
    auto addLibrary = [&](const std::u16string& lib) {
        std::string asciiLib = asciiOfU16(lib);
        for (auto& l : libraries) if (l == asciiLib) return;
        libraries.push_back(std::move(asciiLib));
    };
    for (auto& m : modules) {
        std::string name = baseStem + "." + sanitizeForFilename(m->modulePath) + ".obj";
        fs::path objPath = outDir / name;
        if (!emitModule(*m, "ens_" + sanitizeForFilename(m->modulePath), objPath, targetTriple, sharedCtx)) return false;
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
        // *_test.ens files hold test declarations; they are compiled by `ens test`.
        auto files = getFileTree(source, sourceRoot);
        for (auto& f : files) {
            if (!isTestFile(f)) seeds.push_back(f);
        }
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

    insertPreludeModule(modules, byPath);

    TypeContext sharedCtx;
    if (!runDriverAnalysis(modules, byPath, sharedCtx, explainArc)) return false;

    std::string ext = outputFolder.extension().string();
    for (auto& c : ext) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    const bool linkToExe = !outputFolder.empty() && (ext == ".exe" || ext.empty());

    if (outputFolder.empty()) {
        for (auto& m : modules) {
            CodeGenerator codegen("ens_" + sanitizeForFilename(m->modulePath),
                                  m->source->getFilename(),
                                  *m->source, m->analyzer->result(), m->modulePath, targetTriple,
                                  &sharedCtx);
            if (!codegen.generate(*m->rootNode)) {
                for (const auto& d : codegen.getDiagnostics()) d.print(*m->source, std::cerr);
                return false;
            }
            std::cout << "--- LLVM IR (" << asciiOfU16(m->modulePath) << ") ---\n";
            codegen.print(std::cout);
        }
        return true;
    }
    if (!linkToExe) {
        std::cerr << "Multi-file compilation only supports linking to an executable; got '"
                  << ext << "'\n";
        return false;
    }

    return linkModulesToExe(modules, outputFolder, targetTriple, sharedCtx);
}

namespace {

struct DiscoveredTest {
    std::u16string modulePath;
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
    appendAscii(out, "\n");

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
        appendAscii(out, ");\n    return true;\n} catch (Error e) {\n    print(\"FAIL \" + ");
        out += t.rawLiteral;
        appendAscii(out, " + \": \" + e.message);\n"
                         "    for (let frame in e.getStackFrames()) {\n"
                         "        if (frame.file == \"$ens_test_runner.ens\") { break; }\n"
                         "        print(\"  at \" + frame.function + \" (\" + frame.file + \":\" + frame.line + \")\");\n"
                         "    }\n"
                         "    return false;\n}\n\n");
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

}  // namespace

int Compiler::test(const fs::path& sourceDir, const std::string& filter, bool explainArc) {
    std::error_code ec;
    if (!fs::is_directory(sourceDir, ec)) {
        std::cerr << "ERROR: '" << sourceDir.string() << "' is not a folder\n";
        return 2;
    }
    const fs::path& sourceRoot = sourceDir;

    std::vector<fs::path> testFiles;
    for (auto& rel : getFileTree(sourceRoot, sourceRoot)) {
        if (isTestFile(rel)) testFiles.push_back(rel);
    }
    std::sort(testFiles.begin(), testFiles.end(), [](const fs::path& a, const fs::path& b) {
        return a.generic_string() < b.generic_string();
    });
    if (testFiles.empty()) {
        std::cout << "no tests found under " << sourceRoot.string()
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
        auto module = loadModule(sourceRoot, rel, modulePath);
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
            t.index = index++;
            t.rawLiteral = td.rawDescriptionLiteral().value_or(u"\"\"");
            t.description = utf8OfU16(td.descriptionText().value_or(std::u16string{}));
            tests.push_back(std::move(t));
        }
    }
    if (tests.empty()) {
        std::cout << "no tests found under " << sourceRoot.string() << "\n";
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
    overrides[overrideKey(sourceRoot / runnerRelative)] = buildRunnerSource(tests);
    const std::u16string runnerModulePath = modulePathOfRelative(runnerRelative);

    std::deque<fs::path> seeds;
    for (auto& rel : testFiles) seeds.push_back(rel);
    seeds.push_back(runnerRelative);

    std::vector<std::unique_ptr<Module>> modules;
    std::unordered_map<std::u16string, Module*> byPath;
    if (!buildModuleGraph(sourceRoot, findStdlibRoot(), seeds, modules, byPath, &overrides)) {
        return 2;
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

    insertPreludeModule(modules, byPath);

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
    if (!linkModulesToExe(modules, exePath, /*targetTriple*/ "", sharedCtx)) {
        fs::remove_all(fs::path(tempDir.str().str()), ec);
        return 2;
    }

    std::string exeString = exePath.string();
    std::string errorMessage;
    bool executionFailed = false;
    llvm::StringRef argv[1] = { exeString };
    int exitCode = llvm::sys::ExecuteAndWait(exeString, argv, /*Env*/ std::nullopt,
                                             /*Redirects*/ {}, /*SecondsToWait*/ 0,
                                             /*MemoryLimit*/ 0, &errorMessage, &executionFailed);
    fs::remove_all(fs::path(tempDir.str().str()), ec);

    if (executionFailed || exitCode < 0) {
        std::cerr << "ERROR: could not run the test binary"
                  << (errorMessage.empty() ? "" : ": " + errorMessage) << "\n";
        return 2;
    }
    if (exitCode == 0) return 0;
    if (exitCode != 1) {
        std::cerr << "ens test: the test binary exited with code " << exitCode
                  << " (a crash or panic aborts the whole run)\n";
    }
    return 1;
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
    if (!runDriverAnalysis(modules, byPath, sharedCtx, explainArc)) return false;

    std::string ext = outputFile.extension().string();
    for (auto& c : ext) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    const bool linkToExe = !outputFile.empty() && (ext == ".exe" || ext.empty());

    if (linkToExe) return linkModulesToExe(modules, outputFile, targetTriple, sharedCtx);

    CodeGenerator codegen("ens_" + sanitizeForFilename(user->modulePath),
                          user->source->getFilename(),
                          *user->source, user->analyzer->result(), user->modulePath, targetTriple,
                          &sharedCtx);
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
