// skyggnctl: the command line front end of the engine. the installer runs it to register skyggn,
// the settings app runs it elevated for changes that need admin rights, and it makes test
// thumbnails and reads test details, either directly or the way explorer does.

#include <skyggn/skyggn.h>

#include <fcntl.h>
#include <io.h>
#include <propkey.h>
#include <propvarutil.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <thumbcache.h>
#include <wincodec.h>
#include <wil/com.h>
#include <wil/resource.h>
#include <wil/result.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr wchar_t kUsage[] = LR"(usage: skyggnctl <command> [options]

commands:
  install [--user]                         register skyggn and turn on the recommended file types; over
                                           an existing install, keep the file types that are on
  uninstall [--user]                       give every file type back and unregister skyggn
  enable <.ext>... [--user]                turn file types on
  disable <.ext>... [--user]               turn file types off
  types <+.ext|-.ext>... [--user]          turn file types on (+) and off (-) in one go
  status                                   show which thumbnail handler windows uses per file type
  repair [--user]                          remove thumbnail entries whose program is gone
  refresh <folder> [--recursive]           remake the thumbnails windows keeps for a folder's files
  settings                                 show your settings
  set <name> <value>                       change one of your settings
  thumb <file> <out.png> [--size n]        make a thumbnail with the engine in this process
  check <file>                             say whether the file's own structure proves it empty,
                                           incomplete (cut off) or corrupted, as its thumbnail shows
  shell-thumb <file> <out.png> [--size n] [--cached]
                                           ask windows for a fresh thumbnail, as explorer does, or
                                           with --cached for the one it has stored
  system-details [on|off]                  read mkv and webm details too, which windows keeps for its
                                           own handler (every user, needs admin); without on or off,
                                           say whether skyggn reads them now
  details <file> [--isolated|--shell]      show a media file's details (length, frame size, tags):
                                           read by the engine in this process, --isolated through
                                           skyggn's handler as explorer runs it (in a separate
                                           process), --shell from the handler windows uses

without --user, changes apply to every user of this pc and need an elevated prompt.
)";

// a failure exits with its hresult (negative), so the settings app and the installer, which run
// skyggnctl elevated and cannot see what it prints, can say what went wrong
constexpr int kExitOk = 0;
constexpr int kExitFailed = 1;
constexpr int kExitUsage = 2;

struct arguments {
    std::wstring command;
    std::vector<std::wstring> positional;
    skyggn_scope scope = SKYGGN_SCOPE_MACHINE;
    UINT size = 256;
    bool recursive = false;
    bool cached = false;
    bool isolated = false;
    bool shell = false;
};

bool g_color = false;

const wchar_t* paint(int color) {
    if (!g_color) {
        return L"";
    }
    switch (color) {
    case 1:
        return L"\x1b[31m";  // red
    case 2:
        return L"\x1b[32m";  // green
    case 3:
        return L"\x1b[33m";  // yellow
    case 9:
        return L"\x1b[90m";  // grey
    default:
        return L"\x1b[0m";
    }
}

void enable_console() {
    _setmode(_fileno(stdout), _O_U8TEXT);
    _setmode(_fileno(stderr), _O_U8TEXT);
    if (GetEnvironmentVariableW(L"NO_COLOR", nullptr, 0) != 0) {
        return;
    }
    HANDLE console = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    g_color = GetConsoleMode(console, &mode) && SetConsoleMode(console, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
}

std::wstring describe(HRESULT hr) {
    wil::unique_hlocal_string message;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                   static_cast<DWORD>(hr), 0, reinterpret_cast<wchar_t*>(&message), 0, nullptr);
    std::wstring text = message ? message.get() : L"unknown error";
    while (!text.empty() && (text.back() == L'\n' || text.back() == L'\r' || text.back() == L'.')) {
        text.pop_back();
    }
    wchar_t code[16];
    swprintf_s(code, L" (0x%08lX)", static_cast<unsigned long>(hr));
    return text + code;
}

