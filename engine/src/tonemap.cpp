#include "tonemap.h"

#include <algorithm>
#include <cmath>

extern "C" {
#include <libavutil/mastering_display_metadata.h>
}

namespace skyggn {

namespace {

// sdr reference white (itu-r bt.2408): hdr diffuse white sits here, and it becomes srgb white
constexpr double kSdrWhiteNits = 203.0;
constexpr double kDefaultPeakNits = 1000.0;
// hlg is scene-referred; it is shown as on a 1000-nit display (itu-r bt.2100 reference ootf)
constexpr double kHlgDisplayNits = 1000.0;
constexpr double kHlgSystemGamma = 1.2;

// smpte st 2084 (pq) constants
constexpr double kM1 = 2610.0 / 16384.0;
constexpr double kM2 = 2523.0 / 4096.0 * 128.0;
constexpr double kC1 = 3424.0 / 4096.0;
constexpr double kC2 = 2413.0 / 4096.0 * 32.0;
constexpr double kC3 = 2392.0 / 4096.0 * 32.0;

double pq_to_nits(double signal) {
    const double p = std::pow(std::clamp(signal, 0.0, 1.0), 1.0 / kM2);
    return 10000.0 * std::pow(std::max(p - kC1, 0.0) / (kC2 - kC3 * p), 1.0 / kM1);
}

double nits_to_pq(double nits) {
    const double y = std::pow(std::clamp(nits / 10000.0, 0.0, 1.0), kM1);
    return std::pow((kC1 + kC2 * y) / (1.0 + kC3 * y), kM2);
}

// arib std-b67 (hlg) inverse oetf: signal to relative scene light, 0..1
double hlg_to_scene(double signal) {
    constexpr double a = 0.17883277;
    constexpr double b = 1.0 - 4.0 * a;
    const double c = 0.5 - a * std::log(4.0 * a);
    signal = std::clamp(signal, 0.0, 1.0);
    return signal <= 0.5 ? signal * signal / 3.0 : (std::exp((signal - c) / a) + b) / 12.0;
}

double srgb_encode(double linear) {
    linear = std::clamp(linear, 0.0, 1.0);
    return linear <= 0.0031308 ? 12.92 * linear : 1.055 * std::pow(linear, 1.0 / 2.4) - 0.055;
}

double srgb_decode(double signal) {
    signal = std::clamp(signal, 0.0, 1.0);
    return signal <= 0.04045 ? signal / 12.92 : std::pow((signal + 0.055) / 1.055, 2.4);
}

// linear-light conversions to bt.709 / srgb primaries
constexpr double kBt2020ToBt709[3][3] = {
    {1.6605, -0.5876, -0.0728},
    {-0.1246, 1.1329, -0.0083},
    {-0.0182, -0.1006, 1.1187},
};
constexpr double kDisplayP3ToBt709[3][3] = {
    {1.2249, -0.2249, 0.0},
    {-0.0421, 1.0421, 0.0},
    {-0.0196, -0.0786, 1.0983},
};

const double (*gamut_matrix(AVColorPrimaries primaries))[3] {
    switch (primaries) {
    case AVCOL_PRI_BT2020:
        return kBt2020ToBt709;
    case AVCOL_PRI_SMPTE432:
        return kDisplayP3ToBt709;
    default:
        return nullptr;
    }
}

// itu-r bt.2390 eetf: leaves dark and mid tones alone and rolls highlights off smoothly, from the
// content's peak down to the target peak. works in the pq domain, on nits.
struct highlight_rolloff {
    double source_pq;
    double target;  // target peak, relative to source_pq
    double knee;

    highlight_rolloff(double source_peak, double target_peak)
        : source_pq(nits_to_pq(source_peak)),
          target(nits_to_pq(target_peak) / source_pq),
          knee(1.5 * target - 0.5) {}

