// file type registrations. taking over a file type records the handler it had before; giving it
// back restores that handler, unless its program is gone. a gone program's entry is removed
// instead, so skyggn never leaves dead entries behind.

#include "registration.h"

#include "details.h"
#include "ffmpeg_loader.h"
#include "formats.h"
#include "properties.h"
#include "provider.h"
#include "registry_paths.h"

#include <propsys.h>
#include <shlwapi.h>
#include <wil/com.h>
#include <wil/resource.h>
#include <wil/result.h>
#include <wil/stl.h>
#include <wil/win32_helpers.h>

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

namespace skyggn {

namespace {

constexpr wchar_t kServerName[] = L"skyggn thumbnail provider";
constexpr wchar_t kDetailsName[] = L"skyggn details";
constexpr wchar_t kReaderName[] = L"skyggn details reader";

HKEY root_of(skyggn_scope scope) {
    return scope == SKYGGN_SCOPE_MACHINE ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
}

template <typename Class>
std::wstring id_of() {
    wchar_t text[39];
    StringFromGUID2(__uuidof(Class), text, ARRAYSIZE(text));
    return text;
}

std::wstring server_id() {
    return id_of<ThumbnailProvider>();
}

bool same_id(std::wstring_view a, std::wstring_view b) {
    return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) ==
           CSTR_EQUAL;
}

std::wstring handler_slot(std::wstring_view extension) {
    return std::wstring(kClassesKey) + L"\\" + std::wstring(extension) + L"\\ShellEx\\" + kThumbnailHandlerId;
}

std::wstring server_key(std::wstring_view id) {
    return std::wstring(kClassesKey) + L"\\CLSID\\" + std::wstring(id);
}

std::optional<std::wstring> read_string(HKEY root, const std::wstring& key, const wchar_t* name) {
    constexpr DWORD flags = RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND;
    DWORD size = 0;
    if (RegGetValueW(root, key.c_str(), name, flags, nullptr, nullptr, &size) != ERROR_SUCCESS) {
        return std::nullopt;
    }
    std::wstring text(size / sizeof(wchar_t), L'\0');
    if (RegGetValueW(root, key.c_str(), name, flags, nullptr, text.data(), &size) != ERROR_SUCCESS) {
        return std::nullopt;
    }
    text.resize(wcsnlen(text.c_str(), text.size()));
    return text;
}

HRESULT write_string(HKEY root, const std::wstring& key, const wchar_t* name, const std::wstring& value) {
    const auto bytes = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
    return HRESULT_FROM_WIN32(RegSetKeyValueW(root, key.c_str(), name, REG_SZ, value.c_str(), bytes));
}

bool key_exists(HKEY root, const std::wstring& key) {
    wil::unique_hkey opened;
    return RegOpenKeyExW(root, key.c_str(), 0, KEY_READ, &opened) == ERROR_SUCCESS;
}

// deleting what is already gone is the goal, not an error
HRESULT ignore_missing(LSTATUS status) {
    return status == ERROR_FILE_NOT_FOUND ? S_OK : HRESULT_FROM_WIN32(status);
}

HRESULT delete_if_empty(HKEY root, const std::wstring& key) {
    wil::unique_hkey opened;
    if (RegOpenKeyExW(root, key.c_str(), 0, KEY_READ, &opened) != ERROR_SUCCESS) {
        return S_OK;
    }
    DWORD subkeys = 0;
    DWORD values = 0;
    RETURN_IF_WIN32_ERROR(RegQueryInfoKeyW(opened.get(), nullptr, nullptr, nullptr, &subkeys, nullptr, nullptr,
                                           &values, nullptr, nullptr, nullptr, nullptr));
    opened.reset();
    if (subkeys == 0 && values == 0) {
        return ignore_missing(RegDeleteKeyW(root, key.c_str()));
    }
    return S_OK;
}

// removes a file type's thumbnail handler slot, then the ShellEx and file type keys if that left
// them empty
HRESULT delete_slot(HKEY root, std::wstring_view extension) {
    const std::wstring type_key = std::wstring(kClassesKey) + L"\\" + std::wstring(extension);
    RETURN_IF_FAILED(ignore_missing(RegDeleteKeyW(root, handler_slot(extension).c_str())));
    RETURN_IF_FAILED(delete_if_empty(root, type_key + L"\\ShellEx"));
    return delete_if_empty(root, type_key);
}

// whether a path from a com registration points at an existing file. registrations may quote the
// path, add arguments (local servers), use environment variables, or name a dll on the search path.
bool program_exists(const std::wstring& registered) {
    std::wstring path = wil::ExpandEnvironmentStringsW<std::wstring>(registered.c_str());
    if (!path.empty() && path.front() == L'"') {
        path = path.substr(1, path.find(L'"', 1) - 1);
    }
    if (PathFileExistsW(path.c_str())) {
        return true;
    }
    std::wstring without_arguments = path;
    PathRemoveArgsW(without_arguments.data());
    without_arguments.resize(wcslen(without_arguments.c_str()));
    if (PathFileExistsW(without_arguments.c_str())) {
        return true;
    }
    if (PathIsRelativeW(path.c_str())) {
        wchar_t found[MAX_PATH];
        return SearchPathW(nullptr, path.c_str(), nullptr, ARRAYSIZE(found), found, nullptr) != 0;
    }
    return false;
}

// whether a com class can still be created. only a class that is provably gone counts as dead:
// unregistered, or registered to a file that no longer exists.
bool server_alive(const std::wstring& id) {
    // packaged (msix) apps register their classes in the package catalog, not under CLSID
    if (key_exists(HKEY_CLASSES_ROOT, L"PackagedCom\\ClassIndex\\" + id)) {
        return true;
    }
    const std::wstring key = L"CLSID\\" + id;
    if (auto path = read_string(HKEY_CLASSES_ROOT, key + L"\\InprocServer32", nullptr)) {
        return program_exists(*path);
    }
    if (auto path = read_string(HKEY_CLASSES_ROOT, key + L"\\LocalServer32", nullptr)) {
        return program_exists(*path);
    }
    return key_exists(HKEY_CLASSES_ROOT, key);
}

skyggn_handler classify(const std::wstring& id) {
    if (id.empty()) {
        return SKYGGN_HANDLER_NONE;
    }
    if (!server_alive(id)) {
        return SKYGGN_HANDLER_DEAD;
    }
    return same_id(id, server_id()) ? SKYGGN_HANDLER_SKYGGN : SKYGGN_HANDLER_OTHER;
}

// windows decorates thumbnails by file type: a film strip around videos and a border around photos
// (Treatment 3 and 2 on the video and image perceived types), and the default app's icon in the
// corner (TypeOverlay). skyggn's thumbnails carry their own badge with transparent room around the
// picture, where those decorations would land, so they are switched off for the types it handles.
// SystemFileAssociations\<ext> is read before the perceived type's defaults.
constexpr const wchar_t* kDecorations[] = {L"Treatment", L"TypeOverlay"};
constexpr DWORD kNoTreatment = 0;
constexpr wchar_t kNoTypeOverlay[] = L"";

std::wstring associations_key(std::wstring_view extension) {
    return std::wstring(kSystemAssociationsKey) + L"\\" + std::wstring(extension);
}

std::wstring decorations_backup(std::wstring_view extension) {
    return std::wstring(kDecorationsBackupKey) + L"\\" + std::wstring(extension);
}

// copies one registry value, whatever its type; a value that does not exist is not copied
HRESULT copy_value(HKEY root, const std::wstring& from, const std::wstring& to, const wchar_t* name) {
    DWORD type = 0;
    DWORD size = 0;
    if (RegGetValueW(root, from.c_str(), name, RRF_RT_ANY | RRF_NOEXPAND, &type, nullptr, &size) != ERROR_SUCCESS) {
        return S_FALSE;
    }
    std::vector<BYTE> data(size);
    RETURN_IF_WIN32_ERROR(RegGetValueW(root, from.c_str(), name, RRF_RT_ANY | RRF_NOEXPAND, &type, data.data(), &size));
    return HRESULT_FROM_WIN32(RegSetKeyValueW(root, to.c_str(), name, type, data.data(), size));
}

HRESULT switch_off_decorations(HKEY root, std::wstring_view extension) {
    const std::wstring key = associations_key(extension);
    const std::wstring backup = decorations_backup(extension);
    // what was there before is recorded once, the first time skyggn takes the type
    if (!key_exists(root, backup)) {
        wil::unique_hkey created;
        RETURN_IF_WIN32_ERROR(RegCreateKeyExW(root, backup.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &created, nullptr));
        for (const wchar_t* name : kDecorations) {
            RETURN_IF_FAILED(copy_value(root, key, backup, name));
        }
    }
    RETURN_IF_WIN32_ERROR(
        RegSetKeyValueW(root, key.c_str(), L"Treatment", REG_DWORD, &kNoTreatment, sizeof(kNoTreatment)));
    return HRESULT_FROM_WIN32(
        RegSetKeyValueW(root, key.c_str(), L"TypeOverlay", REG_SZ, kNoTypeOverlay, sizeof(kNoTypeOverlay)));
}

HRESULT restore_decorations(HKEY root, std::wstring_view extension) {
    const std::wstring key = associations_key(extension);
    const std::wstring backup = decorations_backup(extension);
    if (!key_exists(root, backup)) {
        return S_OK;  // skyggn never switched them off for this type
    }
    for (const wchar_t* name : kDecorations) {
        RETURN_IF_FAILED(ignore_missing(RegDeleteKeyValueW(root, key.c_str(), name)));
        RETURN_IF_FAILED(copy_value(root, backup, key, name));
    }
    RETURN_IF_FAILED(ignore_missing(RegDeleteTreeW(root, backup.c_str())));
    return delete_if_empty(root, key);
}

// the details explorer lists for a file type: its details tab (FullDetails) and details pane
// (PreviewDetails). windows' own lists for matroska video and audio, for types that have none:
// without them the details tab shows only the name, size and dates.
constexpr wchar_t kVideoFullDetails[] =
    L"prop:System.PropGroup.Description;System.Title;System.Media.SubTitle;System.Rating;System.Keywords;"
    L"System.Comment;System.PropGroup.Video;System.Media.Duration;System.Video.FrameWidth;System.Video.FrameHeight;"
    L"System.Video.EncodingBitrate;System.Video.TotalBitrate;System.Video.FrameRate;System.PropGroup.Audio;"
    L"System.Audio.EncodingBitrate;System.Audio.ChannelCount;System.Audio.SampleRate;System.PropGroup.Media;"
    L"System.Music.Artist;System.Media.Year;System.Music.Genre;System.PropGroup.Origin;System.Video.Director;"
    L"System.Copyright;System.PropGroup.FileSystem;System.ItemNameDisplay;System.ItemType;"
    L"System.ItemFolderPathDisplay;System.Size;System.DateCreated;System.DateModified;System.FileAttributes;"
    L"System.OfflineAvailability;System.OfflineStatus;System.SharedWith;System.FileOwner;System.ComputerName";
constexpr wchar_t kVideoPreviewDetails[] =
    L"prop:*System.Title;*System.Media.Duration;*System.Size;*System.Video.FrameWidth;*System.Video.FrameHeight;"
    L"System.Rating;*System.Keywords;*System.Comment;*System.Music.Artist;*System.Music.Genre;"
    L"*System.OfflineAvailability;*System.OfflineStatus;*System.DateModified;*System.DateCreated;*System.SharedWith;"
    L"*System.Media.Year;*System.Video.FrameRate;*System.Video.EncodingBitrate;*System.Video.TotalBitrate";
constexpr wchar_t kAudioFullDetails[] =
    L"prop:System.PropGroup.Description;System.Title;System.Media.SubTitle;System.Rating;System.Keywords;"
    L"System.Comment;System.PropGroup.Media;System.Music.Artist;System.Music.AlbumArtist;System.Music.AlbumTitle;"
    L"System.Media.Year;System.Music.TrackNumber;System.Music.Genre;System.Media.Duration;System.PropGroup.Audio;"
    L"System.Audio.EncodingBitrate;System.Audio.ChannelCount;System.Audio.SampleRate;System.Audio.SampleSize;"
    L"System.PropGroup.Origin;System.Copyright;System.PropGroup.Content;System.Music.Composer;"
    L"System.PropGroup.FileSystem;System.ItemNameDisplay;System.ItemType;System.ItemFolderPathDisplay;"
    L"System.DateCreated;System.DateModified;System.Size;System.FileAttributes;System.OfflineAvailability;"
    L"System.OfflineStatus;System.SharedWith;System.FileOwner;System.ComputerName";
constexpr wchar_t kAudioPreviewDetails[] =
    L"prop:System.Music.Artist;System.Music.AlbumTitle;System.Music.Genre;*System.Media.Duration;System.Rating;"
    L"System.Media.Year;*System.Size;System.Music.TrackNumber;System.Music.AlbumArtist;System.Title;"
    L"*System.Audio.EncodingBitrate;*System.DateModified;System.Keywords;*System.OfflineAvailability;"
    L"*System.OfflineStatus;*System.DateCreated;*System.SharedWith";

struct detail_list {
    const wchar_t* name;  // the value under SystemFileAssociations\<ext>
    const wchar_t* added;  // the backup's record that skyggn added the list
    const wchar_t* previous;  // the backup's copy of the list before skyggn added its own details
    const wchar_t* video;
    const wchar_t* audio;
    bool preview;  // the details pane's list, where a '*' shows a detail only when the file has it
};
constexpr detail_list kDetailLists[] = {
    {L"FullDetails", L"AddedFullDetails", L"PreviousFullDetails", kVideoFullDetails, kAudioFullDetails, false},
    {L"PreviewDetails", L"AddedPreviewDetails", L"PreviousPreviewDetails", kVideoPreviewDetails,
     kAudioPreviewDetails, true},
};

// a video type's detail list with skyggn's own details (tracks, chapters) after the frame rate, in
// the video group, or at the end of a list without one
std::wstring with_own_details(std::wstring list, bool preview) {
    if (list.find(own_details().front().name) != std::wstring::npos) {
        return list;
    }
    std::wstring names;
    for (const own_detail& detail : own_details()) {
        names += preview ? L";*" : L";";
        names += detail.name;
    }
    const std::wstring anchor = preview ? L"*System.Video.FrameRate" : L"System.Video.FrameRate";
    const size_t at = list.find(anchor);
    if (at == std::wstring::npos) {
        return list + names;
    }
    list.insert(at + anchor.size(), names);
    return list;
}

bool flag_set(HKEY root, const std::wstring& key, const wchar_t* name) {
    DWORD value = 0;
    DWORD size = sizeof(value);
    return RegGetValueW(root, key.c_str(), name, RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS &&
           value != 0;
}

std::wstring details_slot(std::wstring_view extension) {
    return std::wstring(kPropertyHandlersKey) + L"\\" + std::wstring(extension);
}

std::wstring details_backup(std::wstring_view extension) {
    return std::wstring(kDetailsBackupKey) + L"\\" + std::wstring(extension);
}

// explorer reads property handlers machine-wide only, so this is for machine installs. taking a
// type again (an update) brings its detail lists up to date and keeps the first record of them.
HRESULT take_details(std::wstring_view extension, skyggn_category category) {
    const HKEY root = HKEY_LOCAL_MACHINE;
    const std::wstring slot = details_slot(extension);
    const std::wstring backup = details_backup(extension);
    const std::wstring ours = id_of<PropertyHandler>();
    // the handler before skyggn's is recorded once, the first time skyggn takes the type
    if (!key_exists(root, backup)) {
        const std::optional<std::wstring> current = read_string(root, slot, nullptr);
        RETURN_IF_FAILED(
            write_string(root, backup, L"Handler", current && !same_id(*current, ours) ? *current : L""));
    }
    const bool video = category == SKYGGN_CATEGORY_VIDEO;
    const std::wstring associations = associations_key(extension);
    for (const detail_list& list : kDetailLists) {
        const std::optional<std::wstring> current = read_string(root, associations, list.name);
        std::wstring wanted = current ? *current : (video ? list.video : list.audio);
        if (video) {
            wanted = with_own_details(wanted, list.preview);
        }
        if (current == wanted) {
            continue;
        }
        if (!flag_set(root, backup, list.added) && !read_string(root, backup, list.previous)) {
            if (current) {
                RETURN_IF_FAILED(write_string(root, backup, list.previous, *current));
            } else {
                constexpr DWORD added = 1;
                RETURN_IF_WIN32_ERROR(
                    RegSetKeyValueW(root, backup.c_str(), list.added, REG_DWORD, &added, sizeof(added)));
            }
        }
        RETURN_IF_FAILED(write_string(root, associations, list.name, wanted));
    }
    return write_string(root, slot, nullptr, ours);
}

// windows reads some types' details with its own handler, whatever is registered (its
// SystemPropertyHandlers list, which only windows may change): skyggn's would never run for them.
// which types those are depends on the windows (an n edition without its media feature pack has
// no matroska handler), so the pc's own list decides.
bool windows_keeps_details(std::wstring_view extension) {
    return read_string(HKEY_LOCAL_MACHINE, kSystemPropertyHandlersKey, std::wstring(extension).c_str()).has_value();
}

HRESULT give_back_details(std::wstring_view extension) {
    const HKEY root = HKEY_LOCAL_MACHINE;
    const std::wstring backup = details_backup(extension);
    if (!key_exists(root, backup)) {
        return S_OK;  // skyggn never took this type's details
    }
    const std::wstring slot = details_slot(extension);
    const std::optional<std::wstring> current = read_string(root, slot, nullptr);
    if (current && same_id(*current, id_of<PropertyHandler>())) {
        const std::optional<std::wstring> previous = read_string(root, backup, L"Handler");
        if (previous && !previous->empty() && server_alive(*previous)) {
            RETURN_IF_FAILED(write_string(root, slot, nullptr, *previous));
        } else {
            // no handler before, or its program is gone: restoring it would leave a dead entry
            RETURN_IF_FAILED(ignore_missing(RegDeleteKeyW(root, slot.c_str())));
        }
    }
    const std::wstring associations = associations_key(extension);
    for (const detail_list& list : kDetailLists) {
        if (const std::optional<std::wstring> previous = read_string(root, backup, list.previous)) {
            RETURN_IF_FAILED(write_string(root, associations, list.name, *previous));
        } else if (flag_set(root, backup, list.added)) {
            RETURN_IF_FAILED(ignore_missing(RegDeleteKeyValueW(root, associations.c_str(), list.name)));
        }
    }
    RETURN_IF_FAILED(delete_if_empty(root, associations));
    return ignore_missing(RegDeleteTreeW(root, backup.c_str()));
}

HRESULT enable_privilege(HANDLE token, const wchar_t* name) {
    TOKEN_PRIVILEGES privileges{1};
    RETURN_IF_WIN32_BOOL_FALSE(LookupPrivilegeValueW(nullptr, name, &privileges.Privileges[0].Luid));
    privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    RETURN_IF_WIN32_BOOL_FALSE(AdjustTokenPrivileges(token, FALSE, &privileges, 0, nullptr, nullptr));
    // succeeds without the privilege too, saying so only here
    RETURN_LAST_ERROR_IF(GetLastError() == ERROR_NOT_ALL_ASSIGNED);
    return S_OK;
}

// windows' list of types it keeps the details of belongs to windows (TrustedInstaller); an
// administrator can only read it. opened for backup and restore, as backup programs open keys
// whatever their permissions, it can be changed without taking ownership or changing permissions.
// such a key ignores the access asked for: the backup privilege grants reading, the restore
// privilege writing, so both are needed.
HRESULT open_system_details(wil::unique_hkey& key) {
    wil::unique_handle token;
    RETURN_IF_WIN32_BOOL_FALSE(OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token));
    RETURN_IF_FAILED(enable_privilege(token.get(), SE_BACKUP_NAME));
    RETURN_IF_FAILED(enable_privilege(token.get(), SE_RESTORE_NAME));
    return HRESULT_FROM_WIN32(RegCreateKeyExW(HKEY_LOCAL_MACHINE, kSystemPropertyHandlersKey, 0, nullptr,
                                              REG_OPTION_BACKUP_RESTORE, KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, &key,
                                              nullptr));
}

// windows' entry for a type in its list; nullopt when it has none. any other failure to read is an
// error: taken for "none", the option would quietly do nothing.
HRESULT system_entry(HKEY system, const std::wstring& extension, std::optional<std::wstring>& out) {
    out.reset();
    constexpr DWORD flags = RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND;
    DWORD size = 0;
    const LSTATUS found = RegGetValueW(system, nullptr, extension.c_str(), flags, nullptr, nullptr, &size);
    if (found == ERROR_FILE_NOT_FOUND) {
        return S_OK;
    }
    RETURN_IF_WIN32_ERROR(found);
    std::wstring text(size / sizeof(wchar_t), L'\0');
    RETURN_IF_WIN32_ERROR(RegGetValueW(system, nullptr, extension.c_str(), flags, nullptr, text.data(), &size));
    text.resize(wcsnlen(text.c_str(), text.size()));
    out = std::move(text);
    return S_OK;
}

HRESULT register_class(HKEY root, const std::wstring& id, const wchar_t* name) {
    const std::wstring key = server_key(id);
    RETURN_IF_FAILED(write_string(root, key, nullptr, name));
    RETURN_IF_FAILED(write_string(root, key + L"\\InprocServer32", nullptr,
                                  wil::GetModuleFileNameW<std::wstring>(engine_module())));
    return write_string(root, key + L"\\InprocServer32", L"ThreadingModel", L"Apartment");
}

std::wstring app_key(const std::wstring& id) {
    return std::wstring(kClassesKey) + L"\\AppID\\" + id;
}

// windows learns the names and types of skyggn's own details (tracks, chapters) from a property
// schema file, which it reads from where it was registered: next to the engine. machine-wide only,
// like property handlers.
std::wstring schema_path() {
    std::wstring path = wil::GetModuleFileNameW<std::wstring>(engine_module());
    path.resize(path.find_last_of(L'\\') + 1);
    return path + L"skyggn.propdesc";
}

HRESULT register_schema() {
    const std::wstring path = schema_path();
    const std::string xml = details_schema(wil::GetModuleFileNameW<std::wstring>(engine_module()));
    {
        wil::unique_hfile file(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                           FILE_ATTRIBUTE_NORMAL, nullptr));
        RETURN_LAST_ERROR_IF(!file);
        DWORD written = 0;
        RETURN_IF_WIN32_BOOL_FALSE(
            WriteFile(file.get(), xml.data(), static_cast<DWORD>(xml.size()), &written, nullptr));
    }
    // besides S_OK, windows answers with success codes that report descriptions it turned down
    // (INPLACE_S_TRUNCATED), also when they are all registered: what counts is that each of
    // skyggn's details is known afterwards
    RETURN_IF_FAILED(PSRegisterPropertySchema(path.c_str()));
    RETURN_IF_FAILED(PSRefreshPropertySchema());
    for (const own_detail& detail : own_details()) {
        wil::com_ptr<IPropertyDescription> description;
        RETURN_IF_FAILED_MSG(PSGetPropertyDescriptionByName(detail.name, IID_PPV_ARGS(&description)),
                             "windows did not register %ls", detail.name);
    }
    return S_OK;
}

