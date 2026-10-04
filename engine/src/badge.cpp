#include "badge.h"

#include "ffmpeg_loader.h"
#include "resource_ids.h"
#include "text.h"

#include <d2d1_3.h>
#include <d2d1effects.h>
#include <dwrite_1.h>
#include <shlwapi.h>
#include <wincodec.h>
#include <wil/com.h>
#include <wil/result.h>

#include <algorithm>
#include <cmath>
#include <mutex>
#include <string>
#include <vector>

namespace skyggn {

namespace {

// thumbnails smaller than this get no badge: it would cover too much of them
constexpr UINT kSmallestSize = 48;
// the transparent room around the picture, as a share of the badge's height. the badge hangs out
// into it past the picture's corner, short of the thumbnail's edge (shadow_margin).
constexpr float kOverhang = 0.25f;
// the labelled style shows its text from this thumbnail size up
constexpr UINT kLabelFrom = 128;
constexpr float kLabelFontShare = 0.38f;  // of the badge height
// a tile without a picture names a proven damage under its file type from this tile size up
constexpr float kReasonFrom = 120.0f;

constexpr D2D1_COLOR_F rgba(uint32_t rgb, float alpha) {
    return {((rgb >> 16) & 0xff) / 255.0f, ((rgb >> 8) & 0xff) / 255.0f, (rgb & 0xff) / 255.0f, alpha};
}

constexpr uint32_t kGlass = 0x121216;
constexpr uint32_t kWhite = 0xffffff;
constexpr uint32_t kBlack = 0x000000;
// the mark on the badge of a file proven damaged that still gave a picture
constexpr uint32_t kWarning = 0xf2b01e;
constexpr uint32_t kWarningInk = 0x2b1d00;

uint32_t category_rgb(skyggn_category category) {
    switch (category) {
    case SKYGGN_CATEGORY_AUDIO:
        return 0xc239b3;
    case SKYGGN_CATEGORY_IMAGE:
    case SKYGGN_CATEGORY_RAW:
        return 0x0e9b78;
    case SKYGGN_CATEGORY_BOOK:
        return 0xca5010;
    case SKYGGN_CATEGORY_DOCUMENT:
        return 0x0078d4;
    default:
        return 0x4f6bed;
    }
}

float srgb_gamma(float linear) {
    return linear <= 0.0031308f ? 12.92f * linear : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
}

// a colour in oklch, the space where equal steps look equally far apart. a hue the screen cannot
// show at that chroma loses chroma until it fits.
uint32_t oklch(float lightness, float chroma, float hue) {
    for (float c = chroma;; c -= 0.005f) {
        const float a = c * std::cos(hue);
        const float b = c * std::sin(hue);
        const float l = std::pow(lightness + 0.3963377774f * a + 0.2158037573f * b, 3.0f);
        const float m = std::pow(lightness - 0.1055613458f * a - 0.0638541728f * b, 3.0f);
        const float s = std::pow(lightness - 0.0894841775f * a - 1.2914855480f * b, 3.0f);
        const float rgb[] = {4.0767416621f * l - 3.3077115913f * m + 0.2309699292f * s,
                             -1.2684380046f * l + 2.6097574011f * m - 0.3413193965f * s,
                             -0.0041960863f * l - 0.7034186147f * m + 1.7076147010f * s};
        const bool fits = std::ranges::all_of(rgb, [](float v) { return v >= 0.0f && v <= 1.0f; });
        if (fits || c <= 0.0f) {
            uint32_t packed = 0;
            for (float v : rgb) {
                packed = (packed << 8) |
                         static_cast<uint32_t>(std::lround(std::clamp(srgb_gamma(std::clamp(v, 0.0f, 1.0f)), 0.0f, 1.0f) * 255));
            }
            return packed;
        }
    }
}

uint32_t blend(uint32_t rgb, uint32_t toward, float amount) {
    uint32_t result = 0;
    for (int shift = 0; shift < 24; shift += 8) {
        const float from = static_cast<float>((rgb >> shift) & 0xff);
        const float to = static_cast<float>((toward >> shift) & 0xff);
        result |= static_cast<uint32_t>(std::lround(from + (to - from) * amount)) << shift;
    }
    return result;
}

// a tile's gradient: its two colours and the way it runs, between two points on a tile of size 1
struct tile_colours {
    uint32_t start;
    uint32_t end;
    D2D1_POINT_2F from{0, 0};
    D2D1_POINT_2F to{1, 1};
};

// the eight ways a gradient can run: from each corner, and from the middle of each side, across
constexpr D2D1_POINT_2F kDirections[8][2] = {
    {{0, 0}, {1, 1}}, {{0.5f, 0}, {0.5f, 1}}, {{1, 0}, {0, 1}}, {{1, 0.5f}, {0, 0.5f}},
    {{1, 1}, {0, 0}}, {{0.5f, 1}, {0.5f, 0}}, {{0, 1}, {1, 0}}, {{0, 0.5f}, {1, 0.5f}},
};

// the kind's colour, lighter to darker
tile_colours kind_colours(skyggn_category category) {
    const uint32_t rgb = category_rgb(category);
    return {blend(rgb, kWhite, 0.18f), blend(rgb, kBlack, 0.22f)};
}

// a gradient from a fingerprint, which sets five things apart: one hue anywhere on the wheel, the
// other 45 to 135 degrees on either side of it, how light and how colourful it is, and which of
// eight ways it runs, light to dark. two different files rarely match in all five, so tiles that
// look the same are copies (or songs of one album). every tile stays mid-light, so the white
// symbol stays readable and no tile is near black or white.
tile_colours seed_colours(uint64_t seed) {
    constexpr float kDegree = 6.28318530718f / 360;
    // `bits` bits of the seed from `shift` up, as a share from 0 to 1
    const auto share = [seed](int shift, int bits) {
        const uint64_t most = (uint64_t{1} << bits) - 1;
        return static_cast<float>((seed >> shift) & most) / static_cast<float>(most);
    };
    const auto hue = static_cast<float>(static_cast<double>(seed >> 32) / 4294967296.0) * 360;
    const float offset = 45 + share(8, 8) * 90;
    const float other = hue + ((seed & 1) ? offset : -offset);
    const float lightness = 0.62f + share(19, 4) * 0.1f;
    const float chroma = 0.10f + share(23, 4) * 0.05f;
    const auto& way = kDirections[(seed >> 16) & 7];
    return {oklch(lightness, chroma, hue * kDegree), oklch(lightness - 0.16f, chroma + 0.01f, other * kDegree), way[0],
            way[1]};
}

int glyph_resource(skyggn_category category) {
    switch (category) {
    case SKYGGN_CATEGORY_AUDIO:
        return IDR_BADGE_AUDIO;
    case SKYGGN_CATEGORY_IMAGE:
    case SKYGGN_CATEGORY_RAW:
        return IDR_BADGE_IMAGE;
    case SKYGGN_CATEGORY_BOOK:
        return IDR_BADGE_BOOK;
    case SKYGGN_CATEGORY_DOCUMENT:
        return IDR_BADGE_DOCUMENT;
    default:
        return IDR_BADGE_VIDEO;
    }
}

HRESULT load_glyph(ID2D1DeviceContext5* context, skyggn_category category, float size,
                   wil::com_ptr<ID2D1SvgDocument>& glyph) {
    HRSRC resource = FindResourceW(engine_module(), MAKEINTRESOURCEW(glyph_resource(category)), RT_RCDATA);
    RETURN_LAST_ERROR_IF_NULL(resource);
    HGLOBAL loaded = LoadResource(engine_module(), resource);
    RETURN_LAST_ERROR_IF_NULL(loaded);
    const auto* data = static_cast<const BYTE*>(LockResource(loaded));
    RETURN_HR_IF_NULL(E_UNEXPECTED, data);
    wil::com_ptr<IStream> stream;
    stream.attach(SHCreateMemStream(data, SizeofResource(engine_module(), resource)));
    RETURN_IF_NULL_ALLOC(stream.get());
    return context->CreateSvgDocument(stream.get(), D2D1::SizeF(size, size), &glyph);
}

// a line of semibold text, vertically centred in a box of the given size
HRESULT text_layout(const std::wstring& text, float font_size, float width, float height, bool centred,
                    wil::com_ptr<IDWriteTextLayout>& layout) {
    wil::com_ptr<IDWriteFactory> dwrite;
    RETURN_IF_FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                         reinterpret_cast<IUnknown**>(dwrite.put())));
    wil::com_ptr<IDWriteTextFormat> format;
    RETURN_IF_FAILED(dwrite->CreateTextFormat(L"Segoe UI Variable Text", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                              DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, font_size,
                                              L"en-us", &format));
    RETURN_IF_FAILED(format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER));
    if (centred) {
        RETURN_IF_FAILED(format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER));
    }
    RETURN_IF_FAILED(dwrite->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), format.get(), width,
                                              height, &layout));
    if (auto spaced = layout.try_query<IDWriteTextLayout1>()) {
        spaced->SetCharacterSpacing(0, font_size * 0.05f, 0, {0, static_cast<UINT32>(text.size())});
    }
    return S_OK;
}

