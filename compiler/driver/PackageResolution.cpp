#include "PackageResolution.h"

#include <algorithm>
#include <map>
#include <set>
#include <system_error>

#include "ContentStore.h"
#include "GitSource.h"
#include "Lockfile.h"
#include "module/Manifest.h"
#include "module/Workspace.h"

namespace ens::packages {

namespace {

using ens::modules::Manifest;
using ens::modules::ManifestForm;
using ens::modules::loadManifestFile;

struct Version {
    std::vector<unsigned long long> parts;
    std::string text;
};

bool parseVersion(const std::string& text, Version& version) {
    version.parts.clear();
    version.text = text;
    unsigned long long current = 0;
    bool digitInSegment = false;
    for (char c : text) {
        if (c >= '0' && c <= '9') {
            current = current * 10 + static_cast<unsigned long long>(c - '0');
            digitInSegment = true;
        } else if (c == '.' && digitInSegment) {
            version.parts.push_back(current);
            current = 0;
            digitInSegment = false;
        } else {
            return false;
        }
    }
    if (!digitInSegment) return false;
    version.parts.push_back(current);
    return true;
}

// Versions compare numerically component-wise; missing components count as zero.
int compareVersions(const Version& a, const Version& b) {
    size_t count = std::max(a.parts.size(), b.parts.size());
    for (size_t i = 0; i < count; ++i) {
        unsigned long long left = i < a.parts.size() ? a.parts[i] : 0;
        unsigned long long right = i < b.parts.size() ? b.parts[i] : 0;
        if (left != right) return left < right ? -1 : 1;
    }
    return 0;
}

bool sameMajor(const Version& a, const Version& b) {
    unsigned long long left = a.parts.empty() ? 0 : a.parts[0];
    unsigned long long right = b.parts.empty() ? 0 : b.parts[0];
    return left == right;
}

// One package's version requirement, as declared by one manifest.
struct Requirement {
    std::string package;
    Version version;
    std::string url;       // empty when the declaration carries no from clause
    std::string requirer;  // the declaring manifest, for error messages
};

std::string canonicalKey(const fs::path& folder) {
    std::error_code ec;
    fs::path normalized = fs::weakly_canonical(folder, ec);
    if (ec) normalized = folder.lexically_normal();
    return normalized.string();
}

std::vector<LockArtifact> artifactsOf(const Manifest& manifest) {
    std::vector<LockArtifact> artifacts;
    for (const auto& native : manifest.natives) {
        for (const auto& binding : native.bindings) {
            if (!binding.isArtifact) continue;
            artifacts.push_back({native.name, binding.platform, binding.artifactUrl,
                                 binding.artifactChecksum});
        }
    }
    return artifacts;
}

// The resolver: gathers requirements over an explicit work list, selects each package's
// maximum required version, materializes trees through the content store, and reconciles
// the lock. Everything it learns and reports stays in this object and its outcome.
class Resolver {
public:
    Resolver(const fs::path& rootFolder, bool offline, bool locked)
        : rootFolder_(rootFolder), offline_(offline), locked_(locked) {}

    void run(ResolutionOutcome& outcome);

private:
    struct PackageState {
        std::vector<Requirement> requirements;
        std::string url;
        std::string urlRequirer;
        Version selected;
        bool urlConflictReported = false;
        bool majorConflictReported = false;
        bool lockPinTried = false;
        std::string loadedVersion;  // the version whose tree and manifest are loaded
        bool failed = false;
        // Facts about the loaded version.
        std::string commit;
        std::string contentHash;
        fs::path packageFolder;
        std::vector<LockRequirement> lockRequirements;
        std::vector<LockArtifact> artifacts;
    };

    void collectLocalRequirements();
    std::set<std::string> enclosingMemberNames(const fs::path& packageFolder);
    void scanManifestRequirements(const Manifest& manifest, const std::string& manifestPath,
                                  const std::set<std::string>& memberNames);
    void addRequirement(Requirement requirement);
    void tryLockPin(const std::string& name, PackageState& state);
    void loadSelected(const std::string& name, PackageState& state);
    void reconcileLock(ResolutionOutcome& outcome);