HRESULT unregister_schema() {
    const std::wstring path = schema_path();
    if (!PathFileExistsW(path.c_str())) {
        return S_OK;  // never registered
    }
    RETURN_IF_FAILED(PSUnregisterPropertySchema(path.c_str()));
    RETURN_IF_WIN32_BOOL_FALSE(DeleteFileW(path.c_str()));
    return S_OK;
}

}  // namespace

bool is_registered(skyggn_scope scope) {
    return key_exists(root_of(scope), server_key(server_id()) + L"\\InprocServer32");
}

HRESULT register_server(skyggn_scope scope) {
    const HKEY root = root_of(scope);
    const std::wstring id = server_id();
    RETURN_IF_FAILED(register_class(root, id, kServerName));
    // explorer's details come through PropertyHandler, which has PropertyReader do the reading. the
    // reader's AppID names com's own surrogate (dllhost.exe), so created as a local server it runs
    // there, outside explorer.
    const std::wstring details = id_of<PropertyHandler>();
    const std::wstring reader = id_of<PropertyReader>();
    RETURN_IF_FAILED(register_class(root, details, kDetailsName));
    RETURN_IF_FAILED(register_class(root, reader, kReaderName));
    RETURN_IF_FAILED(write_string(root, server_key(reader), L"AppID", reader));
    RETURN_IF_FAILED(write_string(root, app_key(reader), nullptr, kReaderName));
    RETURN_IF_FAILED(write_string(root, app_key(reader), L"DllSurrogate", L""));
    if (scope == SKYGGN_SCOPE_MACHINE) {
        // windows only loads approved shell extensions when the EnforceShellExtensionSecurity policy is on
        RETURN_IF_FAILED(write_string(root, kApprovedExtensionsKey, id.c_str(), kServerName));
        RETURN_IF_FAILED(write_string(root, kApprovedExtensionsKey, details.c_str(), kDetailsName));
        RETURN_IF_FAILED(register_schema());
    }
    return S_OK;
}

