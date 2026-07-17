#include "TargetPlatform.h"

#include "llvm/Support/VersionTuple.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/Triple.h"

namespace ens {

std::string resolveTargetTriple(const std::string& requestedTriple) {
    const bool useHostDefault = requestedTriple.empty();
    const std::string input = useHostDefault
        ? llvm::sys::getDefaultTargetTriple()
        : llvm::Triple::normalize(requestedTriple);
    llvm::Triple triple(input);

    if (triple.isMacOSX()) {
        llvm::VersionTuple version;
        const bool hasVersion = triple.getMacOSXVersion(version) && !version.empty();
        if (useHostDefault || !hasVersion) {
            triple.setOSName(std::string("macosx") + kDefaultMacOSDeploymentTarget);
        }
    }

    return triple.str();
}

std::string macOSDeploymentTargetForTriple(const std::string& tripleText) {
    llvm::VersionTuple version;
    if (llvm::Triple(tripleText).getMacOSXVersion(version) && !version.empty()) {
        return version.getAsString();
    }
    return kDefaultMacOSDeploymentTarget;
}

}  // namespace ens
