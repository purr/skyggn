#pragma once

#include <wrl/implements.h>
#include <wil/com.h>

#include <propsys.h>
#include <thumbcache.h>

namespace skyggn {

// the com class windows creates to get a thumbnail. it implements IInitializeWithStream, so windows
// runs it in a separate, isolated process: a broken file cannot take explorer down.
class __declspec(uuid("0aad7705-b228-4b77-9d4c-ee48ecebe3d4")) ThumbnailProvider final
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
                                          IInitializeWithStream,
                                          IThumbnailProvider> {
public:
    IFACEMETHODIMP Initialize(IStream* stream, DWORD mode) override;
    IFACEMETHODIMP GetThumbnail(UINT size, HBITMAP* bitmap, WTS_ALPHATYPE* alpha) override;

private:
    wil::com_ptr<IStream> stream_;
};

}  // namespace skyggn
