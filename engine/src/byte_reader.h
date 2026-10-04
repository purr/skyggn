#pragma once

#include <objidl.h>

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace skyggn {

// reads up to `count` bytes at `offset`; `got` says how many there were before the end
HRESULT read_at(IStream* stream, uint64_t offset, void* buffer, ULONG count, ULONG& got);

// numbers stored in a file, most significant byte first (big) or last (little)
inline uint32_t big32(const uint8_t* bytes) {
    return (uint32_t{bytes[0]} << 24) | (uint32_t{bytes[1]} << 16) | (uint32_t{bytes[2]} << 8) | bytes[3];
}

inline uint64_t big64(const uint8_t* bytes) {
    return (uint64_t{big32(bytes)} << 32) | big32(bytes + 4);
}

// a big-endian number of 0 to 8 bytes
inline uint64_t big_n(const uint8_t* bytes, size_t count) {
    uint64_t value = 0;
    for (size_t i = 0; i < count; ++i) {
        value = (value << 8) | bytes[i];
    }
    return value;
}

inline uint32_t little32(const uint8_t* bytes) {
    return (uint32_t{bytes[3]} << 24) | (uint32_t{bytes[2]} << 16) | (uint32_t{bytes[1]} << 8) | bytes[0];
}

inline uint64_t little64(const uint8_t* bytes) {
    return (uint64_t{little32(bytes + 4)} << 32) | little32(bytes);
}

inline std::string_view text_of(const uint8_t* bytes, size_t count) {
    return {reinterpret_cast<const char*>(bytes), count};
}

}  // namespace skyggn
