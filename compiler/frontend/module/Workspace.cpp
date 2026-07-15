#include "module/Workspace.h"

#include <cctype>
#include <fstream>
#include <sstream>
#include <system_error>

namespace ens::modules {

namespace {

std::string canonicalFolderKey(const fs::path& folder) {
    std::error_code ec;
    fs::path norm = fs::weakly_canonical(folder, ec);
    if (ec) norm = folder.lexically_normal();
    return norm.string();
}

std::u16string toU16(std::string_view s) {
    std::u16string out;
    out.reserve(s.size());
    for (char c : s) out.push_back(static_cast<unsigned char>(c));
    return out;
}

std::string trim(std::string_view s) {
    size_t begin = 0;
    size_t end = s.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(s[begin]))) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(s[end - 1]))) --end;
    return std::string(s.substr(begin, end - begin));
}

// Parse `folder/dependencies.txt` into `deps`, resolving each value against `folder`.
// Malformed lines are appended to `errors` and skipped; a missing file is not an error.
void loadDependencies(const fs::path& folder,
                      std::unordered_map<std::u16string, fs::path>& deps,
                      std::vector<std::string>& errors) {
    fs::path depFile = folder / "dependencies.txt";
    std::error_code ec;
    if (!fs::exists(depFile, ec)) return;

    std::ifstream file(depFile);
    if (!file) {
        errors.push_back("Could not read " + depFile.string());
        return;
    }

    std::string line;
    int lineNumber = 0;
    while (std::getline(file, line)) {
        ++lineNumber;
        std::string trimmed = trim(line);
        if (trimmed.empty() || trimmed[0] == '#') continue;

        auto at = [&](const std::string& message) {
            return depFile.string() + ":" + std::to_string(lineNumber) + ": " + message;
        };

        if (trimmed[0] != '@') {
            errors.push_back(at("dependency lines must start with '@', got '" + trimmed + "'"));
            continue;
        }
        auto eq = trimmed.find('=');
        if (eq == std::string::npos) {
            errors.push_back(at("expected '@package.name=path', missing '='"));
            continue;
        }
        std::string key = trim(std::string_view(trimmed).substr(1, eq - 1));
        std::string value = trim(std::string_view(trimmed).substr(eq + 1));
        if (key.empty() || value.empty()) {
            errors.push_back(at("expected '@package.name=path'"));
            continue;
        }

        std::u16string packageKey = toU16(key);
        if (deps.count(packageKey)) {
            errors.push_back(at("duplicate dependency '@" + key + "'"));
            continue;
        }
        deps.emplace(std::move(packageKey), (folder / value).lexically_normal());
    }
}

}  // namespace

std::optional<PackageMatch> matchPackage(
    const std::unordered_map<std::u16string, fs::path>& deps,
    const std::vector<std::u16string>& segments) {
    if (deps.empty() || segments.empty()) return std::nullopt;

    // Try the longest prefix first so `@a.b.c` prefers key `a.b` over key `a`.
    for (size_t count = segments.size(); count >= 1; --count) {
        std::u16string key;
        for (size_t i = 0; i < count; ++i) {
            if (i > 0) key.push_back(u'.');
            key += segments[i];
        }
        auto it = deps.find(key);
        if (it != deps.end()) return PackageMatch{count, it->second};
    }
    return std::nullopt;
}

fs::path discoverWorkspaceRoot(const fs::path& startDir) {
    std::error_code ec;
    fs::path dir = fs::absolute(startDir, ec);
    if (ec) dir = startDir;
    dir = dir.lexically_normal();

    for (fs::path d = dir;; d = d.parent_path()) {
        if (fs::exists(d / "dependencies.txt", ec)) return d;
        if (d == d.parent_path()) break;
    }
    return {};
}

Workspace& WorkspaceRegistry::create(const fs::path& folder, const fs::path& srcRoot,
                                     const fs::path& testsRoot, std::u16string prefix,
                                     bool withDependencies) {
    auto ws = std::make_unique<Workspace>();
    ws->root = folder;
    ws->srcRoot = srcRoot;
    ws->testsRoot = testsRoot;
    ws->packagePrefix = std::move(prefix);
    if (withDependencies) loadDependencies(folder, ws->deps, errors_);

    Workspace* raw = ws.get();
    byFolder_.emplace(canonicalFolderKey(folder), raw);
    owned_.push_back(std::move(ws));
    return *raw;
}

Workspace& WorkspaceRegistry::defineRoot(const fs::path& folder, const fs::path& srcRoot,
                                         const fs::path& testsRoot, bool withDependencies) {
    return create(folder, srcRoot, testsRoot, std::u16string(), withDependencies);
}

Workspace* WorkspaceRegistry::getOrLoad(const fs::path& folder, const std::u16string& prefix) {
    auto it = byFolder_.find(canonicalFolderKey(folder));
    if (it != byFolder_.end()) return it->second;
    // A dependency is consumed through its `src/` only; its own tests never take part in a
    // dependent's build, so it carries no tests root (which would otherwise enable the
    // root-only src/tests fallback for its bare imports).
    return &create(folder, folder / "src", /*testsRoot=*/{}, prefix, /*withDependencies=*/true);
}

}  // namespace ens::modules
