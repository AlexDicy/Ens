#include "Linker.h"

#include "SdkStubs.h"
#include "TargetPlatform.h"
#include "lld/Common/Driver.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdlib>
#include <iostream>
#include <vector>

LLD_HAS_DRIVER(coff)
LLD_HAS_DRIVER(elf)
LLD_HAS_DRIVER(macho)

namespace {

enum class LinkerFlavor { Coff, Elf, MachO };

LinkerFlavor flavorForTriple(const std::string& triple) {
    if (triple.find("windows") != std::string::npos
        || triple.find("win32") != std::string::npos
        || triple.find("msvc")  != std::string::npos) {
        return LinkerFlavor::Coff;
    }
    if (triple.find("darwin") != std::string::npos
        || triple.find("apple")  != std::string::npos) {
        return LinkerFlavor::MachO;
    }
    return LinkerFlavor::Elf;
}

std::string archFromTriple(const std::string& triple) {
    const auto dash = triple.find('-');
    std::string arch = (dash == std::string::npos) ? triple : triple.substr(0, dash);
    if (arch == "aarch64") return "arm64";
    return arch;
}

// extracts the SDK stubs into a temp directory for macOS builds
const std::string& extractEmbeddedSDK() {
    static const std::string cached = [] {
        if (ens::kSdkStubsCount == 0) return std::string();
        llvm::SmallString<128> dir;
        if (llvm::sys::fs::createUniqueDirectory("ens-sdk", dir)) return std::string();
        for (std::size_t i = 0; i < ens::kSdkStubsCount; ++i) {
            llvm::SmallString<256> full(dir);
            llvm::sys::path::append(full, ens::kSdkStubs[i].path);
            llvm::sys::fs::create_directories(llvm::sys::path::parent_path(full));
            std::error_code ec;
            llvm::raw_fd_ostream out(full, ec, llvm::sys::fs::OF_None);
            if (ec) return std::string();
            out.write(reinterpret_cast<const char*>(ens::kSdkStubs[i].data),
                      ens::kSdkStubs[i].size);
        }
        llvm::SmallString<256> alias(dir);
        llvm::sys::path::append(alias, "usr/lib/libSystem.tbd");
        llvm::sys::fs::create_link("libSystem.B.tbd", alias);
        return std::string(dir.str());
    }();
    return cached;
}

std::string queryCCompiler(const llvm::StringRef flag) {
    static const std::string cc = [] {
        for (const char* name : {"cc", "clang", "gcc"}) {
            if (auto path = llvm::sys::findProgramByName(name)) return *path;
        }
        return std::string();
    }();
    if (cc.empty()) return {};

    llvm::SmallString<128> outFile;
    if (llvm::sys::fs::createTemporaryFile("ens-cc", "txt", outFile)) {
        return {};
    }

    const std::optional<llvm::StringRef> redirects[3] = {
        std::nullopt, llvm::StringRef(outFile), std::nullopt};
    const llvm::StringRef args[2] = {cc, flag};
    const int rc = llvm::sys::ExecuteAndWait(cc, args, /*Env*/ std::nullopt, redirects);

    std::string result;
    if (rc == 0) {
        if (auto buf = llvm::MemoryBuffer::getFile(outFile)) {
            result = (*buf)->getBuffer().trim().str();
        }
    }
    llvm::sys::fs::remove(outFile);
    return result;
}

std::string dynamicLinkerFor(const std::string& triple) {
    const std::string arch = archFromTriple(triple);
    std::vector<const char*> candidates;
    if (arch == "arm64" || arch == "aarch64") {
        candidates = {"/lib/ld-linux-aarch64.so.1"};
    } else if (arch == "x86_64") {
        candidates = {"/lib64/ld-linux-x86-64.so.2", "/lib/ld-linux-x86-64.so.2"};
    } else if (arch == "i386" || arch == "i686") {
        candidates = {"/lib/ld-linux.so.2"};
    }
    for (const char* cand : candidates) {
        if (llvm::sys::fs::exists(cand)) return cand;
    }
    return {};
}

void appendUnwinder(std::vector<std::string>& args) {
    for (const char* name : { "libunwind.a", "libgcc_eh.a" }) {
        std::string p = queryCCompiler(std::string("-print-file-name=") + name);
        if (!p.empty() && p != name && llvm::sys::fs::exists(p)) {
            args.push_back(p);
            return;
        }
    }
    args.push_back("-lgcc_s");
}

// Extra library search directories for the ELF and Mach-O linkers, taken from LIBRARY_PATH the
// way lld-link takes them from LIB on Windows. Each becomes both a search path (`-L`) and a
// runtime path (`-rpath`) so a program that links a shared library found this way also loads it
// at run time. Empty when LIBRARY_PATH is unset, leaving ordinary builds untouched.
std::vector<std::string> extraLibrarySearchDirs() {
    std::vector<std::string> dirs;
    const char* value = std::getenv("LIBRARY_PATH");
    if (!value) return dirs;
    std::string text(value);
    std::size_t start = 0;
    while (start <= text.size()) {
        std::size_t sep = text.find(':', start);
        std::string dir = (sep == std::string::npos) ? text.substr(start)
                                                      : text.substr(start, sep - start);
        if (!dir.empty()) dirs.push_back(dir);
        if (sep == std::string::npos) break;
        start = sep + 1;
    }
    return dirs;
}

std::vector<std::string> buildArgv(LinkerFlavor flavor,
                                    const std::string& triple,
                                    const std::vector<std::string>& objs,
                                    const std::vector<std::string>& libraries,
                                    const std::vector<std::string>& libraryFiles,
                                    const std::string& exe) {
    std::vector<std::string> args;
    switch (flavor) {
        case LinkerFlavor::Coff:
            args = {"lld-link", "/nologo", "/subsystem:console"};
            for (auto& o : objs) args.push_back(o);
            for (auto& f : libraryFiles) args.push_back(f);
            args.push_back("/out:" + exe);
            // Default C runtime libs so simple int-returning programs link.
            // Will be customized once Ens has its own runtime / a linker
            // arguments API.
            args.push_back("/defaultlib:libcmt");
            args.push_back("/defaultlib:oldnames");
            args.push_back("/defaultlib:kernel32");   // RtlCaptureStackBackTrace for stack traces
            for (auto& lib : libraries) {
                // The C runtime is already covered by the defaultlibs above.
                if (lib == "msvcrt" || lib == "libcmt") continue;
                args.push_back(lib + ".lib");
            }
            return args;
        case LinkerFlavor::MachO: {
            args = {"ld64.lld"};
            args.push_back("-arch");
            args.push_back(archFromTriple(triple));
            args.push_back("-platform_version");
            args.push_back("macos");
            const std::string deploymentTarget = ens::macOSDeploymentTargetForTriple(triple);
            args.push_back(deploymentTarget);
            args.push_back(ens::kSdkVersion[0] == '\0' ? deploymentTarget : ens::kSdkVersion);
            const std::string& sdk = extractEmbeddedSDK();
            if (!sdk.empty()) {
                args.push_back("-syslibroot");
                args.push_back(sdk);
            }
            for (auto& o : objs) args.push_back(o);
            for (auto& f : libraryFiles) args.push_back(f);
            args.push_back("-o");
            args.push_back(exe);
            for (auto& dir : extraLibrarySearchDirs()) {
                args.push_back("-L" + dir);
                args.push_back("-rpath");
                args.push_back(dir);
            }
            args.push_back("-lSystem");
            for (auto& lib : libraries) {
                if (lib == "System") continue;  // auto-linked
                args.push_back("-l" + lib);
            }
            return args;
        }
        case LinkerFlavor::Elf:
        default: {
            args = {"ld.lld"};
            args.push_back("--eh-frame-hdr");   // so the unwinder finds FDEs under PIE

            const std::string dynLinker = dynamicLinkerFor(triple);
            if (!dynLinker.empty()) {
                args.push_back("-dynamic-linker");
                args.push_back(dynLinker);
            }

            args.push_back("-o");
            args.push_back(exe);

            for (auto& dir : extraLibrarySearchDirs()) {
                args.push_back("-L" + dir);
                args.push_back("-rpath");
                args.push_back(dir);
            }

            // Link the C runtime to add an entry point and libc symbols
            const std::string scrt1 = queryCCompiler("-print-file-name=Scrt1.o");
            const std::string crti  = queryCCompiler("-print-file-name=crti.o");
            const std::string crtn  = queryCCompiler("-print-file-name=crtn.o");
            const bool haveCrt = !scrt1.empty() && scrt1 != "Scrt1.o";
            if (haveCrt) {
                args.push_back(scrt1);
                if (!crti.empty() && crti != "crti.o") args.push_back(crti);
                args.push_back("-L" + llvm::sys::path::parent_path(scrt1).str());
            }

            for (auto& o : objs) args.push_back(o);
            for (auto& f : libraryFiles) args.push_back(f);

            for (auto& lib : libraries) {
                args.push_back("-l" + lib);
            }
            appendUnwinder(args);
            if (haveCrt) {
                args.push_back("-lc");
                if (!crtn.empty() && crtn != "crtn.o") args.push_back(crtn);
            }
            return args;
        }
    }
}

bool invokeDriver(LinkerFlavor flavor,
                  llvm::ArrayRef<const char*> args,
                  llvm::raw_ostream& outStream,
                  llvm::raw_ostream& errStream) {
    switch (flavor) {
        case LinkerFlavor::Coff:
            return lld::coff::link(args, outStream, errStream, /*exitEarly*/ false, /*disableOutput*/ false);
        case LinkerFlavor::MachO:
            return lld::macho::link(args, outStream, errStream, false, false);
        case LinkerFlavor::Elf:
        default:
            return lld::elf::link(args, outStream, errStream, false, false);
    }
}

}  // namespace

