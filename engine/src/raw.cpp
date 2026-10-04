#include "raw.h"

#include <wil/resource.h>
#include <wil/result.h>

#include <libraw/libraw.h>

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstring>
#include <memory>

namespace skyggn {

namespace {

// libraw reads the file through the IStream windows hands to the thumbnail provider. its parsers
// read a few bytes at a time, so reads go through a window of the file.
class stream_reader final : public LibRaw_abstract_datastream {
public:
    explicit stream_reader(IStream* stream) : stream_(stream) {
        STATSTG stat{};
        if (SUCCEEDED(stream_->Stat(&stat, STATFLAG_NONAME))) {
            size_ = static_cast<INT64>(stat.cbSize.QuadPart);
        }
    }

    int valid() override { return size_ > 0 ? 1 : 0; }

    int read(void* buffer, size_t item_size, size_t count) override {
        if (item_size == 0) {
            return 0;
        }
        const size_t wanted = item_size * count;
        size_t done = 0;
        auto* out = static_cast<uint8_t*>(buffer);
        while (done < wanted && fill()) {
            const size_t available = std::min(static_cast<size_t>(window_end() - position_), wanted - done);
            std::memcpy(out + done, window_.data() + (position_ - window_start_), available);
            position_ += static_cast<INT64>(available);
            done += available;
        }
        return static_cast<int>(done / item_size);
    }

    int seek(INT64 offset, int whence) override {
        const INT64 base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? position_ : size_;
        if (base + offset < 0) {
            return -1;
        }
        position_ = std::min(base + offset, size_);
        return 0;
    }

    INT64 tell() override { return position_; }
    INT64 size() override { return size_; }

    int get_char() override {
        if (!fill()) {
            return -1;
        }
        return window_[static_cast<size_t>(position_++ - window_start_)];
    }

    char* gets(char* text, int length) override {
        if (length < 1 || position_ >= size_) {
            return nullptr;
        }
        int count = 0;
        while (count < length - 1) {
            const int c = get_char();
            if (c < 0) {
                break;
            }
            text[count++] = static_cast<char>(c);
            if (c == '\n') {
                break;
            }
        }
        text[count] = '\0';
        return text;
    }

    // parses one value from the next 24 bytes and skips past it, as libraw's own streams do
    int scanf_one(const char* format, void* value) override {
        char text[25] = {};
        const INT64 start = position_;
        const int got = read(text, 1, 24);
        position_ = start;
        if (got <= 0) {
            return 0;
        }
        const int result = sscanf_s(text, format, value);
        if (result > 0) {
            for (int skipped = 1; position_ < size_ - 1; ++skipped) {
                ++position_;
                const int c = peek();
                if (c <= 0 || c == ' ' || c == '\t' || c == '\n' || skipped > 24) {
                    break;
                }
            }
        }
        return result;
    }

    int eof() override { return position_ >= size_ ? 1 : 0; }

private:
    INT64 window_end() const { return window_start_ + static_cast<INT64>(window_length_); }

    // makes the byte at position_ available in the window
    bool fill() {
        if (position_ >= size_) {
            return false;
        }
        if (position_ >= window_start_ && position_ < window_end()) {
            return true;
        }
        LARGE_INTEGER move{};
        move.QuadPart = position_;
        ULONG read = 0;
        if (FAILED(stream_->Seek(move, STREAM_SEEK_SET, nullptr)) ||
            FAILED(stream_->Read(window_.data(), static_cast<ULONG>(window_.size()), &read)) || read == 0) {
            return false;
        }
        window_start_ = position_;
        window_length_ = read;
        return true;
    }

    int peek() {
        if (!fill()) {
            return -1;
        }
        return window_[static_cast<size_t>(position_ - window_start_)];
    }

