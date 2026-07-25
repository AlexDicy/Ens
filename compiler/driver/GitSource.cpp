#include "GitSource.h"

#include <fstream>
#include <string_view>
#include <system_error>
#include <vector>

#include "ContentStore.h"
#include "Process.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/SHA256.h"

namespace ens::packages {

namespace {

std::string trimmed(std::string text) {
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) {
        text.pop_back();
    }
    size_t start = 0;
    while (start < text.size() && (text[start] == '\n' || text[start] == '\r' ||
                                   text[start] == ' ')) {
        start++;
    }
    return text.substr(start);
}

std::string offlineError(const std::string& packageName, const std::string& version,
                         const std::string& url) {
    return "--offline forbids fetching package '" + packageName + "' " + version + " from " +
           url + ", and it is not in the local cache; run the command once without --offline.";
}

std::string gitMissingError(const std::string& packageName) {
    return "Fetching package '" + packageName + "' needs the 'git' command, which was not "
           "found on PATH; install git and retry.";
}

// The bare mirror folder for one source URL: a readable slug plus a hash so distinct URLs
// never collide.
fs::path mirrorPath(const std::string& url) {
    std::string slug;
    for (char c : url) {
        bool keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
        slug.push_back(keep ? c : '-');
    }
    if (slug.size() > 64) slug = slug.substr(slug.size() - 64);
    llvm::SHA256 sha;
    sha.update(url);
    auto digest = sha.final();
    static const char* alphabet = "0123456789abcdef";
    std::string hex;
    for (size_t i = 0; i < 6; ++i) {
        hex.push_back(alphabet[digest[i] >> 4]);
        hex.push_back(alphabet[digest[i] & 0xf]);
    }
    return cacheRoot() / "git" / (slug + "-" + hex);
}

bool commitPresent(const std::string& git, const fs::path& mirror, const std::string& commit) {
    ProcessResult probe = runProcess(git, {"-C", mirror.string(), "cat-file", "-e",
                                           commit + "^{commit}"});
    return probe.exitCode == 0;
}

struct TreeEntry {
    std::string mode;
    std::string type;
    std::string sha;
    std::string path;
};

// Parses `git ls-tree -r -z` output: "<mode> <type> <sha>\t<path>" entries separated by
// zero bytes, with paths in their exact bytes.
bool parseTreeListing(const std::string& listing, std::vector<TreeEntry>& entries,
                      std::string& error) {
    size_t position = 0;
    while (position < listing.size()) {
        size_t end = listing.find('\0', position);
        if (end == std::string::npos) end = listing.size();
        std::string_view record(listing.data() + position, end - position);
        position = end + 1;
        if (record.empty()) continue;
        size_t firstSpace = record.find(' ');
        size_t secondSpace = record.find(' ', firstSpace + 1);
        size_t tab = record.find('\t', secondSpace + 1);
        if (firstSpace == std::string_view::npos || secondSpace == std::string_view::npos ||
            tab == std::string_view::npos) {
            error = "Internal: unexpected git ls-tree output record '" + std::string(record) +
                    "'.";
            return false;
        }
        TreeEntry entry;
        entry.mode = std::string(record.substr(0, firstSpace));
        entry.type = std::string(record.substr(firstSpace + 1, secondSpace - firstSpace - 1));
        entry.sha = std::string(record.substr(secondSpace + 1, tab - secondSpace - 1));
        entry.path = std::string(record.substr(tab + 1));
        entries.push_back(std::move(entry));
    }
    return true;
}

// True when the relative path is safe to create under a staging folder.
bool isSafeTreePath(const std::string& path) {
    if (path.empty() || path.front() == '/') return false;
    size_t position = 0;
    while (position <= path.size()) {
        size_t end = path.find('/', position);
        if (end == std::string::npos) end = path.size();
        std::string_view component(path.data() + position, end - position);
        if (component.empty() || component == "." || component == "..") return false;
        position = end + 1;
    }
    return true;
}

// Removes a staging folder when extraction fails partway; a successful publish consumes the
// folder instead.
struct StagingGuard {
    fs::path folder;
    bool released = false;

    ~StagingGuard() {
        if (released || folder.empty()) return;
        std::error_code ec;
        fs::remove_all(folder, ec);
    }
};

bool writeFileBytes(const fs::path& file, std::string_view bytes, std::string& error) {
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    std::ofstream stream(file, std::ios::binary | std::ios::trunc);
    if (!stream) {
        error = "could not write " + file.string();
        return false;
    }
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    stream.close();
    if (!stream) {
        error = "could not write " + file.string();
        return false;
    }
    return true;
}

}  // namespace