HRESULT install(skyggn_scope scope) {
    // an update installs over a working install: it keeps the file types the user chose, taking
    // each again so its registration is up to date, where turning on the recommended ones would
    // undo every type the user turned off. only a fresh install (no type handled) gets those.
    const auto handled = [&](const format_entry& entry) { return is_handled(scope, entry.format.extension); };
    const bool update = std::ranges::any_of(formats(), handled);
    RETURN_IF_FAILED(register_server(scope));
    for (const format_entry& entry : formats()) {
        if (update ? handled(entry) : entry.format.recommended) {
            RETURN_IF_FAILED(set_handled(scope, entry.format.extension, true));
        }
    }
    return S_OK;
}

HRESULT unregister_server(skyggn_scope scope) {
    const HKEY root = root_of(scope);
    if (scope == SKYGGN_SCOPE_MACHINE) {
        RETURN_IF_FAILED(take_system_details(false));
    }
    for (const format_entry& entry : formats()) {
        const skyggn_format& format = entry.format;
        RETURN_IF_FAILED(set_handled(scope, format.extension, false));
    }
    const std::wstring reader = id_of<PropertyReader>();
    for (const std::wstring& id : {server_id(), id_of<PropertyHandler>(), reader}) {
        RETURN_IF_FAILED(ignore_missing(RegDeleteTreeW(root, server_key(id).c_str())));
        if (scope == SKYGGN_SCOPE_MACHINE) {
            RETURN_IF_FAILED(ignore_missing(RegDeleteKeyValueW(root, kApprovedExtensionsKey, id.c_str())));
        }
    }
    RETURN_IF_FAILED(ignore_missing(RegDeleteTreeW(root, app_key(reader).c_str())));
    if (scope == SKYGGN_SCOPE_MACHINE) {
        RETURN_IF_FAILED(unregister_schema());
    }
    RETURN_IF_FAILED(delete_if_empty(root, kBackupKey));
    RETURN_IF_FAILED(delete_if_empty(root, kDecorationsBackupKey));
    RETURN_IF_FAILED(delete_if_empty(root, kDetailsBackupKey));
    // the user's settings keep the key alive in the user scope
    return delete_if_empty(root, kProductKey);
}