std::wstring label_of(std::wstring_view extension) {
    std::wstring label(extension.starts_with(L'.') ? extension.substr(1) : extension);
    CharUpperBuffW(label.data(), static_cast<DWORD>(label.size()));
    return label;
}

// a direct2d drawing surface over an image's own pixels
struct surface {
    wil::com_ptr<IWICBitmap> bitmap;
    wil::com_ptr<ID2D1DeviceContext5> context;
};

// one direct2d factory serves every thumbnail the process makes: with a new one each time, setting
// up its software renderer took longer than drawing the badge (8 of the 28 ms a badge took). it is
// multithreaded, as thumbnails can be made on several threads at once (the settings app's
// previews). a raw pointer, so nothing releases it while the dll unloads, under the loader lock,
// where direct2d must not run: com's unload releases it first (release_drawing_resources), and
// the end of the process takes it otherwise.
std::mutex g_factory_lock;
ID2D1Factory* g_factory = nullptr;

HRESULT shared_factory(wil::com_ptr<ID2D1Factory>& out) {
    const std::scoped_lock lock(g_factory_lock);
    if (!g_factory) {
        RETURN_IF_FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED, __uuidof(ID2D1Factory), nullptr,
                                           reinterpret_cast<void**>(&g_factory)));
    }
    out = g_factory;
    return S_OK;
}

