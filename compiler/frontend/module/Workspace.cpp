#include "module/Workspace.h"

#include <system_error>
#include <utility>

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

std::string positionOf(const std::string& manifestPath, int line, int column) {
    return manifestPath + ":" + std::to_string(line) + ":" + std::to_string(column) + ": ";
}

}  // namespace

std::optional<PackageMatch> matchPackage(
    const std::unordered_map<std::u16string, fs::path>& deps,
    const std::vector<std::u16string>& segments) {
    if (deps.empty() || segments.empty()) return std::nullopt;

    // Try the longest prefix first so `@a.b.c` prefers the package `a.b` over the package `a`.
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
        if (fs::exists(d / "ens.package", ec)) return d;
        if (d == d.parent_path()) break;
    }
    return {};
}

std::vector<WorkspaceMember> listWorkspaceMembers(const fs::path& workspaceRoot,
                                                  std::vector<std::string>& errors) {
    std::vector<WorkspaceMember> members;
    fs::path manifestFile = workspaceRoot / "ens.package";
    std::string manifestPath = manifestFile.string();
    Manifest manifest = loadManifestFile(manifestFile, errors);
    if (manifest.form != ManifestForm::Workspace) {
        errors.push_back(manifestPath + ": Expected a workspace declaration.");
        return members;
    }
    std::error_code ec;
    for (const auto& member : manifest.members) {
        auto at = [&] { return positionOf(manifestPath, member.line, member.column); };
        fs::path folder = (workspaceRoot / member.folder).lexically_normal();
        fs::path memberManifest = folder / "ens.package";
        if (!fs::exists(memberManifest, ec)) {
            errors.push_back(at() + "Workspace member \"" + member.folder + "\" has no "
                             "ens.package manifest; every member folder declares a package.");
            continue;
        }
        std::vector<std::string> memberErrors;
        Manifest declaration = loadManifestFile(memberManifest, memberErrors);
        if (declaration.form != ManifestForm::Package) {
            errors.push_back(at() + "Workspace member \"" + member.folder + "\" must declare "
                             "a package.");
            continue;
        }
        bool duplicate = false;
        for (const auto& existing : members) {
            if (existing.packageName == declaration.packageName) {
                errors.push_back(at() + "Members \"" + existing.folder.string() + "\" and \"" +
                                 folder.string() + "\" both declare package '" +
                                 declaration.packageName + "'; package names must be unique "
                                 "within a workspace.");
                duplicate = true;
                break;
            }
        }
        if (duplicate) continue;
        WorkspaceMember info;
        info.packageName = declaration.packageName;
        info.folder = folder;
        for (const auto& dependency : declaration.dependencies) {
            info.dependencies.push_back(dependency.name);
        }
        members.push_back(std::move(info));
    }
    return members;
}

Workspace& WorkspaceRegistry::create(const fs::path& folder, const fs::path& srcRoot,
                                     const fs::path& testsRoot, std::u16string prefix) {
    auto ws = std::make_unique<Workspace>();
    ws->root = folder;
    ws->srcRoot = srcRoot;
    ws->testsRoot = testsRoot;
    ws->packagePrefix = std::move(prefix);

    Workspace* raw = ws.get();
    owned_.push_back(std::move(ws));
    return *raw;
}

const Manifest& WorkspaceRegistry::manifestFor(const fs::path& manifestFile) {
    std::string key = canonicalFolderKey(manifestFile);
    auto it = manifests_.find(key);
    if (it != manifests_.end()) return it->second;
    Manifest manifest = loadManifestFile(manifestFile, errors_);
    return manifests_.emplace(std::move(key), std::move(manifest)).first->second;
}

const WorkspaceRegistry::MemberIndex& WorkspaceRegistry::memberIndexFor(const fs::path& folder) {
    std::string key = canonicalFolderKey(folder);
    auto cached = memberIndexes_.find(key);
    if (cached != memberIndexes_.end()) return cached->second;

    MemberIndex index;
    fs::path manifestFile = folder / "ens.package";
    std::error_code ec;
    if (fs::exists(manifestFile, ec)) {
        const Manifest& manifest = manifestFor(manifestFile);
        if (manifest.form == ManifestForm::Workspace) {
            index.isWorkspace = true;
            std::string manifestPath = manifestFile.string();
            for (const auto& member : manifest.members) {
                auto at = [&] { return positionOf(manifestPath, member.line, member.column); };
                fs::path memberFolder = (folder / member.folder).lexically_normal();
                fs::path memberManifest = memberFolder / "ens.package";
                if (!fs::exists(memberManifest, ec)) {
                    errors_.push_back(at() + "Workspace member \"" + member.folder +
                                      "\" has no ens.package manifest; every member folder "
                                      "declares a package.");
                    continue;
                }
                const Manifest& memberDeclaration = manifestFor(memberManifest);
                if (memberDeclaration.form != ManifestForm::Package) {
                    errors_.push_back(at() + "Workspace member \"" + member.folder +
                                      "\" must declare a package.");
                    continue;
                }
                const std::string& name = memberDeclaration.packageName;
                auto existing = index.foldersByName.find(name);
                if (existing != index.foldersByName.end()) {
                    errors_.push_back(at() + "Members \"" + existing->second.string() +
                                      "\" and \"" + memberFolder.string() + "\" both declare "
                                      "package '" + name + "'; package names must be unique "
                                      "within a workspace.");
                    continue;
                }
                index.foldersByName.emplace(name, memberFolder);
                index.memberFolderKeys.insert(canonicalFolderKey(memberFolder));
            }
        }
    }
    return memberIndexes_.emplace(std::move(key), std::move(index)).first->second;
}

