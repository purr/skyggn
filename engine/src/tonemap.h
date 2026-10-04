#pragma once

#include <cstddef>
#include <cstdint>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

namespace skyggn {

// hdr (pq, hlg) and wide gamut (bt.2020, display p3) pictures need mapping to sdr srgb. swscale can
// do it, but builds its colour lookup tables first, which costs about a second per thumbnail; this
// maps the already scaled picture directly.
bool needs_colour_mapping(const AVFrame* frame);

struct colour_source {
    AVColorTransferCharacteristic transfer;
    AVColorPrimaries primaries;
    double peak_nits;  // brightest level in the content, for hdr
};

colour_source colour_source_of(const AVFrame* frame);

// rgb48 pixels, still in the source's transfer and primaries, to bgra srgb
void map_to_srgb(const uint16_t* rgb48, int width, int height, ptrdiff_t stride, const colour_source& source,
                 uint32_t* bgra);

}  // namespace skyggn
