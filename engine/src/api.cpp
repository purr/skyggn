// the exported c api: thin wrappers that keep c++ exceptions inside the dll.

#include <skyggn/skyggn.h>

#include "build_info.h"
#include "decoder.h"
#include "image.h"
#include "prepare.h"
#include "properties.h"
#include "formats.h"
#include "registration.h"
#include "settings.h"

#include <shlobj.h>
#include <shlwapi.h>
#include <wil/com.h>
#include <wil/result.h>

#include <algorithm>

using namespace skyggn;

SKYGGN_API const wchar_t* skyggn_version() {
    return SKYGGN_VERSION;
}

SKYGGN_API UINT skyggn_format_count() {
    return static_cast<UINT>(formats().size());
}

SKYGGN_API const skyggn_format* skyggn_format_at(UINT index) {
    return index < formats().size() ? &formats()[index].format : nullptr;
}

SKYGGN_API UINT skyggn_setting_count() {
    return static_cast<UINT>(setting_definitions().size());
}

SKYGGN_API const skyggn_setting* skyggn_setting_at(UINT index) {
    return index < setting_definitions().size() ? &setting_definitions()[index] : nullptr;
}

SKYGGN_API const wchar_t* skyggn_setting_description(const wchar_t* name, const wchar_t* language) {
    const skyggn_setting* setting = name ? find_setting(name) : nullptr;
    return setting ? describe(*setting, language ? language : L"") : nullptr;
}

SKYGGN_API DWORD skyggn_setting_get(const wchar_t* name) {
    const skyggn_setting* setting = name ? find_setting(name) : nullptr;
    return setting ? read_setting(*setting) : 0;
}

SKYGGN_API HRESULT skyggn_setting_set(const wchar_t* name, DWORD value) {
    RETURN_HR_IF_NULL(E_INVALIDARG, name);
    const skyggn_setting* setting = find_setting(name);
    RETURN_HR_IF_NULL(E_INVALIDARG, setting);
    return write_setting(*setting, value);
}

namespace {

HRESULT open_file(const wchar_t* path, wil::com_ptr<IStream>& stream) {
    RETURN_HR_IF_NULL(E_INVALIDARG, path);
    return SHCreateStreamOnFileEx(path, STGM_READ | STGM_SHARE_DENY_NONE, FILE_ATTRIBUTE_NORMAL, FALSE, nullptr,
                                  &stream);
}

// thumbnails use wic and direct2d, which need com on the calling thread. callers (the settings
// app's worker threads) may not have it: join the multithreaded apartment for the call. a thread
// already in another apartment (RPC_E_CHANGED_MODE) has com, which is all that is needed.
struct com_scope {
    HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ~com_scope() {
        if (SUCCEEDED(result)) {
            CoUninitialize();
        }
    }
};

}  // namespace

SKYGGN_API HRESULT skyggn_take_system_details(BOOL take) try {
    return take_system_details(take != FALSE);
}
CATCH_RETURN()

SKYGGN_API BOOL skyggn_system_details_taken() {
    return system_details_taken() ? TRUE : FALSE;
}

SKYGGN_API HRESULT skyggn_prepare_folder(const wchar_t* folder, BOOL recursive, BOOL force, skyggn_progress progress,
                                         void* context, skyggn_prepare_result* result) try {
    RETURN_HR_IF_NULL(E_POINTER, result);
    *result = {};
    RETURN_HR_IF_NULL(E_INVALIDARG, folder);
    // windows' file type lookups need com on this thread; the workers join com themselves
    const com_scope com;
    RETURN_HR_IF(com.result, FAILED(com.result) && com.result != RPC_E_CHANGED_MODE);
    return prepare_folder(folder, recursive != FALSE, force != FALSE, progress, context, *result);
}
CATCH_RETURN()

SKYGGN_API HRESULT skyggn_details(const wchar_t* path, BOOL isolated, IPropertyStore** store) {
    return details_of(path, isolated != FALSE, store);
}