int fail(std::wstring_view what, HRESULT hr, skyggn_scope scope) {
    fwprintf(stderr, L"%sskyggnctl: %.*s failed: %s%s\n", paint(1), static_cast<int>(what.size()), what.data(),
             describe(hr).c_str(), paint(0));
    if (hr == E_ACCESSDENIED && scope == SKYGGN_SCOPE_MACHINE) {
        fwprintf(stderr, L"this changes every user of the pc: run it from an elevated prompt, or add --user\n");
    }
    return FAILED(hr) ? static_cast<int>(hr) : kExitFailed;
}

std::optional<arguments> parse(int argc, wchar_t** argv) {
    if (argc < 2) {
        return std::nullopt;
    }
    arguments parsed{argv[1]};
    for (int i = 2; i < argc; ++i) {
        const std::wstring_view argument = argv[i];
        if (argument == L"--user") {
            parsed.scope = SKYGGN_SCOPE_USER;
        } else if (argument == L"--recursive") {
            parsed.recursive = true;
        } else if (argument == L"--cached") {
            parsed.cached = true;
        } else if (argument == L"--isolated") {
            parsed.isolated = true;
        } else if (argument == L"--shell") {
            parsed.shell = true;
        } else if (argument == L"--size" && i + 1 < argc) {
            parsed.size = static_cast<UINT>(_wtoi(argv[++i]));
            if (parsed.size == 0) {
                return std::nullopt;
            }
        } else if (argument.starts_with(L"--")) {
            return std::nullopt;
        } else {
            parsed.positional.emplace_back(argument);
        }
    }
    return parsed;
}

const wchar_t* scope_name(skyggn_scope scope) {
    return scope == SKYGGN_SCOPE_MACHINE ? L"every user" : L"this user";
}

int install(const arguments& args) {
    if (HRESULT hr = skyggn_install(args.scope); FAILED(hr)) {
        return fail(L"install", hr, args.scope);
    }
    UINT count = 0;
    for (UINT i = 0; i < skyggn_format_count(); ++i) {
        count += skyggn_is_handled(args.scope, skyggn_format_at(i)->extension) ? 1 : 0;
    }
    skyggn_notify_shell();
    wprintf(L"%sinstalled%s for %s: %u file types\n", paint(2), paint(0), scope_name(args.scope), count);
    return kExitOk;
}

int uninstall(const arguments& args) {
    if (HRESULT hr = skyggn_unregister_server(args.scope); FAILED(hr)) {
        return fail(L"uninstall", hr, args.scope);
    }
    skyggn_notify_shell();
    wprintf(L"%suninstalled%s for %s: every file type has its previous handler back\n", paint(2), paint(0),
            scope_name(args.scope));
    return kExitOk;
}

bool supported(const std::wstring& extension) {
    for (UINT i = 0; i < skyggn_format_count(); ++i) {
        if (CompareStringOrdinal(skyggn_format_at(i)->extension, -1, extension.c_str(), -1, TRUE) == CSTR_EQUAL) {
            return true;
        }
    }
    return false;
}

struct type_change {
    std::wstring extension;
    bool handled;
};

