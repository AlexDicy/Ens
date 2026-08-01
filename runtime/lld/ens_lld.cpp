// The linker bridge: one C entry point over lld's three object-file drivers, since lld itself
// exposes only a C++ interface. It carries no policy - the caller hands over a finished argument
// vector and the flavor to run it under, and everything the linker printed comes back as one
// allocated text the caller releases through ens_lld_free.

#include "lld/Common/CommonLinkerContext.h"
#include "lld/Common/Driver.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

LLD_HAS_DRIVER(coff)
LLD_HAS_DRIVER(elf)
LLD_HAS_DRIVER(macho)

#if defined(_WIN32)
#define ENS_LLD_EXPORT __declspec(dllexport)
#else
#define ENS_LLD_EXPORT __attribute__((visibility("default")))
#endif

namespace {

constexpr int kFlavorCoff = 0;
constexpr int kFlavorElf = 1;
constexpr int kFlavorMachO = 2;

constexpr int kLinked = 0;
constexpr int kLinkFailed = 1;
constexpr int kUnusableRequest = 2;

// The arguments as lld takes them, read out of a block of NUL-terminated strings.
std::vector<std::string> splitArguments(const char* block, long long count) {
    std::vector<std::string> arguments;
    arguments.reserve(static_cast<std::size_t>(count));
    const char* at = block;
    for (long long i = 0; i < count; ++i) {
        arguments.emplace_back(at);
        at += arguments.back().size() + 1;
    }
    return arguments;
}

char* copied(const std::string& text) {
    char* buffer = static_cast<char*>(std::malloc(text.size() + 1));
    if (buffer != nullptr) std::memcpy(buffer, text.c_str(), text.size() + 1);
    return buffer;
}

bool callDriver(int flavor, llvm::ArrayRef<const char*> arguments, llvm::raw_ostream& outStream,
                llvm::raw_ostream& errStream) {
    switch (flavor) {
        case kFlavorElf:
            return lld::elf::link(arguments, outStream, errStream, /*exitEarly=*/false,
                                  /*disableOutput=*/false);
        case kFlavorMachO:
            return lld::macho::link(arguments, outStream, errStream, false, false);
        default:
            return lld::coff::link(arguments, outStream, errStream, false, false);
    }
}

// One link, leaving nothing of itself behind. lld keeps what a run builds up, the files it read and
// the symbols it resolved, in one heap-allocated context, and a driver called directly never takes
// it down: a second link then reads a first link's spent state and resolves nothing out of the
// libraries it thinks it already has. Discarding the context is what lld's own library entry point
// does, and it is what makes the next call a fresh linker.
bool runDriver(int flavor, llvm::ArrayRef<const char*> arguments, llvm::raw_ostream& outStream,
               llvm::raw_ostream& errStream) {
    const bool linked = callDriver(flavor, arguments, outStream, errStream);
    if (lld::hasContext()) lld::CommonLinkerContext::destroy();
    return linked;
}

}  // namespace

extern "C" {

// Runs one link. `argumentBlock` holds `argumentCount` NUL-terminated arguments back to back,
// starting with the linker name the flavor answers to. `output` receives everything the linker
// printed, or stays null when it printed nothing; a null `output` is accepted and drops the text.
// Returns 0 when the executable was written, 1 when the linker refused the link, and 2 when the
// request itself was unusable.
ENS_LLD_EXPORT int ens_lld_link(int flavor, const char* argumentBlock, long long argumentCount,
                                char** output) {
    if (output != nullptr) *output = nullptr;
    if (argumentBlock == nullptr || argumentCount <= 0) {
        if (output != nullptr) *output = copied("no linker arguments were given");
        return kUnusableRequest;
    }
    if (flavor != kFlavorCoff && flavor != kFlavorElf && flavor != kFlavorMachO) {
        if (output != nullptr) {
            *output = copied("unknown linker flavor " + std::to_string(flavor));
        }
        return kUnusableRequest;
    }

    const std::vector<std::string> arguments = splitArguments(argumentBlock, argumentCount);
    std::vector<const char*> argv;
    argv.reserve(arguments.size());
    for (const std::string& argument : arguments) argv.push_back(argument.c_str());

    std::string printed;
    llvm::raw_string_ostream stream(printed);
    const bool linked = runDriver(flavor, argv, stream, stream);
    stream.flush();

    if (output != nullptr && !printed.empty()) *output = copied(printed);
    return linked ? kLinked : kLinkFailed;
}

// Releases a text ens_lld_link handed back, so ownership stays with the allocator that made it.
ENS_LLD_EXPORT void ens_lld_free(char* output) {
    std::free(output);
}

}  // extern "C"
