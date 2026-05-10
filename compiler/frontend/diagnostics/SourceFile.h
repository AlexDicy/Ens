#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

class SourceFile {
public:
    SourceFile(std::string filename, std::u16string source);

    const std::string& getFilename() const { return filename; }
    const std::u16string& getSource() const { return source; }

    std::u16string_view getLine(int lineNumber) const;
    int getLineCount() const { return static_cast<int>(lineStarts.size()); }

    // Convert a 0-based char16_t offset to a 1-based (line, column) pair.
    std::pair<int, int> offsetToPosition(uint32_t offset) const;

    // Convert a 1-based (line, column) pair back to a 0-based char16_t offset.
    // Out-of-range positions are clamped to [0, source.size()].
    uint32_t positionToOffset(int line, int column) const;

private:
    std::string filename;
    std::u16string source;
    std::vector<int> lineStarts;
};
