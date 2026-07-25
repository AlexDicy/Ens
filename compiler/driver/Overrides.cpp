#include "Overrides.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include "module/Manifest.h"
#include "module/Workspace.h"
#include "semantic/Literals.h"

namespace fs = std::filesystem;

namespace {

using namespace ens::modules;

int usageError(const std::string& message) {
    std::cerr << "ERROR: " << message << '\n';
    std::cerr << "Run 'ens help override' for the command's forms.\n";
    return 2;
}

bool isBlank(char16_t c) {
    return c == u' ' || c == u'\t' || c == u'\r';
}

// Reads and decodes ens.overrides. Returns false after reporting a problem. A missing file
// yields empty text and `exists` false.
bool readOverridesText(const fs::path& file, std::u16string& text, bool& exists) {
    std::error_code ec;
    exists = fs::exists(file, ec);
    if (!exists) return true;
    std::ifstream stream(file, std::ios::binary);
    if (!stream) {
        std::cerr << "ERROR: Could not read " << file.string() << ".\n";
        return false;
    }
    std::string bytes((std::istreambuf_iterator<char>(stream)),
                      std::istreambuf_iterator<char>());
    Utf8DecodeError decodeError;
    if (!decodeUtf8ToUtf16(bytes, text, decodeError)) {
        std::cerr << "ERROR: " << file.string() << ": "
                  << describeUtf8DecodeError(decodeError) << '\n';
        return false;
    }
    return true;
}

bool writeOverridesText(const fs::path& file, const std::u16string& text) {
    std::ofstream stream(file, std::ios::binary | std::ios::trunc);
    if (!stream) {
        std::cerr << "ERROR: Could not write " << file.string() << ".\n";
        return false;
    }
    std::string bytes = utf16ToUtf8(text);
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return true;
}

bool isWhitespaceOnly(const std::u16string& text) {
    for (char16_t c : text) {
        if (!isBlank(c) && c != u'\n') return false;
    }
    return true;
}

// Parses the overrides file for editing. Returns false after reporting why it cannot be
// edited (parse errors, or a declaration that is not an overrides block).
bool parseForEditing(const fs::path& file, const std::u16string& text, Manifest& out) {
    std::vector<std::string> errors;
    out = parseManifestText(file.string(), text, errors);
    if (!errors.empty()) {
        for (const auto& e : errors) std::cerr << "ERROR: " << e << '\n';
        std::cerr << "Fix " << file.string() << " by hand before editing it with "
                  << "'ens override'.\n";
        return false;
    }
    if (out.form != ManifestForm::Overrides || !out.hasOverridesClose) {
        std::cerr << "ERROR: " << file.string() << " does not hold an overrides declaration; "
                  << "an ens.overrides file holds a single 'overrides { ... }' block.\n";
        return false;
    }
    return true;
}

// The reason the override's target is not usable, or "" when it is valid: the folder must
// hold an ens.package declaring exactly the overridden package, the same rule the compiler
// applies when it loads the file.
std::string targetProblem(const fs::path& root, const std::string& name,
                          const std::string& folder) {
    fs::path target = (root / folder).lexically_normal();
    std::error_code ec;
    fs::path manifestFile = target / "ens.package";
    if (!fs::exists(manifestFile, ec)) {
        return "there is no ens.package manifest at " + target.string();
    }
    std::vector<std::string> errors;
    Manifest declaration = loadManifestFile(manifestFile, errors);
    if (declaration.form != ManifestForm::Package) {
        return target.string() + " does not declare a package";
    }
    if (declaration.packageName != name) {
        return target.string() + " declares package '" + declaration.packageName +
               "' instead; the names must match exactly";
    }
    return "";
}

int listOverrides(const fs::path& root, const fs::path& file) {
    std::u16string text;
    bool exists = false;
    if (!readOverridesText(file, text, exists)) return 1;
    if (!exists || isWhitespaceOnly(text)) {
        std::cout << "No overrides at " << file.string() << ".\n";
        return 0;
    }
    std::vector<std::string> errors;
    Manifest manifest = parseManifestText(file.string(), text, errors);
    if (!errors.empty()) {
        for (const auto& e : errors) std::cerr << "ERROR: " << e << '\n';
        return 1;
    }
    if (manifest.overrides.empty()) {
        std::cout << "No overrides at " << file.string() << ".\n";
        return 0;
    }
    for (const auto& override : manifest.overrides) {
        std::string problem = targetProblem(root, override.name, override.folder);
        std::cout << override.name << " -> " << override.folder
                  << (problem.empty() ? " (ok)" : " (invalid: " + problem + ")") << '\n';
    }
    return 0;
}

int addOverride(const fs::path& root, const fs::path& file, const std::string& name,
                const std::string& folderArgument) {
    // The name must be a package path; the real manifest grammar decides.
    {
        std::u16string probeText;
        Utf8DecodeError decodeError;
        if (!decodeUtf8ToUtf16("overrides { override " + name + " \"x\"; }", probeText,
                               decodeError)) {
            return usageError("'" + name + "' is not a valid package name.");
        }
        std::vector<std::string> probeErrors;
        Manifest probe = parseManifestText("<arguments>", probeText, probeErrors);
        if (!probeErrors.empty() || probe.overrides.size() != 1 ||
            probe.overrides.front().name != name) {
            return usageError("'" + name + "' is not a valid package name; use dotted "
                              "identifiers such as 'acme.json'.");
        }
    }

    // The folder is taken relative to the current folder and stored relative to the
    // workspace root, the base the compiler resolves it against.
    fs::path given = folderArgument;
    fs::path targetAbsolute =
        (given.is_absolute() ? given : fs::current_path() / given).lexically_normal();
    std::error_code ec;
    fs::path relative = fs::relative(targetAbsolute, root, ec);
    std::string stored = (ec || relative.empty()) ? targetAbsolute.generic_string()
                                                  : relative.generic_string();
    if (stored.find('"') != std::string::npos) {
        return usageError("The folder path cannot contain '\"'.");
    }
    std::string problem = targetProblem(root, name, stored);
    if (!problem.empty()) {
        std::cerr << "ERROR: The override for package '" << name << "' cannot be added: "
                  << problem << ".\n";
        return 1;
    }

    std::u16string declarationText;
    Utf8DecodeError decodeError;
    decodeUtf8ToUtf16("override " + name + " \"" + stored + "\";", declarationText,
                      decodeError);

    std::u16string text;
    bool exists = false;
    if (!readOverridesText(file, text, exists)) return 1;
    if (!exists || isWhitespaceOnly(text)) {
        std::u16string fresh;
        decodeUtf8ToUtf16("overrides {\n", fresh, decodeError);
        fresh += u"    " + declarationText + u"\n}\n";
        if (!writeOverridesText(file, fresh)) return 1;
        std::cout << "Added the override for package '" << name << "': " << stored << ".\n";
        return 0;
    }

    Manifest manifest;
    if (!parseForEditing(file, text, manifest)) return 1;

    const ManifestOverride* existing = nullptr;
    for (const auto& override : manifest.overrides) {
        if (override.name == name) {
            existing = &override;
            break;
        }
    }
    if (existing) {
        text.replace(existing->startOffset, existing->endOffset - existing->startOffset,
                     declarationText);
        if (!writeOverridesText(file, text)) return 1;
        std::cout << "Replaced the override for package '" << name << "': now " << stored
                  << ".\n";
        return 0;
    }

    // Insert before the block's closing brace: on the brace's own line when the brace starts
    // it, otherwise breaking the line in front of the brace.
    size_t close = manifest.overridesCloseOffset;
    size_t lineStart = close;
    while (lineStart > 0 && text[lineStart - 1] != u'\n') lineStart--;
    bool braceStartsLine = true;
    for (size_t i = lineStart; i < close; ++i) {
        if (!isBlank(text[i])) {
            braceStartsLine = false;
            break;
        }
    }
    if (braceStartsLine) {
        text.insert(lineStart, u"    " + declarationText + u"\n");
    } else {
        text.insert(close, u"\n    " + declarationText + u"\n");
    }
    if (!writeOverridesText(file, text)) return 1;
    std::cout << "Added the override for package '" << name << "': " << stored << ".\n";
    return 0;
}

int removeOverride(const fs::path& file, const std::string& name) {
    std::u16string text;
    bool exists = false;
    if (!readOverridesText(file, text, exists)) return 1;
    if (!exists || isWhitespaceOnly(text)) {
        std::cerr << "ERROR: No override for package '" << name << "' at " << file.string()
                  << ".\n";
        return 1;
    }
    Manifest manifest;
    if (!parseForEditing(file, text, manifest)) return 1;

    const ManifestOverride* found = nullptr;
    for (const auto& override : manifest.overrides) {
        if (override.name == name) {
            found = &override;
            break;
        }
    }
    if (!found) {
        std::cerr << "ERROR: No override for package '" << name << "' at " << file.string()
                  << ".\n";
        return 1;
    }

    // Remove whole lines when the declaration stands alone on them.
    size_t begin = found->startOffset;
    size_t end = found->endOffset;
    size_t lineStart = begin;
    while (lineStart > 0 && text[lineStart - 1] != u'\n') lineStart--;
    bool prefixBlank = true;
    for (size_t i = lineStart; i < begin; ++i) {
        if (!isBlank(text[i])) {
            prefixBlank = false;
            break;
        }
    }
    size_t lineEnd = end;
    while (lineEnd < text.size() && text[lineEnd] != u'\n') lineEnd++;
    bool suffixBlank = true;
    for (size_t i = end; i < lineEnd; ++i) {
        if (!isBlank(text[i])) {
            suffixBlank = false;
            break;
        }
    }
    if (prefixBlank && suffixBlank) {
        begin = lineStart;
        end = lineEnd < text.size() ? lineEnd + 1 : lineEnd;
    }
    text.erase(begin, end - begin);
    if (!writeOverridesText(file, text)) return 1;
    std::cout << "Removed the override for package '" << name << "'.\n";
    return 0;
}

}  // namespace

int runOverrideCommand(int argc, char* argv[]) {
    if (argc < 3) {
        return usageError("'ens override' needs a form: add <package> <folder>, "
                          "remove <package>, or list.");
    }
    std::string verb = argv[2];

    fs::path root = discoverWorkspaceRoot(fs::current_path());
    if (root.empty()) {
        return usageError("No ens.package manifest was found here or in any parent folder; "
                          "overrides live next to a workspace's manifest.");
    }
    fs::path file = root / "ens.overrides";

    if (verb == "list") {
        if (argc > 3) return usageError("'ens override list' takes no arguments.");
        return listOverrides(root, file);
    }
    if (verb == "add") {
        if (argc != 5) {
            return usageError("'ens override add' takes a package name and a folder, for "
                              "example 'ens override add acme.json ../json'.");
        }
        return addOverride(root, file, argv[3], argv[4]);
    }
    if (verb == "remove") {
        if (argc != 4) {
            return usageError("'ens override remove' takes a package name, for example "
                              "'ens override remove acme.json'.");
        }
        return removeOverride(file, argv[3]);
    }
    return usageError("Unknown form '" + verb + "' for 'ens override'; use add, remove, or "
                      "list.");
}
