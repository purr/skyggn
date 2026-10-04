#include "details.h"

#include "ffmpeg_loader.h"
#include "media_input.h"
#include "resource_ids.h"
#include "text.h"

#include <propkey.h>
#include <propvarutil.h>
#include <wil/resource.h>
#include <wil/result.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cwctype>
#include <initializer_list>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace skyggn {

namespace {

// the format id of skyggn's own details
constexpr GUID kOwnDetails = {0x72f66e8d, 0xc516, 0x4fd4, {0x9a, 0x13, 0xea, 0x5d, 0x4f, 0xc4, 0x2d, 0xd0}};

enum own_detail_index : size_t { kVideoTracks, kAudioTracks, kSubtitles, kChapters };

constexpr own_detail kOwnDetailList[] = {
    {{kOwnDetails, 2}, L"Skyggn.Media.VideoTracks", IDS_DETAIL_VIDEO_TRACKS, true},
    {{kOwnDetails, 3}, L"Skyggn.Media.AudioTracks", IDS_DETAIL_AUDIO_TRACKS, true},
    {{kOwnDetails, 4}, L"Skyggn.Media.SubtitleTracks", IDS_DETAIL_SUBTITLES, true},
    {{kOwnDetails, 5}, L"Skyggn.Media.ChapterCount", IDS_DETAIL_CHAPTERS, false},
};

HRESULT set_number(IPropertyStore* store, REFPROPERTYKEY key, int64_t value) {
    if (value <= 0) {
        return S_OK;  // unknown
    }
    wil::unique_prop_variant variant;
    RETURN_IF_FAILED(InitPropVariantFromUInt32(static_cast<ULONG>(std::min<int64_t>(value, UINT32_MAX)), &variant));
    return store->SetValue(key, variant);
}

HRESULT set_text(IPropertyStore* store, REFPROPERTYKEY key, const std::wstring& text) {
    if (text.empty()) {
        return S_OK;
    }
    wil::unique_prop_variant variant;
    RETURN_IF_FAILED(InitPropVariantFromString(text.c_str(), &variant));
    return store->SetValue(key, variant);
}

// a tag that can hold several values (artists, genres), separated by semicolons as windows does
HRESULT set_list(IPropertyStore* store, REFPROPERTYKEY key, const std::wstring& text) {
    if (text.empty()) {
        return S_OK;
    }
    wil::unique_prop_variant variant;
    RETURN_IF_FAILED(InitPropVariantFromStringAsVector(text.c_str(), &variant));
    return store->SetValue(key, variant);
}

// a tag of the file or, for formats that keep tags per stream (ogg, opus), of its main stream; the
// first of `names` that is set
std::wstring tag(const AVFormatContext* format, const AVStream* main, std::initializer_list<const char*> names) {
    for (const char* name : names) {
        for (const AVDictionary* tags : {format->metadata, main ? main->metadata : nullptr}) {
            if (const AVDictionaryEntry* entry = av_dict_get(tags, name, nullptr, 0)) {
                if (std::wstring text = from_utf8(entry->value); !text.empty()) {
                    return text;
                }
            }
        }
    }
    return {};
}

// the number a tag starts with: "3/12" is track 3, "2021-05-01" the year 2021
int64_t leading_number(const std::wstring& text) {
    int64_t value = 0;
    for (wchar_t c : text) {
        if (c < L'0' || c > L'9' || value > 1'000'000) {
            break;
        }
        value = value * 10 + (c - L'0');
    }
    return value;
}

// ffmpeg's short codec name in capitals: "HEVC", "AV1", "OPUS"
std::wstring codec_name(AVCodecID id) {
    const AVCodecDescriptor* descriptor = avcodec_descriptor_get(id);
    if (!descriptor) {
        return {};
    }
    std::wstring name = from_utf8(descriptor->name);
    std::ranges::transform(name, name.begin(), [](wchar_t c) { return static_cast<wchar_t>(towupper(c)); });
    return name;
}

// bits per second over the whole file. ffmpeg estimates it only while probing, which is skipped when
// the headers say everything (matroska), so it is worked out from the size and length here.
int64_t overall_bitrate(const AVFormatContext* format) {
    if (format->bit_rate > 0) {
        return format->bit_rate;
    }
    const int64_t size = format->pb ? avio_size(format->pb) : -1;
    if (size <= 0 || format->duration == AV_NOPTS_VALUE || format->duration <= 0) {
        return 0;
    }
    return static_cast<int64_t>(static_cast<double>(size) * 8 * AV_TIME_BASE / static_cast<double>(format->duration));
}

std::wstring tag_of(const AVStream* stream, const char* name) {
    const AVDictionaryEntry* entry = av_dict_get(stream->metadata, name, nullptr, 0);
    return entry ? from_utf8(entry->value) : std::wstring();
}

// " [eng] Commentary": a track's language and title, as matroska and mp4 tag them. semicolons
// would split the track in two in windows' list of values.
std::wstring language_and_title(const AVStream* stream) {
    std::wstring text;
    if (std::wstring language = tag_of(stream, "language"); !language.empty() && language != L"und") {
        text += L" [" + language + L"]";
    }
    if (std::wstring title = tag_of(stream, "title"); !title.empty()) {
        std::ranges::replace(title, L';', L',');
        text += L" " + title;
    }
    return text;
}

// "stereo", "5.1": ffmpeg's name for the layout, without the variant ("5.1(side)")
std::wstring channels_of(const AVChannelLayout& layout) {
    if (layout.nb_channels == 1) {
        return L"mono";
    }
    if (layout.nb_channels == 2) {
        return L"stereo";
    }
    char name[64] = {};
    if (av_channel_layout_describe(&layout, name, sizeof(name)) < 0) {
        return std::to_wstring(layout.nb_channels) + L" ch";
    }
    std::wstring text = from_utf8(name);
    return text.substr(0, text.find(L'('));
}

// subtitle formats by the names people know them by
std::wstring subtitle_codec(AVCodecID id) {
    switch (id) {
    case AV_CODEC_ID_SUBRIP:
        return L"SRT";
    case AV_CODEC_ID_ASS:
        return L"ASS";
    case AV_CODEC_ID_SSA:
        return L"SSA";
    case AV_CODEC_ID_WEBVTT:
        return L"WebVTT";
    case AV_CODEC_ID_HDMV_PGS_SUBTITLE:
        return L"PGS";
    case AV_CODEC_ID_DVD_SUBTITLE:
        return L"VobSub";
    case AV_CODEC_ID_DVB_SUBTITLE:
        return L"DVB";
    default:
        return codec_name(id);
    }
}

// every track of each kind, as windows' list of values: "AAC stereo [eng];AC3 5.1 [ger] Commentary"
HRESULT store_tracks(const AVFormatContext* format, IPropertyStore* store) {
    std::wstring video;
    std::wstring audio;
    std::wstring subtitles;
    auto add = [](std::wstring& list, const std::wstring& track) { list += (list.empty() ? L"" : L";") + track; };
    for (unsigned i = 0; i < format->nb_streams; ++i) {
        const AVStream* stream = format->streams[i];
        const AVCodecParameters* parameters = stream->codecpar;
        if (stream->disposition & (AV_DISPOSITION_ATTACHED_PIC | AV_DISPOSITION_DEPENDENT)) {
            continue;
        }
        switch (parameters->codec_type) {
        case AVMEDIA_TYPE_VIDEO: {
            std::wstring track = codec_name(parameters->codec_id);
            if (parameters->width > 0 && parameters->height > 0) {
                track += L" " + std::to_wstring(parameters->width) + L"\u00d7" + std::to_wstring(parameters->height);
            }
            add(video, track + language_and_title(stream));
            break;
        }
        case AVMEDIA_TYPE_AUDIO:
            add(audio, codec_name(parameters->codec_id) + L" " + channels_of(parameters->ch_layout) +
                           language_and_title(stream));
            break;
        case AVMEDIA_TYPE_SUBTITLE: {
            std::wstring track = subtitle_codec(parameters->codec_id) + language_and_title(stream);
            if (stream->disposition & AV_DISPOSITION_FORCED) {
                track += L" " + loaded_string(IDS_DETAIL_FORCED);
            }
            add(subtitles, track);
            break;
        }
        default:
            break;
        }
    }
    RETURN_IF_FAILED(set_list(store, kOwnDetailList[kVideoTracks].key, video));
    RETURN_IF_FAILED(set_list(store, kOwnDetailList[kAudioTracks].key, audio));
    RETURN_IF_FAILED(set_list(store, kOwnDetailList[kSubtitles].key, subtitles));
    return set_number(store, kOwnDetailList[kChapters].key, format->nb_chapters);
}

std::string to_utf8(const std::wstring& text) {
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0,
                                          nullptr, nullptr);
    std::string narrow(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), narrow.data(), length, nullptr,
                        nullptr);
    return narrow;
}

