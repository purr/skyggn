#pragma once

#include "image.h"

#include <objidl.h>

#include <string_view>

namespace skyggn {

// decodes an image with windows' own codecs (jpeg, png, gif, bmp, tiff, ico, jpeg xr): fast, they
// decode at a reduced size where the format allows it. the picture is turned upright as its exif
// orientation asks, and keeps its transparency.
HRESULT wic_image(IStream* stream, UINT size, std::wstring_view extension, image& out);

}  // namespace skyggn
