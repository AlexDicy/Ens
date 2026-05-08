#pragma once
#include <filesystem>
#include <istream>
#include <optional>
#include <vector>

class Compiler {
public:
    static bool compile(const std::filesystem::path& source,
                        const std::filesystem::path& outputFolder,
                        const std::filesystem::path& sourcePath);

    static bool compileSingle(const std::optional<std::filesystem::path>& root,
                              const std::filesystem::path& source,
                              const std::filesystem::path& outputFolder);

    static bool compileSingle(std::istream& source,
                              const std::filesystem::path& outputFolder,
                              const std::string& filename = "<stdin>");

private:
    static std::vector<std::filesystem::path> getFileTree(
        const std::filesystem::path& root,
        const std::filesystem::path& rootPath);
};