// enable and disable apply one direction to every argument; types reads it from each argument's
// + or - prefix
int set_types(const arguments& args) {
    std::vector<type_change> changes;
    for (const std::wstring& argument : args.positional) {
        if (args.command == L"types") {
            if (argument.size() < 2 || (argument[0] != L'+' && argument[0] != L'-')) {
                fwprintf(stderr, L"skyggnctl: %s needs a + (on) or - (off) in front\n", argument.c_str());
                return kExitUsage;
            }
            changes.push_back({argument.substr(1), argument[0] == L'+'});
        } else {
            changes.push_back({argument, args.command == L"enable"});
        }
    }
    if (changes.empty()) {
        fwprintf(stderr, L"%s", kUsage);
        return kExitUsage;
    }
    bool any_on = false;
    for (const type_change& change : changes) {
        if (!supported(change.extension)) {
            fwprintf(stderr, L"skyggnctl: %s is not a supported file type (see skyggnctl status)\n",
                     change.extension.c_str());
            return kExitUsage;
        }
        any_on = any_on || change.handled;
    }
    if (any_on) {
        if (HRESULT hr = skyggn_register_server(args.scope); FAILED(hr)) {
            return fail(L"registering skyggn", hr, args.scope);
        }
    }
    for (const type_change& change : changes) {
        if (HRESULT hr = skyggn_set_handled(args.scope, change.extension.c_str(), change.handled); FAILED(hr)) {
            return fail((change.handled ? L"turning on " : L"turning off ") + change.extension, hr, args.scope);
        }
        wprintf(L"%s %s\n", change.extension.c_str(), change.handled ? L"on" : L"off");
    }
    skyggn_notify_shell();
    return kExitOk;
}

const wchar_t* category_name(skyggn_category category) {
    switch (category) {
    case SKYGGN_CATEGORY_AUDIO:
        return L"audio";
    case SKYGGN_CATEGORY_IMAGE:
        return L"image";
    case SKYGGN_CATEGORY_RAW:
        return L"raw";
    case SKYGGN_CATEGORY_BOOK:
        return L"book";
    case SKYGGN_CATEGORY_DOCUMENT:
        return L"doc";
    default:
        return L"video";
    }
}

int status() {
    wprintf(L"%-7s %-6s %-9s %s\n", L"type", L"kind", L"skyggn", L"windows uses");
    for (UINT i = 0; i < skyggn_format_count(); ++i) {
        const skyggn_format* format = skyggn_format_at(i);
        const bool machine = skyggn_is_handled(SKYGGN_SCOPE_MACHINE, format->extension);
        const bool user = skyggn_is_handled(SKYGGN_SCOPE_USER, format->extension);
        const wchar_t* on = user && machine ? L"on (both)" : user ? L"on (user)" : machine ? L"on" : L"off";
        const wchar_t* uses = L"";
        int color = 0;
        switch (skyggn_effective_handler(format->extension)) {
        case SKYGGN_HANDLER_SKYGGN:
            uses = L"skyggn";
            color = 2;
            break;
        case SKYGGN_HANDLER_OTHER:
            uses = L"another program";
            color = 0;
            break;
        case SKYGGN_HANDLER_DEAD:
            uses = L"a removed program (no thumbnails; run repair)";
            color = 1;
            break;
        case SKYGGN_HANDLER_NONE:
            uses = L"nothing (file type icon)";
            color = 9;
            break;
        }
        wprintf(L"%-7s %-6s %-9s %s%s%s\n", format->extension, category_name(format->category), on, paint(color),
                uses, paint(0));
    }
    return kExitOk;
}

int repair(const arguments& args) {
    UINT removed = 0;
    if (HRESULT hr = skyggn_repair(args.scope, &removed); FAILED(hr)) {
        return fail(L"repair", hr, args.scope);
    }
    skyggn_notify_shell();
    wprintf(L"%srepaired%s for %s: removed %u dead thumbnail entr%s\n", paint(2), paint(0), scope_name(args.scope),
            removed, removed == 1 ? L"y" : L"ies");
    return kExitOk;
}

// windows keeps a thumbnail per file and size; these are the sizes file explorer's medium to extra
// large views read, 1280 on high-dpi screens. smaller ones carry no badge, so they look the same
// either way.
constexpr UINT kRefreshSizes[] = {96, 256, 1280};

