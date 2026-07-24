#pragma once
#include <string_view>

// The compiler-embedded prelude: a hidden module, implicitly visible to every
// module without an import, that defines the exception base class `Error`. When
// a real standard library exists this source moves there verbatim.
inline constexpr std::u16string_view kPreludeModulePath = u"$prelude";
inline constexpr std::u16string_view kPreludeSource =
    u"export class StackFrame {\n"
    u"    export string function;\n"
    u"    export string file;\n"
    u"    export int line;\n"
    u"    export constructor(this.function, this.file, this.line);\n"
    u"}\n"
    u"export class Error {\n"
    u"    export string message;\n"
    u"    long[]? frames;\n"
    u"    export constructor(this.message);\n"
    u"    export getStackTrace() -> string { return \"\"; }\n"
    u"    export getStackFrames() -> StackFrame[] { return [new StackFrame(\"\", \"\", 0)]; }\n"
    u"}\n";