HRESULT open_surface(image& picture, surface& out) {
    wil::com_ptr<ID2D1Factory> d2d;
    RETURN_IF_FAILED(shared_factory(d2d));
    wil::com_ptr<IWICImagingFactory> wic;
    RETURN_IF_FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)));
    const UINT stride = static_cast<UINT>(picture.width) * 4;
    RETURN_IF_FAILED(wic->CreateBitmapFromMemory(picture.width, picture.height, GUID_WICPixelFormat32bppPBGRA, stride,
                                                 stride * static_cast<UINT>(picture.height),
                                                 reinterpret_cast<BYTE*>(picture.pixels.data()), &out.bitmap));
    wil::com_ptr<ID2D1RenderTarget> target;
    RETURN_IF_FAILED(d2d->CreateWicBitmapRenderTarget(
        out.bitmap.get(),
        D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                                     D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)),
        &target));
    RETURN_IF_FAILED(target->QueryInterface(IID_PPV_ARGS(&out.context)));
    out.context->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    return S_OK;
}

// copies what was drawn back into the image
HRESULT close_surface(surface& drawn, image& picture) {
    const UINT stride = static_cast<UINT>(picture.width) * 4;
    return drawn.bitmap->CopyPixels(nullptr, stride, stride * static_cast<UINT>(picture.height),
                                    reinterpret_cast<BYTE*>(picture.pixels.data()));
}

// the picture in the middle of a canvas with the same transparent room on every side
image with_room(const image& picture, int room) {
    image canvas{picture.width + 2 * room, picture.height + 2 * room, {}, true};
    canvas.pixels.assign(static_cast<size_t>(canvas.width) * canvas.height, 0);
    for (int y = 0; y < picture.height; ++y) {
        std::copy_n(&picture.pixels[static_cast<size_t>(y) * picture.width], picture.width,
                    &canvas.pixels[static_cast<size_t>(y + room) * canvas.width + room]);
    }
    return canvas;
}

// three box blur passes approximate a gaussian blur: the frosted glass behind a badge. each pass
// slides its window along the line, adding the pixel that comes in and taking off the one that
// leaves, so its cost does not grow with the radius. summing the whole window for every pixel took
// half a second for the badge of a 1280 px thumbnail, the size windows asks for to fill its cache.
void blur(std::vector<uint32_t>& pixels, int width, int height, int radius) {
    std::vector<uint32_t> scratch(pixels.size());
    const auto pass = [&](const std::vector<uint32_t>& in, std::vector<uint32_t>& out, bool horizontal) {
        const int lines = horizontal ? height : width;
        const int length = horizontal ? width : height;
        for (int line = 0; line < lines; ++line) {
            const auto at = [&](int i) {
                return horizontal ? static_cast<size_t>(line) * width + i : static_cast<size_t>(i) * width + line;
            };
            uint32_t sum[4] = {};
            const auto take = [&](int i, bool in_window) {
                const uint32_t pixel = in[at(i)];
                for (int c = 0; c < 4; ++c) {
                    const uint32_t value = (pixel >> (c * 8)) & 0xff;
                    sum[c] = in_window ? sum[c] + value : sum[c] - value;
                }
            };
            int first = 0;  // the window, [first, last], of the pixel before
            int last = -1;
            for (int i = 0; i < length; ++i) {
                for (; last < std::min(length - 1, i + radius); ++last) {
                    take(last + 1, true);
                }
                for (; first < std::max(0, i - radius); ++first) {
                    take(first, false);
                }
                const uint32_t count = static_cast<uint32_t>(last - first + 1);
                uint32_t result = 0;
                for (int c = 0; c < 4; ++c) {
                    result |= ((sum[c] + count / 2) / count) << (c * 8);
                }
                out[at(i)] = result;
            }
        }
    };
    for (int round = 0; round < 3; ++round) {
        pass(pixels, scratch, true);
        pass(scratch, pixels, false);
    }
}

