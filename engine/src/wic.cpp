#include "wic.h"

#include <propvarutil.h>
#include <wincodec.h>
#include <wil/com.h>
#include <wil/resource.h>
#include <wil/result.h>

#include <algorithm>
#include <cmath>

namespace skyggn {

namespace {

// exif orientation (1-8) as a clockwise rotation; the mirrored orientations keep their rotation part
int rotation_of_orientation(USHORT orientation) {
    switch (orientation) {
    case 3:
    case 4:
        return 180;
    case 5:
    case 6:
        return 90;
    case 7:
    case 8:
        return 270;
    default:
        return 0;
    }
}

// jpeg keeps exif in its app1 segment, tiff in its first ifd. a picture without the tag is upright.
int rotation_of(IWICBitmapFrameDecode* frame) {
    wil::com_ptr<IWICMetadataQueryReader> metadata;
    if (FAILED(frame->GetMetadataQueryReader(&metadata))) {
        return 0;
    }
    for (const wchar_t* query : {L"/app1/ifd/{ushort=274}", L"/ifd/{ushort=274}"}) {
        wil::unique_prop_variant value;
        if (SUCCEEDED(metadata->GetMetadataByName(query, &value)) && value.vt == VT_UI2) {
            return rotation_of_orientation(value.uiVal);
        }
    }
    return 0;
}

// icons hold one picture per size; the largest makes the best thumbnail
HRESULT best_frame(IWICBitmapDecoder* decoder, std::wstring_view extension, wil::com_ptr<IWICBitmapFrameDecode>& frame) {
    UINT count = 0;
    RETURN_IF_FAILED(decoder->GetFrameCount(&count));
    RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_DATA), count == 0);
    const bool icon = CompareStringOrdinal(extension.data(), static_cast<int>(extension.size()), L".ico", -1, TRUE) ==
                      CSTR_EQUAL;
    UINT best = 0;
    UINT best_area = 0;
    for (UINT i = 0; icon && i < count; ++i) {
        wil::com_ptr<IWICBitmapFrameDecode> candidate;
        UINT width = 0;
        UINT height = 0;
        if (SUCCEEDED(decoder->GetFrame(i, &candidate)) && SUCCEEDED(candidate->GetSize(&width, &height)) &&
            width * height > best_area) {
            best = i;
            best_area = width * height;
        }
    }
    return decoder->GetFrame(best, &frame);
}

}  // namespace

HRESULT wic_image(IStream* stream, UINT size, std::wstring_view extension, image& out) {
    wil::com_ptr<IWICImagingFactory> factory;
    RETURN_IF_FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)));
    wil::com_ptr<IWICBitmapDecoder> decoder;
    RETURN_IF_FAILED_EXPECTED(factory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnDemand, &decoder));
    wil::com_ptr<IWICBitmapFrameDecode> frame;
    RETURN_IF_FAILED(best_frame(decoder.get(), extension, frame));

    UINT width = 0;
    UINT height = 0;
    RETURN_IF_FAILED(frame->GetSize(&width, &height));
    RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_DATA), width == 0 || height == 0);
    const int rotation = rotation_of(frame.get());
    const double scale = fit_scale(width, height, rotation, size);
    const UINT scaled_width = std::max(1u, static_cast<UINT>(std::lround(width * scale)));
    const UINT scaled_height = std::max(1u, static_cast<UINT>(std::lround(height * scale)));

    // the scaler lets jpeg decode straight at a fraction of its size
    wil::com_ptr<IWICBitmapScaler> scaler;
    RETURN_IF_FAILED(factory->CreateBitmapScaler(&scaler));
    RETURN_IF_FAILED(scaler->Initialize(frame.get(), scaled_width, scaled_height, WICBitmapInterpolationModeFant));
    wil::com_ptr<IWICFormatConverter> converter;
    RETURN_IF_FAILED(factory->CreateFormatConverter(&converter));
    RETURN_IF_FAILED(converter->Initialize(scaler.get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
                                           nullptr, 0.0, WICBitmapPaletteTypeMedianCut));

    image picture{static_cast<int>(scaled_width), static_cast<int>(scaled_height), {}};
    picture.pixels.resize(static_cast<size_t>(scaled_width) * scaled_height);
    RETURN_IF_FAILED(converter->CopyPixels(nullptr, scaled_width * 4,
                                           static_cast<UINT>(picture.pixels.size() * sizeof(uint32_t)),
                                           reinterpret_cast<BYTE*>(picture.pixels.data())));
    picture.transparent = std::ranges::any_of(picture.pixels, [](uint32_t pixel) { return (pixel >> 24) != 0xff; });
    out = rotate(picture, rotation);
    return S_OK;
}

}  // namespace skyggn