    IStream* stream_;
    INT64 size_ = 0;
    INT64 position_ = 0;
    std::vector<uint8_t> window_ = std::vector<uint8_t>(64 * 1024);
    INT64 window_start_ = 0;
    size_t window_length_ = 0;
};

HRESULT from_libraw(int error) {
    switch (error) {
    case LIBRAW_SUCCESS:
        return S_OK;
    case LIBRAW_FILE_UNSUPPORTED:
    case LIBRAW_UNSUPPORTED_THUMBNAIL:
        return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    case LIBRAW_NO_THUMBNAIL:
    case LIBRAW_REQUEST_FOR_NONEXISTENT_THUMBNAIL:
        return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    case LIBRAW_UNSUFFICIENT_MEMORY:
    case LIBRAW_TOO_BIG:
        return E_OUTOFMEMORY;
    case LIBRAW_DATA_ERROR:
    case LIBRAW_IO_ERROR:
        return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    case LIBRAW_CANCELLED_BY_CALLBACK:  // the time limit passed
        return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    default:
        return E_FAIL;
    }
}

bool decodable(LibRaw_internal_thumbnail_formats format) {
    switch (format) {
    case LIBRAW_INTERNAL_THUMBNAIL_JPEG:
    case LIBRAW_INTERNAL_THUMBNAIL_JPEGXL:
    case LIBRAW_INTERNAL_THUMBNAIL_PPM:
    case LIBRAW_INTERNAL_THUMBNAIL_PPM16:
    case LIBRAW_INTERNAL_THUMBNAIL_KODAK_RGB:
    case LIBRAW_INTERNAL_THUMBNAIL_KODAK_YCBCR:
    case LIBRAW_INTERNAL_THUMBNAIL_DNG_YCBCR:
        return true;
    default:
        return false;
    }
}

// libraw's flip: 3 upside down, 5 turned left, 6 turned right
int rotation_of_flip(int flip) {
    switch (flip) {
    case 3:
        return 180;
    case 5:
        return 270;
    case 6:
        return 90;
    default:
        return 0;
    }
}

// reads embedded preview `index`
HRESULT unpack_preview(LibRaw& raw, int index, raw_preview& out) {
    RETURN_IF_FAILED_EXPECTED(from_libraw(raw.unpack_thumb_ex(index)));
    const libraw_thumbnail_t& thumbnail = raw.imgdata.thumbnail;
    RETURN_HR_IF_NULL(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), thumbnail.thumb);
    const auto* data = reinterpret_cast<const uint8_t*>(thumbnail.thumb);
    // the size lets a large jpeg be decoded at a fraction of it
    out.width = thumbnail.twidth;
    out.height = thumbnail.theight;
    switch (thumbnail.tformat) {
    case LIBRAW_THUMBNAIL_JPEG:
        out.type = raw_preview::kind::jpeg;
        break;
    case LIBRAW_THUMBNAIL_JPEGXL:
        out.type = raw_preview::kind::jpeg_xl;
        break;
    case LIBRAW_THUMBNAIL_BITMAP: {
        const size_t pixels = static_cast<size_t>(thumbnail.twidth) * thumbnail.theight;
        RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED), thumbnail.tcolors != 1 && thumbnail.tcolors != 3);
        RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_DATA), pixels == 0 || thumbnail.tlength < pixels * thumbnail.tcolors);
        out.type = thumbnail.tcolors == 3 ? raw_preview::kind::rgb24 : raw_preview::kind::gray8;
        break;
    }
    default:
        return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    }
    out.data.assign(data, data + thumbnail.tlength);
    out.rotation = rotation_of_flip(raw.imgdata.sizes.flip);
    return S_OK;
}