// the average colour of the picture's opaque pixels under a rectangle; the glass part hanging
// outside the picture shows it, so the badge reads as one piece
uint32_t average_colour(const image& canvas, int left, int top, int right, int bottom) {
    uint64_t sum[3] = {};
    uint64_t count = 0;
    for (int y = std::max(0, top); y < std::min(canvas.height, bottom); ++y) {
        for (int x = std::max(0, left); x < std::min(canvas.width, right); ++x) {
            const uint32_t pixel = canvas.pixels[static_cast<size_t>(y) * canvas.width + x];
            if ((pixel >> 24) != 0xff) {
                continue;
            }
            for (int c = 0; c < 3; ++c) {
                sum[c] += (pixel >> (c * 8)) & 0xff;
            }
            ++count;
        }
    }
    if (count == 0) {
        return 0xff808080;
    }
    return 0xff000000u | static_cast<uint32_t>(sum[2] / count) << 16 | static_cast<uint32_t>(sum[1] / count) << 8 |
           static_cast<uint32_t>(sum[0] / count);
}

struct badge_plan {
    bool shown = false;
    float height = 0;
    float width = 0;  // wider than high for the labelled style, which grows to the left
    float glyph = 0;  // symbol size
    float text_width = 0;
    std::wstring label;  // the extension, for the labelled style
    UINT room = 0;  // transparent pixels on each side of the picture
};

// the badge's share of the thumbnail size, whatever the picture's shape
float share_of(const badge_look& look) {
    return look.size_percent / 100.0f;
}

float badge_height(UINT size, const badge_look& look) {
    return std::round(size * share_of(look));
}

UINT room_for(float height) {
    return static_cast<UINT>(std::ceil(height * kOverhang));
}

// the badge's shadow: a soft blur, dropped a little. the badge keeps `shadow_margin` from the
// thumbnail's edges, inside its room, so the shadow fades out within the thumbnail; a badge flush
// with the edge had its shadow cut off there in a hard line.
float shadow_blur(float height) {
    return std::max(0.5f, height / 40);
}

float shadow_drop(float height) {
    return height / 60;
}

float shadow_margin(float height) {
    return std::ceil(shadow_drop(height) + 2.5f * shadow_blur(height));
}

// the words for a proven damage (skyggn_damage) under a tile's file type; 0 for none
int damage_words(skyggn_damage damage) {
    switch (damage) {
    case SKYGGN_DAMAGE_EMPTY:
        return IDS_DAMAGE_EMPTY;
    case SKYGGN_DAMAGE_INCOMPLETE:
        return IDS_DAMAGE_INCOMPLETE;
    case SKYGGN_DAMAGE_CORRUPTED:
        return IDS_DAMAGE_CORRUPTED;
    default:
        return 0;
    }
}

// the warning mark's radius, a share of the badge's height, and the width of its hairline edge
float mark_radius(float height) {
    return height * 0.21f;
}

float hairline(float height) {
    return std::max(1.0f, height / 48);
}

// the amber warning mark of a file proven damaged (damage.h): a disc with a hairline edge and an
// exclamation point, the same on a badge and on a tile
void draw_mark(ID2D1DeviceContext* context, ID2D1SolidColorBrush* brush, D2D1_POINT_2F centre, float height) {
    const float radius = mark_radius(height);
    const float line = hairline(height);
    brush->SetColor(rgba(kWarning, 1.0f));
    context->FillEllipse({centre, radius, radius}, brush);
    brush->SetColor(rgba(kWarningInk, 0.35f));
    context->DrawEllipse({centre, radius - line / 2, radius - line / 2}, brush, line);
    // the exclamation point: a rounded bar and a dot
    const float stroke = radius * 0.28f;
    brush->SetColor(rgba(kWarningInk, 1.0f));
    context->FillRoundedRectangle(
        {{centre.x - stroke / 2, centre.y - radius * 0.5f, centre.x + stroke / 2, centre.y + radius * 0.14f}, stroke / 2,
         stroke / 2},
        brush);
    context->FillEllipse({{centre.x, centre.y + radius * 0.5f}, stroke * 0.6f, stroke * 0.6f}, brush);
}

