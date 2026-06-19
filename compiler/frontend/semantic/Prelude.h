#pragma once
#include <string_view>

// The compiler-embedded prelude: a hidden module, implicitly visible to every
// module without an import, that defines the exception base class `Error`. When
// a real standard library exists this source moves there verbatim.
inline constexpr std::u16string_view kPreludeModulePath = u"$prelude";
inline constexpr std::u16string_view kPreludeSource =
    u"class Error { string message; Error(this.message); }\n";
