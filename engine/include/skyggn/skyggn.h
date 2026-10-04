// skyggn engine: the c api used by skyggnctl, the settings app and the installer.
#pragma once

#include <windows.h>
#include <propsys.h>

#ifdef SKYGGN_BUILDING
#define SKYGGN_API extern "C" __declspec(dllexport)
#else
#define SKYGGN_API extern "C" __declspec(dllimport)
#endif

// where a registration lives: the current user only, or every user of the pc (needs admin).
enum skyggn_scope : int {
    SKYGGN_SCOPE_USER = 0,
    SKYGGN_SCOPE_MACHINE = 1,
};

enum skyggn_category : int {
    SKYGGN_CATEGORY_VIDEO = 0,
    SKYGGN_CATEGORY_AUDIO = 1,  // thumbnail is the embedded cover art
    SKYGGN_CATEGORY_IMAGE = 2,
    SKYGGN_CATEGORY_RAW = 3,  // camera raw photo; thumbnail is the camera's preview, or the photo developed
    SKYGGN_CATEGORY_BOOK = 4,  // comic book archive or e-book; thumbnail is its cover
    SKYGGN_CATEGORY_DOCUMENT = 5,  // pdf or opendocument file; thumbnail is its first page
};

// values of the BadgeStyle setting: the mark in a thumbnail's corner
enum skyggn_badge_style : DWORD {
    SKYGGN_BADGE_NONE = 0,
    SKYGGN_BADGE_FROSTED = 1,   // dark glass chip, white symbol
    SKYGGN_BADGE_TINTED = 2,    // chip in the category's colour
    SKYGGN_BADGE_LABELLED = 3,  // glass pill with symbol and extension
};

// values of the BadgeCorner setting: the picture's corner the badge sits on. the BadgeSize setting is
// its height in percent of the thumbnail.
enum skyggn_badge_corner : DWORD {
    SKYGGN_CORNER_BOTTOM_RIGHT = 0,
    SKYGGN_CORNER_BOTTOM_LEFT = 1,
    SKYGGN_CORNER_TOP_RIGHT = 2,
    SKYGGN_CORNER_TOP_LEFT = 3,
};

struct skyggn_format {
    const wchar_t* extension;  // lowercase, with the dot: L".mkv"
    skyggn_category category;
    BOOL recommended;  // turned on by a default install
};

// which thumbnail handler windows uses for a file type
enum skyggn_handler : int {
    SKYGGN_HANDLER_NONE = 0,    // none: windows shows the file type icon
    SKYGGN_HANDLER_SKYGGN = 1,
    SKYGGN_HANDLER_OTHER = 2,   // another program's handler, which still exists
    SKYGGN_HANDLER_DEAD = 3,    // registered, but its program is gone
};

// values of the Placeholder setting: what a file without a picture shows (a song without cover art,
// a damaged video)
enum skyggn_placeholder : DWORD {
    SKYGGN_PLACEHOLDER_NONE = 0,      // windows' usual icon for the file type
    SKYGGN_PLACEHOLDER_KIND = 1,      // a tile with a big symbol, in its kind's colour
    SKYGGN_PLACEHOLDER_PER_FILE = 2,  // the same, in a colour from its contents (copies and an album's songs share it)
};

// values of the Language setting: the language of the settings app
enum skyggn_language : DWORD {
    SKYGGN_LANGUAGE_WINDOWS = 0,  // the one windows uses, where the app has it, else english
    SKYGGN_LANGUAGE_ENGLISH = 1,
    SKYGGN_LANGUAGE_GERMAN = 2,
};

// what skyggn proved wrong with a file from its own structure (see skyggn_check). its thumbnail
// shows it: a tile without a picture names it under the file type, a picture gets a warning mark
// on its badge's corner.
enum skyggn_damage : DWORD {
    SKYGGN_DAMAGE_NONE = 0,        // nothing proven; the file may still be unreadable
    SKYGGN_DAMAGE_EMPTY = 1,       // no bytes, or nothing but zero bytes
    SKYGGN_DAMAGE_INCOMPLETE = 2,  // ends before its own structure says it does, or lacks a part every finished file has
    SKYGGN_DAMAGE_CORRUPTED = 3,   // a checksum over its header fails, or it points past its own end
};

