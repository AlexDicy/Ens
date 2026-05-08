#include "Type.h"

static std::string asciiOf(std::u16string_view s) {
    std::string r;
    r.reserve(s.size());
    for (char16_t c : s) r.push_back(c < 128 ? static_cast<char>(c) : '?');
    return r;
}

void TypeNode::dump(std::ostream& os, int indent) const {
    writeIndent(os, indent);
    os << "Type(" << asciiOf(name);
    if (isOptional) os << "?";
    os << ")\n";
}
