#pragma once

#include <filesystem>
#include <string>

namespace ens::packages {

namespace fs = std::filesystem;

// One resolved release tag at a git source: the tag's name and the commit it points to
// (peeled for annotated tags). The commit is provenance; a package's identity is the
// canonical content hash of its tree.
struct TagInfo {
    bool ok = false;
    std::string tag;
    std::string commit;
    std::string error;
};

// A package tree fetched from a git source and published to the content store.
struct GitTree {
    bool ok = false;
    std::string contentHash;
    fs::path storePath;
    std::string error;
};

// Resolves `version` to a tag at `url` with `git ls-remote --tags`: the tag spelled exactly
// like the version, falling back to "v" plus the version. Both spellings existing is an
// error, and so is neither. `packageName` names the package in every error.
TagInfo resolveVersionTag(const std::string& packageName, const std::string& url,
                          const std::string& version, bool offline);

// Fetches the resolved commit into the per-URL bare mirror (a depth-1 fetch of exactly that
// tag), extracts the committed tree with its exact bytes (blobs are read straight from the
// object store, so no filter or line-ending conversion ever applies), computes the canonical
// content hash, and publishes the tree to the content store. A tree that carries a
// .gitmodules file at its root or any symbolic link is rejected: packages are self-contained.
GitTree fetchTree(const std::string& packageName, const std::string& version,
                  const std::string& url, const TagInfo& tag, bool offline);

}  // namespace ens::packages
