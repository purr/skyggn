#include "decoder.h"

#include "badge.h"
#include "damage.h"
#include "design.h"
#include "deadline.h"
#include "ffmpeg_loader.h"
#include "fingerprint.h"
#include "formats.h"
#include "image.h"
#include "media_input.h"
#include "package.h"
#include "pdf.h"
#include "raw.h"
#include "text.h"
#include "tonemap.h"
#include "wic.h"

#include <shlwapi.h>
#include <wil/com.h>
#include <wil/resource.h>
#include <wil/result.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/display.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

namespace skyggn {

namespace {

// AV_TIME_BASE_Q is a c compound literal, which c++ does not have
constexpr AVRational kMicroseconds{1, AV_TIME_BASE};

// packets read per attempt (all streams) before giving up on a frame
constexpr int kMaxPackets = 8192;
// some streams never flag keyframes; after this many video packets without a frame, decode all
constexpr int kKeyframeOnlyPackets = 256;
// when a frame is black or flat, later positions are tried: this many, this far apart (percent)
constexpr int kRetries = 3;
constexpr int kRetryStep = 15;
constexpr int kLastPosition = 95;

struct codec_closer {
    void operator()(AVCodecContext* c) const { avcodec_free_context(&c); }
};
struct frame_closer {
    void operator()(AVFrame* f) const { av_frame_free(&f); }
};
struct packet_closer {
    void operator()(AVPacket* p) const { av_packet_free(&p); }
};
struct scaler_closer {
    void operator()(SwsContext* s) const { sws_free_context(&s); }
};

using codec_ptr = std::unique_ptr<AVCodecContext, codec_closer>;
using frame_ptr = std::unique_ptr<AVFrame, frame_closer>;
using packet_ptr = std::unique_ptr<AVPacket, packet_closer>;
using scaler_ptr = std::unique_ptr<SwsContext, scaler_closer>;

// whether the file must be probed (avformat_find_stream_info) before its picture can be taken. a
// video whose headers give its frame size and length is not: mp4 and mov give both for the video
// but leave the file's overall length to the probe, which reads and decodes the first packets of
// every stream, and took about a third of such a thumbnail's time. everything else goes by
// needs_probe: still images need their pixel format, songs their covers' size.
bool needs_probe_for_picture(const AVFormatContext* format, bool still) {
    if (const int video = still ? -1 : find_video_stream(format); video >= 0) {
        const AVStream* stream = format->streams[video];
        const bool sized = stream->codecpar->width > 0 && stream->codecpar->height > 0;
        const bool timed = (format->duration != AV_NOPTS_VALUE && format->duration > 0) ||
                           (stream->duration != AV_NOPTS_VALUE && stream->duration > 0);
        if (sized && timed) {
            return false;
        }
    }
    return needs_probe(format);
}

// opens the file and makes sure every stream's format the picture needs is known: from the
// headers, or by probing. `still` is a still image's file type.
HRESULT open_media(IStream* stream, const std::string& name, const AVInputFormat* forced, bool still, deadline* limit,
                   io_ptr& io, format_ptr& format) {
    RETURN_IF_FAILED_EXPECTED(open_input(stream, name, forced, limit, io, format));
    if (needs_probe_for_picture(format.get(), still)) {
        if (int error = avformat_find_stream_info(format.get(), nullptr); error < 0) {
            return from_av(error);
        }
    }
    return S_OK;
}

int cover_rank(const AVStream* stream) {
    // matroska can carry several images (cover.jpg, small_cover.jpg, cover_land.jpg) and id3 tags
    // several pictures; the front cover ranks highest.
    if (const AVDictionaryEntry* name = av_dict_get(stream->metadata, "filename", nullptr, 0)) {
        std::string_view file = name->value;
        if (file.starts_with("cover.")) {
            return 2;
        }
        if (file.find("cover") != std::string_view::npos) {
            return 1;
        }
    }
    if (const AVDictionaryEntry* comment = av_dict_get(stream->metadata, "comment", nullptr, 0)) {
        if (std::string_view{comment->value} == "Cover (front)") {
            return 2;
        }
    }
    return 0;
}

int find_cover_stream(const AVFormatContext* format) {
    int best = -1;
    int best_rank = -1;
    int64_t best_area = -1;
    for (unsigned i = 0; i < format->nb_streams; ++i) {
        const AVStream* stream = format->streams[i];
        if (!(stream->disposition & AV_DISPOSITION_ATTACHED_PIC) || stream->attached_pic.size <= 0) {
            continue;
        }
        const int rank = cover_rank(stream);
        const int64_t area = int64_t{stream->codecpar->width} * stream->codecpar->height;
        if (rank > best_rank || (rank == best_rank && area > best_area)) {
            best = static_cast<int>(i);
            best_rank = rank;
            best_area = area;
        }
    }
    return best;
}

HRESULT open_decoder(const AVStream* stream, UINT size, bool low_impact, codec_ptr& codec) {
    const AVCodec* decoder = avcodec_find_decoder(stream->codecpar->codec_id);
    RETURN_HR_IF_NULL_EXPECTED(HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED), decoder);
    codec.reset(avcodec_alloc_context3(decoder));
    RETURN_IF_NULL_ALLOC(codec.get());
    if (int error = avcodec_parameters_to_context(codec.get(), stream->codecpar); error < 0) {
        return from_av(error);
    }
    codec->pkt_timebase = stream->time_base;
    codec->skip_frame = AVDISCARD_NONKEY;
    // frame threading holds back several frames before the first comes out; slices do not
    codec->thread_type = FF_THREAD_SLICE;
    codec->thread_count = low_impact ? 1 : 0;
    // decoders that can (jpeg covers, mpeg-1/2, mpeg-4 part 2) decode straight to a fraction of
    // the size, as long as it stays at least as big as the thumbnail
    const auto fits = [&](int lowres) {
        return (stream->codecpar->width >> lowres) >= static_cast<int>(size) &&
               (stream->codecpar->height >> lowres) >= static_cast<int>(size);
    };
    int lowres = 0;
    while (lowres < decoder->max_lowres && fits(lowres + 1)) {
        ++lowres;
    }
    codec->lowres = lowres;

