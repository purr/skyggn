#pragma once

#include "deadline.h"

#include <objidl.h>

#include <memory>
#include <string>
#include <string_view>

extern "C" {
#include <libavformat/avformat.h>
}

namespace skyggn {

struct format_closer {
    void operator()(AVFormatContext* f) const { avformat_close_input(&f); }
};
struct io_closer {
    void operator()(AVIOContext* io) const {
        av_freep(&io->buffer);  // avio may have replaced the buffer it was given
        avio_context_free(&io);
    }
};

using format_ptr = std::unique_ptr<AVFormatContext, format_closer>;
using io_ptr = std::unique_ptr<AVIOContext, io_closer>;

// the file's name, as windows gives it with the stream; empty when it gives none
std::wstring stream_name(IStream* stream);

// an ffmpeg error code as an hresult
HRESULT from_av(int error);

// a file name for ffmpeg's format detection, which uses the extension for formats without a
// signature (tga, pcx)
std::string name_hint(std::wstring_view extension);

// opens the file windows hands over as a stream. reads stop once `limit` passes; it must outlive
// the format context. `forced` names the format instead of detecting it. only the headers are
// read: avformat_find_stream_info is the caller's choice.
HRESULT open_input(IStream* stream, const std::string& name, const AVInputFormat* forced, deadline* limit, io_ptr& io,
                   format_ptr& format);

// whether avformat_find_stream_info must run. most formats (matroska, mp4, mp3) state every
// stream's format and the length in their headers; probing then only costs time, since it reads
// and decodes the first packets of every stream. others (mpeg-ts, raw streams, images) need it.
bool needs_probe(const AVFormatContext* format);

// the file's main video stream: the default one, else the largest; -1 when there is none.
// cover pictures and the tiles of a heif grid do not count.
int find_video_stream(const AVFormatContext* format);

}  // namespace skyggn
