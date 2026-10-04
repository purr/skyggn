#include "properties.h"

#include "details.h"
#include "media_input.h"
#include "settings.h"

#include <propvarutil.h>
#include <shlwapi.h>
#include <wil/resource.h>
#include <wil/result.h>
#include <wrl/module.h>

#include <chrono>
#include <string>

namespace skyggn {

namespace {

HRESULT copy_values(IPropertyStore* from, wil::com_ptr<IPropertyStore>& to) {
    wil::com_ptr<IPropertyStore> copy;
    RETURN_IF_FAILED(PSCreateMemoryPropertyStore(IID_PPV_ARGS(&copy)));
    DWORD count = 0;
    RETURN_IF_FAILED(from->GetCount(&count));
    for (DWORD index = 0; index < count; ++index) {
        PROPERTYKEY key{};
        RETURN_IF_FAILED(from->GetAt(index, &key));
        wil::unique_prop_variant value;
        RETURN_IF_FAILED(from->GetValue(key, &value));
        RETURN_IF_FAILED(copy->SetValue(key, value));
    }
    to = std::move(copy);
    return S_OK;
}

HRESULT check_initialize(const wil::com_ptr<IPropertyStore>& values, IStream* stream, DWORD mode) {
    RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED), values != nullptr);
    RETURN_HR_IF_NULL(E_INVALIDARG, stream);
    // the details are read-only; asked for write access, explorer would offer to edit them
    RETURN_HR_IF(STG_E_ACCESSDENIED, (mode & STGM_READWRITE) != 0);
    return S_OK;
}

}  // namespace

IFACEMETHODIMP PropertyHandler::Initialize(IStream* stream, DWORD mode) try {
    RETURN_IF_FAILED(check_initialize(values_, stream, mode));
    // CLSCTX_LOCAL_SERVER: com starts the reader in a surrogate process, as its registration says
    wil::com_ptr<IInitializeWithStream> reader;
    RETURN_IF_FAILED(CoCreateInstance(__uuidof(PropertyReader), nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&reader)));
    RETURN_IF_FAILED_EXPECTED(reader->Initialize(stream, STGM_READ));
    // copied, so the reader and its process can go as soon as it is released
    return copy_values(reader.query<IPropertyStore>().get(), values_);
}
CATCH_RETURN()

IFACEMETHODIMP PropertyHandler::GetCount(DWORD* count) {
    RETURN_HR_IF(E_UNEXPECTED, values_ == nullptr);
    return values_->GetCount(count);
}

IFACEMETHODIMP PropertyHandler::GetAt(DWORD index, PROPERTYKEY* key) {
    RETURN_HR_IF(E_UNEXPECTED, values_ == nullptr);
    return values_->GetAt(index, key);
}

IFACEMETHODIMP PropertyHandler::GetValue(REFPROPERTYKEY key, PROPVARIANT* variant) {
    RETURN_HR_IF(E_UNEXPECTED, values_ == nullptr);
    return values_->GetValue(key, variant);
}

IFACEMETHODIMP PropertyHandler::SetValue(REFPROPERTYKEY, REFPROPVARIANT) {
    return STG_E_ACCESSDENIED;
}

IFACEMETHODIMP PropertyHandler::Commit() {
    return STG_E_ACCESSDENIED;
}

IFACEMETHODIMP PropertyHandler::IsPropertyWritable(REFPROPERTYKEY) {
    return S_FALSE;
}

IFACEMETHODIMP PropertyReader::Initialize(IStream* stream, DWORD mode) try {
    RETURN_IF_FAILED(check_initialize(values_, stream, mode));
    const settings options = load_settings();
    const deadline limit{std::chrono::steady_clock::now() + std::chrono::milliseconds(options.time_limit_ms)};
    const std::wstring name = stream_name(stream);
    wil::com_ptr<IPropertyStore> values;
    RETURN_IF_FAILED(PSCreateMemoryPropertyStore(IID_PPV_ARGS(&values)));
    RETURN_IF_FAILED_EXPECTED(read_details(stream, PathFindExtensionW(name.c_str()), limit, values.get()));
    values_ = std::move(values);
    return S_OK;
}
CATCH_RETURN()

IFACEMETHODIMP PropertyReader::GetCount(DWORD* count) {
    RETURN_HR_IF(E_UNEXPECTED, values_ == nullptr);
    return values_->GetCount(count);
}

IFACEMETHODIMP PropertyReader::GetAt(DWORD index, PROPERTYKEY* key) {
    RETURN_HR_IF(E_UNEXPECTED, values_ == nullptr);
    return values_->GetAt(index, key);
}

IFACEMETHODIMP PropertyReader::GetValue(REFPROPERTYKEY key, PROPVARIANT* variant) {
    RETURN_HR_IF(E_UNEXPECTED, values_ == nullptr);
    return values_->GetValue(key, variant);
}

IFACEMETHODIMP PropertyReader::SetValue(REFPROPERTYKEY, REFPROPVARIANT) {
    return STG_E_ACCESSDENIED;
}

IFACEMETHODIMP PropertyReader::Commit() {
    return STG_E_ACCESSDENIED;
}

HRESULT details_of(const wchar_t* path, bool isolated, IPropertyStore** store) try {
    RETURN_HR_IF_NULL(E_POINTER, store);
    *store = nullptr;
    RETURN_HR_IF_NULL(E_INVALIDARG, path);
    wil::com_ptr<IStream> stream;
    RETURN_IF_FAILED(
        SHCreateStreamOnFileEx(path, STGM_READ | STGM_SHARE_DENY_NONE, FILE_ATTRIBUTE_NORMAL, FALSE, nullptr, &stream));
    wil::com_ptr<IInitializeWithStream> handler;
    if (isolated) {
        // as explorer does it: the registered handler class, which passes the file to its reader
        RETURN_IF_FAILED(
            CoCreateInstance(__uuidof(PropertyHandler), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&handler)));
    } else {
        const auto reader = Microsoft::WRL::Make<PropertyReader>();
        RETURN_IF_NULL_ALLOC(reader.Get());
        RETURN_IF_FAILED(reader.CopyTo(IID_PPV_ARGS(&handler)));
    }
    RETURN_IF_FAILED_EXPECTED(handler->Initialize(stream.get(), STGM_READ));
    return handler->QueryInterface(IID_PPV_ARGS(store));
}
CATCH_RETURN()

CoCreatableClass(PropertyHandler);
CoCreatableClass(PropertyReader);

}  // namespace skyggn