std::string xml_escaped(const std::string& text) {
    std::string escaped;
    for (char c : text) {
        switch (c) {
        case '&':
            escaped += "&amp;";
            break;
        case '<':
            escaped += "&lt;";
            break;
        case '>':
            escaped += "&gt;";
            break;
        case '"':
            escaped += "&quot;";
            break;
        default:
            escaped += c;
        }
    }
    return escaped;
}

HRESULT store_details(const AVFormatContext* format, IPropertyStore* store) {
    const int video_index = find_video_stream(format);
    const int audio_index =
        av_find_best_stream(const_cast<AVFormatContext*>(format), AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    const AVStream* video = video_index >= 0 ? format->streams[video_index] : nullptr;
    const AVStream* audio = audio_index >= 0 ? format->streams[audio_index] : nullptr;

    if (format->duration != AV_NOPTS_VALUE && format->duration > 0) {
        wil::unique_prop_variant duration;  // in 100 ns units; ffmpeg counts microseconds
        RETURN_IF_FAILED(InitPropVariantFromUInt64(static_cast<ULONGLONG>(format->duration) * 10, &duration));
        RETURN_IF_FAILED(store->SetValue(PKEY_Media_Duration, duration));
    }
    if (video) {
        const AVCodecParameters* parameters = video->codecpar;
        RETURN_IF_FAILED(set_number(store, PKEY_Video_FrameWidth, parameters->width));
        RETURN_IF_FAILED(set_number(store, PKEY_Video_FrameHeight, parameters->height));
        const AVRational rate = video->avg_frame_rate.num > 0 && video->avg_frame_rate.den > 0 ? video->avg_frame_rate
                                                                                                : video->r_frame_rate;
        if (rate.num > 0 && rate.den > 0) {
            // frames per 1000 seconds
            RETURN_IF_FAILED(set_number(store, PKEY_Video_FrameRate, std::llround(av_q2d(rate) * 1000)));
        }
        RETURN_IF_FAILED(set_number(store, PKEY_Video_EncodingBitrate, parameters->bit_rate));
        RETURN_IF_FAILED(set_number(store, PKEY_Video_TotalBitrate, overall_bitrate(format)));
        RETURN_IF_FAILED(set_text(store, PKEY_Video_Compression, codec_name(parameters->codec_id)));
    }
    if (audio) {
        const AVCodecParameters* parameters = audio->codecpar;
        RETURN_IF_FAILED(set_number(store, PKEY_Audio_ChannelCount, parameters->ch_layout.nb_channels));
        RETURN_IF_FAILED(set_number(store, PKEY_Audio_SampleRate, parameters->sample_rate));
        // a song file's own bit rate is its audio's when the stream does not state one
        RETURN_IF_FAILED(set_number(store, PKEY_Audio_EncodingBitrate,
                                    parameters->bit_rate > 0 ? parameters->bit_rate : (video ? 0 : overall_bitrate(format))));
        // only lossless audio has a sample size; a lossy codec's is that of its decoder's output
        if (const AVCodecDescriptor* codec = avcodec_descriptor_get(parameters->codec_id);
            codec && (codec->props & AV_CODEC_PROP_LOSSLESS)) {
            RETURN_IF_FAILED(set_number(store, PKEY_Audio_SampleSize,
                                        parameters->bits_per_raw_sample > 0 ? parameters->bits_per_raw_sample
                                                                            : parameters->bits_per_coded_sample));
        }
        RETURN_IF_FAILED(set_text(store, PKEY_Audio_Format, codec_name(parameters->codec_id)));
    }

    const AVStream* main = audio ? audio : video;
    RETURN_IF_FAILED(set_text(store, PKEY_Title, tag(format, main, {"title"})));
    RETURN_IF_FAILED(set_list(store, PKEY_Music_Artist, tag(format, main, {"artist"})));
    RETURN_IF_FAILED(set_text(store, PKEY_Music_AlbumArtist, tag(format, main, {"album_artist"})));
    RETURN_IF_FAILED(set_text(store, PKEY_Music_AlbumTitle, tag(format, main, {"album"})));
    RETURN_IF_FAILED(set_list(store, PKEY_Music_Genre, tag(format, main, {"genre"})));
    RETURN_IF_FAILED(set_list(store, PKEY_Music_Composer, tag(format, main, {"composer"})));
    RETURN_IF_FAILED(set_text(store, PKEY_Comment, tag(format, main, {"comment", "description"})));
    RETURN_IF_FAILED(set_text(store, PKEY_Copyright, tag(format, main, {"copyright"})));
    RETURN_IF_FAILED(set_number(store, PKEY_Music_TrackNumber, leading_number(tag(format, main, {"track"}))));
    const int64_t year = leading_number(tag(format, main, {"date", "year", "date_released", "date_recorded"}));
    if (year >= 1000 && year <= 9999) {
        RETURN_IF_FAILED(set_number(store, PKEY_Media_Year, year));
    }
    if (video) {
        RETURN_IF_FAILED(set_list(store, PKEY_Video_Director, tag(format, main, {"director"})));
    }
    return store_tracks(format, store);
}

}  // namespace

