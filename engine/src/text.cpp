#include "text.h"

#include "ffmpeg_loader.h"

#include <windows.h>

namespace skyggn {

std::wstring from_utf8(const char* text) {
    const int length = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    if (length <= 1) {
        return {};
    }
    std::wstring wide(static_cast<size_t>(length - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text, -1, wide.data(), length);
    return wide;
}

std::wstring loaded_string(int id) {
    const wchar_t* text = nullptr;
    const int length = LoadStringW(engine_module(), id, reinterpret_cast<wchar_t*>(&text), 0);
    return length > 0 ? std::wstring(text, static_cast<size_t>(length)) : std::wstring();
}

}  // namespace skyggn
