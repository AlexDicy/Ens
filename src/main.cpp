#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include "compiler/Compiler.h"

namespace fs = std::filesystem;

static bool execute(const std::unordered_map<std::string, std::string>& arguments) {
    if (arguments.count("--cst-dump")) {
        if (arguments.count("--source")) {
            fs::path source = arguments.at("--source");
            std::ifstream file(source);
            if (!file) {
                std::cerr << "ERROR: Couldn't read " << source << '\n';
                return false;
            }
            return Compiler::dumpCst(file, source.string());
        }
        return Compiler::dumpCst(std::cin);
    }
    if (arguments.count("--cst-analyze")) {
        if (arguments.count("--source")) {
            fs::path source = arguments.at("--source");
            std::ifstream file(source);
            if (!file) {
                std::cerr << "ERROR: Couldn't read " << source << '\n';
                return false;
            }
            return Compiler::analyzeCst(file, source.string());
        }
        return Compiler::analyzeCst(std::cin);
    }

    // --output is the output file path. Empty = print LLVM IR to stdout.
    // The extension drives the pipeline: .ll = IR text, .obj/.o = object file,
    // .exe (or no extension) = link to executable.
    fs::path outputFile = arguments.count("--output") ? fs::path(arguments.at("--output")) : fs::path();
    if (!outputFile.empty() && fs::exists(outputFile) && fs::is_directory(outputFile)) {
        throw std::invalid_argument("The specified output is a directory; please specify a file path");
    }

    if (arguments.count("--source")) {
        fs::path source = arguments.at("--source");
        fs::path sourcePath = fs::is_directory(source) ? source : source.parent_path();
        return Compiler::compile(source, outputFile, sourcePath);
    } else {
        return Compiler::compileSingle(std::cin, outputFile);
    }
}

static bool isBooleanFlag(const std::string& arg) {
    return arg == "-h" || arg == "--help" || arg == "--cst-dump" || arg == "--cst-analyze";
}

static std::unordered_map<std::string, std::string> parseArguments(int argc, char* argv[]) {
    std::unordered_map<std::string, std::string> arguments;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg[0] == '-') {
            if (arg.size() < 2) {
                throw std::invalid_argument("Not a valid argument: " + arg);
            }
            if (isBooleanFlag(arg)) {
                arguments[arg] = "";
                continue;
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

    bool ok = execute(arguments);
    if (arguments.count("--cst-dump") || arguments.count("--cst-analyze")) {
        return ok ? 0 : 1;
    }
    if (ok) {
        auto it = arguments.find("--output");
        std::cout << "Compiled successfully to "
                  << (it == arguments.end() ? "the current folder" : it->second) << '\n';
    } else {
        std::cerr << "Please check the errors and retry.\n";
    }
    return 0;
}