int refresh(const arguments& args) {
    if (args.positional.size() != 1) {
        fwprintf(stderr, L"%s", kUsage);
        return kExitUsage;
    }
    const std::wstring& root = args.positional[0];
    std::vector<std::wstring> files;
    std::vector<std::wstring> folders{root};
    UINT online_only = 0;
    UINT unreadable_folders = 0;
    while (!folders.empty()) {
        const std::wstring folder = std::move(folders.back());
        folders.pop_back();
        WIN32_FIND_DATAW data{};
        wil::unique_hfind find(FindFirstFileW((folder + L"\\*").c_str(), &data));
        if (!find) {
            if (folder == root) {
                return fail(L"reading " + folder, HRESULT_FROM_WIN32(GetLastError()), args.scope);
            }
            ++unreadable_folders;  // a subfolder this account may not open; reported at the end
            continue;
        }
        do {
            const std::wstring_view name = data.cFileName;
            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                // links to other folders are not followed: they can loop, or lead off this folder
                if (args.recursive && name != L"." && name != L".." &&
                    !(data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                    folders.push_back(folder + L"\\" + data.cFileName);
                }
                continue;
            }
            if (skyggn_effective_handler(PathFindExtensionW(data.cFileName)) != SKYGGN_HANDLER_SKYGGN) {
                continue;
            }
            // reading a cloud file that is not on this pc would download it
            if (data.dwFileAttributes & (FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS | FILE_ATTRIBUTE_RECALL_ON_OPEN |
                                         FILE_ATTRIBUTE_OFFLINE)) {
                ++online_only;
                continue;
            }
            files.push_back(folder + L"\\" + data.cFileName);
        } while (FindNextFileW(find.get(), &data));
    }

    auto cache = wil::CoCreateInstance<IThumbnailCache>(CLSID_LocalThumbnailCache);
    // forced extraction replaces the kept thumbnail with a new one
    const auto flags = static_cast<WTS_FLAGS>(WTS_EXTRACT | WTS_FORCEEXTRACTION);
    UINT refreshed = 0;
    UINT without_picture = 0;
    for (size_t i = 0; i < files.size(); ++i) {
        wprintf(L"\rrefreshing %zu of %zu", i + 1, files.size());
        fflush(stdout);
        wil::com_ptr<IShellItem> item;
        bool made = SUCCEEDED(SHCreateItemFromParsingName(files[i].c_str(), nullptr, IID_PPV_ARGS(&item)));
        for (UINT size : kRefreshSizes) {
            wil::com_ptr<ISharedBitmap> bitmap;
            made = made && SUCCEEDED(cache->GetThumbnail(item.get(), size, flags, &bitmap, nullptr, nullptr));
        }
        // open explorer windows draw the file again
        SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATHW, files[i].c_str(), nullptr);
        made ? ++refreshed : ++without_picture;
    }
    wprintf(L"\r%srefreshed%s %u file%s in %s%s", paint(2), paint(0), refreshed, refreshed == 1 ? L"" : L"s",
            root.c_str(), args.recursive ? L" and its subfolders" : L"");
    if (without_picture > 0) {
        wprintf(L"; %u had no picture (an audio file without cover art) or could not be read", without_picture);
    }
    if (online_only > 0) {
        wprintf(L"; %u online-only cloud file%s left alone", online_only, online_only == 1 ? L"" : L"s");
    }
    if (unreadable_folders > 0) {
        wprintf(L"; %u subfolder%s could not be opened", unreadable_folders, unreadable_folders == 1 ? L"" : L"s");
    }
    wprintf(L"\n");
    return kExitOk;
}

int show_settings() {
    wprintf(L"%-16s %-7s %-8s %s\n", L"name", L"value", L"default", L"range");
    for (UINT i = 0; i < skyggn_setting_count(); ++i) {
        const skyggn_setting* setting = skyggn_setting_at(i);
        wprintf(L"%-16s %-7lu %-8lu %lu-%lu\n", setting->name, skyggn_setting_get(setting->name),
                setting->default_value, setting->min_value, setting->max_value);
        wprintf(L"%s  %s%s\n", paint(9), setting->description, paint(0));
    }
    return kExitOk;
}

