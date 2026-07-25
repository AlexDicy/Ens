#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "module/Manifest.h"

namespace ens::modules {

namespace fs = std::filesystem;

// A build workspace: a folder governed by an `ens.package` manifest, with its sources in `src/`
// and (for the root) its tests in `tests/`. `packagePrefix` is the canonical module-path prefix
// for every module in this workspace: empty for the root, or the package name (e.g.
// u"ens.frontend") for a package pulled in as a dependency. `deps` maps each resolved dependency
// name (or, at a workspace-form root, each member's package name) to the absolute folder it
// resolves to.
struct Workspace {
    fs::path root;
    fs::path srcRoot;
    fs::path testsRoot;
    std::u16string packagePrefix;
    std::unordered_map<std::u16string, fs::path> deps;
    // Manifest facts: `hasPackageManifest` when the folder declares a package,
    // `isWorkspaceRoot` when it declares a workspace. Both false for standalone sources.
    bool hasPackageManifest = false;
    bool isWorkspaceRoot = false;
    std::string manifestPath;
    std::u16string packageName;
    std::vector<ManifestNative> natives;
    std::vector<std::u16string> nativeNames;
};

// A dependency name matched against an import's leading path segments, and the workspace
// folder it resolves to. `segmentCount` is how many leading segments the name consumed.
struct PackageMatch {
    size_t segmentCount = 0;
    fs::path folder;
};

// The longest dependency name in `deps` that is a prefix of `segments`, if any.
std::optional<PackageMatch> matchPackage(
    const std::unordered_map<std::u16string, fs::path>& deps,
    const std::vector<std::u16string>& segments);

// The nearest ancestor of `startDir` (inclusive) containing an `ens.package`, or an empty path
// when none is found. Mirrors how `git` finds a repository from a subfolder.
fs::path discoverWorkspaceRoot(const fs::path& startDir);

// A native library declaration together with the file that declares it, for whole-build
// collection and conflict reporting.
struct CollectedNative {
    ManifestNative native;
    std::string declaredIn;
};

// Owns every Workspace in a compilation and hands out stable pointers. Dependency packages are
// loaded lazily and memoized by their canonical folder, so a package reached through several
// import paths (or a dependency cycle) resolves to one object with one canonical prefix. Each
// package's `dependency` declarations resolve by name against the members of its enclosing
// workspace (the nearest ancestor whose ens.package lists the package as a member), with the
// root's `ens.overrides` taking precedence build-wide. Manifest problems accumulate in
// `errors()` rather than aborting the load.
class WorkspaceRegistry {
public:
    // Define the compilation's root workspace. `srcRoot`/`testsRoot` may differ from
    // `folder/src` and `folder/tests` when the driver was given an explicit source or tests
    // folder. With `withManifest`, `folder/ens.package` and the sibling `ens.overrides` are
    // read; the manifest may declare either a package or a workspace.
    Workspace& defineRoot(const fs::path& folder, const fs::path& srcRoot,
                          const fs::path& testsRoot, bool withManifest);

    // Return the package workspace rooted at `folder`, loading its manifest (and resolving its
    // dependencies) on first request and assigning it `prefix`. A folder already known to the
    // registry keeps the prefix it was first given.
    Workspace* getOrLoad(const fs::path& folder, const std::u16string& prefix);

    // Every native library declared by a manifest loaded into this build.
    std::vector<CollectedNative> collectNatives() const;

    const std::vector<std::string>& errors() const { return errors_; }

    // One informational line per override the build resolved a package through.
    const std::vector<std::string>& notices() const { return notices_; }

private:
    // The member index of one workspace-form manifest: package names to member folders.
    struct MemberIndex {
        bool isWorkspace = false;
        std::unordered_map<std::string, fs::path> foldersByName;
        std::unordered_set<std::string> memberFolderKeys;
    };

    Workspace& create(const fs::path& folder, const fs::path& srcRoot,
                      const fs::path& testsRoot, std::u16string prefix);
    const Manifest& manifestFor(const fs::path& manifestFile);
    const MemberIndex& memberIndexFor(const fs::path& folder);
    const MemberIndex* enclosingMemberIndex(const fs::path& packageFolder);
    void applyPackageManifest(Workspace& ws, const Manifest& manifest);
    void resolveDependencies(Workspace& ws, const Manifest& manifest);
    void loadRootOverrides(const fs::path& rootFolder);
    void noticeOverrideUse(const std::string& name, const fs::path& folder);

    std::vector<std::unique_ptr<Workspace>> owned_;
    std::unordered_map<std::string, Workspace*> byFolder_;
    std::unordered_map<std::string, Manifest> manifests_;
    std::unordered_map<std::string, MemberIndex> memberIndexes_;
    std::unordered_map<std::string, fs::path> overrides_;
    std::unordered_set<std::string> noticedOverrides_;
    std::vector<std::string> errors_;
    std::vector<std::string> notices_;
};

}  // namespace ens::modules
