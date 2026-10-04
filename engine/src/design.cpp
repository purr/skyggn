#include "design.h"

#include "byte_reader.h"

#include <wil/result.h>

#include <algorithm>
#include <charconv>
#include <string_view>

namespace skyggn {

namespace {

constexpr size_t kMostPreviews = 16;
constexpr uint64_t kLargestPreview = 32 * 1024 * 1024;  // encoded bytes; previews are far smaller

// base64 (rfc 4648) to bytes, strictly: only its alphabet, then padding, count; line breaks, plain
// or as the xml entities illustrator and indesign write (&#xA;), are skipped. false for anything
// else, as in the broken copies indesign keeps.
bool from_base64(std::string_view text, std::vector<uint8_t>& out) {
    out.clear();
    uint32_t bits = 0;
    int pending = 0;
    int padding = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '&') {
            const size_t end = text.find(';', i);
            const std::string_view entity = end == std::string_view::npos ? "" : text.substr(i, end - i + 1);
            if (entity != "&#xA;" && entity != "&#xa;" && entity != "&#xD;" && entity != "&#xd;" && entity != "&#10;" &&
                entity != "&#13;") {
                return false;
            }
            i = end;
            continue;
        }
        if (c == '\r' || c == '\n' || c == ' ' || c == '\t') {
            continue;
        }
        if (c == '=') {
            ++padding;
            continue;
        }
        int value = -1;
        if (c >= 'A' && c <= 'Z') {
            value = c - 'A';
        } else if (c >= 'a' && c <= 'z') {
            value = c - 'a' + 26;
        } else if (c >= '0' && c <= '9') {
            value = c - '0' + 52;
        } else if (c == '+') {
            value = 62;
        } else if (c == '/') {
            value = 63;
        }
        if (value < 0 || padding > 0) {
            return false;
        }
        bits = (bits << 6) | static_cast<uint32_t>(value);
        pending += 6;
        if (pending >= 8) {
            pending -= 8;
            out.push_back(static_cast<uint8_t>(bits >> pending));
        }
    }
    return padding <= 2 && !out.empty();
}

// an eps file's binary header (encapsulated postscript 3.0, 5.2): where its postscript, windows
// metafile and tiff previews lie. a metafile preview is not read: windows' image codecs do not
// draw metafiles.
HRESULT eps_tiff_preview(IStream* stream, uint64_t size, std::vector<embedded_preview>& out) {
    uint8_t header[30];
    ULONG got = 0;
    RETURN_IF_FAILED(read_at(stream, 0, header, sizeof(header), got));
    if (got < sizeof(header) || big32(header) != 0xC5D0D3C6) {
        return S_OK;
    }
    const uint64_t offset = little32(header + 20);
    const uint64_t length = little32(header + 24);
    if (length == 0 || length > kLargestPreview || offset > size || length > size - offset) {
        return S_OK;
    }
    embedded_preview preview{L".tif", std::vector<uint8_t>(static_cast<size_t>(length))};
    RETURN_IF_FAILED(read_at(stream, offset, preview.data.data(), static_cast<ULONG>(length), got));
    if (got == length) {
        out.push_back(std::move(preview));
    }
    return S_OK;
}

// the page an xmp thumbnail at `marker` shows, from the list item (rdf:li) it is in: its
// xmpTPg:PageNumber, as an element or an attribute; 0 when it does not say
HRESULT page_of(IStream* stream, uint64_t marker, uint32_t& page) {
    page = 0;
    constexpr uint64_t kLookBack = 4096;
    const uint64_t start = marker > kLookBack ? marker - kLookBack : 0;
    std::vector<uint8_t> before(static_cast<size_t>(marker - start));
    ULONG got = 0;
    RETURN_IF_FAILED(read_at(stream, start, before.data(), static_cast<ULONG>(before.size()), got));
    std::string_view text = text_of(before.data(), got);
    const size_t item = text.rfind("<rdf:li");
    if (item == std::string_view::npos) {
        return S_OK;
    }
    text = text.substr(item);
    constexpr std::string_view kPage = "xmpTPg:PageNumber";
    const size_t at = text.find(kPage);
    if (at == std::string_view::npos) {
        return S_OK;
    }
    const size_t digits = text.find_first_of("0123456789", at + kPage.size());
    if (digits != std::string_view::npos && digits <= at + kPage.size() + 2) {
        std::from_chars(text.data() + digits, text.data() + text.size(), page);
    }
    return S_OK;
}

