#include "module/Manifest.h"

#include <cctype>
#include <fstream>
#include <iterator>
#include <optional>

#include "Version.h"
#include "diagnostics/DiagnosticSink.h"
#include "parser/Tokenizer.h"
#include "semantic/Literals.h"

namespace ens::modules {

namespace {

std::string stringValue(const LexedToken& token) {
    std::u16string raw = token.text;
    if (raw.size() >= 2 && raw.front() == u'"' && raw.back() == u'"') {
        raw = raw.substr(1, raw.size() - 2);
    }
    return utf16ToUtf8(raw);
}

// A recursive-descent parser over the significant-token stream. Recovery skips to the next ';'
// or declaration boundary so one malformed item never hides the rest of the file.
class ManifestParser {
public:
    ManifestParser(const std::string& path, const std::vector<LexedToken>& tokens,
                   std::vector<std::string>& errors)
        : path_(path), tokens_(tokens), errors_(errors) {}

    Manifest parse() {
        Manifest manifest;
        while (!atEnd()) {
            if (atDeclarationStart()) {
                if (manifest.form == ManifestForm::None) {
                    parseDeclaration(manifest);
                } else {
                    errorHere("A manifest file holds exactly one declaration; remove the extra "
                              "'" + utf16ToUtf8(current().text) + "' declaration.");
                    Manifest extra;
                    parseDeclaration(extra);
                }
            } else {
                errorHere("Expected 'package', 'workspace', or 'overrides' to begin the "
                          "manifest declaration.");
                skipDeclaration();
            }
        }
        return manifest;
    }

private:
    const std::string& path_;
    const std::vector<LexedToken>& tokens_;
    std::vector<std::string>& errors_;
    size_t index_ = 0;

    const LexedToken& current() const { return tokens_[index_]; }
    SyntaxKind kind() const { return current().kind; }
    bool atEnd() const { return kind() == SyntaxKind::EndOfFile; }
    bool at(SyntaxKind k) const { return kind() == k; }

    bool atWord(std::u16string_view word) const {
        return kind() == SyntaxKind::Identifier && current().text == word;
    }

    bool peekIsWord(std::u16string_view word) const {
        if (index_ + 1 >= tokens_.size()) return false;
        const LexedToken& next = tokens_[index_ + 1];
        return next.kind == SyntaxKind::Identifier && next.text == word;
    }

    SyntaxKind peekKind() const {
        if (index_ + 1 >= tokens_.size()) return SyntaxKind::EndOfFile;
        return tokens_[index_ + 1].kind;
    }

    void advance() {
        if (!atEnd()) ++index_;
    }

    void errorAt(const LexedToken& token, const std::string& message) {
        errors_.push_back(path_ + ":" + std::to_string(token.line) + ":" +
                          std::to_string(token.column) + ": " + message);
    }

    void errorHere(const std::string& message) { errorAt(current(), message); }

    bool atDeclarationStart() const {
        return at(SyntaxKind::KwPackage) || atWord(u"workspace") || atWord(u"overrides");
    }

    void parseDeclaration(Manifest& manifest) {
        if (at(SyntaxKind::KwPackage)) {
            manifest.form = ManifestForm::Package;
            advance();
            if (auto name = parsePackagePath("Expected the package name after 'package', for "
                                             "example 'package acme.tools'.")) {
                manifest.packageName = *name;
            }
            parseBlock([&] { parsePackageItem(manifest); });
            return;
        }
        if (atWord(u"workspace")) {
            manifest.form = ManifestForm::Workspace;
            advance();
            parseBlock([&] { parseWorkspaceItem(manifest); });
            return;
        }
        manifest.form = ManifestForm::Overrides;
        advance();
        if (auto close = parseBlock([&] { parseOverridesItem(manifest); })) {
            manifest.hasOverridesClose = true;
            manifest.overridesCloseOffset = *close;
        }
    }

    // Returns the offset of the closing '}' when the block was closed.
    template <typename ParseItem>
    std::optional<uint32_t> parseBlock(ParseItem parseItem) {
        if (at(SyntaxKind::LBrace)) {
            advance();
        } else {
            errorHere("Expected '{' to begin the declaration's body.");
        }
        while (!at(SyntaxKind::RBrace) && !atEnd()) {
            parseItem();
        }
        if (at(SyntaxKind::RBrace)) {
            uint32_t close = current().offset;
            advance();
            return close;
        }
        errorHere("Expected '}' to close the declaration.");
        return std::nullopt;
    }