    AVDictionary* options = nullptr;
    av_dict_set(&options, "max_frame_delay", "1", 0);  // libdav1d (av1): hand out the first frame at once
    const int error = avcodec_open2(codec.get(), decoder, &options);
    av_dict_free(&options);
    return error < 0 ? from_av(error) : S_OK;
}

// reads packets of one stream until the decoder hands out a frame
// reads the stream's packets and decodes until a frame comes out. frames from before `from` (in the
// stream's time base), when it is set, are passed over.
HRESULT decode_next_frame(AVFormatContext* format, AVCodecContext* codec, int stream_index, const deadline& limit,
                          AVFrame* frame, int64_t from = AV_NOPTS_VALUE) {
    packet_ptr packet(av_packet_alloc());
    RETURN_IF_NULL_ALLOC(packet.get());
    bool flushed = false;
    int stream_packets = 0;
    for (int packets = 0; packets < kMaxPackets; ++packets) {
        if (limit.passed()) {
            return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        }
        int error = avcodec_receive_frame(codec, frame);
        if (error == 0) {
            if (from != AV_NOPTS_VALUE && frame->best_effort_timestamp != AV_NOPTS_VALUE &&
                frame->best_effort_timestamp < from) {
                av_frame_unref(frame);
                continue;
            }
            return S_OK;
        }
        if (error != AVERROR(EAGAIN)) {
            return from_av(error);
        }
        if (flushed) {
            return HRESULT_FROM_WIN32(ERROR_HANDLE_EOF);
        }
        error = av_read_frame(format, packet.get());
        if (error == AVERROR_EOF) {
            avcodec_send_packet(codec, nullptr);  // drain what the decoder still holds
            flushed = true;
            continue;
        }
        if (error < 0) {
            return from_av(error);
        }
        if (packet->stream_index == stream_index) {
            if (++stream_packets == kKeyframeOnlyPackets) {
                codec->skip_frame = AVDISCARD_DEFAULT;
            }
            error = avcodec_send_packet(codec, packet.get());
            // a damaged packet is skipped; the next keyframe may still decode
            if (error < 0 && error != AVERROR_INVALIDDATA) {
                av_packet_unref(packet.get());
                return from_av(error);
            }
            // decoders hold a frame back until the frames after it arrive, to put them in display
            // order. keyframe-only decoding skips those, so a keyframe would only come out with the
            // next one, seconds of video later. draining hands it out now; the flush readies the
            // decoder for more packets if this keyframe gave nothing.
            if (error >= 0 && codec->skip_frame == AVDISCARD_NONKEY && (packet->flags & AV_PKT_FLAG_KEY)) {
                av_packet_unref(packet.get());
                avcodec_send_packet(codec, nullptr);
                const int drained = avcodec_receive_frame(codec, frame);
                avcodec_flush_buffers(codec);
                if (drained == 0) {
                    return S_OK;
                }
                continue;
            }
        }
        av_packet_unref(packet.get());
    }
    return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
}

// returns S_FALSE when the file has no known length or cannot seek; decoding then continues from
// the current position, which still gives a frame. `target` gets the position in the stream's time
// base.
HRESULT seek_to_percent(AVFormatContext* format, const AVStream* stream, int percent, int64_t& target) {
    int64_t duration = format->duration;
    if (duration == AV_NOPTS_VALUE || duration <= 0) {
        if (stream->duration == AV_NOPTS_VALUE || stream->duration <= 0) {
            return S_FALSE;
        }
        duration = av_rescale_q(stream->duration, stream->time_base, kMicroseconds);
    }
    const int64_t start = format->start_time == AV_NOPTS_VALUE ? 0 : format->start_time;
    target = av_rescale_q(start + duration * percent / 100, kMicroseconds, stream->time_base);
    return av_seek_frame(format, stream->index, target, AVSEEK_FLAG_BACKWARD) < 0 ? S_FALSE : S_OK;
}

// clockwise rotation in degrees (0, 90, 180 or 270) from a display matrix
int rotation_from(const uint8_t* matrix, size_t size) {
    if (!matrix || size < 9 * sizeof(int32_t)) {
        return 0;
    }
    // av_display_rotation_get is counter-clockwise
    const double angle = -av_display_rotation_get(reinterpret_cast<const int32_t*>(matrix));
    if (std::isnan(angle)) {
        return 0;
    }
    const int degrees = static_cast<int>(std::lround(angle / 90.0)) * 90 % 360;
    return degrees < 0 ? degrees + 360 : degrees;
}

// the rotation a picture asks for: on the stream for phone recordings and heif images, on the
// frame for jpeg exif orientation
int rotation_of(const AVStream* stream, const AVFrame* frame) {
    if (const AVPacketSideData* data = av_packet_side_data_get(
            stream->codecpar->coded_side_data, stream->codecpar->nb_coded_side_data, AV_PKT_DATA_DISPLAYMATRIX)) {
        return rotation_from(data->data, data->size);
    }
    if (const AVFrameSideData* data = av_frame_get_side_data(frame, AV_FRAME_DATA_DISPLAYMATRIX)) {
        return rotation_from(data->data, data->size);
    }
    return 0;
}

// scales a decoded picture to exactly width x height as sdr srgb bgra
HRESULT scale_frame(AVFrame* source, int width, int height, image& out) {
    const AVPixFmtDescriptor* descriptor = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(source->format));
    RETURN_HR_IF_NULL(HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED), descriptor);
    if (source->colorspace == AVCOL_SPC_UNSPECIFIED && !(descriptor->flags & AV_PIX_FMT_FLAG_RGB)) {
        // untagged video: hd and larger is bt.709 by convention, smaller is bt.601
        source->colorspace = source->height >= 720 ? AVCOL_SPC_BT709 : AVCOL_SPC_SMPTE170M;
    }

    const bool mapping = needs_colour_mapping(source);
    frame_ptr target(av_frame_alloc());
    RETURN_IF_NULL_ALLOC(target.get());
    // the source's colour properties, including hdr mastering and light level data: with nothing
    // to convert, swscale only scales and turns yuv into rgb, the cheap part. any difference (even
    // just missing hdr metadata) makes it build tone mapping tables, about a second of work. hdr
    // and wide gamut are mapped to srgb afterwards, by map_to_srgb.
    if (int error = av_frame_copy_props(target.get(), source); error < 0) {
        return from_av(error);
    }
    target->format = mapping ? AV_PIX_FMT_RGB48 : AV_PIX_FMT_BGRA;
    target->width = width;
    target->height = height;
    target->sample_aspect_ratio = {1, 1};
    target->colorspace = AVCOL_SPC_RGB;
    target->color_range = AVCOL_RANGE_JPEG;

    scaler_ptr scaler(sws_alloc_context());
    RETURN_IF_NULL_ALLOC(scaler.get());
    scaler->flags = SWS_AREA;
    // one thread: the output is thumbnail-sized, and starting a thread per core for it costs more
    // than it saves (a 1080p video's whole thumbnail took 24 ms this way, 38 ms with one per core)
    scaler->threads = 1;
    if (int error = sws_scale_frame(scaler.get(), target.get(), source); error < 0) {
        return from_av(error);
    }

    image scaled{target->width, target->height, {}};
    scaled.pixels.resize(static_cast<size_t>(scaled.width) * scaled.height);
    if (mapping) {
        map_to_srgb(reinterpret_cast<const uint16_t*>(target->data[0]), scaled.width, scaled.height,
                    target->linesize[0], colour_source_of(source), scaled.pixels.data());
    } else {
        for (int y = 0; y < scaled.height; ++y) {
            const uint8_t* row = target->data[0] + static_cast<ptrdiff_t>(y) * target->linesize[0];
            std::memcpy(&scaled.pixels[static_cast<size_t>(y) * scaled.width], row, scaled.width * sizeof(uint32_t));
        }
    }
    // transparent pictures (webp, psd, cover art) stay transparent, as windows shows its own; the
    // thumbnail is premultiplied, which swscale's bgra is not
    if (!mapping && (descriptor->flags & AV_PIX_FMT_FLAG_ALPHA)) {
        for (uint32_t& pixel : scaled.pixels) {
            const uint32_t alpha = pixel >> 24;
            if (alpha == 0xff) {
                continue;
            }
            scaled.transparent = true;
            uint32_t premultiplied = alpha << 24;
            for (int shift = 0; shift < 24; shift += 8) {
                premultiplied |= ((((pixel >> shift) & 0xff) * alpha + 127) / 255) << shift;
            }
            pixel = premultiplied;
        }
    }
    out = std::move(scaled);
    return S_OK;
}

