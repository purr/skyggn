#pragma once

#include <skyggn/skyggn.h>

#include <span>
#include <string_view>

namespace skyggn {

// how the badge looks and where it sits; see badge.h
struct badge_look {
    skyggn_badge_style style;
    skyggn_badge_corner corner;
    DWORD size_percent;  // the badge's height, in percent of the thumbnail
};

struct settings {
    DWORD frame_position;  // percent of the duration where the frame is taken
    bool prefer_cover_art;  // use embedded cover art over a video frame when a file has both
    bool skip_black_frames;
    bool low_impact;  // gentle mode: one decoding thread, at the priority windows gives the call
    DWORD time_limit_ms;  // per file; past it the file keeps its normal icon
    badge_look badge;
    skyggn_placeholder placeholder;
};

std::span<const skyggn_setting> setting_definitions();
const skyggn_setting* find_setting(std::wstring_view name);
// the setting's description in `language` ("de-DE"): german or english
const wchar_t* describe(const skyggn_setting& setting, std::wstring_view language);
DWORD read_setting(const skyggn_setting& setting);
HRESULT write_setting(const skyggn_setting& setting, DWORD value);
settings load_settings();

}  // namespace skyggn