int set_setting(const arguments& args) {
    if (args.positional.size() != 2) {
        fwprintf(stderr, L"%s", kUsage);
        return kExitUsage;
    }
    const std::wstring& name = args.positional[0];
    wchar_t* end = nullptr;
    const unsigned long value = wcstoul(args.positional[1].c_str(), &end, 10);
    if (end == args.positional[1].c_str() || *end != L'\0') {
        fwprintf(stderr, L"skyggnctl: %s is not a number\n", args.positional[1].c_str());
        return kExitUsage;
    }
    if (HRESULT hr = skyggn_setting_set(name.c_str(), value); FAILED(hr)) {
        if (hr == E_INVALIDARG) {
            fwprintf(stderr, L"skyggnctl: no setting %s, or %lu is out of its range (see skyggnctl settings)\n",
                     name.c_str(), value);
            return kExitUsage;
        }
        return fail(L"set " + name, hr, args.scope);
    }
    wprintf(L"%s = %lu\n", name.c_str(), value);
    return kExitOk;
}

HRESULT save_png(HBITMAP bitmap, const std::wstring& path) {
    auto factory = wil::CoCreateInstance<IWICImagingFactory>(CLSID_WICImagingFactory);
    wil::com_ptr<IWICBitmap> source;
    // thumbnails are premultiplied, with transparent room around a badge; the png keeps it
    RETURN_IF_FAILED(factory->CreateBitmapFromHBITMAP(bitmap, nullptr, WICBitmapUsePremultipliedAlpha, &source));
    wil::com_ptr<IWICStream> stream;
    RETURN_IF_FAILED(factory->CreateStream(&stream));
    RETURN_IF_FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE));
    wil::com_ptr<IWICBitmapEncoder> encoder;
    RETURN_IF_FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder));
    RETURN_IF_FAILED(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache));
    wil::com_ptr<IWICBitmapFrameEncode> frame;
    RETURN_IF_FAILED(encoder->CreateNewFrame(&frame, nullptr));
    RETURN_IF_FAILED(frame->Initialize(nullptr));
    UINT width = 0;
    UINT height = 0;
    RETURN_IF_FAILED(source->GetSize(&width, &height));
    RETURN_IF_FAILED(frame->SetSize(width, height));
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    RETURN_IF_FAILED(frame->SetPixelFormat(&format));
    RETURN_IF_FAILED(frame->WriteSource(source.get(), nullptr));
    RETURN_IF_FAILED(frame->Commit());
    return encoder->Commit();
}

void report_bitmap(HBITMAP bitmap, const std::wstring& out, std::chrono::steady_clock::duration took) {
    BITMAP info{};
    GetObjectW(bitmap, sizeof(info), &info);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(took).count();
    wprintf(L"%s %ldx%ld in %lld ms\n", out.c_str(), info.bmWidth, info.bmHeight, static_cast<long long>(ms));
}

int thumb(const arguments& args) {
    if (args.positional.size() != 2) {
        fwprintf(stderr, L"%s", kUsage);
        return kExitUsage;
    }
    const std::wstring& file = args.positional[0];
    const auto started = std::chrono::steady_clock::now();
    wil::unique_hbitmap bitmap;
    if (HRESULT hr = skyggn_thumbnail(file.c_str(), args.size, &bitmap); FAILED(hr)) {
        return fail(L"thumbnail of " + file, hr, args.scope);
    }
    const auto took = std::chrono::steady_clock::now() - started;
    if (HRESULT hr = save_png(bitmap.get(), args.positional[1]); FAILED(hr)) {
        return fail(L"saving " + args.positional[1], hr, args.scope);
    }
    report_bitmap(bitmap.get(), args.positional[1], took);
    return kExitOk;
}

