// The linker bridge: one C entry point over lld's three object-file drivers, since lld itself
// exposes only a C++ interface. It carries no policy - the caller hands over a finished argument
// vector and the flavor to run it under, and everything the linker printed comes back as one
// allocated text the caller releases through ens_lld_free. ens_lld_shutdown stops lld's thread
// pool once, after the last link, while the process can still wait for its threads. A link that
// ends the process prints what the linker reported on standard error first.

#include "lld/Common/CommonLinkerContext.h"
#include "lld/Common/Driver.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/CrashRecoveryContext.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/raw_ostream.h"

#include <atomic>
#include <cstdint>
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

// Everything the linker printed during one link. lld writes to it from every thread it runs a step
// on, so each write takes the lock that taking the text does.
class LinkerText final : public llvm::raw_ostream {
public:
    LinkerText() { SetUnbuffered(); }

    std::string taken() {
        std::lock_guard<std::mutex> lock(guard);
        return std::move(text);
    }

private:
    void write_impl(const char* data, std::size_t size) override {
        std::lock_guard<std::mutex> lock(guard);
        text.append(data, size);
        written += size;
    }

    std::uint64_t current_pos() const override { return written; }

    std::mutex guard;
    std::string text;
    std::atomic<std::uint64_t> written{0};
};

std::atomic<LinkerText*> linkInProgress{nullptr};

void reportStoppedLink(const std::string& text) {
    const llvm::StringRef reported = llvm::StringRef(text).trim();
    llvm::raw_ostream& stream = llvm::errs();
    if (reported.empty()) {
        stream << "ens: linking stopped the build and the linker reported no reason\n";
    } else {
        stream << "ens: linking stopped the build; the linker reported:\n" << reported << "\n";
    }
    stream.flush();
}

// lld ends the process from a thread outside every recovery context, a thread of its pool, by
// running llvm_shutdown and then terminating the process. llvm_shutdown destroys this object, so
// its destructor is the last point at which the text of the link in progress can be printed.
struct StoppedLinkReporter {
    ~StoppedLinkReporter() {
        LinkerText* stopped = linkInProgress.exchange(nullptr);
        if (stopped != nullptr) reportStoppedLink(stopped->taken());
    }
};

llvm::ManagedStatic<StoppedLinkReporter> stoppedLinkReporter;

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
        if (output != nullptr) {
            *output = copied("Internal: ens_lld_link was called with no arguments");
        }
        return kUnusableRequest;
    }
    if (flavor != kFlavorCoff && flavor != kFlavorElf && flavor != kFlavorMachO) {
        if (output != nullptr) {
            *output = copied("Internal: ens_lld_link was called with the unknown linker flavor "
                             + std::to_string(flavor));
        }
        return kUnusableRequest;
    }

    const std::vector<std::string> arguments = splitArguments(argumentBlock, argumentCount);
    std::vector<const char*> argv;
    argv.reserve(arguments.size());
    for (const std::string& argument : arguments) argv.push_back(argument.c_str());

    // Constructed before the first link, so llvm_shutdown has it to destroy.
    *stoppedLinkReporter;
    LinkerText printed;
    linkInProgress.store(&printed);
    const bool linked = runDriver(flavor, argv, printed, printed);
    linkInProgress.store(nullptr);

    const std::string text = printed.taken();
    if (output != nullptr && !text.empty()) *output = copied(text);
    return linked ? kLinked : kLinkFailed;
}

// Releases a text ens_lld_link handed back, so ownership stays with the allocator that made it.
ENS_LLD_EXPORT void ens_lld_free(char* output) {
    std::free(output);
}

// Stops the thread pool lld's parallel steps run on, which outlives the links that started it. It is
// called after the last link, because a stopped pool is never started again, so a link
// after it would wait for workers that are gone. LLVM's own account of the stop, from
// llvm/lib/Support/Parallel.cpp: "The ManagedStatic enables the ThreadPoolExecutor to be stopped
// via llvm_shutdown() which allows a "clean" fast exit, e.g. via _exit(). This stops the thread
// pool and waits for any worker thread creation to complete but does not wait for the threads to
// finish. The wait for worker thread creation to complete is important as it prevents intermittent
// crashes on Windows due to a race condition between thread creation and process exit." Stopping it
// here, while every thread is still alive, is what lets the pool's destructor at process exit return
// at once instead of waiting on threads Windows has already ended.
ENS_LLD_EXPORT void ens_lld_shutdown() {
    llvm::llvm_shutdown();
}

}  // extern "C"
