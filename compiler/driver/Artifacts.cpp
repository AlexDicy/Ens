#include "Artifacts.h"

#include <cctype>
#include <system_error>

#include "ContentStore.h"
#include "Process.h"

namespace ens::packages {

namespace {

std::string lowered(std::string text) {
    for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

// The file name the artifact keeps in the store: the URL's base name without any query or
// fragment, so the linker sees a meaningful library file.
std::string fileNameOf(const std::string& url) {
    std::string trimmed = url;
    size_t marker = trimmed.find_first_of("?#");
    if (marker != std::string::npos) trimmed = trimmed.substr(0, marker);
    while (!trimmed.empty() && trimmed.back() == '/') trimmed.pop_back();
    size_t slash = trimmed.rfind('/');
    std::string name = slash == std::string::npos ? trimmed : trimmed.substr(slash + 1);
    return name.empty() ? "artifact" : name;
}

}  // namespace

ArtifactResult fetchArtifact(const std::string& url, const std::string& hash, bool offline) {
    ArtifactResult result;
    std::string declared = lowered(hash);
    std::string fileName = fileNameOf(url);

    fs::path stored = artifactStorePath(declared, fileName);
    std::error_code ec;
    if (fs::exists(stored, ec)) {
        result.ok = true;
        result.file = stored;
        return result;
    }

    if (offline) {
        result.error = "--offline forbids fetching the native artifact at " + url + ", and "
                       "it is not in the local cache; run the command once without --offline.";
        return result;
    }
    std::string curl = findProgram("curl");
    if (curl.empty()) {
        result.error = "Fetching the native artifact at " + url + " needs the 'curl' "
                       "command, which was not found on PATH; install curl and retry.";
        return result;
    }

    fs::path staging;
    if (!createStagingFolder(staging, result.error)) return result;
    fs::path fetched = staging / fileName;
    ProcessResult download = runProcess(curl, {"--fail", "--silent", "--show-error",
                                               "--location", "--output", fetched.string(),
                                               url});
    if (download.exitCode != 0) {
        result.error = "Could not fetch the native artifact at " + url +
                       (download.startError.empty() ? "" : ": " + download.startError) +
                       (download.errorOutput.empty() ? "" : ": " + download.errorOutput);
        while (!result.error.empty() &&
               (result.error.back() == '\n' || result.error.back() == '\r')) {
            result.error.pop_back();
        }
        result.error += ".";
        fs::remove_all(staging, ec);
        return result;
    }

    std::string computed;
    if (!hashFile(fetched, computed, result.error)) {
        fs::remove_all(staging, ec);
        return result;
    }
    if (computed != declared) {
        result.error = "The artifact fetched from " + url + " hashes to " + computed +
                       ", but the manifest declares " + declared + "; refusing to use it. If "
                       "the artifact changed intentionally, update the hash in the manifest.";
        fs::remove_all(staging, ec);
        return result;
    }

    if (!publishArtifact(fetched, declared, fileName, result.error)) {
        fs::remove_all(staging, ec);
        return result;
    }
    fs::remove_all(staging, ec);
    result.ok = true;
    result.file = artifactStorePath(declared, fileName);
    return result;
}

}  // namespace ens::packages
