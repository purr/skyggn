// making windows' thumbnails of a folder's files ahead of time, through windows' thumbnail cache as
// file explorer asks for them, so they land where explorer looks: the settings app's "make
// thumbnails now", skyggnctl prepare and skyggnctl refresh.

#include "prepare.h"

#include "registration.h"

#include <pathcch.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <thumbcache.h>
#include <wil/com.h>
#include <wil/resource.h>
#include <wil/result.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace skyggn {

namespace {

// reading a cloud file that is not on this pc would download it
constexpr DWORD kOnlineOnly = FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS | FILE_ATTRIBUTE_RECALL_ON_OPEN | FILE_ATTRIBUTE_OFFLINE;

// asked for the size of a large icons view, windows makes the thumbnail at the size it keeps for its
// largest views (1280 px on a high-dpi screen) and answers every smaller size from it: one request
// fills them all
constexpr UINT kPreparedSize = 256;

// made again, a thumbnail is asked for at each size explorer's views read, so none keeps the old
// look: forced extraction replaces only the size asked for
constexpr UINT kForcedSizes[] = {96, 256, 1280};

// windows' thumbnail helper makes several at once: three requests at a time took 60 % of the time
// one after another took, and leave the other cores to the rest of the pc
constexpr unsigned kWorkers = 3;

enum class outcome { made, kept, none };

std::wstring joined(const std::wstring& folder, const wchar_t* name) {
    return folder.ends_with(L'\\') ? folder + name : folder + L'\\' + name;
}

// the files skyggn makes thumbnails for, folder by folder; links to other folders are not followed,
// as they can loop or lead off the folder. `looking` hears of each folder before it is read, with the
// number of files found so far, and returns false to stop: listing a whole drive can take minutes.
template <typename Looking>
HRESULT list_files(const std::wstring& root, bool recursive, std::vector<std::wstring>& files,
                   skyggn_prepare_result& result, const Looking& looking) {
    std::map<std::wstring, bool> handled;  // by lowercase extension: windows' association lookup is slow
    std::vector<std::wstring> folders{root};
    while (!folders.empty()) {
        const std::wstring folder = std::move(folders.back());
        folders.pop_back();
        RETURN_HR_IF_EXPECTED(HRESULT_FROM_WIN32(ERROR_CANCELLED), !looking(folder, files.size()));
        WIN32_FIND_DATAW data{};
        wil::unique_hfind find(FindFirstFileExW(joined(folder, L"*").c_str(), FindExInfoBasic, &data,
                                                FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH));
        if (!find) {
            const DWORD error = GetLastError();
            // a drive's root lists no "." or "..", so an empty one has no entries at all, which
            // FindFirstFileExW reports as "nothing matched": an empty folder, not a failure. a folder
            // that is not there is ERROR_PATH_NOT_FOUND.
            if (error == ERROR_FILE_NOT_FOUND) {
                continue;
            }
            RETURN_HR_IF(HRESULT_FROM_WIN32(error), folder == root);
            ++result.unreadable_folders;  // a subfolder this account may not open
            continue;
        }
        do {
            const std::wstring_view name = data.cFileName;
            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (recursive && name != L"." && name != L".." && !(data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                    folders.push_back(joined(folder, data.cFileName));
                }
                continue;
            }
            std::wstring extension = PathFindExtensionW(data.cFileName);
            CharLowerBuffW(extension.data(), static_cast<DWORD>(extension.size()));
            auto known = handled.find(extension);
            if (known == handled.end()) {
                known = handled.emplace(extension, effective_handler(extension) == SKYGGN_HANDLER_SKYGGN).first;
            }
            if (!known->second) {
                continue;
            }
            if (data.dwFileAttributes & kOnlineOnly) {
                ++result.online_only;
                continue;
            }
            files.push_back(joined(folder, data.cFileName));
        } while (FindNextFileW(find.get(), &data));
    }
    return S_OK;
}

outcome prepare_file(IThumbnailCache* cache, const std::wstring& path, bool force) {
    wil::com_ptr<IShellItem> item;
    if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item)))) {
        return outcome::none;
    }
    if (!force) {
        // a smaller one alone (WTS_LOWQUALITY) is not the one explorer's large views want
        wil::com_ptr<ISharedBitmap> kept;
        WTS_CACHEFLAGS flags = WTS_DEFAULT;
        if (SUCCEEDED(cache->GetThumbnail(item.get(), kPreparedSize, WTS_INCACHEONLY, &kept, &flags, nullptr)) &&
            !(flags & WTS_LOWQUALITY)) {
            return outcome::kept;
        }
        wil::com_ptr<ISharedBitmap> made;
        return SUCCEEDED(cache->GetThumbnail(item.get(), kPreparedSize, WTS_EXTRACT, &made, nullptr, nullptr))
                   ? outcome::made
                   : outcome::none;
    }
    bool made = true;
    for (const UINT size : kForcedSizes) {
        wil::com_ptr<ISharedBitmap> bitmap;
        made = SUCCEEDED(cache->GetThumbnail(item.get(), size, WTS_FORCEEXTRACTION, &bitmap, nullptr, nullptr)) && made;
    }
    // open explorer windows draw the file again
    SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATHW, path.c_str(), nullptr);
    return made ? outcome::made : outcome::none;
}

}  // namespace

