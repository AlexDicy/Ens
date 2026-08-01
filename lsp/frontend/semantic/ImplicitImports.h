#pragma once
#include <array>
#include <string_view>

// The implicitly imported modules: ordinary standard-library modules, loaded into every
// module graph, whose exported names are in scope in every module with no import written.
// Every compilation therefore has to be able to resolve the standard library.

// The module holding the exception base class `Error` and `StackFrame`. Code generation keys
// its interception of the two trace methods on this path.
inline constexpr std::u16string_view kCoreModulePath = u"std.core";

inline constexpr std::array<std::u16string_view, 1> kImplicitImportPaths = {
    kCoreModulePath,
};
