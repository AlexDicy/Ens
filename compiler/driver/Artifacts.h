#pragma once

#include <filesystem>
#include <string>

namespace ens::packages {

namespace fs = std::filesystem;

// A native artifact resolved to a local file in the content store.
struct ArtifactResult {
    bool ok = false;
    fs::path file;
    std::string error;
};

// Resolves the artifact at `url` with the manifest-declared `hash` ("sha256:" + 64 hex
// digits): the cached copy when the store has it, else a fetch with the system curl followed
// by verification against the hash (a mismatch is an error naming both hashes) and an atomic
// publish into the store. Fetching is an error under `offline`.
ArtifactResult fetchArtifact(const std::string& url, const std::string& hash, bool offline);

}  // namespace ens::packages
