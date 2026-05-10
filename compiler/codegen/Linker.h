#pragma once
#include <ostream>
#include <string>

class Linker {
public:
    static bool link(const std::string& objectPath, const std::string& exePath, std::ostream& errStream);
};