    fs::path rootFolder_;
    bool offline_ = false;
    bool locked_ = false;
    Lockfile oldLock_;
    Manifest rootManifest_;
    std::map<std::string, fs::path> overrides_;
    std::map<std::string, PackageState> states_;
    std::vector<std::string> errors_;
    std::vector<std::string> notes_;
};

void Resolver::collectLocalRequirements() {
    std::vector<std::string> scratch;
    rootManifest_ = loadManifestFile(rootFolder_ / "ens.package", scratch);

    // Root overrides bypass git resolution build-wide, and their targets' manifests
    // contribute requirements like any other local package.
    fs::path overridesFile = rootFolder_ / "ens.overrides";
    std::error_code ec;
    if (fs::exists(overridesFile, ec)) {
        std::vector<std::string> overrideScratch;
        Manifest overridesManifest = loadManifestFile(overridesFile, overrideScratch);
        for (const auto& redirect : overridesManifest.overrides) {
            overrides_.emplace(redirect.name,
                               (rootFolder_ / redirect.folder).lexically_normal());
        }
    }

    if (rootManifest_.form == ManifestForm::Workspace) {
        std::vector<std::string> memberScratch;
        auto members = ens::modules::listWorkspaceMembers(rootFolder_, memberScratch);
        std::set<std::string> memberNames;
        for (const auto& member : members) memberNames.insert(member.packageName);
        for (const auto& member : members) {
            std::vector<std::string> manifestScratch;
            Manifest manifest = loadManifestFile(member.folder / "ens.package",
                                                 manifestScratch);
            scanManifestRequirements(manifest, (member.folder / "ens.package").string(),
                                     memberNames);
        }
    } else if (rootManifest_.form == ManifestForm::Package) {
        scanManifestRequirements(rootManifest_, (rootFolder_ / "ens.package").string(),
                                 enclosingMemberNames(rootFolder_));
    }

    for (const auto& [name, folder] : overrides_) {
        std::error_code existsError;
        if (!fs::exists(folder / "ens.package", existsError)) continue;
        std::vector<std::string> manifestScratch;
        Manifest manifest = loadManifestFile(folder / "ens.package", manifestScratch);
        if (manifest.form != ManifestForm::Package) continue;
        scanManifestRequirements(manifest, (folder / "ens.package").string(),
                                 enclosingMemberNames(folder));
    }
}

// The names a package resolves as workspace members: the members of the nearest ancestor
// workspace-form manifest, and only when that workspace lists the package as a member.
// Mirrors the registry's resolution so the resolver never fetches what a member provides.
std::set<std::string> Resolver::enclosingMemberNames(const fs::path& packageFolder) {
    std::error_code ec;
    fs::path folder = fs::absolute(packageFolder, ec);
    if (ec) folder = packageFolder;
    folder = folder.lexically_normal();
    std::string packageKey = canonicalKey(folder);

    for (fs::path d = folder.parent_path();; d = d.parent_path()) {
        if (d.empty()) break;
        if (fs::exists(d / "ens.package", ec)) {
            std::vector<std::string> scratch;
            Manifest manifest = loadManifestFile(d / "ens.package", scratch);
            if (manifest.form == ManifestForm::Workspace) {
                std::vector<std::string> memberScratch;
                auto members = ens::modules::listWorkspaceMembers(d, memberScratch);
                bool listed = false;
                std::set<std::string> names;
                for (const auto& member : members) {
                    names.insert(member.packageName);
                    if (canonicalKey(member.folder) == packageKey) listed = true;
                }
                return listed ? names : std::set<std::string>{};
            }
        }
        if (d == d.parent_path()) break;
    }
    return {};
}

void Resolver::scanManifestRequirements(const Manifest& manifest,
                                        const std::string& manifestPath,
                                        const std::set<std::string>& memberNames) {
    for (const auto& dependency : manifest.dependencies) {
        if (memberNames.count(dependency.name)) continue;
        if (overrides_.count(dependency.name)) continue;
        if (!dependency.hasVersion) continue;
        Requirement requirement;
        requirement.package = dependency.name;
        if (!parseVersion(dependency.version, requirement.version)) continue;
        if (dependency.hasSource) requirement.url = dependency.sourceUrl;
        requirement.requirer = manifestPath;
        addRequirement(std::move(requirement));
    }
}

void Resolver::addRequirement(Requirement requirement) {
    PackageState& state = states_[requirement.package];
    if (!requirement.url.empty()) {
        if (state.url.empty()) {
            state.url = requirement.url;
            state.urlRequirer = requirement.requirer;
        } else if (state.url != requirement.url && !state.urlConflictReported) {
            state.urlConflictReported = true;
            errors_.push_back("Package '" + requirement.package + "' is required from "
                              "different git sources: " + state.urlRequirer + " names " +
                              state.url + " and " + requirement.requirer + " names " +
                              requirement.url + "; every package must agree on a "
                              "dependency's source.");
        }
    }
    if (!state.requirements.empty() && !state.majorConflictReported &&
        !sameMajor(state.requirements.front().version, requirement.version)) {
        state.majorConflictReported = true;
        const Requirement& first = state.requirements.front();
        errors_.push_back("The requirements on package '" + requirement.package + "' span "
                          "major versions: " + first.requirer + " requires \"" +
                          first.version.text + "\" and " + requirement.requirer +
                          " requires \"" + requirement.version.text + "\"; different major "
                          "versions are incompatible, so one requirement must change.");
    }
    if (state.requirements.empty() ||
        compareVersions(requirement.version, state.selected) > 0) {
        state.selected = requirement.version;
    }
    state.requirements.push_back(std::move(requirement));
}

// A lock entry whose source still matches pins its version as one more requirement, so a
// version already locked is never silently downgraded and a satisfied lock reproduces
// exactly.
void Resolver::tryLockPin(const std::string& name, PackageState& state) {
    if (state.lockPinTried || state.url.empty() || state.requirements.empty()) return;
    state.lockPinTried = true;
    const LockPackage* locked = oldLock_.find(name);
    if (!locked || locked->url != state.url) return;
    Requirement pin;
    pin.package = name;
    if (!parseVersion(locked->version, pin.version)) return;
    if (!sameMajor(pin.version, state.requirements.front().version)) return;
    pin.url = locked->url;
    pin.requirer = lockfilePath(rootFolder_).string();
    addRequirement(std::move(pin));
}

// Materializes the selected version of one package: the tree comes from the content store
// when the lock already pins its hash, else from a fetch; the manifest at the tag root then
// names the package folder and contributes the package's own requirements.
void Resolver::loadSelected(const std::string& name, PackageState& state) {
    state.loadedVersion = state.selected.text;
    state.failed = false;
    const std::string& version = state.selected.text;

    const LockPackage* locked = oldLock_.find(name);
    bool pinned = locked && locked->version == version && locked->url == state.url;

    fs::path treeRoot;
    if (pinned) {
        std::error_code ec;
        fs::path stored = treeStorePath(locked->contentHash);
        if (fs::exists(stored / "ens.package", ec)) {
            treeRoot = stored;
            state.commit = locked->commit;
            state.contentHash = locked->contentHash;
        }
    }
    if (treeRoot.empty()) {
        TagInfo tag = resolveVersionTag(name, state.url, version, offline_);
        if (!tag.ok) {
            errors_.push_back(tag.error);
            state.failed = true;
            return;
        }
        GitTree tree = fetchTree(name, version, state.url, tag, offline_);
        if (!tree.ok) {
            errors_.push_back(tree.error);
            state.failed = true;
            return;
        }
        if (pinned && tree.contentHash != locked->contentHash) {
            errors_.push_back("The content of package '" + name + "' " + version +
                              " fetched from " + state.url + " does not match ens.lock: the "
                              "lock records " + locked->contentHash + " but the fetched tree "
                              "hashes to " + tree.contentHash + ". The tag '" + tag.tag +
                              "' may have been moved since the lock was written, or the "
                              "content tampered with; if the new content is intended, delete "
                              "ens.lock and rebuild to lock it afresh.");
            state.failed = true;
            return;
        }
        treeRoot = tree.storePath;
        state.commit = tag.commit;
        state.contentHash = tree.contentHash;
        notes_.push_back("Fetched " + name + " " + version + " from " + state.url + " (tag " +
                         tag.tag + ").");
    }

    // The tag root either declares the required package itself, or is a workspace whose
    // members are searched for it. Manifest problems inside the fetched tree surface later,
    // when the build loads the package through the workspace registry.
    std::vector<std::string> scratch;
    Manifest rootManifest = loadManifestFile(treeRoot / "ens.package", scratch);
    std::set<std::string> memberNames;
    fs::path packageFolder;
    Manifest packageManifest;
    if (rootManifest.form == ManifestForm::Package) {
        if (rootManifest.packageName != name) {
            errors_.push_back("The tag of package '" + name + "' " + version + " at " +
                              state.url + " declares package '" + rootManifest.packageName +
                              "', not '" + name + "'; the names must match exactly.");
            state.failed = true;
            return;
        }
        packageFolder = treeRoot;
        packageManifest = std::move(rootManifest);
    } else if (rootManifest.form == ManifestForm::Workspace) {
        std::vector<std::string> memberScratch;
        auto members = ens::modules::listWorkspaceMembers(treeRoot, memberScratch);
        std::string found;
        for (const auto& member : members) {
            memberNames.insert(member.packageName);
            if (!found.empty()) found += ", ";
            found += "'" + member.packageName + "'";
            if (member.packageName == name) packageFolder = member.folder;
        }
        if (packageFolder.empty()) {
            errors_.push_back("The tag of package '" + name + "' " + version + " at " +
                              state.url + " holds a workspace, and none of its members "
                              "declares package '" + name + "'; it declares " +
                              (found.empty() ? "no members" : found) + ".");
            state.failed = true;
            return;
        }
        std::vector<std::string> packageScratch;
        packageManifest = loadManifestFile(packageFolder / "ens.package", packageScratch);
    } else {
        errors_.push_back("The tag of package '" + name + "' " + version + " at " +
                          state.url + " has no package or workspace declaration in its "
                          "ens.package, so '" + name + "' cannot be resolved there.");
        state.failed = true;
        return;
    }

    state.packageFolder = packageFolder;
    state.artifacts = artifactsOf(packageManifest);
    state.lockRequirements.clear();
    for (const auto& dependency : packageManifest.dependencies) {
        if (!dependency.hasVersion) continue;
        if (memberNames.count(dependency.name)) continue;
        state.lockRequirements.push_back({dependency.name, dependency.version});
    }
    scanManifestRequirements(packageManifest, (packageFolder / "ens.package").string(),
                             memberNames);
}

void Resolver::reconcileLock(ResolutionOutcome& outcome) {
    Lockfile newLock;
    if (rootManifest_.form == ManifestForm::Package && !rootManifest_.packageName.empty()) {
        newLock.rootName = rootManifest_.packageName;
        newLock.rootArtifacts = artifactsOf(rootManifest_);
    }
    for (const auto& [name, state] : states_) {
        if (state.url.empty() || state.failed || state.packageFolder.empty()) continue;
        LockPackage entry;
        entry.name = name;
        entry.version = state.selected.text;
        entry.url = state.url;
        entry.commit = state.commit;
        entry.contentHash = state.contentHash;
        entry.requirements = state.lockRequirements;
        entry.artifacts = state.artifacts;
        newLock.packages.push_back(std::move(entry));
    }

    bool wantLock = !newLock.packages.empty();
    if (!wantLock) {
        if (oldLock_.present) {
            if (locked_) {
                errors_.push_back("The build has no git-sourced packages anymore, so "
                                  "ens.lock would be removed, and --locked forbids changing "
                                  "it; run the command without --locked to update the lock.");
                return;
            }
            std::error_code ec;
            fs::remove(lockfilePath(rootFolder_), ec);
            notes_.push_back("Removed ens.lock: the build has no git-sourced packages.");
        }
        return;
    }

    std::string oldText = oldLock_.present ? renderLockfile(oldLock_) : std::string();
    std::string newText = renderLockfile(newLock);
    if (oldText == newText) return;

    std::vector<std::string> changes;
    for (const auto& entry : newLock.packages) {
        const LockPackage* before = oldLock_.find(entry.name);
        if (!before) {
            changes.push_back("locked " + entry.name + " " + entry.version);
        } else if (before->version != entry.version) {
            changes.push_back("updated " + entry.name + " " + before->version + " -> " +
                              entry.version);
        } else if (before->contentHash != entry.contentHash ||
                   before->commit != entry.commit || before->url != entry.url) {
            changes.push_back("updated " + entry.name + " " + entry.version +
                              " (source changed)");
        }
    }
    for (const auto& before : oldLock_.packages) {
        bool kept = false;
        for (const auto& entry : newLock.packages) {
            if (entry.name == before.name) {
                kept = true;
                break;
            }
        }
        if (!kept) changes.push_back("removed " + before.name + " " + before.version);
    }
    if (changes.empty()) changes.push_back("updated the recorded requirements");
    std::string summary;
    for (const auto& change : changes) {
        if (!summary.empty()) summary += ", ";
        summary += change;
    }

    if (locked_) {
        errors_.push_back("ens.lock is out of date (" + summary + "), and --locked forbids "
                          "changing it; run the command without --locked to update the "
                          "lock.");
        return;
    }
    std::string writeError;
    if (!writeLockfile(rootFolder_, newLock, writeError)) {
        errors_.push_back(writeError);
        return;
    }
    notes_.push_back("Updated ens.lock: " + summary + ".");
}

void Resolver::run(ResolutionOutcome& outcome) {
    std::string lockError;
    oldLock_ = readLockfile(rootFolder_, lockError);
    if (!lockError.empty()) {
        outcome.ok = false;
        outcome.errors.push_back(lockError);
        return;
    }

    collectLocalRequirements();

    bool progress = true;
    while (progress) {
        progress = false;
        for (auto& [name, state] : states_) {
            if (state.requirements.empty() || state.url.empty()) continue;
            tryLockPin(name, state);
            if (state.loadedVersion != state.selected.text) {
                loadSelected(name, state);
                progress = true;
            }
        }
    }

    for (const auto& [name, state] : states_) {
        if (state.url.empty() || state.failed || state.packageFolder.empty()) continue;
        outcome.packages.folders.emplace(name, state.packageFolder);
    }

    if (errors_.empty()) {
        reconcileLock(outcome);
    }

    outcome.errors.insert(outcome.errors.end(), errors_.begin(), errors_.end());
    outcome.notes.insert(outcome.notes.end(), notes_.begin(), notes_.end());
    outcome.ok = outcome.errors.empty();
}

}  // namespace

ResolutionOutcome resolveGitPackages(const fs::path& rootFolder, bool offline, bool locked) {
    ResolutionOutcome outcome;
    outcome.packages.offline = offline;
    if (rootFolder.empty()) return outcome;
    std::error_code ec;
    if (!fs::exists(rootFolder / "ens.package", ec)) return outcome;
    Resolver resolver(rootFolder, offline, locked);
    resolver.run(outcome);
    return outcome;
}

}  // namespace ens::packages
