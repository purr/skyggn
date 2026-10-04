#pragma once

#include "deadline.h"

#include <propsys.h>

#include <span>
#include <string>
#include <string_view>

namespace skyggn {

// reads a media file's details (length, frame size and rate, bit rates, codecs, channels, sample
// rate, title, artist, album and other tags) into `store`, under the keys explorer shows them by
HRESULT read_details(IStream* stream, std::wstring_view extension, const deadline& limit, IPropertyStore* store);

// a detail windows has no property for, so skyggn describes it in its own property schema
struct own_detail {
    PROPERTYKEY key;
    const wchar_t* name;  // canonical name, as windows' detail lists use it
    int label;  // string resource: the label explorer shows, in english or german
    bool list;  // several strings, else one number
};

// the video, audio and subtitle tracks of a file, and its chapters
std::span<const own_detail> own_details();

// the property schema (a .propdesc file's xml) that describes the own details to windows. their
// labels point into the string table of the engine at `engine_path`.
std::string details_schema(const std::wstring& engine_path);

}  // namespace skyggn
