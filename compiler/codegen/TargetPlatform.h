#pragma once

#include <string>

namespace ens {

inline constexpr char kDefaultMacOSDeploymentTarget[] = "11.0";

std::string resolveTargetTriple(const std::string& requestedTriple);
std::string macOSDeploymentTargetForTriple(const std::string& triple);

}  // namespace ens