HRESULT prepare_folder(const wchar_t* folder, bool recursive, bool force, skyggn_progress progress, void* context,
                       skyggn_prepare_result& result) {
    result = {};
    // the full path, with backslashes and without a "\\?\" prefix: the shell parses each file's path,
    // and turns down relative ones ("." from a prompt), forward slashes, ".." and that prefix, which
    // windows' path functions leave as it is; each file then counted as one without a picture
    std::wstring given = folder;
    RETURN_IF_FAILED(PathCchStripPrefix(given.data(), given.size() + 1));
    given.resize(wcslen(given.c_str()));
    std::wstring root;
    for (DWORD needed = MAX_PATH;;) {
        root.resize(needed);
        const DWORD got = GetFullPathNameW(given.c_str(), needed, root.data(), nullptr);
        RETURN_LAST_ERROR_IF(got == 0);
        if (got < needed) {
            root.resize(got);
            break;
        }
        needed = got;  // longer than the room given: asked again with room for it
    }
    while (root.size() > 3 && root.back() == L'\\') {
        root.pop_back();  // "D:\" stays as it is
    }
    std::vector<std::wstring> files;
    RETURN_IF_FAILED_EXPECTED(list_files(root, recursive, files, result, [&](const std::wstring& reading, size_t found) {
        return !progress || progress(context, 0, static_cast<UINT>(found), reading.c_str()) != FALSE;
    }));
    const auto total = static_cast<UINT>(files.size());

    std::mutex reporting;  // progress hears from one thread at a time
    std::atomic<size_t> next = 0;
    std::atomic<UINT> done = 0;
    std::atomic<UINT> made = 0;
    std::atomic<UINT> kept = 0;
    std::atomic<UINT> none = 0;
    std::atomic<bool> stopped = false;
    HRESULT failure = S_OK;  // written under `reporting`
    const auto report = [&](const wchar_t* file) {
        const std::scoped_lock lock(reporting);
        if (progress && !progress(context, done, total, file)) {
            stopped = true;
        }
    };
    report(L"");

    // each worker has com, in the multithreaded apartment, for its own thumbnail cache object; a
    // worker that cannot start stops the others, and its reason is the result
    const auto work = [&] {
        try {
            const HRESULT joined_com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            const auto leave_com = wil::scope_exit([&] {
                if (SUCCEEDED(joined_com)) {
                    CoUninitialize();
                }
            });
            THROW_IF_FAILED(joined_com);
            const auto cache = wil::CoCreateInstance<IThumbnailCache>(CLSID_LocalThumbnailCache);
            while (!stopped) {
                const size_t i = next++;
                if (i >= files.size()) {
                    break;
                }
                switch (prepare_file(cache.get(), files[i], force)) {
                case outcome::made:
                    ++made;
                    break;
                case outcome::kept:
                    ++kept;
                    break;
                case outcome::none:
                    ++none;
                    break;
                }
                ++done;
                report(files[i].c_str());
            }
        } catch (...) {
            const std::scoped_lock lock(reporting);
            failure = wil::ResultFromCaughtException();
            stopped = true;
        }
    };
    std::vector<std::thread> workers;
    HRESULT started = S_OK;
    try {
        for (size_t i = 0; i < std::min<size_t>(kWorkers, files.size()); ++i) {
            workers.emplace_back(work);
        }
    } catch (...) {
        // a thread windows would not start: the ones running finish their file and stop, and are
        // joined below, as a running thread must not be left behind
        started = wil::ResultFromCaughtException();
        stopped = true;
    }
    for (std::thread& worker : workers) {
        worker.join();
    }
    result.made = made;
    result.kept = kept;
    result.without_picture = none;
    RETURN_IF_FAILED(started);
    RETURN_IF_FAILED(failure);
    return stopped ? HRESULT_FROM_WIN32(ERROR_CANCELLED) : S_OK;
}

}  // namespace skyggn
