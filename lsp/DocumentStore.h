#pragma once
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "module/ModuleGraph.h"
#include "semantic/Analyzer.h"

class DocumentStore;

// A module graph seeded with every Ens file in a document's workspace, so files
// that import a given document (reverse dependencies) are part of the graph.
// Used by workspace-scoped features: references and rename.
struct WorkspaceModules {
    std::vector<std::unique_ptr<ens::modules::Module>> modules;
    std::unordered_map<std::u16string, ens::modules::Module*> byPath;
    std::unique_ptr<TypeContext> typeCtx;
};

// The build roots a file resolves against: its `src/` root, the `tests/` root when the file
// itself is a test, the folder to read dependencies.txt from, and whether packages apply.
// A file with no governing dependencies.txt falls back to a single source root and no deps.
struct ResolvedWorkspace {
    std::filesystem::path depsFolder;
    std::filesystem::path srcRoot;
    std::filesystem::path testsRoot;
    bool withDependencies = false;
};

// One open editor buffer. Analysis builds a module graph rooted at the document's source
// root (with all open buffers overlaid), so imports, the standard library, and
// namespace-qualified calls resolve. The public accessors expose the open file's module.
class Document {
public:
    Document(std::string uri, std::u16string text, int version, DocumentStore& store);

    void setText(std::u16string text, int version);
    void analyze();

    const std::string& uri() const { return uri_; }
    int version() const { return version_; }
    const std::u16string& text() const { return text_; }
    std::filesystem::path path() const;
    static std::filesystem::path pathForUri(const std::string& uri);

    const SourceFile& sourceFile() const { return *openModule_->source; }
    const SyntaxNode& root() const { return *openModule_->rootNode; }
    const Analyzer& analyzer() const { return *openModule_->analyzer; }
    Analyzer& analyzer() { return *openModule_->analyzer; }
    const DiagnosticSink& sink() const { return *openModule_->sink; }

    // Module path -> absolute file path for every module in the graph (cross-file
    // go-to-definition).
    const std::unordered_map<std::u16string, std::filesystem::path>& moduleFiles() const {
        return moduleFiles_;
    }

    // Every module in the graph, for graph-wide searches (references, rename).
    const std::vector<std::unique_ptr<ens::modules::Module>>& moduleList() const {
        return modules_;
    }

private:
    DocumentStore& store_;
    std::string uri_;
    int version_;
    std::u16string text_;

    std::unique_ptr<TypeContext> typeCtx_;
    std::vector<std::unique_ptr<ens::modules::Module>> modules_;
    std::unordered_map<std::u16string, ens::modules::Module*> byPath_;
    ens::modules::Module* openModule_ = nullptr;
    std::unordered_map<std::u16string, std::filesystem::path> moduleFiles_;

    void analyzeSingleFileFallback();
};

class DocumentStore {
public:
    Document& upsert(std::string uri, std::u16string text, int version);
    void erase(const std::string& uri);
    Document* find(const std::string& uri);
    const Document* find(const std::string& uri) const;

    template <typename F>
    void forEachDocument(F&& f) {
        for (auto& [uri, doc] : docs) f(*doc);
    }

    void setWorkspaceRoot(std::filesystem::path root) { workspaceRoot_ = std::move(root); }
    const std::filesystem::path& stdlibRoot() const { return stdlibRoot_; }

    // The root used to resolve a file's imports: the workspace folder if the file lives
    // under it, otherwise the file's own directory.
    std::filesystem::path sourceRootFor(const std::filesystem::path& fileAbs) const;

    // Resolve a file to its build roots by walking up for a dependencies.txt (like the
    // compiler). Each file resolves independently, so several nested ens workspaces can
    // coexist under one editor session.
    ResolvedWorkspace resolveWorkspaceFor(const std::filesystem::path& fileAbs) const;

    // In-memory text for every open buffer, keyed for SourceOverrides.
    ens::modules::SourceOverrides collectOverrides() const;

    // Post-edit contents the server itself produced (rename edits): analysis sees
    // them immediately, until the client's buffer or the disk catches up.
    void setTransientOverride(const std::filesystem::path& absolute, std::u16string text);
    void clearTransientOverride(const std::filesystem::path& absolute);

    // Build a graph of `forFile`'s whole workspace (its src/ and tests/, plus package
    // dependencies), seeding every file so reverse dependencies are covered. Empty when the
    // file has no workspace and no root hint, or the graph cannot be built.
    WorkspaceModules buildWorkspaceModules(const std::filesystem::path& forFile) const;

private:
    std::unordered_map<std::string, std::unique_ptr<Document>> docs;
    std::unordered_map<std::string, std::u16string> transientOverrides_;
    std::optional<std::filesystem::path> workspaceRoot_;
    std::filesystem::path stdlibRoot_ = ens::modules::findStdlibRoot();
};