// scales a decoded picture into a thumbnail of at most `size`, upright
HRESULT convert(AVFrame* source, int rotation, UINT size, image& out) {
    double display_width = source->width;
    if (source->sample_aspect_ratio.num > 0 && source->sample_aspect_ratio.den > 0) {
        display_width *= av_q2d(source->sample_aspect_ratio);  // non-square pixels (dvd, some broadcasts)
    }
    const double scale = fit_scale(display_width, source->height, rotation, size);
    image scaled;
    RETURN_IF_FAILED(scale_frame(source, std::max(1, static_cast<int>(std::lround(display_width * scale))),
                                 std::max(1, static_cast<int>(std::lround(source->height * scale))), scaled));
    out = rotate(scaled, rotation);
    return S_OK;
}

// a frame worth showing is neither almost black nor almost white, and has some detail. black,
// white or flat frames (fades, studio logos on black, encoder warm-up) score as dull.
struct detail {
    double mean;
    double deviation;
    bool dull() const { return mean < 24.0 || mean > 240.0 || deviation < 8.0; }
};

detail measure(const image& picture) {
    double sum = 0;
    double sum_squares = 0;
    for (uint32_t pixel : picture.pixels) {
        const double luma = (54.0 * ((pixel >> 16) & 0xff) + 183.0 * ((pixel >> 8) & 0xff) + 19.0 * (pixel & 0xff)) / 256.0;
        sum += luma;
        sum_squares += luma * luma;
    }
    const double count = static_cast<double>(std::max<size_t>(picture.pixels.size(), 1));
    const double mean = sum / count;
    return {mean, std::sqrt(std::max(0.0, sum_squares / count - mean * mean))};
}

