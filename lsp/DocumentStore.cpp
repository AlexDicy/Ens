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

    fs::path sourceRoot = store_.sourceRootFor(fileAbs);
    std::error_code ec;
    fs::path rel = fs::relative(fileAbs, sourceRoot, ec);
    if (ec || rel.empty()) { analyzeSingleFileFallback(); return; }
    std::u16string modPath = ens::modules::modulePathOfRelative(rel);

    ens::modules::SourceOverrides overrides = store_.collectOverrides();
    std::deque<fs::path> seeds{ rel };
    bool ok = ens::modules::buildModuleGraph(sourceRoot, store_.stdlibRoot(),
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

WorkspaceModules DocumentStore::buildWorkspaceModules() const {
    WorkspaceModules workspace;
    if (!workspaceRoot_) return workspace;

    std::deque<fs::path> seeds;
    std::error_code ec;
    auto iterator = fs::recursive_directory_iterator(
        *workspaceRoot_, fs::directory_options::skip_permission_denied, ec);
    if (ec) return workspace;
    for (auto it = fs::begin(iterator); it != fs::end(iterator); it.increment(ec)) {
        if (ec) break;
        const fs::directory_entry& entry = *it;
        std::string name = entry.path().filename().string();
        if (entry.is_directory(ec)) {
            if (!name.empty() && name.front() == '.') it.disable_recursion_pending();
            continue;
        }
        if (entry.path().extension() != ".ens") continue;
        fs::path rel = fs::relative(entry.path(), *workspaceRoot_, ec);
        if (!ec && !rel.empty()) seeds.push_back(std::move(rel));
    }
    if (seeds.empty()) return workspace;

    workspace.typeCtx = std::make_unique<TypeContext>();
    ens::modules::SourceOverrides overrides = collectOverrides();
    bool ok = ens::modules::buildModuleGraph(*workspaceRoot_, stdlibRoot_, seeds,
                                             workspace.modules, workspace.byPath, &overrides);
    if (!ok) return WorkspaceModules{};

    ens::modules::insertPreludeModule(workspace.modules, workspace.byPath);
    ens::modules::analyzeModuleGraph(workspace.modules, workspace.byPath, *workspace.typeCtx);
    return workspace;
}

void DocumentStore::clearTransientOverride(const fs::path& absolute) {
    transientOverrides_.erase(ens::modules::overrideKey(absolute));
}
