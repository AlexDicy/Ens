#pragma once

#include <deque>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "cst/Green.h"
#include "cst/SyntaxNode.h"
#include "diagnostics/DiagnosticSink.h"
#include "diagnostics/SourceFile.h"
#include "semantic/Analyzer.h"
#include "semantic/TypeContext.h"

namespace ens::modules {

namespace fs = std::filesystem;

// One source file in the compilation: its parse tree, diagnostics, and analyzer.
struct Module {
    std::u16string modulePath;
    fs::path absolutePath;
    fs::path relativePath;  // relative to the source root
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
std::unique_ptr<Module> loadPreludeModule();

// Canonical key for SourceOverrides: a normalized absolute path string. Build override
// keys with this so they match the paths buildModuleGraph computes.
std::string overrideKey(const fs::path& absolute);

// In-memory text that replaces the on-disk file at a given normalized absolute path
// (see overrideKey). Lets the LSP analyze unsaved editor buffers.
using SourceOverrides = std::unordered_map<std::string, std::u16string>;

// Load the seed files and everything they transitively import (following `@std` to the
// stdlib root and bare paths to the source root). Returns false if a file cannot be read.
// When testsRoot is non-empty, seeds resolve against it and bare imports fall back to it
// after the source root (`ens test --tests`); a module under both roots is an error.
bool buildModuleGraph(const fs::path& sourceRoot,
                      const fs::path& stdlibRoot,
                      std::deque<fs::path>& seedRelatives,
                      std::vector<std::unique_ptr<Module>>& modulesOut,
                      std::unordered_map<std::u16string, Module*>& byPath,
                      const SourceOverrides* overrides = nullptr,
                      const fs::path& testsRoot = {});

// Insert the prelude as modules[0] / byPath[$prelude].
void insertPreludeModule(std::vector<std::unique_ptr<Module>>& modules,
                         std::unordered_map<std::u16string, Module*>& byPath);

// Run semantic analysis across the whole graph (name binding, signatures, imports,
// whole-program class layout, bodies, and the checked-exception fixpoint), leaving each
// module's diagnostics in its own sink. Does not print or run escape analysis. Returns
// true when no module reported errors.
bool analyzeModuleGraph(std::vector<std::unique_ptr<Module>>& modules,
                        std::unordered_map<std::u16string, Module*>& byPath,
                        TypeContext& sharedCtx);

}  // namespace ens::modules