HRESULT cover_image(const AVStream* stream, UINT size, bool low_impact, image& out) {
    codec_ptr codec;
    RETURN_IF_FAILED(open_decoder(stream, size, low_impact, codec));
    codec->skip_frame = AVDISCARD_DEFAULT;
    if (int error = avcodec_send_packet(codec.get(), &stream->attached_pic); error < 0) {
        return from_av(error);
    }
    avcodec_send_packet(codec.get(), nullptr);
    frame_ptr frame(av_frame_alloc());
    RETURN_IF_NULL_ALLOC(frame.get());
    if (int error = avcodec_receive_frame(codec.get(), frame.get()); error < 0) {
        return from_av(error);
    }
    return convert(frame.get(), rotation_of(stream, frame.get()), size, out);
}

// a frame of a video, or the picture of a still image (`still`: no seeking, no retries)
HRESULT video_image(AVFormatContext* format, int stream_index, bool still, UINT size, const settings& options,
                    const deadline& limit, image& out) {
    const AVStream* stream = format->streams[stream_index];
    codec_ptr codec;
    RETURN_IF_FAILED(open_decoder(stream, size, options.low_impact, codec));
    if (still) {
        codec->skip_frame = AVDISCARD_DEFAULT;
    }
    frame_ptr frame(av_frame_alloc());
    RETURN_IF_NULL_ALLOC(frame.get());

    std::optional<image> best;
    double best_deviation = -1;
    bool seekable = !still;
    int64_t previous_keyframe = AV_NOPTS_VALUE;
    const int attempts = options.skip_black_frames && !still ? 1 + kRetries : 1;
    for (int attempt = 0; attempt < attempts; ++attempt) {
        const int percent = std::min(static_cast<int>(options.frame_position) + attempt * kRetryStep, kLastPosition);
        int64_t target = AV_NOPTS_VALUE;
        // a file that cannot seek retries with the next keyframe instead of a later position
        if (seekable) {
            if (attempt > 0) {
                avcodec_flush_buffers(codec.get());
                codec->skip_frame = AVDISCARD_NONKEY;
            }
            seekable = seek_to_percent(format, stream, percent, target) == S_OK;
        }
        HRESULT decoded = decode_next_frame(format, codec.get(), stream_index, limit, frame.get());
        if (SUCCEEDED(decoded) && attempt > 0 && seekable && frame->best_effort_timestamp != AV_NOPTS_VALUE &&
            frame->best_effort_timestamp == previous_keyframe) {
            // the same keyframe as the try before: the stream has few keyframes (some encoders write
            // only the first), so every later position lands on it. decode on from it, every frame,
            // to the position asked for.
            av_frame_unref(frame.get());
            avcodec_flush_buffers(codec.get());
            codec->skip_frame = AVDISCARD_DEFAULT;
            seek_to_percent(format, stream, percent, target);
            decoded = decode_next_frame(format, codec.get(), stream_index, limit, frame.get(), target);
        } else if (SUCCEEDED(decoded)) {
            previous_keyframe = frame->best_effort_timestamp;
        }
        if (FAILED(decoded)) {
            if (best) {
                break;  // keep the best earlier frame
            }
            return decoded;
        }
        image candidate;
        HRESULT hr = convert(frame.get(), rotation_of(stream, frame.get()), size, candidate);
        av_frame_unref(frame.get());
        RETURN_IF_FAILED(hr);

        const detail score = measure(candidate);
        if (!score.dull()) {
            out = std::move(candidate);
            return S_OK;
        }
        if (score.deviation > best_deviation) {
            best_deviation = score.deviation;
            best = std::move(candidate);
        }
        if (percent == kLastPosition) {
            break;
        }
    }
    out = std::move(*best);
    return S_OK;
}

