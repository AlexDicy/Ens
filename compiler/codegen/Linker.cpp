#include "Linker.h"

#include "SdkStubs.h"
#include "lld/Common/Driver.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Host.h"

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

std::vector<std::string> buildArgv(LinkerFlavor flavor,
                                    const std::string& triple,
                                    const std::vector<std::string>& objs,
                                    const std::string& exe) {
    std::vector<std::string> args;
    switch (flavor) {
        case LinkerFlavor::Coff:
            args = {"lld-link", "/nologo", "/subsystem:console"};
            for (auto& o : objs) args.push_back(o);
            args.push_back("/out:" + exe);
            // Default C runtime libs so simple int-returning programs link.
            // Will be customized once Ens has its own runtime / a linker
            // arguments API.
            args.push_back("/defaultlib:libcmt");
            args.push_back("/defaultlib:oldnames");
            return args;
        case LinkerFlavor::MachO: {
            args = {"ld64.lld"};
            args.push_back("-arch");
            args.push_back(archFromTriple(triple));
            args.push_back("-platform_version");
            args.push_back("macos");
            args.push_back("11.0");
            args.push_back("14.0");
            const std::string& sdk = extractEmbeddedSDK();
            if (!sdk.empty()) {
                args.push_back("-syslibroot");
                args.push_back(sdk);
            }
            for (auto& o : objs) args.push_back(o);
            args.push_back("-o");
            args.push_back(exe);
            args.push_back("-lSystem");
            return args;
        }
        case LinkerFlavor::Elf:
        default:
            args = {"ld.lld"};
            for (auto& o : objs) args.push_back(o);
            args.push_back("-o");
            args.push_back(exe);
            return args;
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
                   std::ostream& errStream) {
    return link(std::vector{objectPath}, exePath, errStream);
}

bool Linker::link(const std::vector<std::string>& objectPaths,
                   const std::string& exePath,
                   std::ostream& errStream) {
    const std::string triple = llvm::sys::getDefaultTargetTriple();
    const LinkerFlavor flavor = flavorForTriple(triple);

    std::vector<std::string> argv = buildArgv(flavor, triple, objectPaths, exePath);
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
