#pragma once
#include <filesystem>
#include <istream>
#include <optional>
#include <vector>

class Compiler {
public:
    static bool compile(const std::filesystem::path& source,
                        const std::filesystem::path& outputFolder,
                        const std::filesystem::path& sourcePath,
                        bool explainArc = false);

    static bool compileSingle(std::istream& source,
                              const std::filesystem::path& outputFile,
                              const std::string& filename,
                              bool explainArc = false);

    static bool dumpCst(std::istream& source, const std::string& filename = "<stdin>");
    static bool analyzeCst(std::istream& source, const std::string& filename = "<stdin>");

private:
    static std::vector<std::filesystem::path> getFileTree(
        const std::filesystem::path& root,
        const std::filesystem::path& rootPath);
};