const AVStreamGroup* find_tile_grid(const AVFormatContext* format) {
    const AVStreamGroup* found = nullptr;
    for (unsigned i = 0; i < format->nb_stream_groups; ++i) {
        const AVStreamGroup* group = format->stream_groups[i];
        if (group->type != AV_STREAM_GROUP_PARAMS_TILE_GRID || group->nb_streams == 0) {
            continue;
        }
        if (group->disposition & AV_DISPOSITION_DEFAULT) {
            return group;  // the file's primary picture
        }
        if (!found) {
            found = group;
        }
    }
    return found;
}

// many heif photos carry a small preview of the grid picture (apple's are 320x240). decoding it is
// far cheaper than the dozens of tiles, so it is used when it is at least as big as the thumbnail.
// a picture only counts as the preview when it is small, in colour and shaped like the grid: the
// same files also carry hdr gain maps and depth maps, which are larger or grey.
constexpr int kLargestPreview = 512;

int find_grid_preview(const AVFormatContext* format, const AVStreamGroup* group, UINT size) {
    const AVStreamGroupTileGrid* grid = group->params.tile_grid;
    if (grid->width <= 0 || grid->height <= 0) {
        return -1;
    }
    const double grid_aspect = static_cast<double>(grid->width) / grid->height;
    int best = -1;
    int best_long_side = 0;
    for (unsigned i = 0; i < format->nb_streams; ++i) {
        const AVStream* stream = format->streams[i];
        const AVCodecParameters* parameters = stream->codecpar;
        if (parameters->codec_type != AVMEDIA_TYPE_VIDEO || (stream->disposition & AV_DISPOSITION_DEPENDENT) ||
            parameters->width <= 0 || parameters->height <= 0) {
            continue;
        }
        const int long_side = std::max(parameters->width, parameters->height);
        const AVPixFmtDescriptor* descriptor = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(parameters->format));
        const double aspect = static_cast<double>(parameters->width) / parameters->height;
        if (long_side > kLargestPreview || long_side < static_cast<int>(size) || !descriptor ||
            descriptor->nb_components < 3 || std::abs(aspect - grid_aspect) > grid_aspect * 0.02) {
            continue;
        }
        if (best < 0 || long_side < best_long_side) {
            best = static_cast<int>(i);
            best_long_side = long_side;
        }
    }
    return best;
}

// heif photos (iphones, recent cameras) store one picture as a grid of separately coded tiles.
// each tile is decoded and scaled straight into its place in the thumbnail, so the full-size
// picture is never built.
HRESULT grid_image(AVFormatContext* format, const AVStreamGroup* group, UINT size, bool low_impact,
                   const deadline& limit, image& out) {
    const AVStreamGroupTileGrid* grid = group->params.tile_grid;
    RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_DATA), grid->nb_tiles == 0 || grid->width <= 0 || grid->height <= 0);
    int rotation = 0;
    if (const AVPacketSideData* data =
            av_packet_side_data_get(grid->coded_side_data, grid->nb_coded_side_data, AV_PKT_DATA_DISPLAYMATRIX)) {
        rotation = rotation_from(data->data, data->size);
    }
    const double scale = fit_scale(grid->width, grid->height, rotation, size);
    image canvas{std::max(1, static_cast<int>(std::lround(grid->width * scale))),
                 std::max(1, static_cast<int>(std::lround(grid->height * scale))), {}};
    const uint32_t background = 0xff000000u | (uint32_t{grid->background[0]} << 16) |
                                (uint32_t{grid->background[1]} << 8) | grid->background[2];
    canvas.pixels.assign(static_cast<size_t>(canvas.width) * canvas.height, background);

    // each tile is one packet of its own stream
    std::vector<packet_ptr> packets(group->nb_streams);
    size_t collected = 0;
    for (int reads = 0; reads < kMaxPackets && collected < packets.size(); ++reads) {
        if (limit.passed()) {
            return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        }
        packet_ptr packet(av_packet_alloc());
        RETURN_IF_NULL_ALLOC(packet.get());
        const int error = av_read_frame(format, packet.get());
        if (error == AVERROR_EOF) {
            break;
        }
        if (error < 0) {
            return from_av(error);
        }
        for (unsigned s = 0; s < group->nb_streams; ++s) {
            if (group->streams[s]->index == packet->stream_index && !packets[s]) {
                packets[s] = std::move(packet);
                ++collected;
                break;
            }
        }
    }

    // the tiles share their codec parameters, so one decoder serves them all
    codec_ptr codec;
    RETURN_IF_FAILED(open_decoder(group->streams[0], size, low_impact, codec));
    codec->skip_frame = AVDISCARD_DEFAULT;
    frame_ptr frame(av_frame_alloc());
    RETURN_IF_NULL_ALLOC(frame.get());
    for (unsigned t = 0; t < grid->nb_tiles; ++t) {
        const unsigned index = grid->offsets[t].idx;
        if (index >= group->nb_streams || !packets[index]) {
            continue;  // a tile the file does not have keeps the grid's background colour
        }
        avcodec_flush_buffers(codec.get());
        int error = avcodec_send_packet(codec.get(), packets[index].get());
        if (error >= 0) {
            avcodec_send_packet(codec.get(), nullptr);
            error = avcodec_receive_frame(codec.get(), frame.get());
        }
        if (error < 0) {
            return from_av(error);
        }
        // the tile's place in the thumbnail; neighbouring tiles share their edges exactly
        const double x = grid->offsets[t].horizontal - grid->horizontal_offset;
        const double y = grid->offsets[t].vertical - grid->vertical_offset;
        const int left = static_cast<int>(std::floor(x * scale));
        const int top = static_cast<int>(std::floor(y * scale));
        const int right = static_cast<int>(std::floor((x + frame->width) * scale));
        const int bottom = static_cast<int>(std::floor((y + frame->height) * scale));
        if (right > left && bottom > top) {
            image tile;
            HRESULT hr = scale_frame(frame.get(), right - left, bottom - top, tile);
            av_frame_unref(frame.get());
            RETURN_IF_FAILED(hr);
            // copy, clipped to the visible part of the grid
            for (int ty = std::max(0, -top); ty < tile.height && top + ty < canvas.height; ++ty) {
                const int first = std::max(0, -left);
                const int last = std::min(tile.width, canvas.width - left);
                if (last > first) {
                    std::copy_n(&tile.pixels[static_cast<size_t>(ty) * tile.width + first], last - first,
                                &canvas.pixels[static_cast<size_t>(top + ty) * canvas.width + left + first]);
                }
            }
        } else {
            av_frame_unref(frame.get());
        }
    }
    out = rotate(canvas, rotation);
    return S_OK;
}

