#include "media_input.h"

#include <wil/resource.h>
#include <wil/result.h>

#include <cstdio>

namespace skyggn {

namespace {

constexpr int kIoBufferSize = 64 * 1024;

// ffmpeg reads the file through the IStream windows hands to skyggn
int read_stream(void* opaque, uint8_t* buffer, int size) {
    ULONG read = 0;
    if (FAILED(static_cast<IStream*>(opaque)->Read(buffer, static_cast<ULONG>(size), &read))) {
        return AVERROR(EIO);
    }
    return read == 0 ? AVERROR_EOF : static_cast<int>(read);
}

int64_t seek_stream(void* opaque, int64_t offset, int whence) {
    auto* stream = static_cast<IStream*>(opaque);
    if (whence & AVSEEK_SIZE) {
        STATSTG stat{};
        if (FAILED(stream->Stat(&stat, STATFLAG_NONAME))) {
            return AVERROR(EIO);
        }
        return static_cast<int64_t>(stat.cbSize.QuadPart);
    }
    DWORD origin = 0;
    switch (whence & ~AVSEEK_FORCE) {
    case SEEK_SET:
        origin = STREAM_SEEK_SET;
        break;
    case SEEK_CUR:
        origin = STREAM_SEEK_CUR;
        break;
    case SEEK_END:
        origin = STREAM_SEEK_END;
        break;
    default:
        return AVERROR(EINVAL);
    }
    LARGE_INTEGER move{};
    move.QuadPart = offset;
    ULARGE_INTEGER position{};
    if (FAILED(stream->Seek(move, origin, &position))) {
        return AVERROR(EIO);
    }
    return static_cast<int64_t>(position.QuadPart);
}

int interrupt(void* opaque) {
    return static_cast<const deadline*>(opaque)->passed() ? 1 : 0;
}

}  // namespace

std::wstring stream_name(IStream* stream) {
    STATSTG stat{};
    if (FAILED(stream->Stat(&stat, STATFLAG_DEFAULT)) || !stat.pwcsName) {
        return {};
    }
    wil::unique_cotaskmem_string name(stat.pwcsName);
    return name.get();
}

HRESULT from_av(int error) {
    switch (error) {
    case AVERROR_EXIT:  // the interrupt callback stopped a read: the time limit passed
        return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    case AVERROR(ENOMEM):
        return E_OUTOFMEMORY;
    case AVERROR_INVALIDDATA:
        return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    case AVERROR_DECODER_NOT_FOUND:
    case AVERROR_DEMUXER_NOT_FOUND:
    case AVERROR_PATCHWELCOME:
        return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    case AVERROR_EOF:
        return HRESULT_FROM_WIN32(ERROR_HANDLE_EOF);
    default:
        return E_FAIL;
    }
}

// a file name for ffmpeg's format detection, which uses the extension for formats without a
// signature (tga, pcx)
std::string name_hint(std::wstring_view extension) {
    std::string hint = "media";
    for (wchar_t c : extension) {
        if (c > 0x7f) {
            return "media";
        }
        hint.push_back(static_cast<char>(c));
    }
    return hint;
}

HRESULT open_input(IStream* stream, const std::string& name, const AVInputFormat* forced, deadline* limit, io_ptr& io,
                   format_ptr& format) {
    auto* buffer = static_cast<unsigned char*>(av_malloc(kIoBufferSize));
    RETURN_IF_NULL_ALLOC(buffer);
    AVIOContext* context = avio_alloc_context(buffer, kIoBufferSize, 0, stream, read_stream, nullptr, seek_stream);
    if (!context) {
        av_free(buffer);
        return E_OUTOFMEMORY;
    }
    io.reset(context);

    AVFormatContext* opened = avformat_alloc_context();
    RETURN_IF_NULL_ALLOC(opened);
    opened->pb = io.get();
    opened->flags |= AVFMT_FLAG_CUSTOM_IO;
    opened->interrupt_callback = {interrupt, limit};
    // frees the context itself when it fails
    if (int error = avformat_open_input(&opened, name.c_str(), forced, nullptr); error < 0) {
        return from_av(error);
    }
    format.reset(opened);
    return S_OK;
}

bool needs_probe(const AVFormatContext* format) {
    if (format->duration == AV_NOPTS_VALUE) {
        return true;
    }
    for (unsigned i = 0; i < format->nb_streams; ++i) {
        const AVStream* stream = format->streams[i];
        const AVCodecParameters* parameters = stream->codecpar;
        if (stream->disposition & AV_DISPOSITION_ATTACHED_PIC) {
            continue;
        }
        if (parameters->codec_type == AVMEDIA_TYPE_VIDEO &&
            (parameters->width == 0 || (stream->avg_frame_rate.num == 0 && stream->r_frame_rate.num == 0))) {
            return true;
        }
        if (parameters->codec_type == AVMEDIA_TYPE_AUDIO &&
            (parameters->sample_rate == 0 || parameters->ch_layout.nb_channels == 0)) {
            return true;
        }
    }
    return false;
}

int find_video_stream(const AVFormatContext* format) {
    int best = -1;
    bool best_default = false;
    int64_t best_area = -1;
    for (unsigned i = 0; i < format->nb_streams; ++i) {
        const AVStream* stream = format->streams[i];
        const AVCodecParameters* parameters = stream->codecpar;
        // dependent streams are parts of another picture, such as the tiles of a heif grid
        if (parameters->codec_type != AVMEDIA_TYPE_VIDEO || parameters->codec_id == AV_CODEC_ID_NONE ||
            (stream->disposition & (AV_DISPOSITION_ATTACHED_PIC | AV_DISPOSITION_DEPENDENT))) {
            continue;
        }
        const bool is_default = (stream->disposition & AV_DISPOSITION_DEFAULT) != 0;
        const int64_t area = int64_t{parameters->width} * parameters->height;
        if (best < 0 || (is_default && !best_default) || (is_default == best_default && area > best_area)) {
            best = static_cast<int>(i);
            best_default = is_default;
            best_area = area;
        }
    }
    return best;
}

}  // namespace skyggn
