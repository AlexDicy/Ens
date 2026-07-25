#pragma once
#include <ostream>
#include <string>
#include <vector>

class Linker {
public:
    static bool link(const std::string& objectPath, const std::string& exePath, std::ostream& errStream,
                     const std::string& targetTriple = "");
    static bool link(const std::vector<std::string>& objectPaths, const std::string& exePath, std::ostream& errStream,
                     const std::string& targetTriple = "");
    static bool link(const std::vector<std::string>& objectPaths,
                     const std::vector<std::string>& libraries,
                     const std::string& exePath, std::ostream& errStream,
                     const std::string& targetTriple = "");

    // `libraries` are logical names resolved by platform convention; `libraryFiles` are
    // concrete library files (fetched artifacts) passed to the linker verbatim.
    static bool link(const std::vector<std::string>& objectPaths,
                     const std::vector<std::string>& libraries,
                     const std::vector<std::string>& libraryFiles,
                     const std::string& exePath, std::ostream& errStream,
                     const std::string& targetTriple = "");
};