HRESULT set_handled(skyggn_scope scope, std::wstring_view requested, bool handled) {
    // only known media types can be taken over; this also normalises the spelling
    const format_entry* entry = find_format(requested);
    const skyggn_format* format = entry ? &entry->format : nullptr;
    RETURN_HR_IF(E_INVALIDARG, format == nullptr);
    const std::wstring extension = format->extension;
    const HKEY root = root_of(scope);
    const std::wstring slot = handler_slot(extension);
    const std::wstring ours = server_id();
    const std::optional<std::wstring> current = read_string(root, slot, nullptr);
    const bool ours_now = current && same_id(*current, ours);

    const bool details = scope == SKYGGN_SCOPE_MACHINE && entry->details;
    if (handled) {
        if (!ours_now) {
            RETURN_IF_FAILED(write_string(root, kBackupKey, extension.c_str(), current.value_or(L"")));
            RETURN_IF_FAILED(write_string(root, slot, nullptr, ours));
        }
        RETURN_IF_FAILED(switch_off_decorations(root, extension));
        if (!details) {
            return S_OK;
        }
        // where windows keeps the details, what an earlier version took is given back
        return windows_keeps_details(extension) ? give_back_details(extension)
                                                : take_details(extension, format->category);
    }

    if (details) {
        RETURN_IF_FAILED(give_back_details(extension));
    }
    RETURN_IF_FAILED(restore_decorations(root, extension));
    if (ours_now) {
        const std::optional<std::wstring> previous = read_string(root, kBackupKey, extension.c_str());
        if (previous && classify(*previous) == SKYGGN_HANDLER_OTHER) {
            RETURN_IF_FAILED(write_string(root, slot, nullptr, *previous));
        } else {
            // no handler before, or its program is gone: restoring it would leave a dead entry
            RETURN_IF_FAILED(delete_slot(root, extension));
        }
    }
    RETURN_IF_FAILED(ignore_missing(RegDeleteKeyValueW(root, kBackupKey, extension.c_str())));
    return S_OK;
}

