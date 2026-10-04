#include "damage.h"

#include "byte_reader.h"

#include <wil/result.h>

#include <algorithm>
#include <bit>
#include <charconv>
#include <initializer_list>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace skyggn {

namespace {

// the crc-32 that png and 7z use, over a few header bytes
uint32_t crc32(const uint8_t* bytes, size_t count) {
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < count; ++i) {
        crc ^= bytes[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

bool is_zero(uint8_t byte) {
    return byte == 0;
}

// --- iso media (mp4, mov, 3gp, m4a, heic, avif, cr3) ---------------------------------------------
// a file of boxes (quicktime calls them atoms): each starts with its size and a four-letter type,
// and some hold more boxes. iso/iec 14496-12; apple's quicktime file format.

struct box {
    uint64_t body = 0;  // where its contents start
    uint64_t end = 0;   // past its last byte, as its size says; may lie past the end of the file
    char letters[4]{};
    std::string_view type() const { return {letters, 4}; }
};

// the box whose header starts at `offset`; `found` is false when there is none: too few bytes, a
// type that is not four printable characters, or a size smaller than its header. size 0 means up
// to `outer_end` (the end of the enclosing box or of the file), size 1 a 64-bit size after the type.
HRESULT read_box(IStream* stream, uint64_t offset, uint64_t outer_end, box& out, bool& found) {
    found = false;
    uint8_t header[16];
    ULONG got = 0;
    RETURN_IF_FAILED(read_at(stream, offset, header, sizeof(header), got));
    if (got < 8 || !std::all_of(header + 4, header + 8, [](uint8_t c) { return c >= 0x20 && c < 0x7f; })) {
        return S_OK;
    }
    uint64_t size = big32(header);
    uint64_t header_size = 8;
    if (size == 1) {
        if (got < 16) {
            return S_OK;
        }
        size = big64(header + 8);
        header_size = 16;
    } else if (size == 0) {
        size = outer_end - offset;
    }
    if (size < header_size || size > UINT64_MAX - offset) {
        return S_OK;
    }
    std::copy(header + 4, header + 8, out.letters);
    out.body = offset + header_size;
    out.end = offset + size;
    found = true;
    return S_OK;
}

// the first box of a type among a box's contents, which start at `from` (after a full box's
// version and flags, for those). a contained box running past its container ends the search, and
// so does the time limit: a crafted container can hold millions of tiny boxes.
HRESULT find_inside(IStream* stream, uint64_t from, const box& outer, std::string_view type, const deadline& limit,
                    box& out, bool& found) {
    found = false;
    for (uint64_t offset = from; offset < outer.end;) {
        if (limit.passed()) {
            return S_OK;
        }
        box inner;
        bool there = false;
        RETURN_IF_FAILED(read_box(stream, offset, outer.end, inner, there));
        if (!there || inner.end > outer.end) {
            return S_OK;
        }
        if (inner.type() == type) {
            out = inner;
            found = true;
            return S_OK;
        }
        offset = inner.end;
    }
    return S_OK;
}

// the box at a path of types below `outer`, each the first of its type there
HRESULT find_path(IStream* stream, const box& outer, std::initializer_list<std::string_view> path,
                  const deadline& limit, box& out, bool& found) {
    box current = outer;
    found = true;
    for (const std::string_view type : path) {
        box inner;
        RETURN_IF_FAILED(find_inside(stream, current.body, current, type, limit, inner, found));
        if (!found) {
            return S_OK;
        }
        current = inner;
    }
    out = current;
    return S_OK;
}

// whether a track's media lies in this file: every entry of its data references (dref) has flag 1,
// "in the same file". a track whose media lies in another file says nothing about this one.
HRESULT media_in_this_file(IStream* stream, const box& dref, bool& here) {
    here = false;
    uint8_t head[8];
    ULONG got = 0;
    RETURN_IF_FAILED(read_at(stream, dref.body, head, sizeof(head), got));
    const uint32_t count = got == sizeof(head) ? big32(head + 4) : 0;
    if (count == 0 || count > 64) {
        return S_OK;
    }
    uint64_t offset = dref.body + 8;
    for (uint32_t i = 0; i < count; ++i) {
        box entry;
        bool found = false;
        RETURN_IF_FAILED(read_box(stream, offset, dref.end, entry, found));
        uint8_t flags[4];
        if (!found || entry.end > dref.end) {
            return S_OK;
        }
        RETURN_IF_FAILED(read_at(stream, entry.body, flags, sizeof(flags), got));
        if (got < sizeof(flags) || (flags[3] & 1) == 0) {
            return S_OK;
        }
        offset = entry.end;
    }
    here = true;
    return S_OK;
}

// reads `count` table entries of `width` bytes from `first`, a block at a time, handing each to
// `use`, which returns false once it needs no more; `complete` is false when the file ends before
// the entries `use` needed or the time is up
template <typename Use>
HRESULT read_entries(IStream* stream, uint64_t first, uint64_t width, uint64_t count, const deadline& limit,
                     bool& complete, const Use& use) {
    complete = false;
    std::vector<uint8_t> block(64 * 1024);
    const uint64_t per_block = block.size() / width;
    for (uint64_t done = 0; done < count;) {
        if (limit.passed()) {
            return S_OK;
        }
        const uint64_t entries = std::min(per_block, count - done);
        ULONG got = 0;
        RETURN_IF_FAILED(read_at(stream, first + done * width, block.data(), static_cast<ULONG>(entries * width), got));
        if (got < entries * width) {
            return S_OK;
        }
        for (uint64_t i = 0; i < entries; ++i) {
            if (!use(done + i, block.data() + i * width)) {
                complete = true;
                return S_OK;
            }
        }
        done += entries;
    }
    complete = true;
    return S_OK;
}

// a table box's entry count (after its version and flags, and `skip` more bytes), when that many
// entries of `width` bytes fit in the box; nullopt otherwise
HRESULT table_count(IStream* stream, const box& table, uint64_t skip, uint64_t width, std::optional<uint64_t>& count) {
    count.reset();
    uint8_t head[12];
    ULONG got = 0;
    RETURN_IF_FAILED(read_at(stream, table.body, head, static_cast<ULONG>(8 + skip), got));
    if (got < 8 + skip) {
        return S_OK;
    }
    const uint64_t entries = big32(head + 4 + skip);
    const uint64_t first = table.body + 8 + skip;
    if (first <= table.end && entries <= (table.end - first) / width) {
        count = entries;
    }
    return S_OK;
}

// where a track's last chunk of media ends, by its own sample tables: the chunk placed furthest
// (stco, co64), plus the sizes (stsz) of the samples in it, counted from the samples per chunk
// (stsc). 0 when the tables do not say.
HRESULT track_media_end(IStream* stream, const box& samples, const deadline& limit, uint64_t& end) {
    end = 0;
    box offsets;
    box runs;
    box sizes;
    bool has_offsets = false;
    bool has_runs = false;
    bool has_sizes = false;
    for (uint64_t at = samples.body; at < samples.end;) {
        if (limit.passed()) {
            return S_OK;
        }
        box table;
        bool found = false;
        RETURN_IF_FAILED(read_box(stream, at, samples.end, table, found));
        if (!found || table.end > samples.end) {
            return S_OK;
        }
        at = table.end;
        if (table.type() == "stco" || table.type() == "co64") {
            offsets = table;
            has_offsets = true;
        } else if (table.type() == "stsc") {
            runs = table;
            has_runs = true;
        } else if (table.type() == "stsz") {
            sizes = table;
            has_sizes = true;
        }
    }
    if (!has_offsets || !has_runs || !has_sizes) {
        return S_OK;  // compact sample sizes (stz2) and other layouts say nothing here
    }

    // the chunk placed furthest, numbered from 1 as the tables number them
    const uint64_t width = offsets.type() == "co64" ? 8 : 4;
    std::optional<uint64_t> chunks;
    RETURN_IF_FAILED(table_count(stream, offsets, 0, width, chunks));
    if (!chunks || *chunks == 0) {
        return S_OK;
    }
    uint64_t chunk = 0;
    uint64_t chunk_offset = 0;
    bool complete = false;
    RETURN_IF_FAILED(read_entries(stream, offsets.body + 8, width, *chunks, limit, complete,
                                  [&](uint64_t index, const uint8_t* entry) {
                                      const uint64_t offset = width == 4 ? uint64_t{big32(entry)} : big64(entry);
                                      if (offset >= chunk_offset) {
                                          chunk_offset = offset;
                                          chunk = index + 1;
                                      }
                                      return true;
                                  }));
    if (!complete) {
        return S_OK;
    }

    // the sample sizes table: one size for all samples, or one each
    uint8_t head[12];
    ULONG got = 0;
    RETURN_IF_FAILED(read_at(stream, sizes.body, head, sizeof(head), got));
    if (got < sizeof(head)) {
        return S_OK;
    }
    const uint64_t each = big32(head + 4);
    const uint64_t sample_count = big32(head + 8);
    // tables that count more samples than the track has are not ones to go by
    const auto within = [&](uint64_t chunk_count, uint64_t per_chunk, uint64_t counted) {
        return per_chunk == 0 || chunk_count <= (sample_count - counted) / per_chunk;
    };

    // its first sample and how many it holds: runs of chunks with the same number of samples, each
    // run from its first chunk to the next run's. each run is settled as soon as the next one's first
    // chunk is read, so the table is never held: a crafted one can be as large as the file.
    std::optional<uint64_t> run_count;
    RETURN_IF_FAILED(table_count(stream, runs, 0, 12, run_count));
    if (!run_count || *run_count == 0) {
        return S_OK;
    }
    uint64_t before = 0;
    uint64_t in_chunk = 0;
    bool usable = true;
    uint64_t run_first = 0;  // the run waiting for the next one: its first chunk and samples per chunk
    uint64_t run_samples = 0;
    const auto settle = [&](uint64_t next) {
        if (run_first == 0 || next <= run_first || chunk < run_first) {
            usable = false;  // runs out of order: not a table to go by
            return;
        }
        const uint64_t chunk_count = std::min(chunk, next) - run_first;
        if (!within(chunk_count, run_samples, before)) {
            usable = false;
            return;
        }
        before += chunk_count * run_samples;
        if (chunk < next) {
            in_chunk = run_samples;
        }
    };
    RETURN_IF_FAILED(read_entries(stream, runs.body + 8, 12, *run_count, limit, complete,
                                  [&](uint64_t index, const uint8_t* entry) {
                                      if (index > 0) {
                                          settle(big32(entry));
                                      }
                                      run_first = big32(entry);
                                      run_samples = big32(entry + 4);
                                      return usable && in_chunk == 0;
                                  }));
    if (!complete || !usable) {
        return S_OK;
    }
    if (in_chunk == 0) {
        settle(UINT64_MAX);  // the last run reaches to the end
    }
    if (!usable || in_chunk == 0 || in_chunk > sample_count - before) {
        return S_OK;
    }
    uint64_t bytes = 0;
    if (each != 0) {
        bytes = each * in_chunk;
    } else {
        std::optional<uint64_t> listed;
        RETURN_IF_FAILED(table_count(stream, sizes, 4, 4, listed));
        if (!listed || *listed < sample_count) {
            return S_OK;
        }
        RETURN_IF_FAILED(read_entries(stream, sizes.body + 12 + before * 4, 4, in_chunk, limit, complete,
                                      [&](uint64_t, const uint8_t* entry) {
                                          bytes += big32(entry);
                                          return true;
                                      }));
        if (!complete) {
            return S_OK;
        }
    }
    end = chunk_offset + bytes;
    return S_OK;
}

// the furthest end a movie's index (moov) gives its media in this file, over every track whose
// media lies in this file
HRESULT furthest_media(IStream* stream, const box& movie, const deadline& limit, uint64_t& furthest) {
    furthest = 0;
    for (uint64_t offset = movie.body; offset < movie.end;) {
        // the tracks walked so far stand: each end is one the file's own index gives
        if (limit.passed()) {
            return S_OK;
        }
        box track;
        bool found = false;
        RETURN_IF_FAILED(read_box(stream, offset, movie.end, track, found));
        if (!found || track.end > movie.end) {
            return S_OK;
        }
        offset = track.end;
        if (track.type() != "trak") {
            continue;
        }
        box references;
        box samples;
        bool here = false;
        RETURN_IF_FAILED(find_path(stream, track, {"mdia", "minf", "dinf", "dref"}, limit, references, here));
        if (here) {
            RETURN_IF_FAILED(media_in_this_file(stream, references, here));
        }
        if (here) {
            RETURN_IF_FAILED(find_path(stream, track, {"mdia", "minf", "stbl"}, limit, samples, here));
        }
        if (!here) {
            continue;
        }
        uint64_t end = 0;
        RETURN_IF_FAILED(track_media_end(stream, samples, limit, end));
        furthest = std::max(furthest, end);
    }
    return S_OK;
}

// the furthest end an image file's item table (iloc, in its meta box) gives an item's data in this
// file. iso/iec 14496-12 8.11.3: offsets and lengths of 0, 4 or 8 bytes, as its header says.
HRESULT furthest_item(IStream* stream, const box& items, const deadline& limit, uint64_t& furthest) {
    furthest = 0;
    box table;
    bool found = false;
    RETURN_IF_FAILED(find_inside(stream, items.body + 4, items, "iloc", limit, table, found));
    if (!found || table.end - table.body > 1024 * 1024) {
        return S_OK;
    }
    std::vector<uint8_t> bytes(static_cast<size_t>(table.end - table.body));
    ULONG got = 0;
    RETURN_IF_FAILED(read_at(stream, table.body, bytes.data(), static_cast<ULONG>(bytes.size()), got));
    if (got < bytes.size()) {
        return S_OK;
    }
    size_t at = 0;
    bool fits = true;
    // the next `count` bytes as a number; past the table's end, the table is not one to go by
    const auto take = [&](size_t count) -> uint64_t {
        if (!fits || count > bytes.size() - at) {
            fits = false;
            return 0;
        }
        const uint64_t value = big_n(bytes.data() + at, count);
        at += count;
        return value;
    };
    const auto version = take(1);
    take(3);  // flags
    const auto sizes = take(1);
    const auto more_sizes = take(1);
    const size_t offset_size = sizes >> 4;
    const size_t length_size = sizes & 0xf;
    const size_t base_size = more_sizes >> 4;
    const size_t index_size = version >= 1 ? more_sizes & 0xf : 0;
    const auto allowed = [](size_t size) { return size == 0 || size == 4 || size == 8; };
    if (version > 2 || !allowed(offset_size) || !allowed(length_size) || !allowed(base_size) || !allowed(index_size)) {
        return S_OK;
    }
    const uint64_t count = take(version < 2 ? 2 : 4);
    uint64_t reached = 0;
    for (uint64_t item = 0; item < count && fits; ++item) {
        // fields of 0 bytes take none of the table, so a crafted one can list billions of extents.
        // a walk cut short proves nothing: whether the table holds up is known only at its end.
        if (limit.passed()) {
            return S_OK;
        }
        take(version < 2 ? 2 : 4);  // item id
        const uint64_t method = version >= 1 ? take(2) & 0xf : 0;
        const uint64_t reference = take(2);
        const uint64_t base = take(base_size);
        const uint64_t extents = take(2);
        for (uint64_t extent = 0; extent < extents && fits; ++extent) {
            take(index_size);
            const uint64_t offset = take(offset_size);
            const uint64_t length = take(length_size);
            // only data placed by file offset in this file counts; a length of 0 means "all of it"
            if (method == 0 && reference == 0 && length > 0 && base <= UINT64_MAX - offset &&
                base + offset <= UINT64_MAX - length) {
                reached = std::max(reached, base + offset + length);
            }
        }
    }
    if (fits) {
        furthest = reached;
    }
    return S_OK;
}

// a box running past the end proves the file was cut when its index cannot be anywhere else: none
// came before it, and none can follow a box that reaches past the end. when the index came first, a
// writer could have got a box's size wrong, so the cut is proven only by the index itself placing
// media past the end. a file of whole boxes with media data (mdat) but no index (moov) is a
// recording that stopped before writing it; heif and avif images index theirs in a meta box. what
// follows a finished file (a vendor's trailing data) is not read. fragmented files index each
// fragment on its own and still play when cut, so they are not judged.
HRESULT check_iso_media(IStream* stream, uint64_t size, const deadline& limit, skyggn_damage& out) {
    std::optional<box> movie;
    std::optional<box> items;
    bool media = false;
    for (uint64_t offset = 0; offset < size;) {
        if (limit.passed()) {
            return S_OK;
        }
        box current;
        bool found = false;
        RETURN_IF_FAILED(read_box(stream, offset, size, current, found));
        if (!found || current.type() == "moof") {
            return S_OK;
        }
        if (current.end > size) {
            bool proven = !movie && !items;
            uint64_t furthest = 0;
            if (movie) {
                RETURN_IF_FAILED(furthest_media(stream, *movie, limit, furthest));
            } else if (items) {
                RETURN_IF_FAILED(furthest_item(stream, *items, limit, furthest));
            }
            proven = proven || furthest > size;
            if (proven) {
                out = SKYGGN_DAMAGE_INCOMPLETE;
            }
            return S_OK;
        }
        if (current.type() == "moov") {
            movie = current;
        } else if (current.type() == "meta") {
            items = current;
        }
        media = media || current.type() == "mdat";
        if ((movie || items) && media) {
            return S_OK;
        }
        offset = current.end;
    }
    if (media && !movie && !items) {
        out = SKYGGN_DAMAGE_INCOMPLETE;
    }
    return S_OK;
}

// iso media's first box is one of these
bool is_iso_media(const uint8_t* head, ULONG got) {
    constexpr std::string_view kFirst[] = {"ftyp", "moov", "mdat", "free", "skip", "wide", "pnot"};
    return got >= 8 && std::ranges::find(kFirst, text_of(head + 4, 4)) != std::end(kFirst);
}

// --- matroska and webm -----------------------------------------------------------------------------
// rfc 9559: an ebml header, then the segment holding everything else. ids and sizes are ebml
// numbers, whose first byte's leading zeros tell their length.

bool ebml_length(const uint8_t* bytes, size_t available, int& length) {
    if (available == 0 || bytes[0] == 0) {
        return false;
    }
    length = std::countl_zero(bytes[0]) + 1;
    return static_cast<size_t>(length) <= available;
}

// an element size; all its value bits set means "unknown"
bool ebml_size(const uint8_t* bytes, size_t available, uint64_t& value, int& length, bool& unknown) {
    if (!ebml_length(bytes, available, length)) {
        return false;
    }
    value = bytes[0] & (0xffu >> length);
    for (int i = 1; i < length; ++i) {
        value = (value << 8) | bytes[i];
    }
    unknown = value == (uint64_t{1} << (7 * length)) - 1;
    return true;
}

// an element id, kept with its length marker as the specification writes them (0x114D9B74)
bool ebml_id(const uint8_t* bytes, size_t available, uint32_t& id, int& length) {
    if (!ebml_length(bytes, available, length) || length > 4) {
        return false;
    }
    id = static_cast<uint32_t>(big_n(bytes, static_cast<size_t>(length)));
    return true;
}

// the furthest segment position the segment's seek head (its index of top-level elements: info,
// tracks, cues…) gives; `listed` is false without one. the seek head is the segment's first
// element, after any void or crc-32.
HRESULT furthest_seek(IStream* stream, uint64_t segment_data, uint64_t& furthest, bool& listed) {
    furthest = 0;
    listed = false;
    constexpr uint32_t kVoid = 0xEC;
    constexpr uint32_t kChecksum = 0xBF;
    constexpr uint32_t kSeekHead = 0x114D9B74;
    constexpr uint32_t kSeek = 0x4DBB;
    constexpr uint32_t kSeekPosition = 0x53AC;
    std::vector<uint8_t> bytes(64 * 1024);
    ULONG got = 0;
    RETURN_IF_FAILED(read_at(stream, segment_data, bytes.data(), static_cast<ULONG>(bytes.size()), got));
    // walks elements in bytes[from, to); `visit` gets each one's id, data start and data end
    const auto walk = [&](size_t from, size_t to, const auto& visit) {
        for (size_t at = from; at < to;) {
            uint32_t id = 0;
            int id_length = 0;
            uint64_t size = 0;
            int size_length = 0;
            bool unknown = false;
            if (!ebml_id(bytes.data() + at, to - at, id, id_length) ||
                !ebml_size(bytes.data() + at + id_length, to - at - id_length, size, size_length, unknown) || unknown) {
                return false;
            }
            const size_t data = at + id_length + size_length;
            if (size > to - data) {
                return false;
            }
            if (!visit(id, data, data + static_cast<size_t>(size))) {
                return false;
            }
            at = data + static_cast<size_t>(size);
        }
        return true;
    };
    uint64_t reached = 0;
    walk(0, got, [&](uint32_t id, size_t data, size_t end) {
        if (id == kVoid || id == kChecksum) {
            return true;
        }
        if (id == kSeekHead) {
            listed = walk(data, end, [&](uint32_t seek, size_t seek_data, size_t seek_end) {
                return seek != kSeek || walk(seek_data, seek_end, [&](uint32_t field, size_t value, size_t value_end) {
                    if (field == kSeekPosition && value_end - value <= 8) {
                        reached = std::max(reached, big_n(bytes.data() + value, value_end - value));
                    }
                    return true;
                });
            });
        }
        return false;  // only the first element that is not void or a checksum
    });
    if (listed) {
        furthest = reached;
    }
    return S_OK;
}

// a file ending inside its own ebml header was cut. a segment longer than the file proves the cut
// only together with its seek head placing an element past the end: a writer could have stated a
// wrong length, it cannot have listed elements that are not there. a live recording states no
// length until it finishes ("unknown"), so one that crashed is not judged.
HRESULT check_matroska(IStream* stream, uint64_t size, skyggn_damage& out) {
    uint8_t bytes[16];
    ULONG got = 0;
    RETURN_IF_FAILED(read_at(stream, 0, bytes, sizeof(bytes), got));
    uint64_t value = 0;
    int length = 0;
    bool unknown = false;
    if (!ebml_size(bytes + 4, got - 4, value, length, unknown) || unknown) {
        return S_OK;
    }
    const uint64_t header_end = 4 + length;
    if (value > size - header_end) {
        out = SKYGGN_DAMAGE_INCOMPLETE;
        return S_OK;
    }
    const uint64_t segment = header_end + value;
    RETURN_IF_FAILED(read_at(stream, segment, bytes, 12, got));
    if (got < 5 || big32(bytes) != 0x18538067 || !ebml_size(bytes + 4, got - 4, value, length, unknown) || unknown) {
        return S_OK;
    }
    const uint64_t segment_data = segment + 4 + length;
    if (value <= size - segment_data) {
        return S_OK;
    }
    uint64_t furthest = 0;
    bool listed = false;
    RETURN_IF_FAILED(furthest_seek(stream, segment_data, furthest, listed));
    // an element starting at the end or later is not in the file
    if (listed && furthest >= size - segment_data) {
        out = SKYGGN_DAMAGE_INCOMPLETE;
    }
    return S_OK;
}

// --- archives --------------------------------------------------------------------------------------

// zip (comic books, e-books, opendocument files): every zip ends with its end of central directory
// record (appnote 4.3.1), which only a comment of up to 64 KB may follow. without one in that
// reach, the file was cut.
HRESULT check_zip(IStream* stream, uint64_t size, skyggn_damage& out) {
    constexpr uint64_t kRecord = 22;
    constexpr uint64_t kLongestComment = 0xffff;
    const uint64_t span = std::min(size, kRecord + kLongestComment);
    std::vector<uint8_t> tail(static_cast<size_t>(span));
    ULONG got = 0;
    RETURN_IF_FAILED(read_at(stream, size - span, tail.data(), static_cast<ULONG>(span), got));
    constexpr uint8_t kEnd[] = {'P', 'K', 5, 6};
    if (std::ranges::search(tail.begin(), tail.begin() + got, std::begin(kEnd), std::end(kEnd)).empty()) {
        out = SKYGGN_DAMAGE_INCOMPLETE;
    }
    return S_OK;
}

// 7z (comic books): its first 32 bytes give where its table of contents lies, with a checksum
// (7zFormat.txt). 7-zip fills them in last, so zeros mean it never finished; a table past the end
// means the file was cut; a failing checksum, that the header is corrupted.
HRESULT check_7z(IStream* stream, uint64_t size, skyggn_damage& out) {
    uint8_t header[32];
    ULONG got = 0;
    RETURN_IF_FAILED(read_at(stream, 0, header, sizeof(header), got));
    if (got < sizeof(header) || std::all_of(header + 8, header + 32, is_zero)) {
        out = SKYGGN_DAMAGE_INCOMPLETE;
        return S_OK;
    }
    const uint8_t* table = header + 12;
    if (crc32(table, 20) != little32(header + 8)) {
        out = SKYGGN_DAMAGE_CORRUPTED;
        return S_OK;
    }
    const uint64_t table_offset = little64(table);
    const uint64_t table_size = little64(table + 8);
    if (table_offset > size - 32 || table_size > size - 32 - table_offset) {
        out = SKYGGN_DAMAGE_INCOMPLETE;
    }
    return S_OK;
}

// --- pictures and sound --------------------------------------------------------------------------

// png (w3c png, third edition): chunks, each with its length (at most 2^31 - 1), a four-letter type
// and a checksum, from the header chunk (IHDR, first) to the end chunk (IEND, last). a chunk past
// the end, or no end chunk, means the file was cut; a header whose checksum fails, or a chunk that
// is not one, that it is corrupted. only the header's checksum is checked: the others cover the
// whole picture. apple's iphone pngs put a CgBI chunk before the header, which is allowed for.
HRESULT check_png(IStream* stream, uint64_t size, const deadline& limit, skyggn_damage& out) {
    uint64_t offset = 8;
    for (bool header_due = true;;) {
        if (limit.passed()) {
            return S_OK;
        }
        uint8_t chunk[8 + 13 + 4];
        ULONG got = 0;
        RETURN_IF_FAILED(read_at(stream, offset, chunk, header_due ? sizeof(chunk) : 8, got));
        if (got < 8) {
            out = SKYGGN_DAMAGE_INCOMPLETE;
            return S_OK;
        }
        const uint32_t length = big32(chunk);
        const std::string_view type = text_of(chunk + 4, 4);
        const bool named = std::ranges::all_of(type, [](char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); });
        const bool apple = header_due && type == "CgBI";
        if (length > 0x7fffffff || !named || (header_due && !apple && (type != "IHDR" || length != 13))) {
            out = SKYGGN_DAMAGE_CORRUPTED;
            return S_OK;
        }
        if (12 + uint64_t{length} > size - offset) {
            out = SKYGGN_DAMAGE_INCOMPLETE;
            return S_OK;
        }
        if (header_due && !apple) {
            if (crc32(chunk + 4, 4 + 13) != big32(chunk + 8 + 13)) {
                out = SKYGGN_DAMAGE_CORRUPTED;
                return S_OK;
            }
            header_due = false;
        }
        if (type == "IEND") {
            return S_OK;
        }
        offset += 12 + uint64_t{length};
    }
}

// flac (rfc 9639): blocks of tags and pictures before the sound, each with its length, the first
// one the 34-byte streaminfo, the last one marked. a block past the end, or the end before the last
// block, means the file was cut. a file not starting with streaminfo is not judged.
HRESULT check_flac(IStream* stream, uint64_t size, const deadline& limit, skyggn_damage& out) {
    uint64_t offset = 4;
    for (bool first = true;; first = false) {
        if (limit.passed()) {
            return S_OK;
        }
        uint8_t header[4];
        ULONG got = 0;
        RETURN_IF_FAILED(read_at(stream, offset, header, sizeof(header), got));
        if (got < sizeof(header)) {
            if (!first) {
                out = SKYGGN_DAMAGE_INCOMPLETE;
            }
            return S_OK;
        }
        const uint64_t length = big_n(header + 1, 3);
        if (first && ((header[0] & 0x7f) != 0 || length != 34)) {
            return S_OK;
        }
        if (4 + length > size - offset) {
            out = SKYGGN_DAMAGE_INCOMPLETE;
            return S_OK;
        }
        if (header[0] & 0x80) {
            return S_OK;
        }
        offset += 4 + length;
    }
}

// an id3v2 tag (tags and cover art before mp3 and other sound; id3.org, id3v2.4.0 structure): "ID3",
// a version (2, 3 or 4), flags with the unused bits clear, and the tag's size in four 7-bit bytes,
// plus 10 for version 4's footer. a tag longer than the file means the file was cut.
HRESULT check_id3(const uint8_t* head, ULONG got, uint64_t size, skyggn_damage& out) {
    if (got < 10) {
        return S_OK;
    }
    const uint8_t version = head[3];
    const uint8_t flags = head[5];
    const uint8_t unused = version == 4 ? 0x0f : version == 3 ? 0x1f : 0x3f;
    if (version < 2 || version > 4 || head[4] == 0xff || (flags & unused) != 0 ||
        std::any_of(head + 6, head + 10, [](uint8_t b) { return b >= 0x80; })) {
        return S_OK;  // not an id3 header after all
    }
    const uint64_t body = (uint64_t{head[6]} << 21) | (uint64_t{head[7]} << 14) | (uint64_t{head[8]} << 7) | head[9];
    const uint64_t footer = version == 4 && (flags & 0x10) ? 10 : 0;
    if (10 + body + footer > size) {
        out = SKYGGN_DAMAGE_INCOMPLETE;
    }
    return S_OK;
}

// --- documents -----------------------------------------------------------------------------------

// pdf white space (iso 32000-1, table 1), which may pad a file after its end
bool is_padding(uint8_t byte) {
    return byte == 0 || byte == ' ' || byte == '\t' || byte == '\r' || byte == '\n' || byte == '\f';
}

// pdf: ends with "%%EOF", after "startxref" and the position of its table of objects. acrobat
// takes the marker anywhere in the last 1 KB; this looks further, through the last 64 KB, after
// skipping zero or blank padding (up to 16 MB). no marker means the file was cut; a table past the
// end, that it is corrupted.
HRESULT check_pdf(IStream* stream, uint64_t size, const deadline& limit, skyggn_damage& out) {
    constexpr uint64_t kBlock = 64 * 1024;
    constexpr uint64_t kMostPadding = 16 * 1024 * 1024;
    std::vector<uint8_t> block(static_cast<size_t>(kBlock));
    uint64_t end = size;
    for (;;) {
        if (end == 0 || size - end > kMostPadding || limit.passed()) {
            return S_OK;  // nothing but padding as reached as it is read
        }
        const uint64_t start = end > kBlock ? end - kBlock : 0;
        ULONG got = 0;
        RETURN_IF_FAILED(read_at(stream, start, block.data(), static_cast<ULONG>(end - start), got));
        const auto last = std::find_if_not(block.rbegin() + (block.size() - got), block.rend(), is_padding);
        if (last != block.rend()) {
            end = start + static_cast<uint64_t>(block.rend() - last);
            break;
        }
        end = start;
    }
    const uint64_t start = end > kBlock ? end - kBlock : 0;
    ULONG got = 0;
    RETURN_IF_FAILED(read_at(stream, start, block.data(), static_cast<ULONG>(end - start), got));
    const std::string_view tail = text_of(block.data(), got);
    const size_t marker = tail.rfind("%%EOF");
    if (marker == std::string_view::npos) {
        out = SKYGGN_DAMAGE_INCOMPLETE;
        return S_OK;
    }
    constexpr std::string_view kPointer = "startxref";
    const size_t pointer = tail.rfind(kPointer, marker);
    if (pointer == std::string_view::npos) {
        return S_OK;
    }
    const size_t digits = tail.find_first_not_of(" \t\r\n\f", pointer + kPointer.size());
    uint64_t table = 0;
    if (digits != std::string_view::npos && digits < marker &&
        std::from_chars(tail.data() + digits, tail.data() + marker, table).ec == std::errc{} && table >= size) {
        out = SKYGGN_DAMAGE_CORRUPTED;
    }
    return S_OK;
}

// a file of only zero bytes (space set aside for a download that never came), read to its end
HRESULT all_zero(IStream* stream, uint64_t size, const deadline& limit, bool& zero) {
    constexpr ULONG kBlock = 1024 * 1024;
    std::vector<uint8_t> block(kBlock);
    zero = false;
    for (uint64_t offset = 0; offset < size; offset += kBlock) {
        if (limit.passed()) {
            return S_OK;  // not read to the end: not proven
        }
        ULONG got = 0;
        RETURN_IF_FAILED(read_at(stream, offset, block.data(), kBlock, got));
        if (got == 0 || !std::all_of(block.begin(), block.begin() + got, is_zero)) {
            return S_OK;
        }
    }
    zero = true;
    return S_OK;
}

}  // namespace

HRESULT find_damage(IStream* stream, bool picture_found, const deadline& limit, skyggn_damage& out) {
    out = SKYGGN_DAMAGE_NONE;
    STATSTG stat{};
    RETURN_IF_FAILED(stream->Stat(&stat, STATFLAG_NONAME));
    const uint64_t size = stat.cbSize.QuadPart;
    if (size == 0) {
        out = SKYGGN_DAMAGE_EMPTY;
        return S_OK;
    }
    uint8_t head[1024];
    ULONG got = 0;
    RETURN_IF_FAILED(read_at(stream, 0, head, sizeof(head), got));
    const std::string_view start = text_of(head, got);
    if (start.starts_with("\x89PNG\r\n\x1a\n")) {
        return check_png(stream, size, limit, out);
    }
    if (start.starts_with("\x1a\x45\xdf\xa3")) {
        return check_matroska(stream, size, out);
    }
    if (start.starts_with("PK\x03\x04")) {
        return check_zip(stream, size, out);
    }
    if (start.starts_with("7z\xbc\xaf\x27\x1c")) {
        return check_7z(stream, size, out);
    }
    if (start.starts_with("fLaC")) {
        return check_flac(stream, size, limit, out);
    }
    if (start.starts_with("ID3")) {
        return check_id3(head, got, size, out);
    }
    if (is_iso_media(head, got)) {
        return check_iso_media(stream, size, limit, out);
    }
    if (picture_found) {
        return S_OK;
    }
    if (start.find("%PDF-") != std::string_view::npos) {
        return check_pdf(stream, size, limit, out);
    }
    if (std::all_of(head, head + got, is_zero)) {
        bool zero = false;
        RETURN_IF_FAILED(all_zero(stream, size, limit, zero));
        if (zero) {
            out = SKYGGN_DAMAGE_EMPTY;
        }
    }
    return S_OK;
}

}  // namespace skyggn
