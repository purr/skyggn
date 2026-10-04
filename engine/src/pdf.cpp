#include "pdf.h"

#include "wic.h"

#include <shcore.h>
#include <wil/com.h>
#include <wil/resource.h>
#include <wil/result.h>
#include <windows.data.pdf.h>
#include <windows.storage.streams.h>
#include <wrl/event.h>
#include <wrl/wrappers/corewrappers.h>

#include <algorithm>
#include <chrono>
#include <cmath>

using ABI::Windows::Data::Pdf::IPdfDocument;
using ABI::Windows::Data::Pdf::IPdfDocumentStatics;
using ABI::Windows::Data::Pdf::IPdfPage;
using ABI::Windows::Data::Pdf::IPdfPageRenderOptions;
using ABI::Windows::Data::Pdf::PdfDocument;
using ABI::Windows::Foundation::AsyncStatus;
using ABI::Windows::Foundation::IAsyncAction;
using ABI::Windows::Foundation::IAsyncActionCompletedHandler;
using ABI::Windows::Foundation::IAsyncInfo;
using ABI::Windows::Foundation::IAsyncOperation;
using ABI::Windows::Foundation::IAsyncOperationCompletedHandler;
using ABI::Windows::Storage::Streams::IRandomAccessStream;
using Microsoft::WRL::Callback;
using Microsoft::WRL::FtmBase;
using Microsoft::WRL::Implements;
using Microsoft::WRL::RuntimeClassFlags;
using Microsoft::WRL::Wrappers::HStringReference;

namespace skyggn {

namespace {

// waits for an operation whose completion handler sets `done`; past the limit it is cancelled
HRESULT finish(IInspectable* operation, const wil::shared_event& done, const deadline& limit) {
    wil::com_ptr<IAsyncInfo> info;
    RETURN_IF_FAILED(operation->QueryInterface(IID_PPV_ARGS(&info)));
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(limit.end - std::chrono::steady_clock::now());
    // the pdf renderer reads the file from its own threads, through the stream windows handed to
    // this thread's apartment; a plain wait here would block those reads. this wait serves them.
    HANDLE handle = done.get();
    DWORD index = 0;
    const HRESULT waited = CoWaitForMultipleHandles(COWAIT_DISPATCH_CALLS, static_cast<DWORD>(std::max<int64_t>(left.count(), 0)),
                                                    1, &handle, &index);
    if (waited == RPC_S_CALLPENDING) {
        info->Cancel();
        return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    }
    RETURN_IF_FAILED(waited);
    AsyncStatus status = AsyncStatus::Started;
    RETURN_IF_FAILED(info->get_Status(&status));
    if (status == AsyncStatus::Completed) {
        return S_OK;
    }
    HRESULT error = E_FAIL;
    info->get_ErrorCode(&error);
    return FAILED(error) ? error : E_FAIL;
}

template <typename Instance>
HRESULT activate(const wchar_t* name, wil::com_ptr<Instance>& out) {
    wil::com_ptr<IInspectable> instance;
    RETURN_IF_FAILED(RoActivateInstance(HStringReference(name).Get(), &instance));
    return instance->QueryInterface(IID_PPV_ARGS(&out));
}

}  // namespace

HRESULT pdf_image(IStream* stream, UINT size, const deadline& limit, image& out) {
    wil::com_ptr<IRandomAccessStream> input;
    RETURN_IF_FAILED(CreateRandomAccessStreamOverStream(stream, BSOS_DEFAULT, IID_PPV_ARGS(&input)));
    wil::com_ptr<IPdfDocumentStatics> documents;
    RETURN_IF_FAILED(RoGetActivationFactory(HStringReference(RuntimeClass_Windows_Data_Pdf_PdfDocument).Get(),
                                            IID_PPV_ARGS(&documents)));

    // the handlers are agile and only set an event, so they run on any thread; the event is shared
    // with them, since one may still fire after a timeout returned
    wil::shared_event loaded;
    loaded.create(wil::EventOptions::ManualReset);
    wil::com_ptr<IAsyncOperation<PdfDocument*>> loading;
    RETURN_IF_FAILED(documents->LoadFromStreamAsync(input.get(), &loading));
    auto on_loaded = Callback<Implements<RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
                                         IAsyncOperationCompletedHandler<PdfDocument*>, FtmBase>>(
        [loaded](IAsyncOperation<PdfDocument*>*, AsyncStatus) {
            loaded.SetEvent();
            return S_OK;
        });
    RETURN_IF_NULL_ALLOC(on_loaded.Get());
    RETURN_IF_FAILED(loading->put_Completed(on_loaded.Get()));
    // a damaged or password-protected pdf fails here
    RETURN_IF_FAILED_EXPECTED(finish(loading.get(), loaded, limit));
    wil::com_ptr<IPdfDocument> document;
    RETURN_IF_FAILED_EXPECTED(loading->GetResults(&document));
    UINT32 pages = 0;
    RETURN_IF_FAILED(document->get_PageCount(&pages));
    RETURN_HR_IF_EXPECTED(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), pages == 0);
    wil::com_ptr<IPdfPage> page;
    RETURN_IF_FAILED(document->GetPage(0, &page));

    // drawn at thumbnail size, so a huge page costs no more than a small one
    ABI::Windows::Foundation::Size page_size{};
    RETURN_IF_FAILED(page->get_Size(&page_size));
    RETURN_HR_IF_EXPECTED(HRESULT_FROM_WIN32(ERROR_INVALID_DATA), page_size.Width <= 0 || page_size.Height <= 0);
    const double scale = static_cast<double>(size) / std::max(page_size.Width, page_size.Height);
    wil::com_ptr<IPdfPageRenderOptions> options;
    RETURN_IF_FAILED(activate(RuntimeClass_Windows_Data_Pdf_PdfPageRenderOptions, options));
    RETURN_IF_FAILED(options->put_DestinationWidth(std::max(1u, static_cast<UINT32>(std::lround(page_size.Width * scale)))));
    RETURN_IF_FAILED(options->put_DestinationHeight(std::max(1u, static_cast<UINT32>(std::lround(page_size.Height * scale)))));

    wil::com_ptr<IRandomAccessStream> drawn;
    RETURN_IF_FAILED(activate(RuntimeClass_Windows_Storage_Streams_InMemoryRandomAccessStream, drawn));
    wil::shared_event rendered;
    rendered.create(wil::EventOptions::ManualReset);
    wil::com_ptr<IAsyncAction> rendering;
    RETURN_IF_FAILED(page->RenderWithOptionsToStreamAsync(drawn.get(), options.get(), &rendering));
    auto on_rendered = Callback<Implements<RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IAsyncActionCompletedHandler, FtmBase>>(
        [rendered](IAsyncAction*, AsyncStatus) {
            rendered.SetEvent();
            return S_OK;
        });
    RETURN_IF_NULL_ALLOC(on_rendered.Get());
    RETURN_IF_FAILED(rendering->put_Completed(on_rendered.Get()));
    RETURN_IF_FAILED_EXPECTED(finish(rendering.get(), rendered, limit));

    // the page arrives as a png, which windows' own codec turns into pixels
    wil::com_ptr<IStream> png;
    RETURN_IF_FAILED(CreateStreamOverRandomAccessStream(drawn.get(), IID_PPV_ARGS(&png)));
    RETURN_IF_FAILED(png->Seek({}, STREAM_SEEK_SET, nullptr));
    return wic_image(png.get(), size, L".png", out);
}

}  // namespace skyggn