struct skyggn_setting {
    const wchar_t* name;
    const wchar_t* description;  // plain words, for people who never changed a setting before; english
    DWORD default_value;
    DWORD min_value;
    DWORD max_value;
};

SKYGGN_API const wchar_t* skyggn_version();

SKYGGN_API UINT skyggn_format_count();
SKYGGN_API const skyggn_format* skyggn_format_at(UINT index);

// settings are per user and apply to the next thumbnail made
SKYGGN_API UINT skyggn_setting_count();
SKYGGN_API const skyggn_setting* skyggn_setting_at(UINT index);
// a setting's description in `language`, a language tag such as "de-DE": german for german, english
// for every language skyggn has no translation for. null for a name that is not a setting.
SKYGGN_API const wchar_t* skyggn_setting_description(const wchar_t* name, const wchar_t* language);
SKYGGN_API DWORD skyggn_setting_get(const wchar_t* name);
SKYGGN_API HRESULT skyggn_setting_set(const wchar_t* name, DWORD value);

// makes a thumbnail in the calling process with the current settings: a 32-bit bitmap with
// premultiplied alpha, transparent around a badge. the caller owns the bitmap.
SKYGGN_API HRESULT skyggn_thumbnail(const wchar_t* path, UINT size, HBITMAP* bitmap);
// the same, as top-down premultiplied bgra pixels: `capacity` is the size of `pixels` in pixels;
// size * size is always enough.
SKYGGN_API HRESULT skyggn_thumbnail_pixels(const wchar_t* path, UINT size, UINT32* pixels, UINT capacity,
                                           UINT* width, UINT* height);
// what damage a file's own structure proves, checked as for its thumbnail
SKYGGN_API HRESULT skyggn_check(const wchar_t* path, skyggn_damage* damage);
// reads a media file's details (length, frame size, artist...) into a store the caller releases: in
// this process, or with `isolated` the way explorer does, through the registered handler, which
// reads in a separate process. the calling thread must have com.
SKYGGN_API HRESULT skyggn_details(const wchar_t* path, BOOL isolated, IPropertyStore** store);

SKYGGN_API BOOL skyggn_is_registered(skyggn_scope scope);
// registers the engine and turns on the recommended file types: what a fresh install does. over an
// existing install (an update) it keeps the file types that are on, and brings their registration
// up to date.
SKYGGN_API HRESULT skyggn_install(skyggn_scope scope);
SKYGGN_API HRESULT skyggn_register_server(skyggn_scope scope);
// hands every file type back to its previous handler, then removes the engine's registration.
SKYGGN_API HRESULT skyggn_unregister_server(skyggn_scope scope);
SKYGGN_API HRESULT skyggn_set_handled(skyggn_scope scope, const wchar_t* extension, BOOL handled);
SKYGGN_API BOOL skyggn_is_handled(skyggn_scope scope, const wchar_t* extension);
SKYGGN_API skyggn_handler skyggn_effective_handler(const wchar_t* extension);
// deletes thumbnail registrations in this scope whose program no longer exists.
SKYGGN_API HRESULT skyggn_repair(skyggn_scope scope, UINT* removed);
// counts what skyggn_repair would delete, without deleting it.
SKYGGN_API HRESULT skyggn_count_dead(skyggn_scope scope, UINT* count);
// tells explorer that thumbnail registrations changed. call once after a batch of changes.
SKYGGN_API void skyggn_notify_shell();
// windows reads some types' details only with its own handler, matroska among them (mkv, webm),
// where it shows little more than the length. TRUE sets those windows entries aside so skyggn's
// details serve the types; FALSE puts them back. every user of the pc; needs admin.
SKYGGN_API HRESULT skyggn_take_system_details(BOOL take);
// whether skyggn serves those types' details now (a windows update can put windows' back)
SKYGGN_API BOOL skyggn_system_details_taken();
