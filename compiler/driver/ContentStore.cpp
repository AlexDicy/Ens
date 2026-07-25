#include "ContentStore.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <system_error>
#include <vector>

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/SHA256.h"

namespace ens::packages {

namespace {

std::string toHex(const std::array<uint8_t, 32>& digest) {
    static const char* alphabet = "0123456789abcdef";
    std::string hex;
    hex.reserve(64);
    for (uint8_t byte : digest) {
        hex.push_back(alphabet[byte >> 4]);
        hex.push_back(alphabet[byte & 0xf]);
    }
    return hex;
}

std::string hexOf(const std::string& hash) {
    constexpr std::string_view prefix = "sha256:";
    if (hash.size() > prefix.size() && hash.compare(0, prefix.size(), prefix) == 0) {
        return hash.substr(prefix.size());
    }
    return hash;
}

bool updateWithFile(llvm::SHA256& sha, const fs::path& file, std::string& error) {
    std::ifstream stream(file, std::ios::binary);
    if (!stream) {
        error = "could not read " + file.string();
        return false;
    }
    std::vector<char> buffer(1 << 16);
    while (stream) {
        stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        std::streamsize got = stream.gcount();
        if (got > 0) {
            sha.update(llvm::StringRef(buffer.data(), static_cast<size_t>(got)));
        }
    }
    if (stream.bad()) {
        error = "could not read " + file.string();
        return false;
    }
    return true;
}

void makeWritable(const fs::path& root) {
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end;
         it.increment(ec)) {
        fs::permissions(it->path(), fs::perms::owner_write, fs::perm_options::add, ec);
        ec.clear();
    }
}

void makeReadOnly(const fs::path& root) {
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end;
         it.increment(ec)) {
        if (it->is_regular_file(ec)) {
            fs::permissions(it->path(),
                            fs::perms::owner_write | fs::perms::group_write |
                                fs::perms::others_write,
                            fs::perm_options::remove, ec);
        }
        ec.clear();
    }
}

void removeStaging(const fs::path& folder) {
    makeWritable(folder);
    std::error_code ec;
    fs::remove_all(folder, ec);
}

// Renames `staged` to `destination`; a destination that already exists (possibly published
// by a concurrent process) wins, and the staging copy is discarded.
bool publishByRename(const fs::path& staged, const fs::path& destination, std::string& error) {
    std::error_code ec;
    fs::create_directories(destination.parent_path(), ec);
    if (fs::exists(destination, ec)) {
        removeStaging(staged);
        return true;
    }
    fs::rename(staged, destination, ec);
    if (!ec) return true;
    if (fs::exists(destination, ec)) {
        removeStaging(staged);
        return true;
    }
    error = "could not move " + staged.string() + " into the cache at " +
            destination.string();
    removeStaging(staged);
    return false;
}

}  // namespace

fs::path cacheRoot() {
    if (const char* configured = std::getenv("ENS_CACHE"); configured && *configured) {
        return fs::path(configured);
    }
    llvm::SmallString<128> home;
    if (llvm::sys::path::home_directory(home)) {
        return fs::path(home.str().str()) / ".ens" / "cache";
    }
    return fs::path(".ens-cache");
}

bool hashTree(const fs::path& root, std::string& hash, std::string& error) {
    std::vector<std::string> paths;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root, ec), end; it != end; it.increment(ec)) {
        if (ec) {
            error = "could not list " + root.string();
            return false;
        }
        if (it->is_directory()) {
            if (it->path().filename() == ".git") it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file()) continue;
        paths.push_back(it->path().lexically_relative(root).generic_u8string());
    }
    if (ec) {
        error = "could not list " + root.string();
        return false;
    }
    std::sort(paths.begin(), paths.end());

    llvm::SHA256 sha;
    for (const auto& relative : paths) {
        fs::path file = root / fs::u8path(relative);
        uintmax_t size = fs::file_size(file, ec);
        if (ec) {
            error = "could not read " + file.string();
            return false;
        }
        sha.update(relative);
        sha.update(llvm::StringRef("\0", 1));
        sha.update(std::to_string(size));
        sha.update(llvm::StringRef("\0", 1));
        if (!updateWithFile(sha, file, error)) return false;
    }
    hash = "sha256:" + toHex(sha.final());
    return true;
}

bool hashFile(const fs::path& file, std::string& hash, std::string& error) {
    llvm::SHA256 sha;
    if (!updateWithFile(sha, file, error)) return false;
    hash = "sha256:" + toHex(sha.final());
    return true;
}

fs::path treeStorePath(const std::string& hash) {
    return cacheRoot() / "trees" / ("sha256-" + hexOf(hash));
}

bool publishTree(const fs::path& tree, const std::string& hash, std::string& error) {
    makeReadOnly(tree);
    return publishByRename(tree, treeStorePath(hash), error);
}

fs::path artifactStorePath(const std::string& hash, const std::string& fileName) {
    return cacheRoot() / "artifacts" / hexOf(hash) / fileName;
}

bool publishArtifact(const fs::path& file, const std::string& hash,
                     const std::string& fileName, std::string& error) {
    fs::path destination = artifactStorePath(hash, fileName);
    std::error_code ec;
    fs::create_directories(destination.parent_path(), ec);
    if (fs::exists(destination, ec)) {
        fs::remove(file, ec);
        return true;
    }
    fs::rename(file, destination, ec);
    if (!ec) {
        fs::permissions(destination,
                        fs::perms::owner_write | fs::perms::group_write |
                            fs::perms::others_write,
                        fs::perm_options::remove, ec);
        return true;
    }
    if (fs::exists(destination, ec)) {
        fs::remove(file, ec);
        return true;
    }
    error = "could not move the fetched artifact into the cache at " + destination.string();
    fs::remove(file, ec);
    return false;
}

bool createStagingFolder(fs::path& folder, std::string& error) {
    fs::path base = cacheRoot() / "tmp";
    std::error_code ec;
    fs::create_directories(base, ec);
    if (ec) {
        error = "could not create the cache folder " + base.string();
        return false;
    }
    llvm::SmallString<128> unique;
    if (llvm::sys::fs::createUniqueDirectory((base / "stage").string(), unique)) {
        error = "could not create a staging folder under " + base.string();
        return false;
    }
    folder = fs::path(unique.str().str());
    return true;
}

}  // namespace ens::packages
