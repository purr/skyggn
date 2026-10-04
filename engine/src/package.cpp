#include "package.h"

#include "formats.h"
#include "text.h"

#include <archive.h>
#include <archive_entry.h>
#include <shlwapi.h>
#include <wil/com.h>
#include <wil/resource.h>
#include <wil/result.h>
#include <xmllite.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <memory>

namespace skyggn {

namespace {

// a cover larger than this is not a picture to decode for a thumbnail
constexpr la_int64_t kLargestCover = 64 * 1024 * 1024;

// libarchive reads the file through the IStream windows hands to the thumbnail provider
class archive_source {
public:
    archive_source(IStream* stream, const deadline& limit) : stream_(stream), limit_(limit) {}

    HRESULT rewind() {
        timed_out_ = false;
        return stream_->Seek({}, STREAM_SEEK_SET, nullptr);
    }

    bool timed_out() const { return timed_out_; }

    // libarchive's callbacks; `data` is the source
    static la_ssize_t read(archive* reader, void* data, const void** buffer) {
        auto* self = static_cast<archive_source*>(data);
        if (self->limit_.passed()) {
            self->timed_out_ = true;
            archive_set_error(reader, ETIMEDOUT, "time limit passed");
            return -1;
        }
        ULONG got = 0;
        if (FAILED(self->stream_->Read(self->buffer_.data(), static_cast<ULONG>(self->buffer_.size()), &got))) {
            archive_set_error(reader, EIO, "the file could not be read");
            return -1;
        }
        *buffer = self->buffer_.data();
        return got;
    }

    static la_int64_t seek(archive*, void* data, la_int64_t offset, int whence) {
        auto* self = static_cast<archive_source*>(data);
        LARGE_INTEGER move{};
        move.QuadPart = offset;
        const DWORD origin = whence == SEEK_SET ? STREAM_SEEK_SET : whence == SEEK_CUR ? STREAM_SEEK_CUR : STREAM_SEEK_END;
        ULARGE_INTEGER position{};
        if (FAILED(self->stream_->Seek(move, origin, &position))) {
            return ARCHIVE_FATAL;
        }
        return static_cast<la_int64_t>(position.QuadPart);
    }