// the soft shadow of a badge or a mark (see shadow_blur): the shapes `fill` draws with the black
// brush it gets, recorded and blurred, for drawing `shadow_drop` lower
template <typename Fill>
HRESULT soft_shadow(ID2D1DeviceContext5* context, float height, const Fill& fill, wil::com_ptr<ID2D1Effect>& shadow) {
    wil::com_ptr<ID2D1SolidColorBrush> black;
    RETURN_IF_FAILED(context->CreateSolidColorBrush(rgba(kBlack, 1.0f), &black));
    wil::com_ptr<ID2D1CommandList> shapes;
    RETURN_IF_FAILED(context->CreateCommandList(&shapes));
    wil::com_ptr<ID2D1Image> target;
    context->GetTarget(&target);
    context->SetTarget(shapes.get());
    context->BeginDraw();
    fill(black.get());
    RETURN_IF_FAILED(context->EndDraw());
    RETURN_IF_FAILED(shapes->Close());
    context->SetTarget(target.get());
    RETURN_IF_FAILED(context->CreateEffect(CLSID_D2D1Shadow, &shadow));
    shadow->SetInput(0, shapes.get());
    RETURN_IF_FAILED(shadow->SetValue(D2D1_SHADOW_PROP_BLUR_STANDARD_DEVIATION, shadow_blur(height)));
    return shadow->SetValue(D2D1_SHADOW_PROP_COLOR, D2D1::Vector4F(0, 0, 0, 0.26f));
}

// the badge for a finished thumbnail of `size` (the picture plus its room). a label that would make
// the badge wider than `max_width` is left out.
HRESULT plan_badge(UINT size, std::wstring_view extension, const badge_look& look, float max_width,
                   badge_plan& plan) {
    plan = {};
    if (look.style == SKYGGN_BADGE_NONE || size < kSmallestSize) {
        return S_OK;
    }
    plan.shown = true;
    plan.height = badge_height(size, look);
    plan.width = plan.height;
    plan.glyph = std::round(plan.height * 0.56f);
    if (look.style == SKYGGN_BADGE_LABELLED && size >= kLabelFrom && !extension.empty()) {
        std::wstring label = label_of(extension);
        wil::com_ptr<IDWriteTextLayout> layout;
        RETURN_IF_FAILED(text_layout(label, plan.height * kLabelFontShare, 1000.0f, plan.height, false, layout));
        DWRITE_TEXT_METRICS metrics{};
        RETURN_IF_FAILED(layout->GetMetrics(&metrics));
        const float glyph = std::round(plan.height * 0.46f);
        const float width = std::round(plan.height * 0.3f + glyph + plan.height * 0.12f +
                                       metrics.widthIncludingTrailingWhitespace + plan.height * 0.36f);
        // a long type on a narrow picture (a portrait poster, a big badge) keeps only the symbol
        if (width <= max_width) {
            plan.label = std::move(label);
            plan.text_width = metrics.widthIncludingTrailingWhitespace;
            plan.glyph = glyph;
            plan.width = width;
        }
    }
    plan.room = room_for(plan.height);
    return S_OK;
}

}  // namespace

void release_drawing_resources() {
    const std::scoped_lock lock(g_factory_lock);
    if (g_factory) {
        g_factory->Release();
        g_factory = nullptr;
    }
}

UINT badge_room(UINT size, const badge_look& look) {
    return look.style == SKYGGN_BADGE_NONE || size < kSmallestSize ? 0 : room_for(badge_height(size, look));
}

