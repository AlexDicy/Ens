#pragma once
#include <filesystem>
#include <istream>
#include <optional>
#include <string>
#include <vector>

class Compiler {
public:
    static bool compile(const std::filesystem::path& source,
                        const std::filesystem::path& outputFolder,
                        const std::filesystem::path& sourcePath,
                        bool explainArc = false,
                        const std::string& targetTriple = "");

    static bool compileSingle(std::istream& source,
                              const std::filesystem::path& outputFile,
                              const std::string& filename,
                              bool explainArc = false,
                              const std::string& targetTriple = "");

    static bool dumpCst(std::istream& source, const std::string& filename = "<stdin>");
    static bool analyzeCst(std::istream& source, const std::string& filename = "<stdin>");

    // Discovers test declarations in *_test.ens files under testsDir (or under
    // sourceDir when testsDir is empty), compiles them with a synthesized
    // runner, executes it, and streams the report. Test imports resolve against
    // sourceDir first, then testsDir.
    // Returns the process exit code: 0 all pass, 1 test failures, 2 errors.
    static int test(const std::filesystem::path& sourceDir,
                    const std::filesystem::path& testsDir,
                    const std::string& filter,
                    bool explainArc = false);

private:
    static std::vector<std::filesystem::path> getFileTree(
        const std::filesystem::path& root,
        const std::filesystem::path& rootPath);
};
