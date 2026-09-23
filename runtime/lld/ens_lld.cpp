// The linker bridge: one C entry point over lld's three object-file drivers, since lld itself
// exposes only a C++ interface. It carries no policy - the caller hands over a finished argument
// vector and the flavor to run it under, and everything the linker printed comes back as one
// allocated text the caller releases through ens_lld_free.

#include "lld/Common/CommonLinkerContext.h"
#include "lld/Common/Driver.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/Support/CrashRecoveryContext.h"
#include "llvm/Support/Parallel.h"
#include "llvm/Support/raw_ostream.h"

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>
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

// Crash recovery is switched on the first time a link needs it and never switched off. The switch
// is one per process rather than one per link. Switching it off at the end of a step would decide
// for every later link as well.
struct CrashRecoverySwitch {
    CrashRecoverySwitch() { llvm::CrashRecoveryContext::Enable(); }
};

// Runs one step of a link where lld giving up is a return rather than the end of the process. lld
// reports a corrupted input, or an output it cannot open, through fatal(), and fatal() leaves
// through llvm::sys::Process::Exit however exitEarly was answered: the caller never hears back, and
// what the linker printed on its way out is discarded along with the process. Process::Exit gives
// control back to the calling thread's crash-recovery context rather than ending the process
// whenever there is one, and nothing puts one in place by default, so the bridge puts one around
// every step it runs lld under.
void runWithoutExiting(llvm::function_ref<void()> step) {
    static const CrashRecoverySwitch switchedOn;
    llvm::CrashRecoveryContext recovery;
    recovery.RunSafely(step);
}

// One link, leaving nothing of itself behind. lld keeps what a run builds up, the files it read and
// the symbols it resolved, in one heap-allocated context, and a driver called directly never takes
// it down: a second link then reads a first link's spent state and resolves nothing out of the
// libraries it thinks it already has. Discarding the context is what lld's own library entry point
// does, and it is what makes the next call a fresh linker. The discarding runs under recovery as
// well, since a link that stopped partway is the state its teardown is likeliest to fault on.
bool runDriver(int flavor, llvm::ArrayRef<const char*> arguments, llvm::raw_ostream& outStream,
               llvm::raw_ostream& errStream) {
    bool linked = false;
    runWithoutExiting([&] { linked = callDriver(flavor, arguments, outStream, errStream); });
    runWithoutExiting([] {
        if (lld::hasContext()) lld::CommonLinkerContext::destroy();
    });
    return linked;
}

constexpr std::chrono::seconds kStartTimeout{5};

// Returns once every thread of lld's pool is running, since on Windows exiting while one is still
// being created can crash the process or hang its exit. The wait ends early when no thread starts
// for five seconds, and the entry point below still prevents the hang then.
void waitForLinkerThreads() {
    llvm::parallel::TaskGroup group;
    if (!group.isParallel()) return;
    const std::size_t count = llvm::parallel::getThreadCount();
    std::mutex mutex;
    std::condition_variable allStarted;
    std::size_t started = 0;
    auto lastStart = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < count; ++i) {
        group.spawn([&] {
            std::unique_lock<std::mutex> lock(mutex);
            lastStart = std::chrono::steady_clock::now();
            if (++started == count) allStarted.notify_all();
            const auto everyStarted = [&] { return started == count; };
            while (!allStarted.wait_until(lock, lastStart + kStartTimeout, everyStarted)) {
                if (std::chrono::steady_clock::now() >= lastStart + kStartTimeout) return;
            }
        });
    }
    group.sync();
}

}  // namespace

extern "C" {

// Runs one link. `argumentBlock` holds `argumentCount` NUL-terminated arguments back to back,
// starting with the linker name the flavor answers to. `output` receives everything the linker
// printed, or stays null when it printed nothing; a null `output` is accepted and drops the text.
// Returns 0 when the executable was written, 1 when the linker refused or gave up on the link, and
// 2 when the request itself was unusable.
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
    waitForLinkerThreads();
    stream.flush();

    if (output != nullptr && !printed.empty()) *output = copied(printed);
    return linked ? kLinked : kLinkFailed;
}

// Releases a text ens_lld_link handed back, so ownership stays with the allocator that made it.
ENS_LLD_EXPORT void ens_lld_free(char* output) {
    std::free(output);
}

}  // extern "C"

#if defined(_WIN32)

#include <windows.h>

extern "C" BOOL WINAPI _DllMainCRTStartup(HINSTANCE instance, DWORD reason, LPVOID reserved);

// Windows ends every other thread before it calls this entry point for process exit, so a static
// destructor waiting on lld's pool would never return. Teardown is skipped then and runs only when
// the library is unloaded from a process that keeps running.
extern "C" BOOL WINAPI ens_lld_entry(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_DETACH && reserved != nullptr) return TRUE;
    return _DllMainCRTStartup(instance, reason, reserved);
}

#pragma comment(linker, "/entry:ens_lld_entry")

#endif
