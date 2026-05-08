#pragma once
#include <string>
#include <string_view>
#include <vector>

class SourceFile {
public:
    SourceFile(std::string filename, std::u16string source);

    const std::string& getFilename() const { return filename; }
    const std::u16string& getSource() const { return source; }

    std::u16string_view getLine(int lineNumber) const;
    int getLineCount() const { return static_cast<int>(lineStarts.size()); }

private:
    std::string filename;
    std::u16string source;
    std::vector<int> lineStarts;
};
