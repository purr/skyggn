// windows loads the engine into its own processes (dllhost.exe, explorer.exe), whose dll search
// path does not include the folder the engine is installed in. ffmpeg is therefore delay-loaded
// and every ffmpeg dll is opened by its full path inside that folder. this also keeps a same-named
// dll elsewhere on the search path from being picked up instead.

#include "ffmpeg_loader.h"

#include "build_info.h"

#include <delayimp.h>
#include <wil/result.h>
#include <wil/stl.h>
#include <wil/win32_helpers.h>

#include <string>

extern "C" {
#include <libavutil/log.h>
}

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace skyggn {

namespace {

std::wstring engine_folder() {
    auto path = wil::GetModuleFileNameW<std::wstring>(engine_module());
    path.resize(path.find_last_of(L'\\') + 1);
    return path;
}

FARPROC WINAPI delay_load_hook(unsigned notification, PDelayLoadInfo info) {
    if (notification != dliNotePreLoadLibrary) {
        return nullptr;
    }
    std::wstring path = engine_folder();
    for (const char* c = info->szDll; *c; ++c) {
        path.push_back(static_cast<wchar_t>(*c));  // dll names in import tables are ascii
    }
    // LOAD_WITH_ALTERED_SEARCH_PATH makes the dll's own dependencies (avcodec needs swresample)
    // resolve from the same folder.
    return reinterpret_cast<FARPROC>(LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH));
}

}  // namespace

HMODULE engine_module() {
    return reinterpret_cast<HMODULE>(&__ImageBase);
}

HRESULT load_ffmpeg() {
    static const HRESULT result = [] {
        for (const char* dll : kFfmpegDlls) {
            RETURN_IF_FAILED(__HrLoadAllImportsForDll(dll));
        }
        av_log_set_level(AV_LOG_QUIET);
        return S_OK;
    }();
    return result;
}

}  // namespace skyggn

extern "C" const PfnDliHook __pfnDliNotifyHook2 = skyggn::delay_load_hook;
