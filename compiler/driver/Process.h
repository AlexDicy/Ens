#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace ens::packages {

namespace fs = std::filesystem;

// The outcome of one external process run. `exitCode` is -1 when the process could not be
// started, with the reason in `startError`.
struct ProcessResult {
    int exitCode = -1;
    std::string output;
    std::string errorOutput;
    std::string startError;
};

// Locates a program on PATH; empty when it is not installed.
std::string findProgram(const std::string& name);

// Runs `program` with `arguments`, capturing stdout and stderr. Standard input reads from
// `stdinFile` when given, else from the null device so the program can never prompt. When
// `stdoutFile` is given, stdout goes there instead of being captured (for large outputs).
ProcessResult runProcess(const std::string& program, const std::vector<std::string>& arguments,
                         const fs::path& stdinFile = {}, const fs::path& stdoutFile = {});

}  // namespace ens::packages
