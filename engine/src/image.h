#pragma once

#include <windows.h>

#include <cstdint>
#include <vector>

namespace skyggn {

struct image {
    int width = 0;
    int height = 0;
    std::vector<uint32_t> pixels;  // premultiplied bgra, top-down rows
    bool transparent = false;  // some pixels are not opaque (transparent pictures, the room around a badge)
};

// turns a picture clockwise by 0, 90, 180 or 270 degrees
image rotate(const image& source, int degrees);

// the factor that fits a picture into a thumbnail of `size`, after rotation; never above 1
double fit_scale(double width, double height, int rotation, UINT size);

}  // namespace skyggn