// the base64 text of an xmp thumbnail whose marker ends at `after`: an element's text, up to the
// next tag, or an attribute's quoted value. it gives up at the time limit: a crafted file can put
// thousands of markers before tens of megabytes without an end, each read again from its marker.
HRESULT thumbnail_text(IStream* stream, uint64_t after, uint64_t size, const deadline& limit, std::string& text,
                       bool& found) {
    found = false;
    text.clear();
    uint8_t opening[2];
    ULONG got = 0;
    RETURN_IF_FAILED(read_at(stream, after, opening, sizeof(opening), got));
    char end = 0;
    uint64_t at = after;
    if (got >= 1 && opening[0] == '>') {
        end = '<';
        at += 1;
    } else if (got == 2 && opening[0] == '=' && (opening[1] == '"' || opening[1] == '\'')) {
        end = static_cast<char>(opening[1]);
        at += 2;
    } else {
        return S_OK;  // the name in some other place, a namespace list for one
    }
    std::vector<uint8_t> block(64 * 1024);
    while (at < size && text.size() < kLargestPreview / 3 * 4 && !limit.passed()) {
        RETURN_IF_FAILED(read_at(stream, at, block.data(), static_cast<ULONG>(block.size()), got));
        if (got == 0) {
            return S_OK;
        }
        const std::string_view part = text_of(block.data(), got);
        const size_t stop = part.find(end);
        text.append(part.substr(0, stop));
        if (stop != std::string_view::npos) {
            found = true;
            return S_OK;
        }
        at += got;
    }
    return S_OK;
}

// a jpeg's size and number of colour components, from its frame header (any SOFn marker)
void describe_jpeg(embedded_preview& preview) {
    const std::vector<uint8_t>& bytes = preview.data;
    for (size_t at = 2; at + 4 <= bytes.size();) {
        if (bytes[at] != 0xFF) {
            return;
        }
        const uint8_t marker = bytes[at + 1];
        const size_t length = (size_t{bytes[at + 2]} << 8) | bytes[at + 3];
        const bool frame = marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC;
        if (frame && at + 10 <= bytes.size()) {
            preview.height = (uint32_t{bytes[at + 5]} << 8) | bytes[at + 6];
            preview.width = (uint32_t{bytes[at + 7]} << 8) | bytes[at + 8];
            preview.colour = bytes[at + 9] >= 3;
            return;
        }
        at += 2 + length;
    }
}

// a tiff's size and colour, from its first image's tags: width (256), height (257) and how its
// samples mean colour (262: 0 and 1 grey, 4 a mask, the rest colour)
void describe_tiff(embedded_preview& preview) {
    const std::vector<uint8_t>& bytes = preview.data;
    if (bytes.size() < 8 || (bytes[0] != 'I' && bytes[0] != 'M') || bytes[0] != bytes[1]) {
        return;
    }
    const bool little = bytes[0] == 'I';
    const auto u16 = [&](size_t at) {
        return little ? uint32_t{bytes[at]} | (uint32_t{bytes[at + 1]} << 8) : (uint32_t{bytes[at]} << 8) | bytes[at + 1];
    };
    const auto u32 = [&](size_t at) { return little ? little32(bytes.data() + at) : big32(bytes.data() + at); };
    const size_t directory = u32(4);
    if (directory + 2 > bytes.size()) {
        return;
    }
    const size_t count = u16(directory);
    uint32_t photometric = 0;
    for (size_t i = 0; i < count && directory + 2 + i * 12 + 12 <= bytes.size(); ++i) {
        const size_t entry = directory + 2 + i * 12;
        const uint32_t tag = u16(entry);
        const uint32_t value = u16(entry + 2) == 3 ? u16(entry + 8) : u32(entry + 8);  // short or long
        if (tag == 256) {
            preview.width = value;
        } else if (tag == 257) {
            preview.height = value;
        } else if (tag == 262) {
            photometric = value;
        }
    }
    preview.colour = photometric != 0 && photometric != 1 && photometric != 4;
}

