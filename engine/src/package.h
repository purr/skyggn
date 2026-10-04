#pragma once

#include "deadline.h"
#include "formats.h"

#include <objidl.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace skyggn {

// a picture file packed inside another file: a comic's or an e-book's cover, a document's thumbnail
struct package_picture {
    std::wstring name;  // its path inside the archive; its extension says how to decode it
    std::vector<uint8_t> data;
};

// a comic book archive (cbz, cbr, cb7, cbt, in any of zip, rar, 7z and tar): its first picture, in
// the order explorer sorts names. an e-book (epub): the cover picture its package names, else the
// first picture it lists. an opendocument file (odt, ods, odp, odg): the thumbnail its program
// saved with it.
HRESULT read_package_picture(IStream* stream, const format_entry& format, const deadline& limit,
                             package_picture& out);

}  // namespace skyggn
