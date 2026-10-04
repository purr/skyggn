#include "image.h"

#include <algorithm>

namespace skyggn {

image rotate(const image& source, int degrees) {
    if (degrees == 0) {
        return source;
    }
    const bool sideways = degrees != 180;
    image result{sideways ? source.height : source.width, sideways ? source.width : source.height, {},
                 source.transparent};
    result.pixels.resize(source.pixels.size());
    for (int y = 0; y < source.height; ++y) {
        for (int x = 0; x < source.width; ++x) {
            int tx = x;
            int ty = y;
            if (degrees == 90) {
                tx = source.height - 1 - y;
                ty = x;
            } else if (degrees == 180) {
                tx = source.width - 1 - x;
                ty = source.height - 1 - y;
            } else {
                tx = y;
                ty = source.width - 1 - x;
            }
            result.pixels[static_cast<size_t>(ty) * result.width + tx] =
                source.pixels[static_cast<size_t>(y) * source.width + x];
        }
    }
    return result;
}

double fit_scale(double width, double height, int rotation, UINT size) {
    const bool sideways = rotation == 90 || rotation == 270;
    return std::min({static_cast<double>(size) / (sideways ? height : width),
                     static_cast<double>(size) / (sideways ? width : height), 1.0});
}

}  // namespace skyggn
