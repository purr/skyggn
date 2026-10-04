#pragma once

#include <skyggn/skyggn.h>

#include <span>
#include <string_view>

namespace skyggn {

// what reads a file type's picture
enum class reader {
    ffmpeg,  // video, audio cover art, most images
    wic,  // the image formats windows decodes itself: fast, and it knows their embedded previews
    libraw,  // camera raw previews
    archive,  // comic book archives, e-books and opendocument files: the picture inside
    pdf,  // the first page, drawn by windows' own pdf renderer
    design,  // adobe design files: the preview they carry (design.h), else an illustrator file's page
};

struct format_entry {
    skyggn_format format;
    reader read;
    // skyggn's property handler gives explorer its details (length, frame size, tags): windows has
    // no handler for the type, or for matroska one that shows only the length
    bool details;
};

std::span<const format_entry> formats();
const format_entry* find_format(std::wstring_view extension);

}  // namespace skyggn
