#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace ens::modules {

namespace fs = std::filesystem;

// A build workspace: a folder holding `dependencies.txt`, with its sources in `src/`
// and (for the root) its tests in `tests/`. `packagePrefix` is the canonical
// module-path prefix for every module in this workspace: empty for the root, or the
// dependency key (e.g. u"ens.frontend") for a package pulled in through another
// workspace. `deps` maps each dependency key to the absolute folder it resolves to.
struct Workspace {
    fs::path root;
    fs::path srcRoot;
    fs::path testsRoot;
    std::u16string packagePrefix;
    std::unordered_map<std::u16string, fs::path> deps;
};

// A dependency key matched against an import's leading path segments, and the workspace
// folder it resolves to. `segmentCount` is how many leading segments the key consumed.
struct PackageMatch {
    size_t segmentCount = 0;
    fs::path folder;
};

// The longest dependency key in `deps` that is a prefix of `segments`, if any.
std::optional<PackageMatch> matchPackage(
    const std::unordered_map<std::u16string, fs::path>& deps,
    const std::vector<std::u16string>& segments);

// The nearest ancestor of `startDir` (inclusive) containing a `dependencies.txt`, or an
// empty path when none is found. Mirrors how `git` finds a repository from a subfolder.
fs::path discoverWorkspaceRoot(const fs::path& startDir);

// Owns every Workspace in a compilation and hands out stable pointers. Dependency
// packages are loaded lazily and memoized by their canonical folder, so a package
// reached through several import paths (or a dependency cycle) resolves to one object
// with one canonical prefix. Parse problems in any `dependencies.txt` accumulate in
// `errors()` rather than aborting the load.
class WorkspaceRegistry {
public:
    // Define the compilation's root workspace. `srcRoot`/`testsRoot` may differ from
    // `folder/src` and `folder/tests` when the driver was given an explicit source or
    // tests folder. Dependencies are read from `folder/dependencies.txt` when present.
    Workspace& defineRoot(const fs::path& folder, const fs::path& srcRoot,
                          const fs::path& testsRoot, bool withDependencies);

    // Return the package workspace rooted at `folder`, loading it (and its own
    // dependencies) on first request and assigning it `prefix`. A folder already known
    // to the registry keeps the prefix it was first given.
    Workspace* getOrLoad(const fs::path& folder, const std::u16string& prefix);

    const std::vector<std::string>& errors() const { return errors_; }

private:
    Workspace& create(const fs::path& folder, const fs::path& srcRoot,
                      const fs::path& testsRoot, std::u16string prefix,
                      bool withDependencies);

    std::vector<std::unique_ptr<Workspace>> owned_;
    std::unordered_map<std::string, Workspace*> byFolder_;
    std::vector<std::string> errors_;
};

}  // namespace ens::modules
