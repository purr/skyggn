#pragma once

#include "deadline.h"

#include <objidl.h>

#include <cstdint>
#include <vector>

namespace skyggn {

// a raw file's picture: the preview its camera embedded, or the sensor data developed at half size
struct raw_preview {
    enum class kind { jpeg, jpeg_xl, rgb24, gray8 } type = kind::jpeg;
    std::vector<uint8_t> data;  // the compressed picture, or rows of width x height pixels
    int width = 0;  // 0 when the camera did not record it
    int height = 0;
    int rotation = 0;  // clockwise degrees the camera recorded
};

// picks the smallest embedded preview that still covers `size`, or else the largest. a file whose
// previews are all far smaller than `size`, or that has none, is developed from its sensor data
// instead, within `limit`.
HRESULT read_raw_preview(IStream* stream, UINT size, const deadline& limit, raw_preview& out);

}  // namespace skyggn
