#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include "Compiler.h"

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

    bool explainArc = arguments.count("--explain-arc") > 0;
    std::string targetTriple = arguments.count("--target") ? arguments.at("--target") : "";

    if (arguments.count("--source")) {
        fs::path source = arguments.at("--source");
        fs::path sourcePath = fs::is_directory(source) ? source : source.parent_path();
        return Compiler::compile(source, outputFile, sourcePath, explainArc, targetTriple);
    } else {
        return Compiler::compileSingle(std::cin, outputFile, "<stdin>", explainArc, targetTriple);
    }
}

static bool isBooleanFlag(const std::string& arg) {
    return arg == "-h" || arg == "--help" || arg == "--cst-dump" || arg == "--cst-analyze" || arg == "--explain-arc";
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

// `ens test [--source <folder>] [--filter <substring>] [--explain-arc]`:
// discover tests, build and run them, and return the runner's exit code.
static int runTestCommand(int argc, char* argv[]) {
    std::unordered_map<std::string, std::string> arguments;
    try {
        arguments = parseArguments(argc - 1, argv + 1);
    } catch (const std::exception& e) {
        std::cerr << "ERROR: " << e.what() << '\n';
        return 2;
    }
    for (const auto& [key, value] : arguments) {
        if (key != "--source" && key != "--filter" && key != "--explain-arc") {
            std::cerr << "ERROR: unknown option '" << key << "' for 'ens test'\n";
            return 2;
        }
    }
    fs::path source = arguments.count("--source") ? fs::path(arguments.at("--source")) : fs::path(".");
    std::string filter = arguments.count("--filter") ? arguments.at("--filter") : "";
    return Compiler::test(source, filter, arguments.count("--explain-arc") > 0);
}

int main(int argc, char* argv[]) {
    if (argc >= 2 && std::string(argv[1]) == "test") {
        return runTestCommand(argc, argv);
    }

    auto arguments = parseArguments(argc, argv);

    if (arguments.count("-h") || arguments.count("--help")) {
        std::cout << "Use --source to specify the input file or folder to compile, otherwise use stdin\n";
        std::cout << "Use --output to specify the output folder\n";
        std::cout << "Use 'ens test [--source <folder>] [--filter <substring>]' to run the folder's tests\n";
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