SKYGGN_API HRESULT skyggn_thumbnail(const wchar_t* path, UINT size, HBITMAP* bitmap) try {
    RETURN_HR_IF_NULL(E_POINTER, bitmap);
    *bitmap = nullptr;
    const com_scope com;
    RETURN_HR_IF(com.result, FAILED(com.result) && com.result != RPC_E_CHANGED_MODE);
    wil::com_ptr<IStream> stream;
    RETURN_IF_FAILED(open_file(path, stream));
    bool transparent = false;
    return make_thumbnail(stream.get(), size, load_settings(), bitmap, &transparent);
}
CATCH_RETURN()

SKYGGN_API HRESULT skyggn_check(const wchar_t* path, skyggn_damage* damage) try {
    RETURN_HR_IF_NULL(E_POINTER, damage);
    *damage = SKYGGN_DAMAGE_NONE;
    const com_scope com;
    RETURN_HR_IF(com.result, FAILED(com.result) && com.result != RPC_E_CHANGED_MODE);
    wil::com_ptr<IStream> stream;
    RETURN_IF_FAILED(open_file(path, stream));
    // with the tile on, a file without a picture still makes a thumbnail, so any failure left is real
    settings options = load_settings();
    options.placeholder = SKYGGN_PLACEHOLDER_PER_FILE;
    image picture;
    return make_thumbnail_image(stream.get(), 256, options, picture, damage);
}
CATCH_RETURN()

SKYGGN_API HRESULT skyggn_thumbnail_pixels(const wchar_t* path, UINT size, UINT32* pixels, UINT capacity, UINT* width,
                                           UINT* height) try {
    RETURN_HR_IF_NULL(E_POINTER, pixels);
    RETURN_HR_IF_NULL(E_POINTER, width);
    RETURN_HR_IF_NULL(E_POINTER, height);
    *width = 0;
    *height = 0;
    const com_scope com;
    RETURN_HR_IF(com.result, FAILED(com.result) && com.result != RPC_E_CHANGED_MODE);
    wil::com_ptr<IStream> stream;
    RETURN_IF_FAILED(open_file(path, stream));
    image picture;
    RETURN_IF_FAILED_EXPECTED(make_thumbnail_image(stream.get(), size, load_settings(), picture));
    RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER), picture.pixels.size() > capacity);
    std::copy(picture.pixels.begin(), picture.pixels.end(), pixels);
    *width = static_cast<UINT>(picture.width);
    *height = static_cast<UINT>(picture.height);
    return S_OK;
}
CATCH_RETURN()

SKYGGN_API BOOL skyggn_is_registered(skyggn_scope scope) try {
    return is_registered(scope);
} catch (...) {
    return FALSE;  // only std::bad_alloc can get here
}

SKYGGN_API HRESULT skyggn_install(skyggn_scope scope) try {
    return install(scope);
}
CATCH_RETURN()

SKYGGN_API HRESULT skyggn_register_server(skyggn_scope scope) try {
    return register_server(scope);
}
CATCH_RETURN()

SKYGGN_API HRESULT skyggn_unregister_server(skyggn_scope scope) try {
    return unregister_server(scope);
}
CATCH_RETURN()

SKYGGN_API HRESULT skyggn_set_handled(skyggn_scope scope, const wchar_t* extension, BOOL handled) try {
    RETURN_HR_IF_NULL(E_INVALIDARG, extension);
    return set_handled(scope, extension, handled != FALSE);
}
CATCH_RETURN()

SKYGGN_API BOOL skyggn_is_handled(skyggn_scope scope, const wchar_t* extension) try {
    return extension && is_handled(scope, extension);
} catch (...) {
    return FALSE;  // only std::bad_alloc can get here; "not handled" is the safe answer
}

SKYGGN_API skyggn_handler skyggn_effective_handler(const wchar_t* extension) try {
    return extension ? effective_handler(extension) : SKYGGN_HANDLER_NONE;
} catch (...) {
    return SKYGGN_HANDLER_NONE;  // only std::bad_alloc can get here
}

SKYGGN_API HRESULT skyggn_repair(skyggn_scope scope, UINT* removed) try {
    return find_dead(scope, true, removed);
}
CATCH_RETURN()

SKYGGN_API HRESULT skyggn_count_dead(skyggn_scope scope, UINT* count) try {
    return find_dead(scope, false, count);
}
CATCH_RETURN()

SKYGGN_API void skyggn_notify_shell() {
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
}