    // skipping is a seek; 0 tells libarchive to read past the data instead
    static la_int64_t skip(archive*, void* data, la_int64_t request) {
        auto* self = static_cast<archive_source*>(data);
        LARGE_INTEGER move{};
        move.QuadPart = request;
        return SUCCEEDED(self->stream_->Seek(move, STREAM_SEEK_CUR, nullptr)) ? request : 0;
    }

private:
    IStream* stream_;
    const deadline& limit_;
    std::vector<uint8_t> buffer_ = std::vector<uint8_t>(64 * 1024);
    bool timed_out_ = false;
};

using archive_ptr = wil::unique_any<archive*, decltype(&archive_read_free), archive_read_free>;

// libarchive's error numbers are platform-specific by its own account; past the time limit, every
// failure means the same here: not an archive it can read
HRESULT failure(const archive_source& source) {
    return HRESULT_FROM_WIN32(source.timed_out() ? ERROR_TIMEOUT : ERROR_INVALID_DATA);
}

HRESULT open_archive(archive_source& source, archive_ptr& out) {
    RETURN_IF_FAILED(source.rewind());
    archive_ptr reader(archive_read_new());
    RETURN_IF_NULL_ALLOC(reader.get());
    // every container comics come in, whatever the extension says: plenty of .cbr files are zips
    archive_read_support_format_zip(reader.get());
    archive_read_support_format_rar(reader.get());
    archive_read_support_format_rar5(reader.get());
    archive_read_support_format_7zip(reader.get());
    archive_read_support_format_tar(reader.get());
    archive_read_set_callback_data(reader.get(), &source);
    archive_read_set_read_callback(reader.get(), &archive_source::read);
    archive_read_set_seek_callback(reader.get(), &archive_source::seek);
    archive_read_set_skip_callback(reader.get(), &archive_source::skip);
    if (archive_read_open1(reader.get()) != ARCHIVE_OK) {
        return failure(source);
    }
    out = std::move(reader);
    return S_OK;
}

// moves to the next entry; S_FALSE at the end of the archive
HRESULT next_entry(archive* reader, const archive_source& source, archive_entry*& entry) {
    const int result = archive_read_next_header(reader, &entry);
    if (result == ARCHIVE_EOF) {
        return S_FALSE;
    }
    return result == ARCHIVE_OK || result == ARCHIVE_WARN ? S_OK : failure(source);
}

// the entry's path with forward slashes and without a leading "./" or "/"; empty when the archive
// stores it in an encoding libarchive cannot convert
std::wstring entry_name(archive_entry* entry) {
    std::wstring name;
    if (const wchar_t* wide = archive_entry_pathname_w(entry)) {
        name = wide;
    } else if (const char* utf8 = archive_entry_pathname_utf8(entry)) {
        name = from_utf8(utf8);
    }
    std::ranges::replace(name, L'\\', L'/');
    while (name.starts_with(L"./") || name.starts_with(L"/")) {
        name.erase(0, name.starts_with(L"/") ? 1 : 2);
    }
    return name;
}

struct archive_file {
    int index;  // its position among the archive's entries
    std::wstring name;
};

HRESULT list_files(archive_source& source, std::vector<archive_file>& files) {
    archive_ptr reader;
    RETURN_IF_FAILED_EXPECTED(open_archive(source, reader));
    archive_entry* entry = nullptr;
    for (int index = 0;; ++index) {
        const HRESULT hr = next_entry(reader.get(), source, entry);
        RETURN_IF_FAILED_EXPECTED(hr);
        if (hr == S_FALSE) {
            return S_OK;
        }
        // the data is skipped by the next header read
        if (archive_entry_filetype(entry) == AE_IFREG) {
            if (std::wstring name = entry_name(entry); !name.empty()) {
                files.push_back({index, std::move(name)});
            }
        }
    }
}

HRESULT read_file(archive_source& source, const archive_file& file, std::vector<uint8_t>& data) {
    archive_ptr reader;
    RETURN_IF_FAILED_EXPECTED(open_archive(source, reader));
    archive_entry* entry = nullptr;
    for (int index = 0; index <= file.index; ++index) {
        const HRESULT hr = next_entry(reader.get(), source, entry);
        RETURN_IF_FAILED_EXPECTED(hr);
        RETURN_HR_IF_EXPECTED(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), hr == S_FALSE);
    }
    data.clear();
    if (archive_entry_size_is_set(entry)) {
        RETURN_HR_IF_EXPECTED(HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE), archive_entry_size(entry) > kLargestCover);
        data.reserve(static_cast<size_t>(archive_entry_size(entry)));
    }
    std::vector<uint8_t> chunk(64 * 1024);
    for (;;) {
        const la_ssize_t got = archive_read_data(reader.get(), chunk.data(), chunk.size());
        if (got == 0) {
            return S_OK;
        }
        if (got < 0) {
            return failure(source);
        }
        RETURN_HR_IF_EXPECTED(HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE),
                              static_cast<la_int64_t>(data.size()) + got > kLargestCover);
        data.insert(data.end(), chunk.begin(), chunk.begin() + got);
    }
}

bool is_picture(std::wstring_view name) {
    const format_entry* known = find_format(PathFindExtensionW(std::wstring(name).c_str()));
    return known && known->format.category == SKYGGN_CATEGORY_IMAGE;
}

// mac os leaves "__MACOSX/" folders and "._" files in archives it makes; neither is a page
bool hidden(std::wstring_view name) {
    if (name.starts_with(L"__MACOSX/")) {
        return true;
    }
    for (size_t start = 0; start < name.size();) {
        if (name[start] == L'.') {
            return true;
        }
        const size_t slash = name.find(L'/', start);
        start = slash == std::wstring_view::npos ? name.size() : slash + 1;
    }
    return false;
}

bool same_name(std::wstring_view a, std::wstring_view b) {
    return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) ==
           CSTR_EQUAL;
}

HRESULT comic_cover(archive_source& source, package_picture& out) {
    std::vector<archive_file> files;
    RETURN_IF_FAILED_EXPECTED(list_files(source, files));
    // the first page, in the order explorer shows names in: "page 2" before "page 10"
    const archive_file* first = nullptr;
    for (const archive_file& file : files) {
        if (is_picture(file.name) && !hidden(file.name) &&
            (!first || StrCmpLogicalW(file.name.c_str(), first->name.c_str()) < 0)) {
            first = &file;
        }
    }
    RETURN_HR_IF_NULL_EXPECTED(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), first);
    out.name = first->name;
    return read_file(source, *first, out.data);
}

