#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace ens::packages {

namespace fs = std::filesystem;

// ens.lock records the exact result of resolving a build's git-sourced packages, so later
// builds reproduce it without the network and so the native code a build links is reviewable
// in one place. The file is machine-owned: the driver writes it, users commit it, nobody
// edits it. The format is line-oriented and deterministic:
//
//   lock 1
//   root <package name>                            the root manifest's package, when it is one
//   package <name> <version>                       one entry per fetched package, sorted by name
//   source <url> <commit>                          where it came from (the commit is provenance)
//   content sha256:<hex>                           the canonical content hash - the identity
//   require <name> <version>                       the entry's own git requirements, sorted
//   artifact <library> <platform> <url> sha256:<hex>   native artifact bindings, sorted
//
// `root` carries only artifact lines; workspace-form roots have no root line. Members and
// overridden packages never appear.

struct LockRequirement {
    std::string name;
    std::string version;
};

struct LockArtifact {
    std::string library;
    std::string platform;
    std::string url;
    std::string hash;
};

struct LockPackage {
    std::string name;
    std::string version;
    std::string url;
    std::string commit;
    std::string contentHash;
    std::vector<LockRequirement> requirements;
    std::vector<LockArtifact> artifacts;
};

struct Lockfile {
    bool present = false;
    std::string rootName;
    std::vector<LockArtifact> rootArtifacts;
    std::vector<LockPackage> packages;

    const LockPackage* find(const std::string& name) const;
};

fs::path lockfilePath(const fs::path& rootFolder);

// Reads the lock next to the root manifest. A missing file yields `present` false; an
// unreadable or unrecognized file is an error (the file is machine-owned, so the remedy is
// to delete it and rebuild).
Lockfile readLockfile(const fs::path& rootFolder, std::string& error);

// Renders the lock in its canonical byte-exact form: entries sorted by package name, lines
// in a fixed order within each entry, "\n" line endings.
std::string renderLockfile(const Lockfile& lock);

bool writeLockfile(const fs::path& rootFolder, const Lockfile& lock, std::string& error);

}  // namespace ens::packages
