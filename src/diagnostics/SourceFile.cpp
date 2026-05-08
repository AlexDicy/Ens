#include "SourceFile.h"

SourceFile::SourceFile(std::string fname, std::u16string src)
    : filename(std::move(fname)), source(std::move(src)) {
    lineStarts.push_back(0);
    for (size_t i = 0; i < source.size(); ++i) {
        if (source[i] == u'\n') {
            lineStarts.push_back(static_cast<int>(i) + 1);
        }
    }
}

std::u16string_view SourceFile::getLine(int lineNumber) const {
    if (lineNumber < 1 || lineNumber > static_cast<int>(lineStarts.size())) {
        return {};
    }
    int start = lineStarts[lineNumber - 1];
    int end = lineNumber < static_cast<int>(lineStarts.size())
                  ? lineStarts[lineNumber]
                  : static_cast<int>(source.size());
    while (end > start && (source[end - 1] == u'\n' || source[end - 1] == u'\r')) {
        end--;
    }
    return std::u16string_view(source.data() + start, end - start);
}
