#pragma once

namespace skyggn {

// skyggn's own key. under HKEY_CURRENT_USER it holds the user's settings as values.
inline constexpr wchar_t kProductKey[] = L"Software\\skyggn";

// the handler each file type had before skyggn took it over, under the scope's root key
inline constexpr wchar_t kBackupKey[] = L"Software\\skyggn\\PreviousHandlers";

// the decorations (film strip, photo border, app icon) a file type had before skyggn switched them
// off, one subkey per type, under the scope's root key
inline constexpr wchar_t kDecorationsBackupKey[] = L"Software\\skyggn\\PreviousDecorations";

// a file type's property handler before skyggn took it over, and the detail lists skyggn added
// for it, one subkey per type. machine-wide only, like property handlers themselves.
inline constexpr wchar_t kDetailsBackupKey[] = L"Software\\skyggn\\PreviousDetails";

inline constexpr wchar_t kClassesKey[] = L"Software\\Classes";

// explorer's property handlers (details: length, frame size, tags), one key per file type. windows
// reads them under HKEY_LOCAL_MACHINE only.
inline constexpr wchar_t kPropertyHandlersKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\PropertySystem\\PropertyHandlers";

// the types whose details windows reads with its own handler whatever PropertyHandlers says
// (mp4, mp3, matroska and others). the key belongs to windows itself (TrustedInstaller).
inline constexpr wchar_t kSystemPropertyHandlersKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\PropertySystem\\SystemPropertyHandlers";

// the entries skyggn set aside from that list when asked to read those types' details itself, one
// value per type, as windows had them; machine-wide
inline constexpr wchar_t kTakenSystemDetailsKey[] = L"Software\\skyggn\\TakenSystemDetails";

// per-type association settings windows reads before a perceived type's defaults
inline constexpr wchar_t kSystemAssociationsKey[] = L"Software\\Classes\\SystemFileAssociations";

// the shell extension slot windows reads thumbnail handlers from (IThumbnailProvider), found under
// a file type's key as ShellEx\{id}
inline constexpr wchar_t kThumbnailHandlerId[] = L"{e357fccd-a995-4576-b01f-234630154e96}";

inline constexpr wchar_t kApprovedExtensionsKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Shell Extensions\\Approved";

}  // namespace skyggn