    void parsePackageItem(Manifest& manifest) {
        if (atWord(u"version")) {
            if (manifest.hasVersion) {
                errorHere("The package already declares 'version'; remove the duplicate.");
            }
            manifest.hasVersion = true;
            parseValueItem(manifest.version,
                           "Expected the package version as a string, for example "
                           "'version \"1.3.0\";'.");
            return;
        }
        if (atWord(u"ens")) {
            if (manifest.hasEnsVersion) {
                errorHere("The package already declares 'ens'; remove the duplicate.");
            }
            manifest.hasEnsVersion = true;
            parseValueItem(manifest.ensVersion,
                           "Expected the language version as a string, for example "
                           "'ens \"1.2\";'.");
            return;
        }
        if (atWord(u"dependency")) {
            parseDependency(manifest);
            return;
        }
        if (atWord(u"native")) {
            parseNative(manifest);
            return;
        }
        errorHere("Expected a package item: 'version', 'ens', 'dependency', or 'native'.");
        skipItem();
    }

    void parseValueItem(std::string& value, const char* valueMessage) {
        advance();
        if (at(SyntaxKind::StringLiteral)) {
            value = stringValue(current());
            advance();
        } else {
            errorHere(valueMessage);
            skipItem();
            return;
        }
        expectSemicolon();
    }

    void parseDependency(Manifest& manifest) {
        ManifestDependency dependency;
        dependency.line = current().line;
        dependency.column = current().column;
        advance();
        auto name = parsePackagePath("Expected the dependency's package name, for example "
                                     "'dependency acme.json;'.");
        if (!name) {
            skipItem();
            return;
        }
        dependency.name = *name;
        if (at(SyntaxKind::StringLiteral)) {
            dependency.hasVersion = true;
            dependency.version = stringValue(current());
            advance();
        }
        if (atWord(u"from")) {
            advance();
            if (at(SyntaxKind::StringLiteral)) {
                dependency.hasSource = true;
                dependency.sourceUrl = stringValue(current());
                advance();
            } else {
                errorHere("Expected the package's git URL as a string after 'from', for "
                          "example 'dependency " + dependency.name + " \"2.0\" from "
                          "\"https://github.com/acme/json.git\";'.");
                skipItem();
                manifest.dependencies.push_back(std::move(dependency));
                return;
            }
        }
        expectSemicolon();
        manifest.dependencies.push_back(std::move(dependency));
    }

    void parseNative(Manifest& manifest) {
        ManifestNative native;
        native.line = current().line;
        native.column = current().column;
        advance();
        if (!at(SyntaxKind::Identifier)) {
            errorHere("Expected the native library's name, for example 'native zlib;'.");
            skipItem();
            return;
        }
        native.name = utf16ToUtf8(current().text);
        advance();
        if (atWord(u"system")) {
            native.isSystem = true;
            advance();
        }
        if (at(SyntaxKind::LBrace)) {
            native.hasBindingBlock = true;
            advance();
            while (!at(SyntaxKind::RBrace) && !atEnd()) {
                parseNativeBinding(native);
            }
            if (at(SyntaxKind::RBrace)) {
                advance();
            } else {
                errorHere("Expected '}' to close the binding block.");
            }
        } else {
            expectSemicolon();
        }
        manifest.natives.push_back(std::move(native));
    }