HRESULT xml_reader(const std::vector<uint8_t>& xml, wil::com_ptr<IXmlReader>& reader) {
    wil::com_ptr<IStream> stream;
    stream.attach(SHCreateMemStream(xml.data(), static_cast<UINT>(xml.size())));
    RETURN_IF_NULL_ALLOC(stream.get());
    RETURN_IF_FAILED(CreateXmlReader(__uuidof(IXmlReader), reader.put_void(), nullptr));
    // some packages carry a doctype; xmllite resolves no external entity and caps expansion
    RETURN_IF_FAILED(reader->SetProperty(XmlReaderProperty_DtdProcessing, DtdProcessing_Parse));
    return reader->SetInput(stream.get());
}

// the current element's attribute, empty when it has none
std::wstring attribute(IXmlReader* reader, const wchar_t* name) {
    std::wstring value;
    if (reader->MoveToAttributeByName(name, nullptr) == S_OK) {
        const wchar_t* text = nullptr;
        UINT length = 0;
        if (SUCCEEDED(reader->GetValue(&text, &length))) {
            value.assign(text, length);
        }
        reader->MoveToElement();
    }
    return value;
}

// calls `visit(local name)` for every element, in document order
template <typename Visit>
HRESULT each_element(IXmlReader* reader, Visit visit) {
    XmlNodeType type{};
    HRESULT hr = S_OK;
    while ((hr = reader->Read(&type)) == S_OK) {
        if (type == XmlNodeType_Element) {
            const wchar_t* name = nullptr;
            RETURN_IF_FAILED(reader->GetLocalName(&name, nullptr));
            visit(std::wstring_view(name));
        }
    }
    return SUCCEEDED(hr) ? S_OK : hr;
}

struct manifest_item {
    std::wstring id;
    std::wstring href;
    std::wstring media_type;
    std::wstring properties;
};

bool contains_word(std::wstring_view text, std::wstring_view word) {
    for (size_t start = 0; start < text.size();) {
        const size_t space = std::min(text.find(L' ', start), text.size());
        if (text.substr(start, space - start) == word) {
            return true;
        }
        start = space + 1;
    }
    return false;
}

bool mentions_cover(std::wstring_view text) {
    return StrStrIW(std::wstring(text).c_str(), L"cover") != nullptr;
}

// `href` from the package file at `package`: relative to its folder, percent-encoded, maybe with
// a fragment
std::wstring resolve(std::wstring_view package, std::wstring_view href) {
    std::wstring target(href.substr(0, std::min(href.find(L'#'), href.size())));
    if (!target.empty()) {
        UrlUnescapeW(target.data(), nullptr, nullptr, URL_UNESCAPE_INPLACE | URL_UNESCAPE_AS_UTF8);
        target.resize(wcslen(target.c_str()));
    }
    const size_t folder = package.rfind(L'/');
    std::wstring path = (folder == std::wstring_view::npos ? std::wstring() : std::wstring(package.substr(0, folder + 1))) + target;
    // fold "." and ".." segments
    std::vector<std::wstring> segments;
    for (size_t start = 0; start <= path.size();) {
        const size_t slash = std::min(path.find(L'/', start), path.size());
        const std::wstring segment = path.substr(start, slash - start);
        if (segment == L"..") {
            if (!segments.empty()) {
                segments.pop_back();
            }
        } else if (!segment.empty() && segment != L".") {
            segments.push_back(segment);
        }
        start = slash + 1;
    }
    std::wstring folded;
    for (const std::wstring& segment : segments) {
        folded += (folded.empty() ? L"" : L"/") + segment;
    }
    return folded;
}

HRESULT find_file(const std::vector<archive_file>& files, std::wstring_view name, const archive_file*& out) {
    const auto it = std::ranges::find_if(files, [&](const archive_file& file) { return same_name(file.name, name); });
    RETURN_HR_IF_EXPECTED(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), it == files.end());
    out = &*it;
    return S_OK;
}

