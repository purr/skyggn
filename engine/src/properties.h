#pragma once

#include <wil/com.h>
#include <wrl/implements.h>

#include <propsys.h>

namespace skyggn {

// explorer reads a file's details (length, frame size, artist...) through this class, inside its
// own process: windows loads property handlers in-process. so this class only passes the file on to
// PropertyReader, which com runs in a separate process (dllhost.exe), and keeps the answer. a file
// that crashes or hangs the reader costs explorer that file's details, nothing more. it never
// touches ffmpeg, whose dlls therefore never load into explorer.
class __declspec(uuid("3e0200f0-2133-41dc-bd8f-40dd1bfcd623")) PropertyHandler final
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
                                          IInitializeWithStream,
                                          IPropertyStore,
                                          IPropertyStoreCapabilities> {
public:
    IFACEMETHODIMP Initialize(IStream* stream, DWORD mode) override;
    IFACEMETHODIMP GetCount(DWORD* count) override;
    IFACEMETHODIMP GetAt(DWORD index, PROPERTYKEY* key) override;
    IFACEMETHODIMP GetValue(REFPROPERTYKEY key, PROPVARIANT* variant) override;
    IFACEMETHODIMP SetValue(REFPROPERTYKEY key, REFPROPVARIANT value) override;
    IFACEMETHODIMP Commit() override;
    IFACEMETHODIMP IsPropertyWritable(REFPROPERTYKEY key) override;

private:
    wil::com_ptr<IPropertyStore> values_;
};

// reads a file's details with ffmpeg. PropertyHandler creates it in a com surrogate process.
class __declspec(uuid("78d5ef0b-2a55-4112-9f89-757e1260b5b2")) PropertyReader final
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
                                          IInitializeWithStream,
                                          IPropertyStore> {
public:
    IFACEMETHODIMP Initialize(IStream* stream, DWORD mode) override;
    IFACEMETHODIMP GetCount(DWORD* count) override;
    IFACEMETHODIMP GetAt(DWORD index, PROPERTYKEY* key) override;
    IFACEMETHODIMP GetValue(REFPROPERTYKEY key, PROPVARIANT* variant) override;
    IFACEMETHODIMP SetValue(REFPROPERTYKEY key, REFPROPVARIANT value) override;
    IFACEMETHODIMP Commit() override;

private:
    wil::com_ptr<IPropertyStore> values_;
};

// a file's details read in this process, for skyggnctl; isolated reads them the way explorer does,
// through PropertyHandler and a separate process
HRESULT details_of(const wchar_t* path, bool isolated, IPropertyStore** store);

}  // namespace skyggn