    void parseNativeBinding(ManifestNative& native) {
        if (!at(SyntaxKind::Identifier)) {
            errorHere("Expected a platform binding, for example 'windows \"LLVM-C\";'.");
            skipItem();
            return;
        }
        ManifestNativeBinding binding;
        binding.platform = utf16ToUtf8(current().text);
        binding.line = current().line;
        binding.column = current().column;
        if (peekIsWord(u"artifact")) {
            binding.isArtifact = true;
            advance();
            advance();
            if (at(SyntaxKind::StringLiteral)) {
                binding.artifactUrl = stringValue(current());
                advance();
            } else {
                errorHere("Expected the artifact URL as a string.");
                skipItem();
                return;
            }
            if (atWord(u"hash")) {
                advance();
            } else {
                errorHere("Expected 'hash' and the artifact's checksum after the URL.");
            }
            if (at(SyntaxKind::StringLiteral)) {
                binding.artifactChecksum = stringValue(current());
                advance();
            } else {
                errorHere("Expected the artifact's checksum as a string, for example "
                          "'hash \"sha256:...\"'.");
                skipItem();
                native.bindings.push_back(std::move(binding));
                return;
            }
            expectSemicolon();
            native.bindings.push_back(std::move(binding));
            return;
        }
        advance();
        if (!at(SyntaxKind::StringLiteral)) {
            errorHere("Expected at least one library base name as a string, for example "
                      "'windows \"LLVM-C\";'.");
            skipItem();
            return;
        }
        while (at(SyntaxKind::StringLiteral)) {
            binding.baseNames.push_back(stringValue(current()));
            advance();
        }
        expectSemicolon();
        native.bindings.push_back(std::move(binding));
    }

    void parseWorkspaceItem(Manifest& manifest) {
        if (!atWord(u"member")) {
            errorHere("Expected a member declaration, for example 'member \"frontend\";'.");
            skipItem();
            return;
        }
        ManifestMember member;
        member.line = current().line;
        member.column = current().column;
        advance();
        if (at(SyntaxKind::StringLiteral)) {
            member.folder = stringValue(current());
            advance();
        } else {
            errorHere("Expected the member's folder as a string, for example "
                      "'member \"frontend\";'.");
            skipItem();
            return;
        }
        expectSemicolon();
        manifest.members.push_back(std::move(member));
    }

    void parseOverridesItem(Manifest& manifest) {
        if (!at(SyntaxKind::KwOverride)) {
            errorHere("Expected an override declaration, for example "
                      "'override alex.library \"../library\";'.");
            skipItem();
            return;
        }
        ManifestOverride override;
        override.line = current().line;
        override.column = current().column;
        override.startOffset = current().offset;
        advance();
        auto name = parsePackagePath("Expected the overridden package's name after 'override'.");
        if (!name) {
            skipItem();
            return;
        }
        override.name = *name;
        if (at(SyntaxKind::StringLiteral)) {
            override.folder = stringValue(current());
            advance();
        } else {
            errorHere("Expected the replacement folder as a string, for example "
                      "'override alex.library \"../library\";'.");
            skipItem();
            return;
        }
        override.endOffset = current().offset;
        if (at(SyntaxKind::Semi)) {
            override.endOffset += static_cast<uint32_t>(current().text.size());
            advance();
        } else {
            errorHere("Expected ';' after the declaration.");
        }
        manifest.overrides.push_back(std::move(override));
    }

    std::optional<std::string> parsePackagePath(const char* missingMessage) {
        if (!at(SyntaxKind::Identifier)) {
            errorHere(missingMessage);
            return std::nullopt;
        }
        std::string name = utf16ToUtf8(current().text);
        advance();
        while (at(SyntaxKind::Dot) && peekKind() == SyntaxKind::Identifier) {
            advance();
            name += "." + utf16ToUtf8(current().text);
            advance();
        }
        return name;
    }

    void expectSemicolon() {
        if (at(SyntaxKind::Semi)) {
            advance();
        } else {
            errorHere("Expected ';' after the declaration.");
        }
    }

    // Junk inside a block: a ';' ends it and is consumed, a braced region is skipped whole, and
    // a '}' ends it unconsumed so the enclosing block can close.
    void skipItem() {
        while (!atEnd()) {
            if (at(SyntaxKind::RBrace)) return;
            if (at(SyntaxKind::Semi)) {
                advance();
                return;
            }
            if (at(SyntaxKind::LBrace)) {
                skipBalancedBraces();
                return;
            }
            advance();
        }
    }

    void skipDeclaration() {
        bool first = true;
        while (!atEnd()) {
            if (!first && atDeclarationStart()) return;
            if (at(SyntaxKind::LBrace)) {
                skipBalancedBraces();
                return;
            }
            advance();
            first = false;
        }
    }

