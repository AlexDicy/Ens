#include "Process.h"

#include <fstream>
#include <iterator>

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Program.h"

namespace ens::packages {

namespace {

std::string readWholeFile(const fs::path& file) {
    std::ifstream stream(file, std::ios::binary);
    if (!stream) return {};
    return std::string((std::istreambuf_iterator<char>(stream)),
                       std::istreambuf_iterator<char>());
}

}  // namespace

std::string findProgram(const std::string& name) {
    auto found = llvm::sys::findProgramByName(name);
    return found ? *found : std::string();
}

ProcessResult runProcess(const std::string& program, const std::vector<std::string>& arguments,
                         const fs::path& stdinFile, const fs::path& stdoutFile) {
    ProcessResult result;

    llvm::SmallString<128> outFile;
    if (stdoutFile.empty()) {
        if (llvm::sys::fs::createTemporaryFile("ens-out", "tmp", outFile)) {
            result.startError = "could not create a temporary file for the process output";
            return result;
        }
    } else {
        outFile = stdoutFile.string();
    }
    llvm::SmallString<128> errFile;
    if (llvm::sys::fs::createTemporaryFile("ens-err", "tmp", errFile)) {
        if (stdoutFile.empty()) llvm::sys::fs::remove(outFile);
        result.startError = "could not create a temporary file for the process output";
        return result;
    }

    std::vector<llvm::StringRef> argv;
    argv.reserve(arguments.size() + 1);
    argv.push_back(program);
    for (const auto& argument : arguments) argv.push_back(argument);

    std::string stdinString = stdinFile.string();
    // An empty redirect string sends the stream to the null device, so a program that tries
    // to prompt fails instead of hanging.
    std::optional<llvm::StringRef> redirects[3] = {
        llvm::StringRef(stdinString), llvm::StringRef(outFile), llvm::StringRef(errFile)};

    std::string errorMessage;
    bool executionFailed = false;
    result.exitCode = llvm::sys::ExecuteAndWait(program, argv, /*Env=*/std::nullopt, redirects,
                                                /*SecondsToWait=*/0, /*MemoryLimit=*/0,
                                                &errorMessage, &executionFailed);
    if (executionFailed) {
        result.exitCode = -1;
        result.startError = errorMessage.empty() ? "could not start the process" : errorMessage;
    }
    if (stdoutFile.empty()) {
        result.output = readWholeFile(fs::path(outFile.str().str()));
        llvm::sys::fs::remove(outFile);
    }
    result.errorOutput = readWholeFile(fs::path(errFile.str().str()));
    llvm::sys::fs::remove(errFile);
    return result;
}

}  // namespace ens::packages