bool is_handled(skyggn_scope scope, std::wstring_view requested) {
    const format_entry* entry = find_format(requested);
    const skyggn_format* format = entry ? &entry->format : nullptr;
    if (!format) {
        return false;
    }
    const std::optional<std::wstring> current = read_string(root_of(scope), handler_slot(format->extension), nullptr);
    return current && same_id(*current, server_id());
}

HRESULT take_system_details(bool take) {
    wil::unique_hkey system;
    RETURN_IF_FAILED(open_system_details(system));
    const HKEY root = HKEY_LOCAL_MACHINE;
    for (const format_entry& entry : formats()) {
        const std::wstring extension = entry.format.extension;
        if (!entry.details) {
            continue;
        }
        if (take) {
            std::optional<std::wstring> windows_entry;
            RETURN_IF_FAILED(system_entry(system.get(), extension, windows_entry));
            if (!windows_entry) {
                continue;  // windows does not keep this type's details
            }
            // what windows had is recorded once; taking again after a windows update keeps it
            if (!read_string(root, kTakenSystemDetailsKey, extension.c_str())) {
                RETURN_IF_FAILED(write_string(root, kTakenSystemDetailsKey, extension.c_str(), *windows_entry));
            }
            RETURN_IF_FAILED(ignore_missing(RegDeleteValueW(system.get(), extension.c_str())));
        } else {
            const std::optional<std::wstring> windows_entry = read_string(root, kTakenSystemDetailsKey, extension.c_str());
            if (!windows_entry) {
                continue;
            }
            const auto bytes = static_cast<DWORD>((windows_entry->size() + 1) * sizeof(wchar_t));
            RETURN_IF_WIN32_ERROR(RegSetValueExW(system.get(), extension.c_str(), 0, REG_SZ,
                                                 reinterpret_cast<const BYTE*>(windows_entry->c_str()), bytes));
            RETURN_IF_FAILED(ignore_missing(RegDeleteKeyValueW(root, kTakenSystemDetailsKey, extension.c_str())));
        }
        // the type's details registration follows: skyggn's handler where the type is skyggn's, or
        // windows' own back
        if (is_handled(SKYGGN_SCOPE_MACHINE, extension)) {
            RETURN_IF_FAILED(set_handled(SKYGGN_SCOPE_MACHINE, extension, true));
        }
    }
    return delete_if_empty(root, kTakenSystemDetailsKey);
}

