#pragma once

#include <skyggn/skyggn.h>

namespace skyggn {

// has windows make and keep the thumbnails of a folder's files, the way file explorer asks for
// them (skyggn_prepare_folder)
HRESULT prepare_folder(const wchar_t* folder, bool recursive, bool force, skyggn_progress progress, void* context,
                       skyggn_prepare_result& result);

}  // namespace skyggn