HRESULT to_bitmap(const image& picture, HBITMAP* bitmap) {
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = picture.width;
    info.bmiHeader.biHeight = -picture.height;  // negative: top-down rows
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    wil::unique_hbitmap result(CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0));
    RETURN_LAST_ERROR_IF_NULL(result.get());
    std::memcpy(bits, picture.pixels.data(), picture.pixels.size() * sizeof(uint32_t));
    *bitmap = result.release();
    return S_OK;
}

// decodes a compressed picture held in memory (a raw file's jpeg or jpeg xl preview)
HRESULT decode_in_memory(AVCodecID codec_id, const std::vector<uint8_t>& bytes, int width, int height, UINT size,
                         bool low_impact, AVFrame* frame) {
    const AVCodec* decoder = avcodec_find_decoder(codec_id);
    RETURN_HR_IF_NULL(HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED), decoder);
    codec_ptr codec(avcodec_alloc_context3(decoder));
    RETURN_IF_NULL_ALLOC(codec.get());
    codec->thread_type = FF_THREAD_SLICE;
    codec->thread_count = low_impact ? 1 : 0;
    // camera previews are often full-size jpegs: decode them at a fraction of that where possible
    int lowres = 0;
    while (lowres < decoder->max_lowres && (width >> (lowres + 1)) >= static_cast<int>(size) &&
           (height >> (lowres + 1)) >= static_cast<int>(size)) {
        ++lowres;
    }
    codec->lowres = lowres;
    if (int error = avcodec_open2(codec.get(), decoder, nullptr); error < 0) {
        return from_av(error);
    }
    packet_ptr packet(av_packet_alloc());
    RETURN_IF_NULL_ALLOC(packet.get());
    if (int error = av_new_packet(packet.get(), static_cast<int>(bytes.size())); error < 0) {
        return from_av(error);
    }
    std::memcpy(packet->data, bytes.data(), bytes.size());
    if (int error = avcodec_send_packet(codec.get(), packet.get()); error < 0) {
        return from_av(error);
    }
    avcodec_send_packet(codec.get(), nullptr);
    const int error = avcodec_receive_frame(codec.get(), frame);
    return error < 0 ? from_av(error) : S_OK;
}

HRESULT camera_raw_image(IStream* stream, UINT size, bool low_impact, const deadline& limit, image& out) {
    raw_preview preview;
    RETURN_IF_FAILED_EXPECTED(read_raw_preview(stream, size, limit, preview));
    frame_ptr frame(av_frame_alloc());
    RETURN_IF_NULL_ALLOC(frame.get());
    switch (preview.type) {
    case raw_preview::kind::jpeg:
    case raw_preview::kind::jpeg_xl:
        RETURN_IF_FAILED(decode_in_memory(preview.type == raw_preview::kind::jpeg ? AV_CODEC_ID_MJPEG
                                                                                   : AV_CODEC_ID_JPEGXL,
                                          preview.data, preview.width, preview.height, size, low_impact,
                                          frame.get()));
        break;
    case raw_preview::kind::rgb24:
    case raw_preview::kind::gray8: {
        const bool rgb = preview.type == raw_preview::kind::rgb24;
        frame->format = rgb ? AV_PIX_FMT_RGB24 : AV_PIX_FMT_GRAY8;
        frame->width = preview.width;
        frame->height = preview.height;
        frame->data[0] = preview.data.data();  // borrowed: preview outlives the frame's use below
        frame->linesize[0] = preview.width * (rgb ? 3 : 1);
        frame->color_range = AVCOL_RANGE_JPEG;
        frame->colorspace = AVCOL_SPC_RGB;
        break;
    }
    }
    // the camera's own orientation applies; any exif orientation inside the preview is ignored so
    // the picture is not turned twice
    return convert(frame.get(), preview.rotation, size, out);
}

