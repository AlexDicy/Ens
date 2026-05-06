#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include "compiler/Compiler.h"

namespace fs = std::filesystem;

static bool execute(const std::unordered_map<std::string, std::string>& arguments) {
    fs::path outputFolder = arguments.count("--output") ? arguments.at("--output") : ".";
    if (fs::exists(outputFolder) && !fs::is_directory(outputFolder)) {
        throw std::invalid_argument("The specified output directory is not a directory, please choose a different folder");
    }

    if (arguments.count("--source")) {
        fs::path source = arguments.at("--source");
        fs::path sourcePath = fs::is_directory(source) ? source : source.parent_path();
        return Compiler::compile(source, outputFolder, sourcePath);
    } else {
        return Compiler::compileSingle(std::cin, outputFolder);
    }
}

static std::unordered_map<std::string, std::string> parseArguments(int argc, char* argv[]) {
    std::unordered_map<std::string, std::string> arguments;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg[0] == '-') {
            if (arg.size() < 2) {
                throw std::invalid_argument("Not a valid argument: " + arg);
            }
            if (i + 1 >= argc) {
                throw std::invalid_argument("Expected arg after: " + arg);
            }
            arguments[arg] = argv[i + 1];
            i++;
        }
    }
    return arguments;
}

int main(int argc, char* argv[]) {
    auto arguments = parseArguments(argc, argv);

    if (arguments.count("-h") || arguments.count("--help")) {
        std::cout << "Use --source to specify the input file or folder to compile, otherwise use stdin\n";
        std::cout << "Use --output to specify the output folder\n";
        return 0;
    }

    if (execute(arguments)) {
        auto it = arguments.find("--output");
        std::cout << "Compiled successfully to "
                  << (it == arguments.end() ? "the current folder" : it->second) << '\n';
    } else {
        std::cerr << "Please check the errors and retry.\n";
    }
    return 0;
}
