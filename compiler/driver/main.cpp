#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <vector>

#include "Compiler.h"
#include "Overrides.h"
#include "Version.h"
#include "module/Manifest.h"
#include "module/Workspace.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Program.h"

namespace fs = std::filesystem;

namespace {

int usageError(const std::string& message) {
    std::cerr << "ERROR: " << message << '\n';
    std::cerr << "Run 'ens help' for the available commands.\n";
    return 2;
}

// The arguments of one subcommand: positional values, valued options, boolean flags, and
// (for `ens run`) everything after `--`, passed to the program verbatim.
struct ParsedCommand {
    std::vector<std::string> positionals;
    std::vector<std::pair<std::string, std::string>> valued;
    std::set<std::string> flags;
    std::vector<std::string> programArguments;

    std::string option(const std::string& name) const {
        for (const auto& [key, value] : valued) {
            if (key == name) return value;
        }
        return "";
    }

    bool flag(const std::string& name) const { return flags.count(name) > 0; }
};

// Parses a subcommand's arguments. Returns 0 on success, else the exit code of the usage
// error it reported.
int parseCommand(int argc, char* argv[], int firstIndex,
                 const std::set<std::string>& valuedNames,
                 const std::set<std::string>& flagNames,
                 size_t maxPositionals, bool allowProgramArguments,
                 const std::string& commandName, ParsedCommand& out) {
    for (int i = firstIndex; i < argc; i++) {
        std::string arg = argv[i];
        if (allowProgramArguments && arg == "--") {
            for (int j = i + 1; j < argc; j++) out.programArguments.push_back(argv[j]);
            return 0;
        }
        if (!arg.empty() && arg[0] == '-' && arg != "-") {
            if (valuedNames.count(arg)) {
                if (i + 1 >= argc) {
                    return usageError("The option '" + arg + "' needs a value.");
                }
                out.valued.emplace_back(arg, argv[i + 1]);
                i++;
                continue;
            }
            if (flagNames.count(arg)) {
                out.flags.insert(arg);
                continue;
            }
            if (arg == "--source") {
                return usageError("The --source option was retired; pass the path directly, "
                                  "for example 'ens " + commandName + " <path>'.");
            }
            return usageError("Unknown option '" + arg + "' for 'ens " + commandName + "'.");
        }
        if (out.positionals.size() >= maxPositionals) {
            return usageError("Unexpected argument '" + arg + "' for 'ens " + commandName +
                              "'.");
        }
        out.positionals.push_back(arg);
    }
    return 0;
}

std::string lastSegment(const std::string& dottedName) {
    auto dot = dottedName.rfind('.');
    return dot == std::string::npos ? dottedName : dottedName.substr(dot + 1);
}

// The resolved target of build/check/run/test: a single source (file, folder, or a package's
// src/), or a workspace root whose members are built individually.
struct Target {
    bool isWorkspace = false;
    bool isFile = false;
    fs::path root;            // the file or folder the path resolved to
    fs::path source;          // what the compiler consumes (file, folder, or <package>/src)
    std::string name;         // default artifact name
    fs::path testsFolder;     // <package>/tests when present
    std::string packageName;  // package form only
    std::vector<ens::modules::WorkspaceMember> members;  // workspace form, dependency order
};

// Orders members so each one follows the members it depends on. A dependency that is not a
// member imposes no ordering; members caught in a dependency cycle keep manifest order.
std::vector<ens::modules::WorkspaceMember> sortByDependencies(
        std::vector<ens::modules::WorkspaceMember> members) {
    std::vector<ens::modules::WorkspaceMember> ordered;
    std::vector<bool> placed(members.size(), false);
    auto isPlaced = [&](const std::string& name) -> bool {
        for (size_t i = 0; i < members.size(); ++i) {
            if (members[i].packageName == name) return placed[i];
        }
        return true;
    };
    while (ordered.size() < members.size()) {
        bool progress = false;
        for (size_t i = 0; i < members.size(); ++i) {
            if (placed[i]) continue;
            bool ready = true;
            for (const auto& dependency : members[i].dependencies) {
                if (!isPlaced(dependency)) {
                    ready = false;
                    break;
                }
            }
            if (ready) {
                placed[i] = true;
                ordered.push_back(members[i]);
                progress = true;
            }
        }
        if (!progress) {
            for (size_t i = 0; i < members.size(); ++i) {
                if (!placed[i]) {
                    placed[i] = true;
                    ordered.push_back(members[i]);
                }
            }
        }
    }
    return ordered;
}

// Resolves the optional path argument shared by build/check/run/test: an .ens file, a package
// or workspace folder, or a plain folder of sources; no path discovers the nearest ens.package
// walking up from the current folder. Returns 0 and fills `out`, or the exit code of the error
// it reported.
int resolveTarget(const std::vector<std::string>& positionals, const std::string& commandName,
                  Target& out) {
    std::error_code ec;
    fs::path given;
    if (!positionals.empty()) {
        given = positionals.front();
    } else {
        given = ens::modules::discoverWorkspaceRoot(fs::current_path());
        if (given.empty()) {
            return usageError("No ens.package manifest was found here or in any parent "
                              "folder; pass a path, for example 'ens " + commandName +
                              " src/main.ens'.");
        }
    }

    out.root = given;
    if (fs::is_regular_file(given, ec)) {
        if (given.extension() != ".ens") {
            return usageError("'" + given.string() + "' is not an .ens source file.");
        }
        out.isFile = true;
        out.source = given;
        out.name = given.stem().string();
        return 0;
    }
    if (!fs::is_directory(given, ec)) {
        return usageError("'" + given.string() + "' does not exist.");
    }

    fs::path manifestFile = given / "ens.package";
    if (!fs::exists(manifestFile, ec)) {
        out.source = given;
        out.name = fs::absolute(given).lexically_normal().filename().string();
        if (out.name.empty()) out.name = "program";
        return 0;
    }

    // Manifest problems are not reported here: the build loads the manifest again through the
    // workspace registry and reports them with the compilation.
    std::vector<std::string> errors;
    ens::modules::Manifest manifest = ens::modules::loadManifestFile(manifestFile, errors);
    if (manifest.form == ens::modules::ManifestForm::Workspace) {
        out.isWorkspace = true;
        std::vector<std::string> memberErrors;
        out.members = sortByDependencies(
            ens::modules::listWorkspaceMembers(given, memberErrors));
        if (!memberErrors.empty()) {
            for (const auto& e : memberErrors) std::cerr << "ERROR: " << e << '\n';
            return 1;
        }
        return 0;
    }
    fs::path src = given / "src";
    if (manifest.form == ens::modules::ManifestForm::Package && fs::is_directory(src, ec)) {
        out.source = src;
        out.name = lastSegment(manifest.packageName);
        out.packageName = manifest.packageName;
        fs::path tests = given / "tests";
        if (fs::is_directory(tests, ec)) out.testsFolder = tests;
        return 0;
    }
    // A manifest folder without a src/ layout compiles its root-level sources; a malformed
    // manifest surfaces its problems the same way.
    out.source = given;
    out.name = manifest.packageName.empty()
        ? fs::absolute(given).lexically_normal().filename().string()
        : lastSegment(manifest.packageName);
    if (out.name.empty()) out.name = "program";
    return 0;
}

const char kGeneralHelp[] =
    "Ens compiler and build tool.\n"
    "\n"
    "Usage: ens <command> [arguments]\n"
    "\n"
    "Commands:\n"
    "  build [path]     Compile a source file, a package, or a workspace's members\n"
    "  run [path]       Build an application and run it\n"
    "  check [path]     Look for errors without building anything\n"
    "  test [path]      Build and run the tests\n"
    "  override <form>  Manage dependency overrides in ens.overrides\n"
    "  version          Print the toolchain version\n"
    "  help [command]   Show help for a command\n"
    "\n"
    "With no path, build/run/check/test use the nearest ens.package above the current "
    "folder.\n";

int printHelp(const std::string& command) {
    if (command.empty()) {
        std::cout << kGeneralHelp;
        return 0;
    }
    if (command == "build") {
        std::cout <<
            "Compile Ens sources.\n"
            "\n"
            "Usage: ens build [path] [options]\n"
            "\n"
            "The path is an .ens file, a package folder, or a workspace root; with no path\n"
            "the nearest ens.package above the current folder is used. A program whose main\n"
            "module defines main() builds an executable; one without is a library and is\n"
            "fully compiled with no artifact kept. At a workspace root every member is built\n"
            "in dependency order.\n"
            "\n"
            "Options:\n"
            "  --output <file>    Where to put the executable (single-target builds only)\n"
            "  --target <triple>  Cross-compile for the given LLVM target triple\n"
            "  --explain-arc      Print what the ARC optimizer elided and why\n";
        return 0;
    }
    if (command == "run") {
        std::cout <<
            "Build an application and run it.\n"
            "\n"
            "Usage: ens run [path] [-- arguments]\n"
            "\n"
            "The path names an application: an .ens file, a package folder, or a workspace\n"
            "root with exactly one application member. Everything after '--' is passed to\n"
            "the program, and the program's exit code becomes ens's exit code.\n";
        return 0;
    }
    if (command == "check") {
        std::cout <<
            "Look for errors without building.\n"
            "\n"
            "Usage: ens check [path]\n"
            "\n"
            "Runs the front end and semantic analysis and prints any diagnostics, but\n"
            "generates no code. The path rules match 'ens build'; at a workspace root every\n"
            "member is checked.\n";
        return 0;
    }
    if (command == "test") {
        std::cout <<
            "Build and run the tests.\n"
            "\n"
            "Usage: ens test [path] [options]\n"
            "\n"
            "Discovers tests in *_test.ens files and runs them. The path rules match\n"
            "'ens build'; at a workspace root every member's tests run.\n"
            "\n"
            "Options:\n"
            "  --filter <substring>  Run only the tests whose description contains it\n"
            "  --tests <folder>      Look for test files in this folder\n"
            "  --explain-arc         Print what the ARC optimizer elided and why\n";
        return 0;
    }
    if (command == "override") {
        std::cout <<
            "Manage dependency overrides.\n"
            "\n"
            "Usage: ens override add <package> <folder>\n"
            "       ens override remove <package>\n"
            "       ens override list\n"
            "\n"
            "Overrides redirect a dependency to a local folder and live in the ens.overrides\n"
            "file next to the workspace's ens.package, found from the current folder. 'add'\n"
            "checks that the folder's manifest declares exactly the given package; 'list'\n"
            "shows every override and whether its target is currently valid.\n";
        return 0;
    }
    if (command == "version") {
        std::cout << "Print the toolchain version.\n\nUsage: ens version\n";
        return 0;
    }
    if (command == "help") {
        std::cout << "Show help for a command.\n\nUsage: ens help [command]\n";
        return 0;
    }
    return usageError("Unknown command '" + command + "'.");
}

int runBuild(int argc, char* argv[]) {
    ParsedCommand arguments;
    int rc = parseCommand(argc, argv, 2, {"--output", "--target"}, {"--explain-arc"},
                          1, false, "build", arguments);
    if (rc != 0) return rc;

    Target target;
    rc = resolveTarget(arguments.positionals, "build", target);
    if (rc != 0) return rc;

    fs::path outputFile = arguments.option("--output");
    std::string targetTriple = arguments.option("--target");
    bool explainArc = arguments.flag("--explain-arc");
    if (!outputFile.empty() && fs::exists(outputFile) && fs::is_directory(outputFile)) {
        return usageError("The output '" + outputFile.string() + "' is a folder; give a "
                          "file path.");
    }

    if (target.isWorkspace) {
        if (!outputFile.empty()) {
            return usageError("--output names one executable, but a workspace root builds "
                              "every member; build one member and pass --output there.");
        }
        size_t total = target.members.size();
        if (total == 0) {
            std::cout << "The workspace at " << target.root.string()
                      << " declares no members; nothing to build.\n";
            return 0;
        }
        for (size_t i = 0; i < total; ++i) {
            const auto& member = target.members[i];
            std::string label =
                "[" + std::to_string(i + 1) + "/" + std::to_string(total) + "] " +
                member.packageName;
            Compiler::BuildResult result =
                Compiler::build(member.folder / "src", {}, lastSegment(member.packageName),
                                explainArc, targetTriple, target.root);
            switch (result.outcome) {
                case Compiler::BuildOutcome::BuiltExecutable:
                    std::cout << label << ": built "
                              << result.executable.filename().string() << '\n';
                    break;
                case Compiler::BuildOutcome::ValidatedLibrary:
                    std::cout << label << ": library ok\n";
                    break;
                case Compiler::BuildOutcome::Failed:
                    std::cout << label << ": failed\n";
                    std::cerr << "Please check the errors and retry.\n";
                    return 1;
            }
        }
        return 0;
    }

    Compiler::BuildResult result = Compiler::build(target.source, outputFile, target.name,
                                                   explainArc, targetTriple);
    switch (result.outcome) {
        case Compiler::BuildOutcome::BuiltExecutable:
            std::cout << "Compiled successfully to " << result.executable.string() << '\n';
            return 0;
        case Compiler::BuildOutcome::ValidatedLibrary:
            std::cout << "Compiled successfully (library; no executable produced)\n";
            return 0;
        case Compiler::BuildOutcome::Failed:
            break;
    }
    std::cerr << "Please check the errors and retry.\n";
    return 1;
}

int runCheck(int argc, char* argv[]) {
    ParsedCommand arguments;
    int rc = parseCommand(argc, argv, 2, {}, {}, 1, false, "check", arguments);
    if (rc != 0) return rc;

    Target target;
    rc = resolveTarget(arguments.positionals, "check", target);
    if (rc != 0) return rc;

    if (target.isWorkspace) {
        size_t total = target.members.size();
        if (total == 0) {
            std::cout << "The workspace at " << target.root.string()
                      << " declares no members; nothing to check.\n";
            return 0;
        }
        bool anyFailed = false;
        for (size_t i = 0; i < total; ++i) {
            const auto& member = target.members[i];
            std::string label =
                "[" + std::to_string(i + 1) + "/" + std::to_string(total) + "] " +
                member.packageName;
            if (Compiler::check(member.folder / "src", target.root)) {
                std::cout << label << ": ok\n";
            } else {
                std::cout << label << ": failed\n";
                anyFailed = true;
            }
        }
        if (anyFailed) {
            std::cerr << "Please check the errors and retry.\n";
            return 1;
        }
        return 0;
    }

    if (!Compiler::check(target.source)) {
        std::cerr << "Please check the errors and retry.\n";
        return 1;
    }
    std::cout << "No problems found.\n";
    return 0;
}

int runTest(int argc, char* argv[]) {
    ParsedCommand arguments;
    int rc = parseCommand(argc, argv, 2, {"--filter", "--tests"}, {"--explain-arc"},
                          1, false, "test", arguments);
    if (rc != 0) return rc;

    Target target;
    rc = resolveTarget(arguments.positionals, "test", target);
    if (rc != 0) return rc;

    if (target.isFile) {
        return usageError("'" + target.root.string() + "' is a file; 'ens test' runs the "
                          "tests of a folder or package.");
    }
    if (target.isWorkspace) {
        size_t total = target.members.size();
        if (total == 0) {
            std::cout << "The workspace at " << target.root.string()
                      << " declares no members; nothing to test.\n";
            return 0;
        }
        int worst = 0;
        for (size_t i = 0; i < total; ++i) {
            const auto& member = target.members[i];
            std::cout << "[" << (i + 1) << "/" << total << "] " << member.packageName
                      << ":\n";
            std::error_code ec;
            fs::path memberTests = member.folder / "tests";
            if (!fs::is_directory(memberTests, ec)) memberTests.clear();
            int rc = Compiler::test(member.folder / "src", memberTests,
                                    arguments.option("--filter"),
                                    arguments.flag("--explain-arc"), target.root);
            if (rc > worst) worst = rc;
        }
        return worst;
    }

    fs::path testsFolder = arguments.option("--tests");
    if (testsFolder.empty()) testsFolder = target.testsFolder;
    return Compiler::test(target.source, testsFolder, arguments.option("--filter"),
                          arguments.flag("--explain-arc"));
}

int runRun(int argc, char* argv[]) {
    ParsedCommand arguments;
    int rc = parseCommand(argc, argv, 2, {}, {}, 1, /*allowProgramArguments=*/true, "run",
                          arguments);
    if (rc != 0) return rc;

    Target target;
    rc = resolveTarget(arguments.positionals, "run", target);
    if (rc != 0) return rc;

    fs::path source;
    std::string name;
    fs::path overridesRoot;
    if (target.isWorkspace) {
        std::vector<const ens::modules::WorkspaceMember*> applications;
        for (const auto& member : target.members) {
            if (Compiler::definesEntryPoint(member.folder / "src" / "main.ens")) {
                applications.push_back(&member);
            }
        }
        if (applications.empty()) {
            return usageError("The workspace at '" + target.root.string() + "' has no "
                              "application member: no member's src/main.ens defines main().");
        }
        if (applications.size() > 1) {
            std::string names;
            for (const auto* application : applications) {
                if (!names.empty()) names += ", ";
                names += application->packageName;
            }
            return usageError("The workspace at '" + target.root.string() + "' has more "
                              "than one application member (" + names + "); run one of them "
                              "by path.");
        }
        source = applications.front()->folder / "src";
        name = lastSegment(applications.front()->packageName);
        overridesRoot = target.root;
    } else {
        if (!Compiler::definesEntryPoint(target.source)) {
            return usageError("'" + target.root.string() + "' is not an application: its "
                              "main module does not define main().");
        }
        source = target.source;
        name = target.name;
    }

    llvm::SmallString<128> tempDir;
    if (llvm::sys::fs::createUniqueDirectory("ens-run", tempDir)) {
        std::cerr << "ERROR: could not create a temporary folder for the build\n";
        return 1;
    }
    fs::path tempFolder(tempDir.str().str());
#ifdef _WIN32
    fs::path exePath = tempFolder / (name + ".exe");
#else
    fs::path exePath = tempFolder / name;
#endif

    std::error_code ec;
    Compiler::BuildResult built = Compiler::build(source, exePath, name, /*explainArc=*/false,
                                                  /*targetTriple=*/"", overridesRoot);
    if (built.outcome != Compiler::BuildOutcome::BuiltExecutable) {
        fs::remove_all(tempFolder, ec);
        std::cerr << "Please check the errors and retry.\n";
        return 1;
    }

    std::string exeString = exePath.string();
    std::vector<llvm::StringRef> processArguments;
    processArguments.push_back(exeString);
    for (const auto& argument : arguments.programArguments) {
        processArguments.push_back(argument);
    }
    std::string errorMessage;
    bool executionFailed = false;
    int exitCode = llvm::sys::ExecuteAndWait(exeString, processArguments, /*Env=*/std::nullopt,
                                             /*Redirects=*/{}, /*SecondsToWait=*/0,
                                             /*MemoryLimit=*/0, &errorMessage,
                                             &executionFailed);
    fs::remove_all(tempFolder, ec);
    if (executionFailed) {
        std::cerr << "ERROR: could not run the program"
                  << (errorMessage.empty() ? "" : ": " + errorMessage) << '\n';
        return 2;
    }
    return exitCode;
}

// Hidden debug tools: dump or analyze the CST of one file (or stdin when no path is given).
int runCstTool(bool analyze, int argc, char* argv[]) {
    ParsedCommand arguments;
    std::string name = analyze ? "cst-analyze" : "cst-dump";
    int rc = parseCommand(argc, argv, 2, {}, {}, 1, false, name, arguments);
    if (rc != 0) return rc;

    if (!arguments.positionals.empty() && arguments.positionals.front() != "-") {
        fs::path source = arguments.positionals.front();
        std::ifstream file(source);
        if (!file) {
            std::cerr << "ERROR: Couldn't read " << fs::absolute(source).string() << '\n';
            return 1;
        }
        bool ok = analyze ? Compiler::analyzeCst(file, source.string())
                          : Compiler::dumpCst(file, source.string());
        return ok ? 0 : 1;
    }
    bool ok = analyze ? Compiler::analyzeCst(std::cin) : Compiler::dumpCst(std::cin);
    return ok ? 0 : 1;
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cout << kGeneralHelp;
        return 0;
    }

    std::string command = argv[1];
    if (command == "build") return runBuild(argc, argv);
    if (command == "run") return runRun(argc, argv);
    if (command == "check") return runCheck(argc, argv);
    if (command == "test") return runTest(argc, argv);
    if (command == "override") return runOverrideCommand(argc, argv);
    if (command == "version" || command == "--version") {
        std::cout << "ens " << ens::kToolchainVersion << '\n';
        return 0;
    }
    if (command == "help") return printHelp(argc >= 3 ? argv[2] : "");
    if (command == "-h" || command == "--help") return printHelp("");
    if (command == "cst-dump") return runCstTool(/*analyze=*/false, argc, argv);
    if (command == "cst-analyze") return runCstTool(/*analyze=*/true, argc, argv);

    if (command == "--source") {
        return usageError("The --source option was retired; use 'ens build <path>' to "
                          "compile, or 'ens check <path>' to only look for errors.");
    }
    if (command == "--cst-dump" || command == "--cst-analyze") {
        return usageError("The " + command + " option was retired; use 'ens " +
                          command.substr(2) + " [path]' instead.");
    }
    if (!command.empty() && command[0] == '-') {
        return usageError("Unknown option '" + command + "'.");
    }
    return usageError("Unknown command '" + command + "'.");
}