    void skipBalancedBraces() {
        int depth = 0;
        while (!atEnd()) {
            if (at(SyntaxKind::LBrace)) depth++;
            if (at(SyntaxKind::RBrace)) depth--;
            advance();
            if (depth == 0) return;
        }
    }
};

bool isDottedNumerals(std::string_view text) {
    if (text.empty()) return false;
    bool digitInSegment = false;
    for (char c : text) {
        if (c == '.') {
            if (!digitInSegment) return false;
            digitInSegment = false;
        } else if (std::isdigit(static_cast<unsigned char>(c))) {
            digitInSegment = true;
        } else {
            return false;
        }
    }
    return digitInSegment;
}

bool isKnownPlatform(const std::string& platform) {
    return platform == "windows" || platform == "linux" || platform == "macos";
}

bool isValidChecksum(std::string_view text) {
    constexpr std::string_view prefix = "sha256:";
    if (text.size() != prefix.size() + 64) return false;
    if (text.substr(0, prefix.size()) != prefix) return false;
    for (char c : text.substr(prefix.size())) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    }
    return true;
}

void validateNative(const ManifestNative& native, const std::string& path,
                    std::vector<std::string>& errors) {
    auto errorAt = [&](int line, int column, const std::string& message) {
        errors.push_back(path + ":" + std::to_string(line) + ":" + std::to_string(column) +
                         ": " + message);
    };
    if (native.isSystem && native.hasBindingBlock) {
        errorAt(native.line, native.column, "Native library '" + native.name + "' is marked "
                "'system', so it is linked by platform defaults and cannot carry a binding "
                "block.");
    }
    std::vector<std::string> seenPlatforms;
    for (const auto& binding : native.bindings) {
        if (!isKnownPlatform(binding.platform)) {
            errorAt(binding.line, binding.column, "Unknown platform '" + binding.platform +
                    "' in native library '" + native.name + "'; bindings use 'windows', "
                    "'linux', or 'macos'.");
        }
        for (const auto& seen : seenPlatforms) {
            if (seen == binding.platform) {
                errorAt(binding.line, binding.column, "Platform '" + binding.platform +
                        "' is bound twice for native library '" + native.name + "'; remove "
                        "the duplicate.");
                break;
            }
        }
        seenPlatforms.push_back(binding.platform);
        if (binding.isArtifact && !isValidChecksum(binding.artifactChecksum)) {
            errorAt(binding.line, binding.column, "The artifact checksum for native "
                    "library '" + native.name + "' must look like \"sha256:\" followed by "
                    "64 hex digits.");
        }
        for (const auto& baseName : binding.baseNames) {
            if (baseName.empty()) {
                errorAt(binding.line, binding.column, "A library base name for native library "
                        "'" + native.name + "' cannot be empty.");
            }
        }
    }
}

}  // namespace

bool isValidVersionText(std::string_view text) {
    return isDottedNumerals(text);
}

bool isValidEnsVersionText(std::string_view text) {
    if (!isDottedNumerals(text)) return false;
    int dots = 0;
    for (char c : text) {
        if (c == '.') dots++;
    }
    return dots == 1;
}

Manifest parseManifestText(const std::string& path, std::u16string_view text,
                           std::vector<std::string>& errors) {
    DiagnosticSink lexSink;
    Tokenizer tokenizer(text, lexSink);
    std::vector<LexedToken> tokens;
    while (true) {
        LexedToken token = tokenizer.next();
        bool eof = token.kind == SyntaxKind::EndOfFile;
        if (!isTrivia(token.kind)) tokens.push_back(std::move(token));
        if (eof) break;
    }
    for (const auto& diagnostic : lexSink.list()) {
        errors.push_back(path + ":" + std::to_string(diagnostic.getSpan().line) + ":" +
                         std::to_string(diagnostic.getSpan().column) + ": " +
                         diagnostic.getMessage());
    }
    ManifestParser parser(path, tokens, errors);
    return parser.parse();
}