// every xmp thumbnail (xmpGImg:image) of the file's first page, or of no page in particular,
// whose text is clean base64
HRESULT xmp_thumbnails(IStream* stream, uint64_t size, const deadline& limit, std::vector<embedded_preview>& out) {
    constexpr std::string_view kMarker = "xmpGImg:image";
    constexpr size_t kBlock = 1024 * 1024;
    std::vector<uint8_t> block(kBlock);
    // blocks overlap by the marker's length less one, so a marker across two blocks is found once
    for (uint64_t offset = 0; offset < size && out.size() < kMostPreviews; offset += kBlock - (kMarker.size() - 1)) {
        if (limit.passed()) {
            return S_OK;
        }
        ULONG got = 0;
        RETURN_IF_FAILED(read_at(stream, offset, block.data(), kBlock, got));
        const std::string_view text = text_of(block.data(), got);
        for (size_t at = text.find(kMarker); at != std::string_view::npos && out.size() < kMostPreviews;
             at = text.find(kMarker, at + 1)) {
            if (limit.passed()) {
                return S_OK;
            }
            const uint64_t marker = offset + at;
            uint32_t page = 0;
            RETURN_IF_FAILED(page_of(stream, marker, page));
            if (page > 1) {
                continue;
            }
            std::string encoded;
            bool found = false;
            RETURN_IF_FAILED(thumbnail_text(stream, marker + kMarker.size(), size, limit, encoded, found));
            embedded_preview preview{L".jpg", {}};
            // only jpeg thumbnails are written; a copy that is not clean base64 is a broken one
            if (!found || !from_base64(encoded, preview.data) || preview.data.size() < 2 || preview.data[0] != 0xFF ||
                preview.data[1] != 0xD8) {
                continue;
            }
            const bool seen = std::ranges::any_of(out, [&](const embedded_preview& other) { return other.data == preview.data; });
            if (!seen) {
                out.push_back(std::move(preview));
            }
        }
        if (got < kBlock) {
            break;
        }
    }
    return S_OK;
}

}  // namespace

HRESULT find_design_previews(IStream* stream, const deadline& limit, std::vector<embedded_preview>& out) {
    out.clear();
    STATSTG stat{};
    RETURN_IF_FAILED(stream->Stat(&stat, STATFLAG_NONAME));
    const uint64_t size = stat.cbSize.QuadPart;
    RETURN_IF_FAILED(eps_tiff_preview(stream, size, out));
    RETURN_IF_FAILED(xmp_thumbnails(stream, size, limit, out));
    for (embedded_preview& preview : out) {
        if (preview.extension == L".jpg") {
            describe_jpeg(preview);
        } else {
            describe_tiff(preview);
        }
    }
    std::ranges::stable_sort(out, [](const embedded_preview& a, const embedded_preview& b) {
        if (a.colour != b.colour) {
            return a.colour;
        }
        return uint64_t{a.width} * a.height > uint64_t{b.width} * b.height;
    });
    return S_OK;
}

HRESULT is_pdf(IStream* stream, bool& pdf) {
    // the pdf header may follow up to 1 KB of other bytes, which readers skip
    uint8_t head[1024];
    ULONG got = 0;
    RETURN_IF_FAILED(read_at(stream, 0, head, sizeof(head), got));
    pdf = text_of(head, got).find("%PDF-") != std::string_view::npos;
    return S_OK;
}

}  // namespace skyggn