int check(const arguments& args) {
    if (args.positional.size() != 1) {
        fwprintf(stderr, L"%s", kUsage);
        return kExitUsage;
    }
    const std::wstring& file = args.positional[0];
    skyggn_damage damage = SKYGGN_DAMAGE_NONE;
    if (HRESULT hr = skyggn_check(file.c_str(), &damage); FAILED(hr)) {
        return fail(L"check of " + file, hr, args.scope);
    }
    switch (damage) {
    case SKYGGN_DAMAGE_EMPTY:
        wprintf(L"%sempty%s: no bytes, or nothing but zero bytes\n", paint(1), paint(0));
        break;
    case SKYGGN_DAMAGE_INCOMPLETE:
        wprintf(L"%sincomplete%s: it ends before its own structure says it does, or lacks a part every "
                L"finished file has\n",
                paint(1), paint(0));
        break;
    case SKYGGN_DAMAGE_CORRUPTED:
        wprintf(L"%scorrupted%s: a checksum over its header fails, or it points past its own end\n", paint(1),
                paint(0));
        break;
    default:
        wprintf(L"%sno damage proven%s\n", paint(2), paint(0));
        break;
    }
    return kExitOk;
}

int shell_thumb(const arguments& args) {
    if (args.positional.size() != 2) {
        fwprintf(stderr, L"%s", kUsage);
        return kExitUsage;
    }
    const std::wstring& file = args.positional[0];
    wil::com_ptr<IShellItem> item;
    if (HRESULT hr = SHCreateItemFromParsingName(file.c_str(), nullptr, IID_PPV_ARGS(&item)); FAILED(hr)) {
        return fail(L"opening " + file, hr, args.scope);
    }
    auto cache = wil::CoCreateInstance<IThumbnailCache>(CLSID_LocalThumbnailCache);
    // a fresh extraction through the registered handler, kept out of the cache; or what the cache holds
    const auto flags = args.cached ? WTS_INCACHEONLY
                                   : static_cast<WTS_FLAGS>(WTS_EXTRACT | WTS_FORCEEXTRACTION | WTS_EXTRACTDONOTCACHE);
    const auto started = std::chrono::steady_clock::now();
    wil::com_ptr<ISharedBitmap> shared;
    if (HRESULT hr = cache->GetThumbnail(item.get(), args.size, flags, &shared, nullptr, nullptr); FAILED(hr)) {
        return fail(L"windows thumbnail of " + file, hr, args.scope);
    }
    WTS_ALPHATYPE alpha = WTSAT_UNKNOWN;
    shared->GetFormat(&alpha);
    wprintf(L"alpha type: %s\n", alpha == WTSAT_ARGB ? L"argb (transparent parts kept)" : alpha == WTSAT_RGB ? L"rgb (no transparency)" : L"unknown");
    const auto took = std::chrono::steady_clock::now() - started;
    HBITMAP bitmap = nullptr;  // owned by the shared bitmap
    if (HRESULT hr = shared->GetSharedBitmap(&bitmap); FAILED(hr)) {
        return fail(L"windows thumbnail of " + file, hr, args.scope);
    }
    if (HRESULT hr = save_png(bitmap, args.positional[1]); FAILED(hr)) {
        return fail(L"saving " + args.positional[1], hr, args.scope);
    }
    report_bitmap(bitmap, args.positional[1], took);
    return kExitOk;
}

