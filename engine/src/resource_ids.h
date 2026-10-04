#pragma once

// rcdata resources embedded in skyggn-engine.dll (resources.rc)
#define IDR_BADGE_VIDEO 101
#define IDR_BADGE_AUDIO 102
#define IDR_BADGE_IMAGE 103
#define IDR_BADGE_BOOK 104
#define IDR_BADGE_DOCUMENT 105

// string table (resources.rc), in english and german: the labels of skyggn's own file details,
// which windows loads in the user's language
#define IDS_DETAIL_VIDEO_TRACKS 201
#define IDS_DETAIL_AUDIO_TRACKS 202
#define IDS_DETAIL_SUBTITLES 203
#define IDS_DETAIL_CHAPTERS 204
// a forced subtitle track: shown even when subtitles are off, for foreign-language lines
#define IDS_DETAIL_FORCED 205

// what a tile without a picture says under its file type when the file is proven damaged
// (skyggn_damage), in windows' display language
#define IDS_DAMAGE_EMPTY 211
#define IDS_DAMAGE_INCOMPLETE 212
#define IDS_DAMAGE_CORRUPTED 213
