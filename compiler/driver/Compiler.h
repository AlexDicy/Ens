#pragma once
#include <filesystem>
#include <istream>
#include <string>

#include "PackageResolution.h"

class Compiler {
public:
    enum class BuildOutcome { Failed, BuiltExecutable, ValidatedLibrary };

    struct BuildResult {
        BuildOutcome outcome = BuildOutcome::Failed;
        std::filesystem::path executable;
    };

    // Compiles `source`, a single .ens file or a folder of sources (a package's src/). A
    // program whose main module defines main() links an executable: `outputFile` when given,
    // else `defaultName` plus the target's executable suffix, in the current folder. A program
    // without an entry point is a library: the full pipeline runs, including codegen, and no
    // artifact is kept; a non-empty `outputFile` is then an error. A non-empty `overridesRoot`
    // names the folder whose ens.overrides governs the build (the workspace root when it
    // builds its members); by default the source's own workspace root does. `packages` holds
    // the git-sourced packages the driver resolved for this invocation.
    static BuildResult build(const std::filesystem::path& source,
                             const std::filesystem::path& outputFile,
                             const std::string& defaultName,
                             bool explainArc = false,
                             const std::string& targetTriple = "",
                             const std::filesystem::path& overridesRoot = {},
                             const ens::packages::ResolvedPackages* packages = nullptr);

    // Front end and semantic analysis only: prints diagnostics and generates nothing.
    static bool check(const std::filesystem::path& source,
                      const std::filesystem::path& overridesRoot = {},
                      const ens::packages::ResolvedPackages* packages = nullptr);

    // True when the target's main module (the file itself, or main.ens in a folder) defines a
    // top-level main(), making the target an application.
    static bool definesEntryPoint(const std::filesystem::path& source);

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
                    bool explainArc = false,
                    const std::filesystem::path& overridesRoot = {},
                    const ens::packages::ResolvedPackages* packages = nullptr);
};