void validateManifest(const Manifest& manifest, const std::string& path,
                      std::vector<std::string>& errors) {
    auto errorAt = [&](int line, int column, const std::string& message) {
        errors.push_back(path + ":" + std::to_string(line) + ":" + std::to_string(column) +
                         ": " + message);
    };
    if (manifest.hasVersion && !isValidVersionText(manifest.version)) {
        errors.push_back(path + ": The version '" + manifest.version + "' is not a valid "
                         "version; use dotted numerals such as \"1.3.0\".");
    }
    if (manifest.form == ManifestForm::Package && !manifest.hasEnsVersion &&
        !manifest.packageName.empty()) {
        errors.push_back(path + ": Package '" + manifest.packageName + "' does not declare "
                         "the language version it is written for; add 'ens \"" +
                         std::string(kToolchainVersion) + "\";' to the package declaration.");
    }
    if (manifest.hasEnsVersion && !isValidEnsVersionText(manifest.ensVersion)) {
        errors.push_back(path + ": The language version '" + manifest.ensVersion + "' is not "
                         "valid; use major.minor, such as \"1.2\".");
    }
    for (size_t i = 0; i < manifest.dependencies.size(); ++i) {
        const auto& dependency = manifest.dependencies[i];
        if (dependency.hasVersion && !isValidVersionText(dependency.version)) {
            errorAt(dependency.line, dependency.column, "The version '" + dependency.version +
                    "' of dependency '" + dependency.name + "' is not a valid version; use "
                    "dotted numerals such as \"2.0\".");
        }
        if (dependency.hasSource && !dependency.hasVersion) {
            errorAt(dependency.line, dependency.column, "Dependency '" + dependency.name +
                    "' declares a git source but no version; a git source fetches a tagged "
                    "version, for example 'dependency " + dependency.name + " \"1.0\" from \"" +
                    dependency.sourceUrl + "\";'.");
        }
        if (dependency.hasSource && dependency.sourceUrl.empty()) {
            errorAt(dependency.line, dependency.column, "The git URL of dependency '" +
                    dependency.name + "' cannot be empty.");
        }
        for (size_t j = 0; j < i; ++j) {
            if (manifest.dependencies[j].name == dependency.name) {
                errorAt(dependency.line, dependency.column, "Package '" + dependency.name +
                        "' is already declared as a dependency; remove the duplicate.");
                break;
            }
        }
    }
    for (size_t i = 0; i < manifest.natives.size(); ++i) {
        const auto& native = manifest.natives[i];
        validateNative(native, path, errors);
        for (size_t j = 0; j < i; ++j) {
            if (manifest.natives[j].name == native.name) {
                errorAt(native.line, native.column, "Native library '" + native.name +
                        "' is already declared; remove the duplicate.");
                break;
            }
        }
    }
    for (size_t i = 0; i < manifest.members.size(); ++i) {
        const auto& member = manifest.members[i];
        if (member.folder.empty()) {
            errorAt(member.line, member.column, "A member folder cannot be empty.");
        }
        for (size_t j = 0; j < i; ++j) {
            if (manifest.members[j].folder == member.folder) {
                errorAt(member.line, member.column, "Member '" + member.folder + "' is already "
                        "listed; remove the duplicate.");
                break;
            }
        }
    }
    for (size_t i = 0; i < manifest.overrides.size(); ++i) {
        const auto& override = manifest.overrides[i];
        if (override.folder.empty()) {
            errorAt(override.line, override.column, "An override folder cannot be empty.");
        }
        for (size_t j = 0; j < i; ++j) {
            if (manifest.overrides[j].name == override.name) {
                errorAt(override.line, override.column, "Package '" + override.name + "' is "
                        "already overridden; remove the duplicate.");
                break;
            }
        }
    }
}

Manifest loadManifestFile(const fs::path& file, std::vector<std::string>& errors) {
    std::string pathText = file.string();
    std::ifstream stream(file, std::ios::binary);
    if (!stream) {
        errors.push_back("Could not read " + pathText + ".");
        return {};
    }
    std::string bytes((std::istreambuf_iterator<char>(stream)),
                      std::istreambuf_iterator<char>());
    std::u16string text;
    Utf8DecodeError decodeError;
    if (!decodeUtf8ToUtf16(bytes, text, decodeError)) {
        errors.push_back(pathText + ": " + describeUtf8DecodeError(decodeError));
        return {};
    }
    Manifest manifest = parseManifestText(pathText, text, errors);
    validateManifest(manifest, pathText, errors);
    return manifest;
}

}  // namespace ens::modules