HRESULT picture_of(IStream* stream, UINT size, const settings& options, const deadline& limit,
                   const std::wstring& extension, const format_entry* known, image& picture, std::wstring& album);

// an adobe design file's picture: the best of the previews it carries that decodes (design.h).
// without one, an illustrator file with pdf content gets its page drawn. the previews come first
// because an illustrator file saved without pdf content has a page that only says so.
HRESULT design_image(IStream* stream, UINT size, const settings& options, const deadline& limit, image& out) {
    std::vector<embedded_preview> previews;
    RETURN_IF_FAILED(find_design_previews(stream, limit, previews));
    for (const embedded_preview& preview : previews) {
        wil::com_ptr<IStream> inner;
        inner.attach(SHCreateMemStream(preview.data.data(), static_cast<UINT>(preview.data.size())));
        RETURN_IF_NULL_ALLOC(inner.get());
        std::wstring no_album;
        // a preview that does not decode (indesign keeps broken copies) is passed over for the next
        if (SUCCEEDED(picture_of(inner.get(), size, options, limit, preview.extension, find_format(preview.extension),
                                 out, no_album))) {
            return S_OK;
        }
    }
    bool pdf = false;
    RETURN_IF_FAILED(is_pdf(stream, pdf));
    RETURN_HR_IF_EXPECTED(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), !pdf);
    return pdf_image(stream, size, limit, out);
}

// the thumbnail's picture: a raw file's preview, an image, a heif grid, cover art or a video frame.
// `album` gets the album tag of a song, when it has one.
HRESULT picture_of(IStream* stream, UINT size, const settings& options, const deadline& limit,
                   const std::wstring& extension, const format_entry* known, image& picture, std::wstring& album) {
    if (known && known->read == reader::libraw) {
        return camera_raw_image(stream, size, options.low_impact, limit, picture);
    }
    if (known && known->read == reader::pdf) {
        return pdf_image(stream, size, limit, picture);
    }
    if (known && known->read == reader::design) {
        return design_image(stream, size, options, limit, picture);
    }
    if (known && known->read == reader::archive) {
        // the picture is an image file inside the archive, read the way that image type is
        package_picture packed;
        RETURN_IF_FAILED_EXPECTED(read_package_picture(stream, *known, limit, packed));
        wil::com_ptr<IStream> inner;
        inner.attach(SHCreateMemStream(packed.data.data(), static_cast<UINT>(packed.data.size())));
        RETURN_IF_NULL_ALLOC(inner.get());
        const std::wstring inner_extension = PathFindExtensionW(packed.name.c_str());
        std::wstring no_album;
        return picture_of(inner.get(), size, options, limit, inner_extension, find_format(inner_extension), picture,
                          no_album);
    }
    if (known && known->read == reader::wic) {
        if (SUCCEEDED(wic_image(stream, size, extension, picture))) {
            return S_OK;
        }
        // windows' codecs skip some variants ffmpeg reads (tiff in ycbcr, for one): try those next
        RETURN_IF_FAILED(stream->Seek({}, STREAM_SEEK_SET, nullptr));
    }

    const std::string name = name_hint(extension);
    const bool still = known && known->format.category == SKYGGN_CATEGORY_IMAGE;
    io_ptr io;  // declared before format: the format context reads through it until closed
    format_ptr format;
    // the limit is copied: the format context's interrupt callback holds a pointer to it
    deadline io_limit = limit;
    HRESULT opened = open_media(stream, name, nullptr, still, &io_limit, io, format);
    if (FAILED(opened) && known && known->format.category == SKYGGN_CATEGORY_IMAGE) {
        // image formats without a signature (tga, pcx) are only recognised by their extension, which
        // ffmpeg's image pipe reader goes by
        format.reset();
        io.reset();
        RETURN_IF_FAILED(stream->Seek({}, STREAM_SEEK_SET, nullptr));
        opened = open_media(stream, name, av_find_input_format("image2pipe"), still, &io_limit, io, format);
    }
    RETURN_IF_FAILED_EXPECTED(opened);

    if (const AVDictionaryEntry* tag = av_dict_get(format->metadata, "album", nullptr, 0)) {
        album = from_utf8(tag->value);
    }
    const AVStreamGroup* grid = find_tile_grid(format.get());
    const int cover = find_cover_stream(format.get());
    const int video = find_video_stream(format.get());
    HRESULT hr = HRESULT_FROM_WIN32(ERROR_NOT_FOUND);  // no picture in this file
    if (grid) {
        if (const int preview = find_grid_preview(format.get(), grid, size); preview >= 0) {
            hr = video_image(format.get(), preview, true, size, options, limit, picture);
        }
        if (FAILED(hr)) {
            hr = grid_image(format.get(), grid, size, options.low_impact, limit, picture);
        }
    }
    // a broken cover falls back to a video frame, and the other way round
    if (FAILED(hr) && cover >= 0 && (options.prefer_cover_art || video < 0)) {
        hr = cover_image(format->streams[cover], size, options.low_impact, picture);
    }
    if (FAILED(hr) && video >= 0) {
        hr = video_image(format.get(), video, still, size, options, limit, picture);
    }
    if (FAILED(hr) && cover >= 0 && !options.prefer_cover_art) {
        hr = cover_image(format->streams[cover], size, options.low_impact, picture);
    }
    return hr;
}

}  // namespace

