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

uint32_t SourceFile::positionToOffset(int line, int column) const {
    if (line < 1 || lineStarts.empty()) return 0;
    int idx = line - 1;
    if (idx >= static_cast<int>(lineStarts.size())) return static_cast<uint32_t>(source.size());
    int start = lineStarts[idx];
    int lineEnd = (idx + 1 < static_cast<int>(lineStarts.size()))
                      ? lineStarts[idx + 1]
                      : static_cast<int>(source.size());
    int col0 = column > 0 ? column - 1 : 0;
    int offset = start + col0;
    if (offset > lineEnd) offset = lineEnd;
    return static_cast<uint32_t>(offset);
}

std::pair<int, int> SourceFile::offsetToPosition(uint32_t offset) const {
    int lo = 0;
    int hi = static_cast<int>(lineStarts.size()) - 1;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (static_cast<uint32_t>(lineStarts[mid]) <= offset) lo = mid;
        else hi = mid - 1;
    }
    int line = lo + 1;
    int column = static_cast<int>(offset) - lineStarts[lo] + 1;
    return {line, column};
}