// develops the sensor data at half size, one pixel per 2x2 block of sensor cells: a quarter of the
// work of a full develop and no demosaicing. it reads the whole file, so it is the slow path.
HRESULT develop(LibRaw& raw, const deadline& limit, raw_preview& out) {
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(limit.end - std::chrono::steady_clock::now());
    RETURN_HR_IF_EXPECTED(HRESULT_FROM_WIN32(ERROR_TIMEOUT), remaining.count() <= 0);
    // the time limit reaches libraw two ways: its processing stages ask the progress callback, and its
    // raw decoders check a cancel flag in their loops, which only another thread can set while they run
    raw.set_progress_handler(
        [](void* data, LibRaw_progress, int, int) { return static_cast<const deadline*>(data)->passed() ? 1 : 0; },
        const_cast<deadline*>(&limit));
    wil::unique_threadpool_timer timer(CreateThreadpoolTimer(
        [](PTP_CALLBACK_INSTANCE, void* context, PTP_TIMER) { static_cast<LibRaw*>(context)->setCancelFlag(); }, &raw,
        nullptr));
    RETURN_LAST_ERROR_IF_NULL(timer.get());
    // a negative due time is relative, in 100 ns units
    ULARGE_INTEGER due{};
    due.QuadPart = static_cast<ULONGLONG>(-remaining.count() * 10000);
    FILETIME due_time{due.LowPart, due.HighPart};
    SetThreadpoolTimer(timer.get(), &due_time, 0, 0);

    libraw_output_params_t& params = raw.imgdata.params;
    params.half_size = 1;
    params.use_camera_wb = 1;  // the white balance the camera chose, as its own preview has it
    params.output_bps = 8;
    RETURN_IF_FAILED_EXPECTED(from_libraw(raw.unpack()));
    RETURN_IF_FAILED_EXPECTED(from_libraw(raw.dcraw_process()));
    int error = LIBRAW_SUCCESS;
    std::unique_ptr<libraw_processed_image_t, decltype(&LibRaw::dcraw_clear_mem)> developed(
        raw.dcraw_make_mem_image(&error), &LibRaw::dcraw_clear_mem);
    RETURN_IF_FAILED_EXPECTED(from_libraw(error));
    RETURN_HR_IF_NULL(E_OUTOFMEMORY, developed);
    RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED), developed->type != LIBRAW_IMAGE_BITMAP ||
                                                              developed->bits != 8 ||
                                                              (developed->colors != 1 && developed->colors != 3));
    out.type = developed->colors == 3 ? raw_preview::kind::rgb24 : raw_preview::kind::gray8;
    out.width = developed->width;
    out.height = developed->height;
    out.data.assign(developed->data, developed->data + developed->data_size);
    out.rotation = 0;  // libraw turns the developed picture the way the camera recorded
    return S_OK;
}

}  // namespace

HRESULT read_raw_preview(IStream* stream, UINT size, const deadline& limit, raw_preview& out) {
    stream_reader reader(stream);
    RETURN_HR_IF_EXPECTED(HRESULT_FROM_WIN32(ERROR_INVALID_DATA), !reader.valid());
    // libraw's object is large; keep it off the stack
    auto raw = std::make_unique<LibRaw>();
    RETURN_IF_FAILED_EXPECTED(from_libraw(raw->open_datastream(&reader)));

    const libraw_thumbnail_list_t& list = raw->imgdata.thumbs_list;
    int pick = -1;
    int pick_side = 0;
    for (int i = 0; i < std::min(list.thumbcount, LIBRAW_THUMBNAIL_MAXCOUNT); ++i) {
        const libraw_thumbnail_item_t& item = list.thumblist[i];
        if (!decodable(item.tformat) || item.tlength == 0) {
            continue;
        }
        // a preview of unrecorded size (canon's cr3 full-size jpeg) is taken to be large
        const int side = item.twidth > 0 && item.theight > 0 ? std::max(item.twidth, item.theight) : INT_MAX;
        const bool covers = side >= static_cast<int>(size);
        const bool pick_covers = pick_side >= static_cast<int>(size);
        // the smallest that covers the thumbnail; failing that, the largest
        if (pick < 0 || (covers && (!pick_covers || side < pick_side)) || (!covers && !pick_covers && side > pick_side)) {
            pick = i;
            pick_side = side;
        }
    }
    raw_preview preview;
    const HRESULT unpacked = pick >= 0 ? unpack_preview(*raw, pick, preview) : HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    if (SUCCEEDED(unpacked) && pick_side >= static_cast<int>(size) / 2) {
        out = std::move(preview);
        return S_OK;
    }
    // some cameras embed no preview, or one too small to fill the thumbnail: older canon crw files,
    // chdk raws (128 px), leica m dngs (160 px). their sensor data is developed instead.
    const HRESULT developed = develop(*raw, limit, out);
    if (FAILED(developed) && SUCCEEDED(unpacked)) {
        // the small preview beats no picture; the develop failure is logged for debugging
        LOG_HR(developed);
        out = std::move(preview);
        return S_OK;
    }
    return developed;
}

}  // namespace skyggn