HRESULT make_thumbnail_image(IStream* stream, UINT size, const settings& options, image& picture,
                             skyggn_damage* damage) {
    RETURN_HR_IF(E_INVALIDARG, size == 0);
    RETURN_IF_FAILED(load_ffmpeg());

    // gentle mode lowers only this thread's cpu priority while it works, and decodes on this thread
    // alone: games and other programs at normal priority go first, and an idle core makes the
    // thumbnail at full speed. windows' background mode (THREAD_MODE_BACKGROUND_BEGIN) also lowers
    // disk and memory priority, which made thumbnails take up to 13 s instead of 50 ms whenever
    // anything else used the disk or the cpu, as explorer itself does while a folder opens. a thread
    // windows already runs lower keeps its priority.
    const int previous_priority = GetThreadPriority(GetCurrentThread());
    const bool lowered = options.low_impact && previous_priority != THREAD_PRIORITY_ERROR_RETURN &&
                         previous_priority > THREAD_PRIORITY_BELOW_NORMAL &&
                         SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    auto restore_priority = wil::scope_exit([&] {
        if (lowered) {
            SetThreadPriority(GetCurrentThread(), previous_priority);
        }
    });

    const deadline limit{std::chrono::steady_clock::now() + std::chrono::milliseconds(options.time_limit_ms)};
    const std::wstring name = stream_name(stream);
    const std::wstring extension = PathFindExtensionW(name.c_str());
    const format_entry* known = find_format(extension);
    // a file type skyggn does not list (a handler registered by hand) counts as video
    const skyggn_category category = known ? known->format.category : SKYGGN_CATEGORY_VIDEO;
    // the picture fits inside the room the badge needs around it
    const UINT picture_size = size - 2 * badge_room(size, options.badge);
    std::wstring album;
    const HRESULT found = picture_of(stream, picture_size, options, limit, extension, known, picture, album);
    // what the file's structure proves wrong with it, for the tile's note or the badge's mark; checked
    // only when one of them shows, or when asked
    skyggn_damage proven = SKYGGN_DAMAGE_NONE;
    const bool shown = FAILED(found) ? options.placeholder != SKYGGN_PLACEHOLDER_NONE
                                     : options.badge.style != SKYGGN_BADGE_NONE;
    if ((shown || damage) && FAILED(LOG_IF_FAILED(find_damage(stream, SUCCEEDED(found), limit, proven)))) {
        // a check that could not read the file proves nothing; the thumbnail goes on without a note
        proven = SKYGGN_DAMAGE_NONE;
    }
    if (damage) {
        *damage = proven;
    }
    if (FAILED(found)) {
        // no picture (a song without cover art) or none that could be read (a damaged file)
        RETURN_HR_IF_EXPECTED(found, options.placeholder == SKYGGN_PLACEHOLDER_NONE);
        // a tile in a colour of its own takes it from the song's album, so an album matches, or from
        // all of the file's contents, so copies match whatever their names and other files do not. a
        // file over 64 MB, or one not read within the time limit, gets its kind's colour: its
        // contents were not compared, and a colour of its own would claim they were.
        skyggn_placeholder style = options.placeholder;
        uint64_t seed = 0;
        if (style == SKYGGN_PLACEHOLDER_PER_FILE) {
            if (album.empty()) {
                bool complete = false;
                RETURN_IF_FAILED(content_fingerprint(stream, limit, seed, complete));
                if (!complete) {
                    style = SKYGGN_PLACEHOLDER_KIND;
                }
            } else {
                seed = text_fingerprint(album);
            }
        }
        return draw_placeholder(size, extension, seed, category, style, options.badge, proven, picture);
    }
    // a thumbnail without its badge beats no thumbnail; the failure is logged for debugging
    LOG_IF_FAILED(draw_badge(picture, size, extension, category, options.badge, proven != SKYGGN_DAMAGE_NONE));
    return S_OK;
}

HRESULT make_thumbnail(IStream* stream, UINT size, const settings& options, HBITMAP* bitmap, bool* transparent) {
    image picture;
    RETURN_IF_FAILED_EXPECTED(make_thumbnail_image(stream, size, options, picture));
    *transparent = picture.transparent;
    return to_bitmap(picture, bitmap);
}

}  // namespace skyggn