bool Linker::link(const std::string& objectPath,
                   const std::string& exePath,
                   std::ostream& errStream,
                   const std::string& targetTriple) {
    return link(std::vector{objectPath}, {}, exePath, errStream, targetTriple);
}

bool Linker::link(const std::vector<std::string>& objectPaths,
                   const std::string& exePath,
                   std::ostream& errStream,
                   const std::string& targetTriple) {
    return link(objectPaths, {}, exePath, errStream, targetTriple);
}

bool Linker::link(const std::vector<std::string>& objectPaths,
                   const std::vector<std::string>& libraries,
                   const std::string& exePath,
                   std::ostream& errStream,
                   const std::string& targetTriple) {
    return link(objectPaths, libraries, {}, exePath, errStream, targetTriple);
}

bool Linker::link(const std::vector<std::string>& objectPaths,
                   const std::vector<std::string>& libraries,
                   const std::vector<std::string>& libraryFiles,
                   const std::string& exePath,
                   std::ostream& errStream,
                   const std::string& targetTriple) {
    const std::string triple = ens::resolveTargetTriple(targetTriple);
    const LinkerFlavor flavor = flavorForTriple(triple);

    std::vector<std::string> argv = buildArgv(flavor, triple, objectPaths, libraries,
                                              libraryFiles, exePath);
    std::vector<const char*> args;
    args.reserve(argv.size());
    for (auto& s : argv) args.push_back(s.c_str());

    std::string outBuf, errBuf;
    llvm::raw_string_ostream outStream(outBuf);
    llvm::raw_string_ostream lldErrStream(errBuf);

    bool ok = invokeDriver(flavor, args, outStream, lldErrStream);
    outStream.flush();
    lldErrStream.flush();

    if (!outBuf.empty()) std::cout << outBuf;
    if (!ok) {
        errStream << "Linker failed:\n" << errBuf;
    } else if (!errBuf.empty()) {
        std::cerr << errBuf;
    }
    return ok;
}
