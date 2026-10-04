#pragma once

#include <string>

namespace skyggn {

// utf-8 text (ffmpeg's tags, archive names) as a windows string; empty for invalid text
std::wstring from_utf8(const char* text);

// a string from the engine's string table (resources.rc), in windows' display language
std::wstring loaded_string(int id);

}  // namespace skyggn
