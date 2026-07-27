#pragma once

#include <deque>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "cst/Green.h"
#include "cst/SyntaxNode.h"
#include "diagnostics/DiagnosticSink.h"
#include "diagnostics/SourceFile.h"
#include "module/Workspace.h"
#include "semantic/Analyzer.h"
#include "semantic/TypeContext.h"

namespace ens::modules {

namespace fs = std::filesystem;

// One source file in the compilation: its parse tree, diagnostics, and analyzer.
struct Module {
    std::u16string modulePath;
    fs::path absolutePath;
    fs::path relativePath;  // relative to the source root
    // Canonical module-path prefix of the owning workspace: empty for the root, or the
    // package name (e.g. u"ens.frontend") for a module pulled in as a package. The
    // analyzer prepends it to bare imports so a package's sibling imports resolve to the
    // same canonical module as an external `@package` import.
    std::u16string packagePrefix;
    // Native-library policy from the owning workspace: when a package manifest governs the
    // module, `external from` may name only the natives it declares. Without a manifest the
    // names bind by convention at link time.
    bool restrictNatives = false;
    std::vector<std::u16string> declaredNatives;
    std::string manifestPath;
    std::unique_ptr<SourceFile> source;
    std::unique_ptr<DiagnosticSink> sink;
    GreenElementPtr cstRoot;
    std::unique_ptr<SyntaxNode> rootNode;
    std::unique_ptr<Analyzer> analyzer;
};

// Path <-> module-path conversions.
std::u16string modulePathOfRelative(const fs::path& relative);
fs::path relativeFromModulePath(const std::u16string& modulePath);
std::string sanitizeForFilename(std::u16string_view s);

// True for `std`/`std.*` (used to suggest the `@std` form for bare stdlib imports).
bool isStdlibPath(const std::u16string& modulePath);

// The standard library root (directory containing `std/`): ENS_STDLIB if set, else the
// nearest `libs/` walking up from the working directory. Empty if none is found.
fs::path findStdlibRoot();

std::unique_ptr<Module> loadModule(const fs::path& sourceRoot,
                                   const fs::path& relativePath,
                                   const std::u16string& modulePath);
std::unique_ptr<Module> makeInMemoryModule(const std::u16string& modulePath,
                                           const std::string& filename, std::u16string code);

// Canonical key for SourceOverrides: a normalized absolute path string. Build override
// keys with this so they match the paths buildModuleGraph computes.
std::string overrideKey(const fs::path& absolute);

// In-memory text that replaces the on-disk file at a given normalized absolute path
// (see overrideKey). Lets the LSP analyze unsaved editor buffers.
using SourceOverrides = std::unordered_map<std::string, std::u16string>;

// Load the implicitly imported standard-library modules, the seed files, and everything they
// transitively import. `@std` follows the stdlib root; `@package` imports follow the owning
// workspace's dependencies into the package's `src/`; bare paths stay within the importing
// module's workspace. Module paths are package-qualified so the same file resolves to one
// module however it is reached. Returns false if a file cannot be read or the standard
// library cannot be resolved, reporting the reason on stderr. `registry` supplies the root
// and (lazily) every dependency workspace and outlives the call.
bool buildModuleGraph(Workspace& root,
                      WorkspaceRegistry& registry,
                      const fs::path& stdlibRoot,
                      std::deque<fs::path>& seedRelatives,
                      std::vector<std::unique_ptr<Module>>& modulesOut,
                      std::unordered_map<std::u16string, Module*>& byPath,
                      const SourceOverrides* overrides = nullptr);

// Multi-root seeding: each seed is a (base folder, path relative to it) pair, so a graph
// can be seeded from both a workspace's `src/` and `tests/` at once (used by the LSP for
// workspace-wide references and rename). All seeds belong to `root`.
bool buildModuleGraph(Workspace& root,
                      WorkspaceRegistry& registry,
                      const fs::path& stdlibRoot,
                      const std::vector<std::pair<fs::path, fs::path>>& seeds,
                      std::vector<std::unique_ptr<Module>>& modulesOut,
                      std::unordered_map<std::u16string, Module*>& byPath,
                      const SourceOverrides* overrides = nullptr);

// Backward-compatible entry point with no package dependencies: only `@std` packages
// resolve. Seeds resolve against `sourceRoot`, or against `testsRoot` when it is non-empty,
// with bare imports falling back to it after the source root (`ens test --tests`); a module
// under both roots is an error.
bool buildModuleGraph(const fs::path& sourceRoot,
                      const fs::path& stdlibRoot,
                      std::deque<fs::path>& seedRelatives,
                      std::vector<std::unique_ptr<Module>>& modulesOut,
                      std::unordered_map<std::u16string, Module*>& byPath,
                      const SourceOverrides* overrides = nullptr,
                      const fs::path& testsRoot = {});

// Analyze one in-memory source as module `modulePath`, alongside the implicitly imported
// standard-library modules when `stdlibRoot` resolves them. For the callers that hold a
// single buffer and no workspace: the CST debug tool and the language server's fallback
// path. Returns the module the source was analyzed as; its diagnostics are in its own sink.
Module* analyzeStandaloneSource(const std::u16string& modulePath, const std::string& filename,
                                std::u16string code, const fs::path& stdlibRoot,
                                std::vector<std::unique_ptr<Module>>& modules,
                                std::unordered_map<std::u16string, Module*>& byPath,
                                TypeContext& sharedCtx);

// Run semantic analysis across the whole graph (name binding, signatures, imports,
// whole-program class layout, bodies, and the checked-exception fixpoint), leaving each
// module's diagnostics in its own sink. Does not print or run escape analysis. Returns
// true when no module reported errors.
bool analyzeModuleGraph(std::vector<std::unique_ptr<Module>>& modules,
                        std::unordered_map<std::u16string, Module*>& byPath,
                        TypeContext& sharedCtx);

}  // namespace ens::modules