// The member index a package resolves its dependencies against: the nearest ancestor holding a
// workspace-form manifest, and only when that workspace lists the package as a member.
const WorkspaceRegistry::MemberIndex* WorkspaceRegistry::enclosingMemberIndex(
        const fs::path& packageFolder) {
    std::error_code ec;
    fs::path folder = fs::absolute(packageFolder, ec);
    if (ec) folder = packageFolder;
    folder = folder.lexically_normal();
    std::string packageKey = canonicalFolderKey(folder);

    for (fs::path d = folder.parent_path();; d = d.parent_path()) {
        if (d.empty()) break;
        if (fs::exists(d / "ens.package", ec)) {
            const MemberIndex& index = memberIndexFor(d);
            if (index.isWorkspace) {
                return index.memberFolderKeys.count(packageKey) ? &index : nullptr;
            }
        }
        if (d == d.parent_path()) break;
    }
    return nullptr;
}

void WorkspaceRegistry::applyPackageManifest(Workspace& ws, const Manifest& manifest) {
    ws.hasPackageManifest = true;
    ws.packageName = toU16(manifest.packageName);
    ws.natives = manifest.natives;
    for (const auto& native : manifest.natives) {
        ws.nativeNames.push_back(toU16(native.name));
    }
    resolveDependencies(ws, manifest);
}

void WorkspaceRegistry::resolveDependencies(Workspace& ws, const Manifest& manifest) {
    const MemberIndex* enclosing = nullptr;
    bool enclosingComputed = false;
    for (const auto& dependency : manifest.dependencies) {
        auto at = [&] {
            return positionOf(ws.manifestPath, dependency.line, dependency.column);
        };
        auto overridden = overrides_.find(dependency.name);
        if (overridden != overrides_.end()) {
            if (!dependency.hasVersion) {
                errors_.push_back(at() + "Dependency '" + dependency.name + "' declares no "
                                  "version; a dependency is versionless only when it resolves "
                                  "to a workspace member. Add the version this package "
                                  "requires, for example 'dependency " + dependency.name +
                                  " \"1.0\";'.");
            }
            ws.deps.emplace(toU16(dependency.name), overridden->second);
            continue;
        }
        if (!enclosingComputed) {
            enclosing = enclosingMemberIndex(ws.root);
            enclosingComputed = true;
        }
        if (enclosing) {
            auto member = enclosing->foldersByName.find(dependency.name);
            if (member != enclosing->foldersByName.end()) {
                if (dependency.hasVersion) {
                    errors_.push_back(at() + "Dependency '" + dependency.name + "' resolves "
                                      "to the workspace member at " + member->second.string() +
                                      "; a member is used as-is, so remove the version \"" +
                                      dependency.version + "\".");
                }
                if (dependency.hasSource) {
                    errors_.push_back(at() + "Dependency '" + dependency.name + "' resolves "
                                      "to the workspace member at " + member->second.string() +
                                      "; a member is used as-is, so remove the from clause.");
                }
                ws.deps.emplace(toU16(dependency.name), member->second);
                continue;
            }
        }
        auto fetched = gitPackages_.find(dependency.name);
        if (fetched != gitPackages_.end()) {
            if (!dependency.hasVersion) {
                errors_.push_back(at() + "Dependency '" + dependency.name + "' declares no "
                                  "version; a dependency is versionless only when it resolves "
                                  "to a workspace member. Add the version this package "
                                  "requires, for example 'dependency " + dependency.name +
                                  " \"1.0\";'.");
            }
            ws.deps.emplace(toU16(dependency.name), fetched->second);
            continue;
        }
        if (dependency.hasSource) {
            errors_.push_back(at() + "Dependency '" + dependency.name + "' names a git "
                              "source that was not fetched for this build; git sources are "
                              "fetched by the ens build, check, run, and test commands. To "
                              "use a local copy instead, point an override at a checkout "
                              "with 'ens override add " + dependency.name + " <folder>'.");
            continue;
        }
        errors_.push_back(at() + "No source for package '" + dependency.name + "': it is not "
                          "a member of the enclosing workspace, no override redirects it, and "
                          "it names no git source. Add a from clause with the package's git "
                          "URL, for example 'dependency " + dependency.name + " \"1.0\" from "
                          "\"https://github.com/owner/repo.git\";', or point an override at a "
                          "local folder.");
    }
}

void WorkspaceRegistry::setResolvedGitPackages(std::unordered_map<std::string, fs::path> folders) {
    gitPackages_ = std::move(folders);
}