TagInfo resolveVersionTag(const std::string& packageName, const std::string& url,
                          const std::string& version, bool offline) {
    TagInfo info;
    if (offline) {
        info.error = offlineError(packageName, version, url);
        return info;
    }
    std::string git = findProgram("git");
    if (git.empty()) {
        info.error = gitMissingError(packageName);
        return info;
    }
    ProcessResult listing = runProcess(git, {"ls-remote", "--tags", url});
    if (listing.exitCode != 0) {
        info.error = "Could not list the tags of package '" + packageName + "' at " + url +
                     (listing.startError.empty() ? "" : ": " + listing.startError) +
                     (listing.errorOutput.empty() ? "" : ": " + trimmed(listing.errorOutput)) +
                     ".";
        return info;
    }

    std::string plainCommit, plainPeeled, prefixedCommit, prefixedPeeled;
    bool plainExists = false, prefixedExists = false;
    size_t position = 0;
    while (position < listing.output.size()) {
        size_t end = listing.output.find('\n', position);
        if (end == std::string::npos) end = listing.output.size();
        std::string_view line(listing.output.data() + position, end - position);
        position = end + 1;
        size_t tab = line.find('\t');
        if (tab == std::string_view::npos) continue;
        std::string commit(line.substr(0, tab));
        std::string_view ref = line.substr(tab + 1);
        while (!ref.empty() && (ref.back() == '\r' || ref.back() == ' ')) {
            ref.remove_suffix(1);
        }
        constexpr std::string_view tagPrefix = "refs/tags/";
        if (ref.substr(0, tagPrefix.size()) != tagPrefix) continue;
        std::string_view name = ref.substr(tagPrefix.size());
        bool peeled = false;
        constexpr std::string_view peelSuffix = "^{}";
        if (name.size() > peelSuffix.size() &&
            name.substr(name.size() - peelSuffix.size()) == peelSuffix) {
            peeled = true;
            name.remove_suffix(peelSuffix.size());
        }
        if (name == version) {
            plainExists = true;
            (peeled ? plainPeeled : plainCommit) = commit;
        } else if (name == "v" + version) {
            prefixedExists = true;
            (peeled ? prefixedPeeled : prefixedCommit) = commit;
        }
    }

    if (plainExists && prefixedExists) {
        info.error = "Both tags '" + version + "' and 'v" + version + "' exist at " + url +
                     ", so version \"" + version + "\" of package '" + packageName +
                     "' is ambiguous; remove or rename one of the tags.";
        return info;
    }
    if (!plainExists && !prefixedExists) {
        info.error = "Package '" + packageName + "' has no tag '" + version + "' or 'v" +
                     version + "' at " + url + "; a git dependency fetches a release tag. For "
                     "unreleased code, point an override at a local checkout with "
                     "'ens override add " + packageName + " <folder>'.";
        return info;
    }
    info.ok = true;
    info.tag = plainExists ? version : "v" + version;
    const std::string& peeled = plainExists ? plainPeeled : prefixedPeeled;
    const std::string& direct = plainExists ? plainCommit : prefixedCommit;
    info.commit = peeled.empty() ? direct : peeled;
    if (info.commit.empty()) {
        info.error = "Internal: git ls-remote returned tag '" + info.tag + "' at " + url +
                     " without a commit id.";
        info.ok = false;
    }
    return info;
}

