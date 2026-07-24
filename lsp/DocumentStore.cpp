#include "DocumentStore.h"

#include <deque>
#include <system_error>

#include <lsp/fileuri.h>

namespace fs = std::filesystem;

Document::Document(std::string uri, std::u16string text, int version, DocumentStore& store)
    : store_(store), uri_(std::move(uri)), version_(version), text_(std::move(text)) {}

void Document::setText(std::u16string text, int version) {
    text_ = std::move(text);
    version_ = version;
}

fs::path Document::path() const {
    return pathForUri(uri_);
}

fs::path Document::pathForUri(const std::string& uri) {
    if (uri.empty()) return {};
    lsp::Uri parsed = lsp::Uri::parse(uri);
    if (parsed.scheme() != "file") return {};
    lsp::FileUri fileUri(parsed);
    auto p = fileUri.path();
    if (p.empty()) return {};
    return fs::path(std::string(p));
}

void Document::analyze() {
    modules_.clear();
    byPath_.clear();
    moduleFiles_.clear();
    openModule_ = nullptr;
    typeCtx_ = std::make_unique<TypeContext>();

    fs::path fileAbs = path();
    if (fileAbs.empty()) { analyzeSingleFileFallback(); return; }

    ResolvedWorkspace rw = store_.resolveWorkspaceFor(fileAbs);
    fs::path seedBase = rw.testsRoot.empty() ? rw.srcRoot : rw.testsRoot;
    std::error_code ec;
    fs::path rel = fs::relative(fileAbs, seedBase, ec);
    if (ec || rel.empty()) { analyzeSingleFileFallback(); return; }
    std::u16string modPath = ens::modules::modulePathOfRelative(rel);

    ens::modules::SourceOverrides overrides = store_.collectOverrides();
    std::deque<fs::path> seeds{ rel };
    ens::modules::WorkspaceRegistry registry;
    ens::modules::Workspace& root = registry.defineRoot(
        rw.depsFolder, rw.srcRoot, rw.testsRoot, rw.withDependencies);
    bool ok = ens::modules::buildModuleGraph(root, registry, store_.stdlibRoot(),
                                             seeds, modules_, byPath_, &overrides);
    if (!ok) { analyzeSingleFileFallback(); return; }

    ens::modules::insertPreludeModule(modules_, byPath_);
    ens::modules::analyzeModuleGraph(modules_, byPath_, *typeCtx_);

    auto it = byPath_.find(modPath);
    if (it == byPath_.end()) { analyzeSingleFileFallback(); return; }
    openModule_ = it->second;
    for (auto& m : modules_) moduleFiles_[m->modulePath] = m->absolutePath;
}

// Analyze just this buffer with no module resolution. Used when the file is outside any
// source root or the graph could not be built.
void Document::analyzeSingleFileFallback() {
    modules_.clear();
    byPath_.clear();
    moduleFiles_.clear();
    typeCtx_.reset();

    auto m = ens::modules::makeInMemoryModule(u"main", uri_, text_);
    m->analyzer = std::make_unique<Analyzer>(*m->source, *m->sink);
    m->analyzer->analyze(*m->rootNode);
    openModule_ = m.get();
    modules_.push_back(std::move(m));
}

Document& DocumentStore::upsert(std::string uri, std::u16string text, int version) {
    auto it = docs.find(uri);
    if (it != docs.end()) {
        it->second->setText(std::move(text), version);
        it->second->analyze();
        return *it->second;
    }
    auto doc = std::make_unique<Document>(uri, std::move(text), version, *this);
    Document* raw = doc.get();
    docs.emplace(std::move(uri), std::move(doc));
    raw->analyze();  // after insertion, so collectOverrides() includes this document
    return *raw;
}

void DocumentStore::erase(const std::string& uri) {
    docs.erase(uri);
}

Document* DocumentStore::find(const std::string& uri) {
    auto it = docs.find(uri);
    return it == docs.end() ? nullptr : it->second.get();
}

const Document* DocumentStore::find(const std::string& uri) const {
    auto it = docs.find(uri);
    return it == docs.end() ? nullptr : it->second.get();
}

fs::path DocumentStore::sourceRootFor(const fs::path& fileAbs) const {
    if (workspaceRoot_) {
        fs::path rel = fileAbs.lexically_relative(*workspaceRoot_);
        if (!rel.empty() && rel.string().compare(0, 2, "..") != 0) return *workspaceRoot_;
    }
    return fileAbs.parent_path();
}

static bool isUnder(const fs::path& p, const fs::path& base) {
    fs::path rel = p.lexically_relative(base);
    return !rel.empty() && rel.string().compare(0, 2, "..") != 0;
}