void WorkspaceRegistry::loadRootOverrides(const fs::path& rootFolder) {
    fs::path file = rootFolder / "ens.overrides";
    std::error_code ec;
    if (!fs::exists(file, ec)) return;

    std::string filePath = file.string();
    const Manifest& manifest = manifestFor(file);
    if (manifest.form != ManifestForm::Overrides && manifest.form != ManifestForm::None) {
        errors_.push_back(filePath + ": An ens.overrides file holds a single overrides "
                          "declaration; packages and workspaces are declared in ens.package.");
        return;
    }
    for (const auto& override : manifest.overrides) {
        auto at = [&] { return positionOf(filePath, override.line, override.column); };
        fs::path target = (rootFolder / override.folder).lexically_normal();
        fs::path targetManifest = target / "ens.package";
        if (!fs::exists(targetManifest, ec)) {
            errors_.push_back(at() + "The override for package '" + override.name +
                              "' points at " + target.string() + ", which has no ens.package "
                              "manifest.");
            continue;
        }
        const Manifest& targetDeclaration = manifestFor(targetManifest);
        if (targetDeclaration.form != ManifestForm::Package) {
            errors_.push_back(at() + "The override for package '" + override.name +
                              "' points at " + target.string() + ", which does not declare a "
                              "package.");
            continue;
        }
        if (targetDeclaration.packageName != override.name) {
            errors_.push_back(at() + "The override for package '" + override.name +
                              "' points at " + target.string() + ", which declares package '" +
                              targetDeclaration.packageName + "' instead; the names must match "
                              "exactly.");
            continue;
        }
        overrides_.emplace(override.name, target);
    }
}

void WorkspaceRegistry::noticeOverrideUse(const std::string& name, const fs::path& folder) {
    if (!noticedOverrides_.insert(name).second) return;
    notices_.push_back("Using the override for package '" + name + "' at " + folder.string() +
                       ".");
}

Workspace& WorkspaceRegistry::defineRoot(const fs::path& folder, const fs::path& srcRoot,
                                         const fs::path& testsRoot, bool withManifest,
                                         const fs::path& overridesFolder) {
    Workspace& ws = create(folder, srcRoot, testsRoot, std::u16string());
    byFolder_.emplace(canonicalFolderKey(folder), &ws);
    if (!withManifest) return ws;

    fs::path manifestFile = folder / "ens.package";
    ws.manifestPath = manifestFile.string();
    const Manifest& manifest = manifestFor(manifestFile);
    loadRootOverrides(overridesFolder.empty() ? folder : overridesFolder);

    switch (manifest.form) {
        case ManifestForm::Package:
            applyPackageManifest(ws, manifest);
            break;
        case ManifestForm::Workspace:
            // A workspace declares members without being a package itself: modules under its
            // root belong to no package and resolve no dependencies. The index is built here
            // so member problems surface on every build rooted at the workspace.
            ws.isWorkspaceRoot = true;
            memberIndexFor(folder);
            break;
        case ManifestForm::Overrides:
            errors_.push_back(ws.manifestPath + ": An ens.package file holds a package or "
                              "workspace declaration; overrides live in the ens.overrides "
                              "file next to it.");
            break;
        case ManifestForm::None:
            errors_.push_back(ws.manifestPath + ": Expected a 'package' or 'workspace' "
                              "declaration.");
            break;
    }
    return ws;
}

Workspace* WorkspaceRegistry::getOrLoad(const fs::path& folder, const std::u16string& prefix) {
    std::string key = canonicalFolderKey(folder);
    for (const auto& [name, overrideFolder] : overrides_) {
        if (canonicalFolderKey(overrideFolder) == key) {
            noticeOverrideUse(name, overrideFolder);
            break;
        }
    }
    auto it = byFolder_.find(key);
    if (it != byFolder_.end()) return it->second;

    // A dependency is consumed through its `src/` only; its own tests never take part in a
    // dependent's build, so it carries no tests root (which would otherwise enable the
    // root-only src/tests fallback for its bare imports).
    Workspace& ws = create(folder, folder / "src", /*testsRoot=*/{}, prefix);
    byFolder_.emplace(std::move(key), &ws);

    fs::path manifestFile = folder / "ens.package";
    ws.manifestPath = manifestFile.string();
    std::error_code ec;
    if (!fs::exists(manifestFile, ec)) {
        errors_.push_back("Package folder " + folder.string() + " has no ens.package "
                          "manifest.");
        return &ws;
    }
    const Manifest& manifest = manifestFor(manifestFile);
    if (manifest.form == ManifestForm::Package) {
        applyPackageManifest(ws, manifest);
    } else {
        errors_.push_back(ws.manifestPath + ": Expected a package declaration; a package "
                          "consumed as a dependency cannot hold a workspace or overrides "
                          "declaration.");
    }
    return &ws;
}

std::vector<CollectedNative> WorkspaceRegistry::collectNatives() const {
    std::vector<CollectedNative> collected;
    for (const auto& ws : owned_) {
        for (const auto& native : ws->natives) {
            collected.push_back({native, ws->manifestPath});
        }
    }
    return collected;
}

}  // namespace ens::modules
