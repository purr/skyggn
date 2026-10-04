#include "formats.h"

#include <algorithm>

namespace skyggn {

namespace {

constexpr format_entry video(const wchar_t* extension, BOOL recommended = TRUE) {
    return {{extension, SKYGGN_CATEGORY_VIDEO, recommended}, reader::ffmpeg, false};
}

constexpr format_entry audio(const wchar_t* extension) {
    return {{extension, SKYGGN_CATEGORY_AUDIO, TRUE}, reader::ffmpeg, false};
}

constexpr format_entry image(const wchar_t* extension, reader read = reader::ffmpeg) {
    return {{extension, SKYGGN_CATEGORY_IMAGE, TRUE}, read, false};
}

constexpr format_entry camera_raw(const wchar_t* extension, BOOL recommended = TRUE) {
    return {{extension, SKYGGN_CATEGORY_RAW, recommended}, reader::libraw, false};
}

constexpr format_entry detailed(format_entry entry) {
    entry.details = true;
    return entry;
}

constexpr format_entry book(const wchar_t* extension) {
    return {{extension, SKYGGN_CATEGORY_BOOK, TRUE}, reader::archive, false};
}

constexpr format_entry document(const wchar_t* extension, reader read = reader::archive) {
    return {{extension, SKYGGN_CATEGORY_DOCUMENT, TRUE}, read, false};
}

// off by default: .ts and .mts, which are also typescript source files (every one of those would
// be handed to the video decoder only to fail), and .raw, which many programs use for files that
// are not camera raws.
constexpr format_entry kFormats[] = {
    video(L".3g2"), video(L".3gp"), video(L".3gpp"), detailed(video(L".amv")), video(L".asf"), video(L".avi"),
    detailed(video(L".bik")), detailed(video(L".divx")), detailed(video(L".dv")), detailed(video(L".evo")),
    detailed(video(L".f4v")), detailed(video(L".flv")), detailed(video(L".ivf")), video(L".m1v"),
    detailed(video(L".m2p")), video(L".m2t"), video(L".m2ts"), video(L".m2v"), video(L".m4v"),
    detailed(video(L".mjpeg")), detailed(video(L".mjpg")), detailed(video(L".mk3d")), detailed(video(L".mkv")),
    video(L".mov"), video(L".mp4"), video(L".mpe"), video(L".mpeg"), video(L".mpg"), detailed(video(L".mpv")),
    video(L".mts", FALSE), detailed(video(L".mxf")), detailed(video(L".nut")), video(L".ogm"), video(L".ogv"),
    detailed(video(L".qt")), detailed(video(L".rm")), detailed(video(L".rmvb")), video(L".tod"),
    detailed(video(L".trp")), video(L".ts", FALSE), video(L".vob"), detailed(video(L".vro")),
    detailed(video(L".webm")), video(L".wmv"), video(L".wtv"), detailed(video(L".xvid")),

    audio(L".aac"), audio(L".ac3"), detailed(audio(L".aif")), detailed(audio(L".aifc")), detailed(audio(L".aiff")),
    detailed(audio(L".amr")), detailed(audio(L".ape")), detailed(audio(L".au")), detailed(audio(L".caf")),
    detailed(audio(L".dff")), detailed(audio(L".dsf")), detailed(audio(L".dts")), audio(L".flac"), audio(L".m4a"),
    audio(L".m4b"), detailed(audio(L".m4r")), detailed(audio(L".mka")), audio(L".mp2"), audio(L".mp3"),
    detailed(audio(L".mpc")), audio(L".oga"), audio(L".ofr"), audio(L".ofs"), audio(L".ogg"), audio(L".opus"),
    detailed(audio(L".spx")), detailed(audio(L".tak")), detailed(audio(L".tta")), audio(L".wav"),
    detailed(audio(L".weba")), audio(L".wma"), detailed(audio(L".wv")),

    image(L".ai", reader::design), image(L".ait", reader::design), image(L".apng"), image(L".avif"), image(L".avifs"), image(L".bmp", reader::wic), image(L".dds"),
    image(L".dib", reader::wic), image(L".dpx"), image(L".eps", reader::design), image(L".exr"), image(L".gif", reader::wic), image(L".hdr"),
    image(L".heic"), image(L".heif"), image(L".hif"), image(L".ico", reader::wic), image(L".j2k"),
    image(L".jfif", reader::wic), image(L".jp2"), image(L".jpe", reader::wic), image(L".jpeg", reader::wic),
    image(L".jpf"), image(L".jpg", reader::wic), image(L".jpx"), image(L".jxl"), image(L".jxr", reader::wic),
    image(L".pam"), image(L".pbm"), image(L".pcx"), image(L".pfm"), image(L".pgm"), image(L".png", reader::wic),
    image(L".pnm"), image(L".ppm"), image(L".psd"), image(L".qoi"), image(L".ras"), image(L".sgi"), image(L".svg"),
    image(L".tga"), image(L".tif", reader::wic), image(L".tiff", reader::wic), image(L".wdp", reader::wic),
    image(L".webp"), image(L".xbm"), image(L".xpm"),

    camera_raw(L".3fr"), camera_raw(L".ari"), camera_raw(L".arw"), camera_raw(L".bay"), camera_raw(L".cap"),
    camera_raw(L".cr2"), camera_raw(L".cr3"), camera_raw(L".crw"), camera_raw(L".dcr"), camera_raw(L".dcs"),
    camera_raw(L".dng"), camera_raw(L".drf"), camera_raw(L".eip"), camera_raw(L".erf"), camera_raw(L".fff"),
    camera_raw(L".gpr"), camera_raw(L".iiq"), camera_raw(L".k25"), camera_raw(L".kdc"), camera_raw(L".mdc"),
    camera_raw(L".mef"), camera_raw(L".mos"), camera_raw(L".mrw"), camera_raw(L".nef"), camera_raw(L".nrw"),
    camera_raw(L".orf"), camera_raw(L".ori"), camera_raw(L".pef"), camera_raw(L".raf"), camera_raw(L".raw", FALSE),
    camera_raw(L".rdc"), camera_raw(L".rw2"), camera_raw(L".rwl"), camera_raw(L".rwz"), camera_raw(L".sr2"),
    camera_raw(L".srf"), camera_raw(L".srw"), camera_raw(L".x3f"),

    book(L".cb7"), book(L".cbr"), book(L".cbt"), book(L".cbz"), book(L".epub"),

    // text, spreadsheet, presentation and drawing, and their templates; pdf; indesign layouts and
    // their templates
    document(L".indd", reader::design), document(L".indt", reader::design), document(L".odg"), document(L".odp"), document(L".ods"), document(L".odt"), document(L".otg"),
    document(L".otp"), document(L".ots"), document(L".ott"), document(L".pdf", reader::pdf),
};

}  // namespace

std::span<const format_entry> formats() {
    return kFormats;
}

const format_entry* find_format(std::wstring_view extension) {
    auto it = std::ranges::find_if(kFormats, [&](const format_entry& entry) {
        return CompareStringOrdinal(entry.format.extension, -1, extension.data(), static_cast<int>(extension.size()),
                                    TRUE) == CSTR_EQUAL;
    });
    return it == std::end(kFormats) ? nullptr : &*it;
}

}  // namespace skyggn