HRESULT draw_badge(image& picture, UINT size, std::wstring_view extension, skyggn_category category,
                   const badge_look& look, bool warned) {
    const skyggn_badge_style style = look.style;
    // the finished thumbnail's size, from the picture's: the room is a fixed share of it, so
    // long side = picture + 2 * overhang * share * long side. a picture that filled its space gives
    // back the size windows asked for; a smaller one (not enlarged) a smaller thumbnail.
    const int long_side = std::max(picture.width, picture.height);
    const auto finished = std::min(
        size, static_cast<UINT>(std::lround(long_side / (1.0f - 2 * kOverhang * share_of(look)))));
    badge_plan plan;
    // the badge may reach across the picture into the room on the far side, no further
    const float height = badge_height(finished, look);
    const float max_width = static_cast<float>(picture.width + room_for(height)) - shadow_margin(height);
    RETURN_IF_FAILED(plan_badge(finished, extension, look, max_width, plan));
    if (!plan.shown || picture.width < plan.height || picture.height < plan.height) {
        return S_OK;  // too small a picture to carry a badge
    }
    image canvas = with_room(picture, static_cast<int>(plan.room));
    // over the picture's corner, reaching into the room short of the edge, where its shadow fades;
    // a wide label grows towards the middle
    const bool on_left = look.corner == SKYGGN_CORNER_BOTTOM_LEFT || look.corner == SKYGGN_CORNER_TOP_LEFT;
    const bool on_top = look.corner == SKYGGN_CORNER_TOP_RIGHT || look.corner == SKYGGN_CORNER_TOP_LEFT;
    const float margin = shadow_margin(plan.height);
    const float box_left = on_left ? margin : static_cast<float>(canvas.width) - plan.width - margin;
    const float box_top = on_top ? margin : static_cast<float>(canvas.height) - plan.height - margin;
    const D2D1_RECT_F box{box_left, box_top, box_left + plan.width, box_top + plan.height};
    const float radius = style == SKYGGN_BADGE_LABELLED ? plan.height / 2 : plan.height * 0.3f;
    const D2D1_ROUNDED_RECT shape_of_badge{box, radius, radius};

    surface drawing;
    RETURN_IF_FAILED(open_surface(canvas, drawing));
    ID2D1DeviceContext5* context = drawing.context.get();

    // frosted and labelled sit on a blurred copy of the picture behind them, over its average colour
    wil::com_ptr<ID2D1BitmapBrush> backdrop;
    if (style != SKYGGN_BADGE_TINTED) {
        const int spread = std::max(1, static_cast<int>(std::lround(plan.height / 8)));
        const int left = std::max(0, static_cast<int>(box.left) - spread);
        const int top = std::max(0, static_cast<int>(box.top) - spread);
        const int region_right = std::min(canvas.width, static_cast<int>(std::ceil(box.right)) + spread);
        const int region_bottom = std::min(canvas.height, static_cast<int>(std::ceil(box.bottom)) + spread);
        const uint32_t base = average_colour(canvas, left, top, region_right, region_bottom);
        const int region_width = region_right - left;
        const int region_height = region_bottom - top;
        std::vector<uint32_t> region(static_cast<size_t>(region_width) * region_height);
        for (int y = top; y < region_bottom; ++y) {
            for (int x = left; x < region_right; ++x) {
                const uint32_t pixel = canvas.pixels[static_cast<size_t>(y) * canvas.width + x];
                region[static_cast<size_t>(y - top) * region_width + (x - left)] = (pixel >> 24) == 0xff ? pixel : base;
            }
        }
        blur(region, region_width, region_height, spread);
        wil::com_ptr<ID2D1Bitmap> blurred;
        RETURN_IF_FAILED(context->CreateBitmap(
            D2D1::SizeU(region_width, region_height), region.data(), region_width * 4,
            D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)),
            &blurred));
        RETURN_IF_FAILED(context->CreateBitmapBrush(blurred.get(), &backdrop));
        backdrop->SetTransform(D2D1::Matrix3x2F::Translation(static_cast<float>(left), static_cast<float>(top)));
    }

    wil::com_ptr<IDWriteTextLayout> text;
    if (!plan.label.empty()) {
        RETURN_IF_FAILED(text_layout(plan.label, plan.height * kLabelFontShare, 1000.0f, plan.height, false, text));
    }
    wil::com_ptr<ID2D1SvgDocument> glyph;
    RETURN_IF_FAILED(load_glyph(context, category, plan.glyph, glyph));
    wil::com_ptr<ID2D1SolidColorBrush> brush;
    RETURN_IF_FAILED(context->CreateSolidColorBrush(rgba(kBlack, 1.0f), &brush));
    // a file proven damaged (damage.h): the warning mark in the badge's bottom right corner, over its
    // rounding. the badge itself stays as it is.
    const float radius_of_mark = mark_radius(plan.height);
    const D2D1_ELLIPSE mark{{box.right - radius_of_mark, box.bottom - radius_of_mark}, radius_of_mark, radius_of_mark};
    wil::com_ptr<ID2D1Effect> shadow;
    RETURN_IF_FAILED(soft_shadow(context, plan.height, [&](ID2D1SolidColorBrush* black) {
        context->FillRoundedRectangle(shape_of_badge, black);
        if (warned) {
            context->FillEllipse(mark, black);
        }
    }, shadow));

    context->BeginDraw();
    context->DrawImage(shadow.get(), D2D1::Point2F(0, shadow_drop(plan.height)));

    if (backdrop) {
        context->FillRoundedRectangle(shape_of_badge, backdrop.get());
        brush->SetColor(rgba(kGlass, style == SKYGGN_BADGE_LABELLED ? 0.6f : 0.48f));
    } else {
        brush->SetColor(rgba(category_rgb(category), 1.0f));
    }
    context->FillRoundedRectangle(shape_of_badge, brush.get());
    // a hairline of light along the edge, as on glass
    const float line = hairline(plan.height);
    brush->SetColor(rgba(kWhite, 0.28f));
    context->DrawRoundedRectangle({{box.left + line / 2, box.top + line / 2, box.right - line / 2, box.bottom - line / 2},
                                   radius - line / 2, radius - line / 2},
                                  brush.get(), line);

    const float glyph_left =
        text ? box.left + std::round(plan.height * 0.3f) : box.left + (plan.width - plan.glyph) / 2;
    context->SetTransform(D2D1::Matrix3x2F::Translation(glyph_left, box.top + (plan.height - plan.glyph) / 2));
    context->DrawSvgDocument(glyph.get());
    context->SetTransform(D2D1::Matrix3x2F::Identity());
    if (text) {
        brush->SetColor(rgba(kWhite, 1.0f));
        context->DrawTextLayout({glyph_left + plan.glyph + std::round(plan.height * 0.12f), box.top}, text.get(),
                                brush.get());
    }
    if (warned) {
        draw_mark(context, brush.get(), mark.point, plan.height);
    }
    RETURN_IF_FAILED(context->EndDraw());
    RETURN_IF_FAILED(close_surface(drawing, canvas));
    picture = std::move(canvas);
    return S_OK;
}

