#pragma once

#include <filesystem>
#include <string>

namespace ens::packages {

namespace fs = std::filesystem;

// The on-disk cache shared by every ens process: the ENS_CACHE environment variable when
// set, else ~/.ens/cache. Layout under the root:
//   git/<key>/           bare git mirrors, one per source URL; a disposable fetch optimization
//   trees/sha256-<hex>/  immutable package trees, keyed by their canonical content hash
//   artifacts/<hex>/     fetched native artifacts, keyed by their declared sha256
//   tmp/                 staging folders, on the same volume so renames into the store are atomic
fs::path cacheRoot();

// The canonical content hash of a package tree - the tree's identity everywhere: the
// `content` value in ens.lock and the tree store's key. This function is the one definition
// of the construction: every regular file under `root` (the `.git` folder excluded), ordered
// by its relative path written with forward slashes in UTF-8 and compared as raw bytes,
// contributes its path, a zero byte, its size in decimal ASCII, a zero byte, and its exact
// content bytes to a single sha256. The result is "sha256:" followed by 64 lowercase hex
// digits.
bool hashTree(const fs::path& root, std::string& hash, std::string& error);

// The sha256 of one file in the same "sha256:<hex>" spelling, for artifact verification.
bool hashFile(const fs::path& file, std::string& hash, std::string& error);

// Where the tree with `hash` lives in the store, whether or not it is present.
fs::path treeStorePath(const std::string& hash);

// Publishes `tree`, a staging folder produced by this process, into the store under `hash`.
// The publish is atomic and idempotent: the folder is renamed into place, an entry that
// already exists wins (concurrent ens processes share the cache), and the stored files are
// read-only. `tree` is consumed either way.
bool publishTree(const fs::path& tree, const std::string& hash, std::string& error);

// The same for a single artifact file, keyed by its verified hash. `fileName` keeps the
// URL's base name so the linker receives a meaningfully named file.
fs::path artifactStorePath(const std::string& hash, const std::string& fileName);
bool publishArtifact(const fs::path& file, const std::string& hash,
                     const std::string& fileName, std::string& error);

// A fresh uniquely named staging folder under the cache's tmp/.
bool createStagingFolder(fs::path& folder, std::string& error);

}  // namespace ens::packages
