#include "settings.h"

#include "registry_paths.h"

#include <algorithm>

namespace skyggn {

namespace {

enum setting_index : size_t {
    kFramePosition,
    kPreferCoverArt,
    kSkipBlackFrames,
    kLowImpact,
    kTimeLimit,
    kBadgeStyle,
    kBadgeCorner,
    kBadgeSize,
    kPlaceholder,
    kLanguage,
    kSettingCount,
};

// the descriptions are what the settings app and skyggnctl show next to each setting
constexpr skyggn_setting kSettings[] = {
    {L"FramePosition",
     L"How far into a video its picture is taken, in percent: 0 is the very start, 50 the middle.", 20, 0, 95},
    {L"PreferCoverArt",
     L"When a video file carries its own cover picture, like a movie poster, show that instead of a frame "
     L"from the video.",
     1, 0, 1},
    {L"SkipBlackFrames",
     L"If the chosen frame is black, white or empty, like a fade-in or a title on black, look a little later "
     L"in the video for a better one.",
     1, 0, 1},
    {L"LowImpact",
     L"Make each thumbnail on one processor core, so games and other programs keep all the others. Thumbnails "
     L"of very large videos and photos take a moment longer.",
     1, 0, 1},
    {L"TimeLimitMs",
     L"The longest time spent on one file. A file that takes longer is shown as if it had no picture, so "
     L"nothing can hang.",
     5000, 500, 30000},
    {L"BadgeStyle", L"The small mark in the corner of each thumbnail that shows what kind of file it is.",
     SKYGGN_BADGE_FROSTED, SKYGGN_BADGE_NONE, SKYGGN_BADGE_LABELLED},
    {L"BadgeCorner", L"Which corner of the picture the badge sits on.", SKYGGN_CORNER_BOTTOM_RIGHT,
     SKYGGN_CORNER_BOTTOM_RIGHT, SKYGGN_CORNER_TOP_LEFT},
    {L"BadgeSize",
     L"How big the badge is, in percent of the thumbnail. It is the same size on every file, so a bigger badge "
     L"leaves a little less room for the picture.",
     24, 16, 32},
    {L"Placeholder",
     L"What a file without a picture shows, like a song without cover art or a damaged video: Windows' usual "
     L"icon, or a tile with a big symbol.",
     SKYGGN_PLACEHOLDER_PER_FILE, SKYGGN_PLACEHOLDER_NONE, SKYGGN_PLACEHOLDER_PER_FILE},
    {L"Language", L"The language of the skyggn app: the one Windows uses, English or German.",
     SKYGGN_LANGUAGE_WINDOWS, SKYGGN_LANGUAGE_WINDOWS, SKYGGN_LANGUAGE_GERMAN},
};
static_assert(std::size(kSettings) == kSettingCount);

// the same descriptions in german, in the same order
constexpr const wchar_t* kGermanDescriptions[] = {
    L"Wie weit im Video das Bild entnommen wird, in Prozent: 0 ist der Anfang, 50 die Mitte.",
    L"Bringt eine Videodatei ein eigenes Titelbild mit, etwa ein Filmplakat, wird dieses statt eines Bildes "
    L"aus dem Video gezeigt.",
    L"Ist das gewählte Bild schwarz, weiß oder leer, etwa bei einer Einblendung oder einem Titel auf Schwarz, "
    L"wird etwas später im Video nach einem besseren gesucht.",
    L"Jede Miniaturansicht auf einem einzigen Prozessorkern erstellen, damit Spielen und anderen Programmen alle "
    L"übrigen Kerne bleiben. Bei sehr großen Videos und Fotos dauert das Erstellen dafür etwas länger.",
    L"Die längste Zeit für eine Datei. Braucht eine Datei länger, wird sie wie eine Datei ohne Bild angezeigt, "
    L"damit nichts hängen bleibt.",
    L"Das kleine Zeichen in der Ecke jeder Miniaturansicht, das zeigt, um welche Art von Datei es sich handelt.",
    L"In welcher Ecke des Bildes das Badge sitzt.",
    L"Wie groß das Badge ist, in Prozent der Miniaturansicht. Es ist bei jeder Datei gleich groß; ein größeres "
    L"Badge lässt dem Bild etwas weniger Platz.",
    L"Was eine Datei ohne Bild zeigt, etwa ein Song ohne Cover oder ein beschädigtes Video: das übliche Symbol "
    L"von Windows oder eine Kachel mit großem Symbol.",
    L"Die Sprache der skyggn-App: die von Windows, Englisch oder Deutsch.",
};
static_assert(std::size(kGermanDescriptions) == kSettingCount);

}  // namespace

std::span<const skyggn_setting> setting_definitions() {
    return kSettings;
}

const skyggn_setting* find_setting(std::wstring_view name) {
    auto it = std::ranges::find_if(kSettings, [&](const skyggn_setting& s) {
        return CompareStringOrdinal(s.name, -1, name.data(), static_cast<int>(name.size()), TRUE) == CSTR_EQUAL;
    });
    return it == std::end(kSettings) ? nullptr : &*it;
}

const wchar_t* describe(const skyggn_setting& setting, std::wstring_view language) {
    // "de" and its regional forms: "de-DE", "de-AT", "de-CH"
    const bool german = language == L"de" || language.starts_with(L"de-");
    return german ? kGermanDescriptions[&setting - kSettings] : setting.description;
}

DWORD read_setting(const skyggn_setting& setting) {
    DWORD value = 0;
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, kProductKey, setting.name, RRF_RT_REG_DWORD, nullptr, &value, &size) !=
        ERROR_SUCCESS) {
        return setting.default_value;
    }
    return std::clamp(value, setting.min_value, setting.max_value);
}

HRESULT write_setting(const skyggn_setting& setting, DWORD value) {
    if (value < setting.min_value || value > setting.max_value) {
        return E_INVALIDARG;
    }
    return HRESULT_FROM_WIN32(
        RegSetKeyValueW(HKEY_CURRENT_USER, kProductKey, setting.name, REG_DWORD, &value, sizeof(value)));
}

settings load_settings() {
    return {
        .frame_position = read_setting(kSettings[kFramePosition]),
        .prefer_cover_art = read_setting(kSettings[kPreferCoverArt]) != 0,
        .skip_black_frames = read_setting(kSettings[kSkipBlackFrames]) != 0,
        .low_impact = read_setting(kSettings[kLowImpact]) != 0,
        .time_limit_ms = read_setting(kSettings[kTimeLimit]),
        .badge = {.style = static_cast<skyggn_badge_style>(read_setting(kSettings[kBadgeStyle])),
                  .corner = static_cast<skyggn_badge_corner>(read_setting(kSettings[kBadgeCorner])),
                  .size_percent = read_setting(kSettings[kBadgeSize])},
        .placeholder = static_cast<skyggn_placeholder>(read_setting(kSettings[kPlaceholder])),
    };
}

}  // namespace skyggn
