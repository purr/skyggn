#include "provider.h"

#include "decoder.h"
#include "settings.h"

#include <wil/result.h>
#include <wrl/module.h>

namespace skyggn {

IFACEMETHODIMP ThumbnailProvider::Initialize(IStream* stream, DWORD) {
    RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED), stream_ != nullptr);
    RETURN_HR_IF_NULL(E_INVALIDARG, stream);
    stream_ = stream;
    return S_OK;
}

IFACEMETHODIMP ThumbnailProvider::GetThumbnail(UINT size, HBITMAP* bitmap, WTS_ALPHATYPE* alpha) try {
    RETURN_HR_IF_NULL(E_POINTER, bitmap);
    RETURN_HR_IF_NULL(E_POINTER, alpha);
    *bitmap = nullptr;
    *alpha = WTSAT_UNKNOWN;
    RETURN_HR_IF(E_UNEXPECTED, stream_ == nullptr);

    bool transparent = false;
    RETURN_IF_FAILED(make_thumbnail(stream_.get(), size, load_settings(), bitmap, &transparent));
    *alpha = transparent ? WTSAT_ARGB : WTSAT_RGB;
    return S_OK;
}
CATCH_RETURN()

CoCreatableClass(ThumbnailProvider);

}  // namespace skyggn