int details(const arguments& args) {
    if (args.positional.size() != 1 || (args.isolated && args.shell)) {
        fwprintf(stderr, L"%s", kUsage);
        return kExitUsage;
    }
    const std::wstring& file = args.positional[0];
    const auto started = std::chrono::steady_clock::now();
    wil::com_ptr<IPropertyStore> store;
    const HRESULT hr = args.shell ? SHGetPropertyStoreFromParsingName(file.c_str(), nullptr, GPS_HANDLERPROPERTIESONLY,
                                                                     IID_PPV_ARGS(&store))
                                  : skyggn_details(file.c_str(), args.isolated, &store);
    if (FAILED(hr)) {
        return fail(L"details of " + file, hr, args.scope);
    }
    const auto took = std::chrono::steady_clock::now() - started;
    DWORD count = 0;
    THROW_IF_FAILED(store->GetCount(&count));
    for (DWORD index = 0; index < count; ++index) {
        PROPERTYKEY key{};
        THROW_IF_FAILED(store->GetAt(index, &key));
        wil::unique_prop_variant value;
        THROW_IF_FAILED(store->GetValue(key, &value));
        // the label and the text explorer shows; a key windows has no description for, by its id
        wil::unique_cotaskmem_string label;
        if (wil::com_ptr<IPropertyDescription> description;
            SUCCEEDED(PSGetPropertyDescription(key, IID_PPV_ARGS(&description)))) {
            description->GetDisplayName(&label);
        }
        wil::unique_cotaskmem_string text;
        if (FAILED(PSFormatForDisplayAlloc(key, value, PDFF_DEFAULT, &text))) {
            PropVariantToStringAlloc(value, &text);
        }
        wchar_t id[64];
        PSStringFromPropertyKey(key, id, ARRAYSIZE(id));
        wprintf(L"%-22s %s\n", label ? label.get() : id, text ? text.get() : L"?");
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(took).count();
    wprintf(L"%s%lu details in %lld ms%s\n", paint(9), count, static_cast<long long>(ms), paint(0));
    return kExitOk;
}

int system_details(const arguments& args) {
    if (args.positional.empty()) {
        wprintf(L"skyggn %s mkv and webm details\n", skyggn_system_details_taken() ? L"reads" : L"leaves windows");
        return kExitOk;
    }
    if (args.positional.size() != 1 || (args.positional[0] != L"on" && args.positional[0] != L"off")) {
        fwprintf(stderr, L"%s", kUsage);
        return kExitUsage;
    }
    const bool take = args.positional[0] == L"on";
    if (HRESULT hr = skyggn_take_system_details(take); FAILED(hr)) {
        return fail(L"system-details", hr, SKYGGN_SCOPE_MACHINE);
    }
    skyggn_notify_shell();
    wprintf(L"%s\n", take ? L"skyggn reads mkv and webm details now" : L"windows reads mkv and webm details again");
    return kExitOk;
}

int run(const arguments& args) {
    if (args.command == L"install") {
        return install(args);
    }
    if (args.command == L"uninstall") {
        return uninstall(args);
    }
    if (args.command == L"enable" || args.command == L"disable" || args.command == L"types") {
        return set_types(args);
    }
    if (args.command == L"status") {
        return status();
    }
    if (args.command == L"repair") {
        return repair(args);
    }
    if (args.command == L"refresh") {
        return refresh(args);
    }
    if (args.command == L"settings") {
        return show_settings();
    }
    if (args.command == L"set") {
        return set_setting(args);
    }
    if (args.command == L"thumb") {
        return thumb(args);
    }
    if (args.command == L"check") {
        return check(args);
    }
    if (args.command == L"shell-thumb") {
        return shell_thumb(args);
    }
    if (args.command == L"details") {
        return details(args);
    }
    if (args.command == L"system-details") {
        return system_details(args);
    }
    fwprintf(stderr, L"%s", kUsage);
    return kExitUsage;
}

}  // namespace

int wmain(int argc, wchar_t** argv) try {
    enable_console();
    const std::optional<arguments> args = parse(argc, argv);
    if (!args) {
        fwprintf(stderr, L"%s", kUsage);
        return kExitUsage;
    }
    auto com = wil::CoInitializeEx(COINIT_APARTMENTTHREADED);
    return run(*args);
} catch (const wil::ResultException& error) {
    fwprintf(stderr, L"skyggnctl: %s\n", describe(error.GetErrorCode()).c_str());
    return static_cast<int>(error.GetErrorCode());
} catch (const std::exception& error) {
    fwprintf(stderr, L"skyggnctl: %hs\n", error.what());
    return kExitFailed;
}
