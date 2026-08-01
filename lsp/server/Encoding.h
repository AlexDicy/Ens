#pragma once
#include <string>
#include <string_view>

std::u16string utf8To16(std::string_view s);
std::string utf16To8(std::u16string_view s);