    double operator()(double nits) const {
        const double e = nits_to_pq(nits) / source_pq;
        if (e <= knee || knee >= 1.0) {
            return nits;
        }
        const double t = (e - knee) / (1.0 - knee);
        const double t2 = t * t;
        const double t3 = t2 * t;
        const double mapped = (2 * t3 - 3 * t2 + 1) * knee + (t3 - 2 * t2 + t) * (1.0 - knee) + (-2 * t3 + 3 * t2) * target;
        return pq_to_nits(std::min(mapped, 1.0) * source_pq);
    }
};

}  // namespace

bool needs_colour_mapping(const AVFrame* frame) {
    return frame->color_trc == AVCOL_TRC_SMPTE2084 || frame->color_trc == AVCOL_TRC_ARIB_STD_B67 ||
           gamut_matrix(frame->color_primaries) != nullptr;
}

colour_source colour_source_of(const AVFrame* frame) {
    double peak = kDefaultPeakNits;
    if (const AVFrameSideData* data = av_frame_get_side_data(frame, AV_FRAME_DATA_CONTENT_LIGHT_LEVEL)) {
        const auto* level = reinterpret_cast<const AVContentLightMetadata*>(data->data);
        if (level->MaxCLL > 0) {
            peak = level->MaxCLL;
        }
    } else if (const AVFrameSideData* mastering =
                   av_frame_get_side_data(frame, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA)) {
        const auto* display = reinterpret_cast<const AVMasteringDisplayMetadata*>(mastering->data);
        if (display->has_luminance && display->max_luminance.den > 0) {
            peak = av_q2d(display->max_luminance);
        }
    }
    if (frame->color_trc == AVCOL_TRC_ARIB_STD_B67) {
        peak = kHlgDisplayNits;
    }
    return {frame->color_trc, frame->color_primaries, std::clamp(peak, kSdrWhiteNits * 1.01, 10000.0)};
}

void map_to_srgb(const uint16_t* rgb48, int width, int height, ptrdiff_t stride, const colour_source& source,
                 uint32_t* bgra) {
    const bool pq = source.transfer == AVCOL_TRC_SMPTE2084;
    const bool hlg = source.transfer == AVCOL_TRC_ARIB_STD_B67;
    const bool srgb = source.transfer == AVCOL_TRC_IEC61966_2_1;
    const double (*gamut)[3] = gamut_matrix(source.primaries);
    const highlight_rolloff rolloff(source.peak_nits, kSdrWhiteNits);

    for (int y = 0; y < height; ++y) {
        const auto* row = reinterpret_cast<const uint16_t*>(reinterpret_cast<const uint8_t*>(rgb48) + y * stride);
        for (int x = 0; x < width; ++x) {
            double rgb[3];
            for (int c = 0; c < 3; ++c) {
                rgb[c] = row[x * 3 + c] / 65535.0;
            }
            // to linear light, where 1.0 is sdr white
            if (pq) {
                for (double& v : rgb) {
                    v = pq_to_nits(v);
                }
            } else if (hlg) {
                for (double& v : rgb) {
                    v = hlg_to_scene(v);
                }
                const double scene_luma = 0.2627 * rgb[0] + 0.6780 * rgb[1] + 0.0593 * rgb[2];
                const double ootf = kHlgDisplayNits * std::pow(std::max(scene_luma, 1e-6), kHlgSystemGamma - 1.0);
                for (double& v : rgb) {
                    v *= ootf;
                }
            } else {
                // sdr wide gamut: display p3 photos use the srgb curve, video the bt.1886 gamma
                for (double& v : rgb) {
                    v = (srgb ? srgb_decode(v) : std::pow(v, 2.4)) * kSdrWhiteNits;
                }
            }
            if (pq || hlg) {
                // roll off on the brightest channel, which keeps hues where per-channel mapping
                // would shift them
                const double brightest = std::max({rgb[0], rgb[1], rgb[2]});
                if (brightest > 0.0) {
                    const double scale = rolloff(brightest) / brightest;
                    for (double& v : rgb) {
                        v *= scale;
                    }
                }
            }
            for (double& v : rgb) {
                v /= kSdrWhiteNits;
            }
            if (gamut) {
                const double r = gamut[0][0] * rgb[0] + gamut[0][1] * rgb[1] + gamut[0][2] * rgb[2];
                const double g = gamut[1][0] * rgb[0] + gamut[1][1] * rgb[1] + gamut[1][2] * rgb[2];
                const double b = gamut[2][0] * rgb[0] + gamut[2][1] * rgb[1] + gamut[2][2] * rgb[2];
                rgb[0] = r;
                rgb[1] = g;
                rgb[2] = b;
            }
            const auto channel = [](double linear) {
                return static_cast<uint32_t>(std::lround(srgb_encode(linear) * 255.0));
            };
            bgra[static_cast<size_t>(y) * width + x] =
                0xff000000u | (channel(rgb[0]) << 16) | (channel(rgb[1]) << 8) | channel(rgb[2]);
        }
    }
}

}  // namespace skyggn
