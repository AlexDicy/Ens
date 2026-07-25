#include "Lockfile.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <sstream>

namespace ens::packages {

namespace {

std::vector<std::string> splitWords(const std::string& line) {
    std::vector<std::string> words;
    std::istringstream stream(line);
    std::string word;
    while (stream >> word) words.push_back(word);
    return words;
}

std::string unreadableError(const fs::path& file, const std::string& reason) {
    return file.string() + " " + reason + "; ens.lock is machine-owned, so delete it and run "
           "the build again to regenerate it.";
}

void renderArtifacts(std::string& out, std::vector<LockArtifact> artifacts) {
    std::sort(artifacts.begin(), artifacts.end(),
              [](const LockArtifact& a, const LockArtifact& b) {
                  if (a.library != b.library) return a.library < b.library;
                  return a.platform < b.platform;
              });
    for (const auto& artifact : artifacts) {
        out += "artifact " + artifact.library + " " + artifact.platform + " " + artifact.url +
               " " + artifact.hash + "\n";
    }
}

}  // namespace

const LockPackage* Lockfile::find(const std::string& name) const {
    for (const auto& package : packages) {
        if (package.name == name) return &package;
    }
    return nullptr;
}

fs::path lockfilePath(const fs::path& rootFolder) {
    return rootFolder / "ens.lock";
}

Lockfile readLockfile(const fs::path& rootFolder, std::string& error) {
    Lockfile lock;
    fs::path file = lockfilePath(rootFolder);
    std::error_code ec;
    if (!fs::exists(file, ec)) return lock;

    std::ifstream stream(file, std::ios::binary);
    if (!stream) {
        error = unreadableError(file, "could not be read");
        return lock;
    }
    std::string text((std::istreambuf_iterator<char>(stream)),
                     std::istreambuf_iterator<char>());

    bool sawHeader = false;
    bool inRoot = false;
    LockPackage* current = nullptr;
    size_t lineNumber = 0;
    size_t position = 0;
    while (position <= text.size()) {
        size_t end = text.find('\n', position);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(position, end - position);
        position = end + 1;
        lineNumber++;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) {
            if (position > text.size()) break;
            continue;
        }
        std::vector<std::string> words = splitWords(line);
        const std::string& keyword = words.front();
        auto malformed = [&]() {
            error = unreadableError(file, "has an unrecognized line " +
                                    std::to_string(lineNumber) + " ('" + line + "')");
            return lock;
        };
        if (!sawHeader) {
            if (words.size() != 2 || keyword != "lock" || words[1] != "1") {
                error = unreadableError(file, "is not in a lock format this ens understands "
                                        "(expected 'lock 1')");
                return lock;
            }
            sawHeader = true;
            continue;
        }
        if (keyword == "root" && words.size() == 2) {
            lock.rootName = words[1];
            inRoot = true;
            current = nullptr;
            continue;
        }
        if (keyword == "package" && words.size() == 3) {
            lock.packages.push_back(LockPackage{});
            current = &lock.packages.back();
            current->name = words[1];
            current->version = words[2];
            inRoot = false;
            continue;
        }
        if (keyword == "source" && words.size() == 3 && current) {
            current->url = words[1];
            current->commit = words[2];
            continue;
        }
        if (keyword == "content" && words.size() == 2 && current) {
            current->contentHash = words[1];
            continue;
        }
        if (keyword == "require" && words.size() == 3 && current) {
            current->requirements.push_back({words[1], words[2]});
            continue;
        }
        if (keyword == "artifact" && words.size() == 5 && (current || inRoot)) {
            LockArtifact artifact{words[1], words[2], words[3], words[4]};
            if (current) {
                current->artifacts.push_back(std::move(artifact));
            } else {
                lock.rootArtifacts.push_back(std::move(artifact));
            }
            continue;
        }
        return malformed();
    }
    if (!sawHeader) {
        error = unreadableError(file, "is empty");
        return lock;
    }
    lock.present = true;
    return lock;
}

std::string renderLockfile(const Lockfile& lock) {
    std::string out = "lock 1\n";
    if (!lock.rootName.empty()) {
        out += "root " + lock.rootName + "\n";
        renderArtifacts(out, lock.rootArtifacts);
    }
    std::vector<LockPackage> packages = lock.packages;
    std::sort(packages.begin(), packages.end(),
              [](const LockPackage& a, const LockPackage& b) { return a.name < b.name; });
    for (const auto& package : packages) {
        out += "package " + package.name + " " + package.version + "\n";
        out += "source " + package.url + " " + package.commit + "\n";
        out += "content " + package.contentHash + "\n";
        std::vector<LockRequirement> requirements = package.requirements;
        std::sort(requirements.begin(), requirements.end(),
                  [](const LockRequirement& a, const LockRequirement& b) {
                      return a.name < b.name;
                  });
        for (const auto& requirement : requirements) {
            out += "require " + requirement.name + " " + requirement.version + "\n";
        }
        renderArtifacts(out, package.artifacts);
    }
    return out;
}

bool writeLockfile(const fs::path& rootFolder, const Lockfile& lock, std::string& error) {
    fs::path file = lockfilePath(rootFolder);
    std::ofstream stream(file, std::ios::binary | std::ios::trunc);
    if (!stream) {
        error = "Could not write " + file.string() + ".";
        return false;
    }
    std::string text = renderLockfile(lock);
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    stream.close();
    if (!stream) {
        error = "Could not write " + file.string() + ".";
        return false;
    }
    return true;
}

}  // namespace ens::packages
