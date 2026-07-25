#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace ens::modules {

namespace fs = std::filesystem;

// In-memory form of an `ens.package` or `ens.overrides` manifest. Everything the compiler knows
// about the manifest file format lives in Manifest.cpp; the workspace loader consumes this model.

struct ManifestNativeBinding {
    std::string platform;
    std::vector<std::string> baseNames;
    bool isArtifact = false;
    std::string artifactUrl;
    std::string artifactChecksum;
    int line = 0;
    int column = 0;
};

struct ManifestNative {
    std::string name;
    bool isSystem = false;
    bool hasBindingBlock = false;
    std::vector<ManifestNativeBinding> bindings;
    int line = 0;
    int column = 0;
};

struct ManifestDependency {
    std::string name;
    bool hasVersion = false;
    std::string version;
    int line = 0;
    int column = 0;
};

struct ManifestMember {
    std::string folder;
    int line = 0;
    int column = 0;
};

struct ManifestOverride {
    std::string name;
    std::string folder;
    int line = 0;
    int column = 0;
    // The declaration's span in the manifest text, in UTF-16 code units: from the `override`
    // keyword through the terminating ';'. Lets tooling edit one declaration in place.
    uint32_t startOffset = 0;
    uint32_t endOffset = 0;
};

enum class ManifestForm { None, Package, Workspace, Overrides };

struct Manifest {
    ManifestForm form = ManifestForm::None;
    std::string packageName;
    bool hasVersion = false;
    std::string version;
    bool hasEnsVersion = false;
    std::string ensVersion;
    std::vector<ManifestDependency> dependencies;
    std::vector<ManifestNative> natives;
    std::vector<ManifestMember> members;
    std::vector<ManifestOverride> overrides;
    // The offset of the '}' closing an overrides declaration, in UTF-16 code units, when the
    // block was closed. Lets tooling insert a declaration at the end of the block.
    bool hasOverridesClose = false;
    uint32_t overridesCloseOffset = 0;
};

// Parses manifest text. Problems are appended to `errors` as "<path>:<line>:<col>: <message>";
// parsing recovers and continues where possible.
Manifest parseManifestText(const std::string& path, std::u16string_view text,
                           std::vector<std::string>& errors);

// Semantic validation shared by every manifest load: version and hash shapes, the system/binding
// exclusivity rule, and the artifact-fetching limitation.
void validateManifest(const Manifest& manifest, const std::string& path,
                      std::vector<std::string>& errors);

// Reads, parses, and validates `file`. An unreadable file appends an error and yields an empty
// manifest.
Manifest loadManifestFile(const fs::path& file, std::vector<std::string>& errors);

// True when `text` is a well-formed version string: numerals separated by dots ("1", "1.3.0").
bool isValidVersionText(std::string_view text);

// True when `text` is a well-formed language version: exactly major.minor ("1.2").
bool isValidEnsVersionText(std::string_view text);

}  // namespace ens::modules