std::span<const own_detail> own_details() {
    return kOwnDetailList;
}

std::string details_schema(const std::wstring& engine_path) {
    wchar_t format_id[39];
    StringFromGUID2(kOwnDetails, format_id, ARRAYSIZE(format_id));
    std::string xml =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        "<!-- skyggn's own file details, written by its registration -->\n"
        "<schema xmlns=\"http://schemas.microsoft.com/windows/2006/propertydescription\" schemaVersion=\"1.0\">\n"
        "  <propertyDescriptionList publisher=\"skyggn\" product=\"skyggn\">\n";
    for (const own_detail& detail : kOwnDetailList) {
        // innate: worked out from the file, so explorer offers no editing
        xml += "    <propertyDescription name=\"" + to_utf8(detail.name) + "\" formatID=\"" + to_utf8(format_id) +
               "\" propID=\"" + std::to_string(detail.key.pid) + "\">\n"
               "      <searchInfo inInvertedIndex=\"false\" isColumn=\"false\"/>\n"
               "      <typeInfo type=\"" + (detail.list ? "String\" multipleValues=\"true" : "UInt32\" multipleValues=\"false") +
               "\" isViewable=\"true\" isInnate=\"true\"/>\n"
               "      <labelInfo label=\"" + xml_escaped("@" + to_utf8(engine_path) + ",-" + std::to_string(detail.label)) + "\"/>\n"
               "      <displayInfo displayType=\"" + (detail.list ? "String\" defaultColumnWidth=\"30" : "Number\" defaultColumnWidth=\"10") +
               "\"/>\n"
               "    </propertyDescription>\n";
    }
    xml += "  </propertyDescriptionList>\n</schema>\n";
    return xml;
}

HRESULT read_details(IStream* stream, std::wstring_view extension, const deadline& limit, IPropertyStore* store) {
    RETURN_IF_FAILED(load_ffmpeg());
    io_ptr io;  // declared before format: the format context reads through it until closed
    format_ptr format;
    // the limit is copied: the format context's interrupt callback holds a pointer to it
    deadline io_limit = limit;
    RETURN_IF_FAILED_EXPECTED(open_input(stream, name_hint(extension), nullptr, &io_limit, io, format));
    if (needs_probe(format.get())) {
        // a probe that fails (a damaged file, the time limit) leaves what the headers said, which is
        // still worth showing; the details it could not find stay empty
        avformat_find_stream_info(format.get(), nullptr);
    }
    return store_details(format.get(), store);
}

}  // namespace skyggn