HRESULT draw_placeholder(UINT size, std::wstring_view extension, uint64_t colour_seed,
                         skyggn_category category, skyggn_placeholder style, const badge_look& badge,
                         skyggn_damage damage, image& out) {
    RETURN_HR_IF(E_INVALIDARG, style == SKYGGN_PLACEHOLDER_NONE);
    const UINT room = badge_room(size, badge);
    const auto side = static_cast<int>(size - 2 * room);
    RETURN_HR_IF(E_INVALIDARG, side <= 0);
    image tile{side, side, std::vector<uint32_t>(static_cast<size_t>(side) * side, 0), true};

    surface drawing;
    RETURN_IF_FAILED(open_surface(tile, drawing));
    ID2D1DeviceContext5* context = drawing.context.get();
    const auto extent = static_cast<float>(side);

    // a soft gradient, lighter where it starts
    const tile_colours colours = style == SKYGGN_PLACEHOLDER_KIND ? kind_colours(category) : seed_colours(colour_seed);
    const D2D1_GRADIENT_STOP stops[] = {{0.0f, rgba(colours.start, 1.0f)}, {1.0f, rgba(colours.end, 1.0f)}};
    wil::com_ptr<ID2D1GradientStopCollection> collection;
    RETURN_IF_FAILED(context->CreateGradientStopCollection(stops, ARRAYSIZE(stops), &collection));
    wil::com_ptr<ID2D1LinearGradientBrush> gradient;
    RETURN_IF_FAILED(context->CreateLinearGradientBrush(
        {{colours.from.x * extent, colours.from.y * extent}, {colours.to.x * extent, colours.to.y * extent}},
        collection.get(), &gradient));

    // the kind's symbol, big, with the file type below it
    const float glyph_size = std::round(extent * 0.42f);
    wil::com_ptr<ID2D1SvgDocument> glyph;
    RETURN_IF_FAILED(load_glyph(context, category, glyph_size, glyph));
    const std::wstring label = label_of(extension);
    wil::com_ptr<IDWriteTextLayout> text;
    if (!label.empty()) {
        RETURN_IF_FAILED(text_layout(label, extent * 0.1f, extent, extent * 0.16f, true, text));
    }
    // a file proven damaged says so under its file type, smaller; the symbol and the type stay where
    // they always are. a tile too small to read it at leaves it out.
    wil::com_ptr<IDWriteTextLayout> reason;
    if (const int words = damage_words(damage); words != 0 && extent >= kReasonFrom) {
        const std::wstring said = loaded_string(words);
        RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_RESOURCE_NAME_NOT_FOUND), said.empty());
        RETURN_IF_FAILED(text_layout(said, extent * 0.056f, extent, extent * 0.09f, true, reason));
    }
    wil::com_ptr<ID2D1SolidColorBrush> brush;
    RETURN_IF_FAILED(context->CreateSolidColorBrush(rgba(kWhite, 0.9f), &brush));
    wil::com_ptr<ID2D1SolidColorBrush> reason_brush;
    RETURN_IF_FAILED(context->CreateSolidColorBrush(rgba(kWhite, 0.8f), &reason_brush));

    // the symbol and the label are recorded once, so a soft shadow can be made from their shape;
    // it keeps them readable on the lighter colours
    wil::com_ptr<ID2D1CommandList> marks;
    RETURN_IF_FAILED(context->CreateCommandList(&marks));
    wil::com_ptr<ID2D1Image> canvas_target;
    context->GetTarget(&canvas_target);
    context->SetTarget(marks.get());
    context->BeginDraw();
    context->SetTransform(D2D1::Matrix3x2F::Translation((extent - glyph_size) / 2, extent * 0.44f - glyph_size / 2));
    context->DrawSvgDocument(glyph.get());
    context->SetTransform(D2D1::Matrix3x2F::Identity());
    if (text) {
        context->DrawTextLayout({0, extent * 0.7f}, text.get(), brush.get());
    }
    if (reason) {
        context->DrawTextLayout({0, extent * 0.845f}, reason.get(), reason_brush.get());
    }
    RETURN_IF_FAILED(context->EndDraw());
    RETURN_IF_FAILED(marks->Close());
    context->SetTarget(canvas_target.get());

    // two shadows below them: a wide, soft one that lifts them off the tile, and a tight one that
    // keeps their edges clear on the lighter colours. sizes are shares of the tile.
    struct shadow_layer {
        float blur;
        float opacity;
        float drop;
    };
    constexpr shadow_layer kShadows[] = {{0.035f, 0.5f, 0.025f}, {0.01f, 0.5f, 0.01f}};
    wil::com_ptr<ID2D1Effect> shadows[std::size(kShadows)];
    for (size_t i = 0; i < std::size(kShadows); ++i) {
        RETURN_IF_FAILED(context->CreateEffect(CLSID_D2D1Shadow, &shadows[i]));
        shadows[i]->SetInput(0, marks.get());
        RETURN_IF_FAILED(shadows[i]->SetValue(D2D1_SHADOW_PROP_BLUR_STANDARD_DEVIATION,
                                              std::max(0.5f, extent * kShadows[i].blur)));
        RETURN_IF_FAILED(shadows[i]->SetValue(D2D1_SHADOW_PROP_COLOR, D2D1::Vector4F(0, 0, 0, kShadows[i].opacity)));
    }

    context->BeginDraw();
    const float radius = extent * 0.08f;
    context->FillRoundedRectangle({{0, 0, extent, extent}, radius, radius}, gradient.get());
    for (size_t i = 0; i < std::size(kShadows); ++i) {
        context->DrawImage(shadows[i].get(), D2D1::Point2F(0, std::max(0.5f, extent * kShadows[i].drop)));
    }
    context->DrawImage(marks.get());
    RETURN_IF_FAILED(context->EndDraw());
    RETURN_IF_FAILED(close_surface(drawing, tile));
    out = room > 0 ? with_room(tile, static_cast<int>(room)) : std::move(tile);
    if (damage == SKYGGN_DAMAGE_NONE || room == 0) {
        return S_OK;
    }
    // a damaged file's tile also gets the badge's warning mark: on the corner a badge would take,
    // hanging out into the room as a badge does, short of the edge where its shadow fades. it shows
    // even where the tile is too small for the words. without a badge (turned off, or too small a
    // thumbnail) there is no room, and no mark, as on a picture.
    const float height = badge_height(size, badge);
    const float mark = mark_radius(height);
    const float reach = static_cast<float>(room) - shadow_margin(height);
    const bool on_left = badge.corner == SKYGGN_CORNER_BOTTOM_LEFT || badge.corner == SKYGGN_CORNER_TOP_LEFT;
    const bool on_top = badge.corner == SKYGGN_CORNER_TOP_RIGHT || badge.corner == SKYGGN_CORNER_TOP_LEFT;
    const float near_x = static_cast<float>(room);
    const float near_y = static_cast<float>(room);
    const float far_x = static_cast<float>(out.width - static_cast<int>(room));
    const float far_y = static_cast<float>(out.height - static_cast<int>(room));
    const D2D1_POINT_2F centre{on_left ? near_x - reach + mark : far_x + reach - mark,
                               on_top ? near_y - reach + mark : far_y + reach - mark};
    surface marking;
    RETURN_IF_FAILED(open_surface(out, marking));
    ID2D1DeviceContext5* canvas = marking.context.get();
    wil::com_ptr<ID2D1Effect> shadow;
    RETURN_IF_FAILED(soft_shadow(canvas, height, [&](ID2D1SolidColorBrush* black) {
        canvas->FillEllipse({centre, mark, mark}, black);
    }, shadow));
    wil::com_ptr<ID2D1SolidColorBrush> ink;
    RETURN_IF_FAILED(canvas->CreateSolidColorBrush(rgba(kWarning, 1.0f), &ink));
    canvas->BeginDraw();
    canvas->DrawImage(shadow.get(), D2D1::Point2F(0, shadow_drop(height)));
    draw_mark(canvas, ink.get(), centre, height);
    RETURN_IF_FAILED(canvas->EndDraw());
    return close_surface(marking, out);
}

}  // namespace skyggn