bool system_details_taken() {
    wil::unique_hkey taken;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kTakenSystemDetailsKey, 0, KEY_READ, &taken) != ERROR_SUCCESS) {
        return false;
    }
    DWORD values = 0;
    if (RegQueryInfoKeyW(taken.get(), nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &values, nullptr, nullptr,
                         nullptr, nullptr) != ERROR_SUCCESS || values == 0) {
        return false;
    }
    for (DWORD index = 0; index < values; ++index) {
        wchar_t name[64];
        DWORD length = ARRAYSIZE(name);
        if (RegEnumValueW(taken.get(), index, name, &length, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS &&
            windows_keeps_details(name)) {
            return false;  // a windows update put windows' entry back
        }
    }
    return true;
}

skyggn_handler effective_handler(std::wstring_view extension) {
    // the same lookup explorer does: the user's chosen app, the file type, then system defaults
    wchar_t id[64];
    DWORD length = ARRAYSIZE(id);
    if (FAILED(AssocQueryStringW(ASSOCF_NONE, ASSOCSTR_SHELLEXTENSION, std::wstring(extension).c_str(),
                                 kThumbnailHandlerId, id, &length))) {
        return SKYGGN_HANDLER_NONE;
    }
    return classify(id);
}

HRESULT find_dead(skyggn_scope scope, bool remove, UINT* count) {
    RETURN_HR_IF_NULL(E_POINTER, count);
    *count = 0;
    const HKEY root = root_of(scope);
    wil::unique_hkey classes;
    RETURN_IF_WIN32_ERROR(RegOpenKeyExW(root, kClassesKey, 0, KEY_READ, &classes));
    std::vector<std::wstring> dead;
    for (DWORD index = 0;; ++index) {
        wchar_t name[256];
        DWORD length = ARRAYSIZE(name);
        const LSTATUS status = RegEnumKeyExW(classes.get(), index, name, &length, nullptr, nullptr, nullptr, nullptr);
        if (status == ERROR_NO_MORE_ITEMS) {
            break;
        }
        RETURN_IF_WIN32_ERROR(status);
        if (name[0] != L'.') {
            continue;
        }
        const std::wstring extension(name, length);
        const auto id = read_string(root, handler_slot(extension), nullptr);
        if (id && classify(*id) == SKYGGN_HANDLER_DEAD) {
            dead.push_back(extension);
        }
    }
    classes.reset();
    if (!remove) {
        *count = static_cast<UINT>(dead.size());
        return S_OK;
    }
    for (const std::wstring& extension : dead) {
        RETURN_IF_FAILED(delete_slot(root, extension));
        ++*count;
    }
    return S_OK;
}

}  // namespace skyggn