ResolvedWorkspace DocumentStore::resolveWorkspaceFor(const fs::path& fileAbs) const {
    ResolvedWorkspace r;
    fs::path wsRoot = ens::modules::discoverWorkspaceRoot(fileAbs.parent_path());
    if (wsRoot.empty()) {
        r.srcRoot = sourceRootFor(fileAbs);
        r.depsFolder = r.srcRoot;
        return r;
    }

    std::error_code ec;
    fs::path src = wsRoot / "src";
    fs::path tests = wsRoot / "tests";
    r.depsFolder = wsRoot;
    r.withDependencies = true;
    bool hasSrc = fs::is_directory(src, ec);
    if (hasSrc && isUnder(fileAbs, src)) {
        r.srcRoot = src;
    } else if (fs::is_directory(tests, ec) && isUnder(fileAbs, tests)) {
        r.srcRoot = hasSrc ? src : wsRoot;
        r.testsRoot = tests;
    } else {
        // The file sits directly under the workspace root (no src/ layout).
        r.srcRoot = wsRoot;
    }
    return r;
}

ens::modules::SourceOverrides DocumentStore::collectOverrides() const {
    ens::modules::SourceOverrides overrides;
    for (auto& [uri, doc] : docs) {
        fs::path p = doc->path();
        if (p.empty()) continue;
        overrides[ens::modules::overrideKey(p)] = doc->text();
    }
    for (const auto& [key, text] : transientOverrides_) {
        overrides[key] = text;
    }
    return overrides;
}

void DocumentStore::setTransientOverride(const fs::path& absolute, std::u16string text) {
    transientOverrides_[ens::modules::overrideKey(absolute)] = std::move(text);
}

WorkspaceModules DocumentStore::buildWorkspaceModules(const fs::path& forFile) const {
    WorkspaceModules workspace;

    // Scope the graph to the file's own workspace so nested workspaces stay isolated. A file
    // with no ens.package manifest falls back to the single workspace-root hint (flat project).
    fs::path depsFolder, srcRoot, testsRoot;
    bool withDependencies;
    fs::path wsRoot = ens::modules::discoverWorkspaceRoot(forFile.parent_path());
    std::error_code ec;
    if (!wsRoot.empty()) {
        depsFolder = wsRoot;
        srcRoot = fs::is_directory(wsRoot / "src", ec) ? wsRoot / "src" : wsRoot;
        testsRoot = fs::is_directory(wsRoot / "tests", ec) ? wsRoot / "tests" : fs::path();
        withDependencies = true;
    } else if (workspaceRoot_) {
        depsFolder = *workspaceRoot_;
        srcRoot = *workspaceRoot_;
        withDependencies = false;
    } else {
        return workspace;
    }

    // Seed every .ens file under the source root (and tests root) so files that import a
    // given document are in the graph, not just its forward dependencies.
    std::vector<std::pair<fs::path, fs::path>> seeds;
    auto addTree = [&](const fs::path& base) {
        if (base.empty()) return;
        std::error_code walkEc;
        auto iterator = fs::recursive_directory_iterator(
            base, fs::directory_options::skip_permission_denied, walkEc);
        if (walkEc) return;
        for (auto it = fs::begin(iterator); it != fs::end(iterator); it.increment(walkEc)) {
            if (walkEc) break;
            const fs::directory_entry& entry = *it;
            std::string name = entry.path().filename().string();
            if (entry.is_directory(walkEc)) {
                if (!name.empty() && name.front() == '.') it.disable_recursion_pending();
                continue;
            }
            if (entry.path().extension() != ".ens") continue;
            fs::path rel = fs::relative(entry.path(), base, walkEc);
            if (!walkEc && !rel.empty()) seeds.emplace_back(base, std::move(rel));
        }
    };
    addTree(srcRoot);
    addTree(testsRoot);
    if (seeds.empty()) return workspace;

    workspace.typeCtx = std::make_unique<TypeContext>();
    ens::modules::SourceOverrides overrides = collectOverrides();
    ens::modules::WorkspaceRegistry registry;
    ens::modules::Workspace& root =
        registry.defineRoot(depsFolder, srcRoot, testsRoot, withDependencies);
    bool ok = ens::modules::buildModuleGraph(root, registry, stdlibRoot_, seeds,
                                             workspace.modules, workspace.byPath, &overrides);
    if (!ok) return WorkspaceModules{};

    ens::modules::insertPreludeModule(workspace.modules, workspace.byPath);
    ens::modules::analyzeModuleGraph(workspace.modules, workspace.byPath, *workspace.typeCtx);
    return workspace;
}

void DocumentStore::clearTransientOverride(const fs::path& absolute) {
    transientOverrides_.erase(ens::modules::overrideKey(absolute));
}
