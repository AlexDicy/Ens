#pragma once

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace ens::packages {

namespace fs = std::filesystem;

// The git-sourced packages of one build, resolved by the driver before compilation: each
// dependency name maps to the folder holding the fetched package (a content-store tree, or
// a member folder inside one).
struct ResolvedPackages {
    std::unordered_map<std::string, fs::path> folders;
    bool offline = false;
};

struct ResolutionOutcome {
    bool ok = true;
    ResolvedPackages packages;
    std::vector<std::string> errors;
    std::vector<std::string> notes;
};

// Resolves every git-sourced dependency reachable from the root manifest at `rootFolder`
// (the manifests of its workspace members and override targets included), and reconciles
// the result with the ens.lock next to it. Requirements are gathered transitively - a
// fetched package's manifest contributes its own - and each package resolves to the highest
// version any requirer names; requirements that span major versions are an error, and so is
// the same package required from two different URLs. A lock that already satisfies the
// requirements pins every version and content hash, so cached builds touch no network;
// otherwise versions are re-resolved minimally and the lock is rewritten, which `locked`
// turns into an error. Any fetch is an error under `offline`.
ResolutionOutcome resolveGitPackages(const fs::path& rootFolder, bool offline, bool locked);

}  // namespace ens::packages