GitTree fetchTree(const std::string& packageName, const std::string& version,
                  const std::string& url, const TagInfo& tag, bool offline) {
    GitTree tree;
    std::string git = findProgram("git");
    if (git.empty()) {
        tree.error = gitMissingError(packageName);
        return tree;
    }

    fs::path mirror = mirrorPath(url);
    std::error_code ec;
    if (!fs::exists(mirror / "HEAD", ec)) {
        fs::create_directories(mirror, ec);
        ProcessResult init = runProcess(git, {"init", "--bare", "--quiet", mirror.string()});
        if (init.exitCode != 0) {
            tree.error = "Could not prepare the git cache at " + mirror.string() +
                         (init.errorOutput.empty() ? "" : ": " + trimmed(init.errorOutput)) +
                         ".";
            return tree;
        }
    }

    if (!commitPresent(git, mirror, tag.commit)) {
        if (offline) {
            tree.error = offlineError(packageName, version, url);
            return tree;
        }
        ProcessResult fetched = runProcess(
            git, {"-C", mirror.string(), "fetch", "--quiet", "--no-tags", "--depth", "1", url,
                  "+refs/tags/" + tag.tag + ":refs/tags/" + tag.tag});
        if (fetched.exitCode != 0) {
            tree.error = "Could not fetch tag '" + tag.tag + "' of package '" + packageName +
                         "' from " + url +
                         (fetched.errorOutput.empty() ? ""
                                                      : ": " + trimmed(fetched.errorOutput)) +
                         ".";
            return tree;
        }
        if (!commitPresent(git, mirror, tag.commit)) {
            tree.error = "The tag '" + tag.tag + "' at " + url + " moved while it was being "
                         "fetched; retry the command.";
            return tree;
        }
    }

    ProcessResult listing = runProcess(git, {"-C", mirror.string(), "ls-tree", "-r", "-z",
                                             tag.commit});
    if (listing.exitCode != 0) {
        tree.error = "Could not read the tree of tag '" + tag.tag + "' of package '" +
                     packageName + "'" +
                     (listing.errorOutput.empty() ? "" : ": " + trimmed(listing.errorOutput)) +
                     ".";
        return tree;
    }
    std::vector<TreeEntry> entries;
    if (!parseTreeListing(listing.output, entries, tree.error)) return tree;

    for (const auto& entry : entries) {
        if (entry.path == ".gitmodules" || entry.type == "commit") {
            tree.error = "Package '" + packageName + "' " + version + " at " + url + " uses "
                         "git submodules, but packages must be self-contained: either declare "
                         "the submodule's content as a dependency of '" + packageName + "', "
                         "or commit the files into the repository.";
            return tree;
        }
        if (entry.mode == "120000") {
            tree.error = "Package '" + packageName + "' " + version + " at " + url +
                         " contains the symbolic link '" + entry.path + "', which packages "
                         "cannot carry; commit regular files instead.";
            return tree;
        }
        if (!isSafeTreePath(entry.path)) {
            tree.error = "Package '" + packageName + "' " + version + " contains the unsafe "
                         "path '" + entry.path + "' and cannot be extracted.";
            return tree;
        }
    }

    fs::path staging;
    if (!createStagingFolder(staging, tree.error)) return tree;
    StagingGuard guard{staging};

    // One `git cat-file --batch` run streams every blob's exact bytes: requests go in
    // through a file on stdin, responses come back as '<sha> blob <size>', the bytes, and a
    // closing newline each.
    llvm::SmallString<128> requestsFile;
    if (llvm::sys::fs::createTemporaryFile("ens-blobs", "txt", requestsFile)) {
        tree.error = "could not create a temporary file for the fetch";
        return tree;
    }
    {
        std::string requests;
        for (const auto& entry : entries) requests += entry.sha + "\n";
        if (!writeFileBytes(fs::path(requestsFile.str().str()), requests, tree.error)) {
            llvm::sys::fs::remove(requestsFile);
            return tree;
        }
    }
    ProcessResult blobs = runProcess(git, {"-C", mirror.string(), "cat-file", "--batch"},
                                     fs::path(requestsFile.str().str()));
    llvm::sys::fs::remove(requestsFile);
    if (blobs.exitCode != 0) {
        tree.error = "Could not read the files of package '" + packageName + "' from the git "
                     "cache" +
                     (blobs.errorOutput.empty() ? "" : ": " + trimmed(blobs.errorOutput)) + ".";
        return tree;
    }

    const std::string& batch = blobs.output;
    size_t position = 0;
    for (const auto& entry : entries) {
        size_t headerEnd = batch.find('\n', position);
        if (headerEnd == std::string::npos) {
            tree.error = "Internal: truncated git cat-file output for package '" +
                         packageName + "'.";
            return tree;
        }
        std::string_view header(batch.data() + position, headerEnd - position);
        size_t lastSpace = header.rfind(' ');
        if (lastSpace == std::string_view::npos || header.find(" missing") != std::string_view::npos) {
            tree.error = "Internal: git object " + entry.sha + " of package '" + packageName +
                         "' is missing from the mirror.";
            return tree;
        }
        size_t size = 0;
        for (char c : header.substr(lastSpace + 1)) {
            if (c < '0' || c > '9') {
                tree.error = "Internal: unexpected git cat-file header '" +
                             std::string(header) + "'.";
                return tree;
            }
            size = size * 10 + static_cast<size_t>(c - '0');
        }
        position = headerEnd + 1;
        if (position + size > batch.size()) {
            tree.error = "Internal: truncated git cat-file output for package '" +
                         packageName + "'.";
            return tree;
        }
        std::string_view bytes(batch.data() + position, size);
        position += size + 1;  // the response's closing newline
        if (!writeFileBytes(staging / fs::u8path(entry.path), bytes, tree.error)) {
            return tree;
        }
    }

    if (!hashTree(staging, tree.contentHash, tree.error)) return tree;
    guard.released = true;
    if (!publishTree(staging, tree.contentHash, tree.error)) return tree;
    tree.storePath = treeStorePath(tree.contentHash);
    tree.ok = true;
    return tree;
}

}  // namespace ens::packages
