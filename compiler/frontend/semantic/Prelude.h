#pragma once
#include <string_view>

// The compiler-embedded prelude: a hidden module, implicitly visible to every
// module without an import, that defines the exception base class `Error`. When
// a real standard library exists this source moves there verbatim.
inline constexpr std::u16string_view kPreludeModulePath = u"$prelude";
inline constexpr std::u16string_view kPreludeSource =
    u"class StackFrame {\n"
    u"    string function;\n"
    u"    string file;\n"
    u"    int line;\n"
    u"    StackFrame(this.function, this.file, this.line);\n"
    u"}\n"
    u"class Error {\n"
    u"    string message;\n"
    u"    long[]? frames;\n"
    u"    Error(this.message);\n"
    u"    getStackTrace() -> string { return \"\"; }\n"
    u"    getStackFrames() -> StackFrame[] { return [new StackFrame(\"\", \"\", 0)]; }\n"
    u"}\n";
