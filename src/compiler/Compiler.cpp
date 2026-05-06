#include "Compiler.h"
#include "../tokenizer/Tokenizer.h"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>

namespace fs = std::filesystem;

bool Compiler::compile(const fs::path& source,
                       const fs::path& outputFolder,
                       const fs::path& sourcePath) {
    if (fs::is_directory(source)) {
        auto files = getFileTree(source, sourcePath);
        for (const auto& file : files) {
            compileSingle(source, file, outputFolder);
        }
    } else {
        return compileSingle(std::nullopt, source, outputFolder);
    }
    return true;
}

std::vector<fs::path> Compiler::getFileTree(const fs::path& root, const fs::path& rootPath) {
    std::vector<fs::path> files;

    auto rel = fs::relative(root, rootPath);
    bool hasSubfolder = !rel.empty() && rel != fs::path(".");

    for (const auto& entry : fs::directory_iterator(root)) {
        const auto& path = entry.path();
        if (fs::is_directory(path)) {
            auto sub = getFileTree(path, rootPath);
            files.insert(files.end(), sub.begin(), sub.end());
        } else if (fs::is_regular_file(path) && path.extension() == ".ens") {
            files.push_back(hasSubfolder ? rel / path.filename() : path.filename());
        }
    }

    // Make sure subfiles are last
    std::reverse(files.begin(), files.end());
    return files;
}

bool Compiler::compileSingle(const std::optional<fs::path>& root,
                              const fs::path& source,
                              const fs::path& outputFolder) {
    fs::path filePath = root.has_value() ? (*root / source) : source;
    std::ifstream file(filePath);
    if (!file) {
        std::cerr << "ERROR: Couldn't read " << source << '\n';
        return false;
    }

    // If compiling a single file the file path shouldn't be added to the output path
    fs::path relativePath = root.has_value() ? source : source.filename();
    // Add output folder + subfolder + file name
    fs::path output = outputFolder.filename() / fs::path(relativePath).replace_extension(".cpp");
    // Print result
    std::cout << "Compiling " << source << " to " << output << '\n';

    return compileSingle(file, outputFolder);
}

bool Compiler::compileSingle(std::istream& source, const fs::path& outputFolder) {
    std::string code((std::istreambuf_iterator<char>(source)), std::istreambuf_iterator<char>());
    std::u16string u16code(code.begin(), code.end());
    Tokenizer::tokenize(u16code);
    return true;
}