// an epub: META-INF/container.xml names the package file, and the package lists the cover
HRESULT ebook_cover(archive_source& source, package_picture& out) {
    std::vector<archive_file> files;
    RETURN_IF_FAILED_EXPECTED(list_files(source, files));

    const archive_file* container = nullptr;
    RETURN_IF_FAILED_EXPECTED(find_file(files, L"META-INF/container.xml", container));
    std::vector<uint8_t> xml;
    RETURN_IF_FAILED_EXPECTED(read_file(source, *container, xml));
    wil::com_ptr<IXmlReader> reader;
    RETURN_IF_FAILED(xml_reader(xml, reader));
    std::wstring package;
    const HRESULT container_read = each_element(reader.get(), [&](std::wstring_view name) {
        if (package.empty() && name == L"rootfile") {
            package = attribute(reader.get(), L"full-path");
        }
    });
    RETURN_IF_FAILED_EXPECTED(container_read);
    RETURN_HR_IF_EXPECTED(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), package.empty());

    const archive_file* package_file = nullptr;
    RETURN_IF_FAILED_EXPECTED(find_file(files, package, package_file));
    RETURN_IF_FAILED_EXPECTED(read_file(source, *package_file, xml));
    RETURN_IF_FAILED(xml_reader(xml, reader));
    std::vector<manifest_item> items;
    std::wstring cover_id;  // epub 2: <meta name="cover" content="the cover item's id"/>
    const HRESULT package_read = each_element(reader.get(), [&](std::wstring_view name) {
        if (name == L"item") {
            items.push_back({attribute(reader.get(), L"id"), attribute(reader.get(), L"href"),
                             attribute(reader.get(), L"media-type"), attribute(reader.get(), L"properties")});
        } else if (name == L"meta" && attribute(reader.get(), L"name") == L"cover") {
            cover_id = attribute(reader.get(), L"content");
        }
    });
    RETURN_IF_FAILED_EXPECTED(package_read);

    // epub 3 marks the cover; epub 2 names it in a meta; older books only call it "cover"
    auto picture = [](const manifest_item& item) { return item.media_type.starts_with(L"image/"); };
    auto first_where = [&](auto match) -> const manifest_item* {
        const auto it = std::ranges::find_if(items, match);
        return it == items.end() ? nullptr : &*it;
    };
    const manifest_item* cover =
        first_where([](const manifest_item& item) { return contains_word(item.properties, L"cover-image"); });
    if (!cover && !cover_id.empty()) {
        cover = first_where([&](const manifest_item& item) { return picture(item) && item.id == cover_id; });
    }
    if (!cover) {
        cover = first_where(
            [&](const manifest_item& item) { return picture(item) && (mentions_cover(item.id) || mentions_cover(item.href)); });
    }
    if (!cover) {
        cover = first_where(picture);
    }
    RETURN_HR_IF_NULL_EXPECTED(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), cover);

    const archive_file* cover_file = nullptr;
    RETURN_IF_FAILED_EXPECTED(find_file(files, resolve(package, cover->href), cover_file));
    out.name = cover_file->name;
    return read_file(source, *cover_file, out.data);
}

// libreoffice and openoffice save a picture of the first page or slide with every file, at most
// 256 px. a file saved without one (an option, or another program) has no picture.
HRESULT document_thumbnail(archive_source& source, package_picture& out) {
    std::vector<archive_file> files;
    RETURN_IF_FAILED_EXPECTED(list_files(source, files));
    const archive_file* thumbnail = nullptr;
    RETURN_IF_FAILED_EXPECTED(find_file(files, L"Thumbnails/thumbnail.png", thumbnail));
    out.name = thumbnail->name;
    return read_file(source, *thumbnail, out.data);
}

}  // namespace

HRESULT read_package_picture(IStream* stream, const format_entry& format, const deadline& limit,
                             package_picture& out) {
    archive_source source(stream, limit);
    if (format.format.category == SKYGGN_CATEGORY_DOCUMENT) {
        return document_thumbnail(source, out);
    }
    if (same_name(format.format.extension, L".epub")) {
        return ebook_cover(source, out);
    }
    return comic_cover(source, out);
}

}  // namespace skyggn
