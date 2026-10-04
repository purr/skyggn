#include "fingerprint.h"

#include <bcrypt.h>
#include <wil/resource.h>
#include <wil/result.h>

#include <cwctype>
#include <vector>

namespace skyggn {

namespace {

struct fnv1a {
    uint64_t hash = 14695981039346656037ull;
    void add(const uint8_t* bytes, size_t count) {
        for (size_t i = 0; i < count; ++i) {
            hash = (hash ^ bytes[i]) * 1099511628211ull;
        }
    }
};

// fnv-1a mixes its last bytes into the low bits only; this spreads every input bit over all 64
// (splitmix64's finaliser), so similar files get unrelated numbers
uint64_t spread(uint64_t value) {
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ull;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebull;
    return value ^ (value >> 31);
}

}  // namespace

HRESULT content_fingerprint(IStream* stream, const deadline& limit, uint64_t& out, bool& complete) {
    complete = false;
    // a larger file is not read at all. reading on until the time limit held up every thumbnail
    // after it for seconds (windows makes them one at a time), and made the colour depend on the
    // disk's speed: one file got its own colour on a fast disk and its kind's on a slow one.
    STATSTG stat{};
    RETURN_IF_FAILED(stream->Stat(&stat, STATFLAG_NONAME));
    if (stat.cbSize.QuadPart > kLargestFingerprinted) {
        return S_OK;
    }
    // every byte counts: samples of a file would give files that differ elsewhere the same colour
    wil::unique_bcrypt_hash hash;
    RETURN_IF_NTSTATUS_FAILED(BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, &hash, nullptr, 0, nullptr, 0, 0));
    RETURN_IF_FAILED(stream->Seek({}, STREAM_SEEK_SET, nullptr));
    std::vector<uint8_t> block(1024 * 1024);
    for (;;) {
        if (limit.passed()) {
            return S_OK;
        }
        ULONG read = 0;
        RETURN_IF_FAILED(stream->Read(block.data(), static_cast<ULONG>(block.size()), &read));
        if (read == 0) {
            break;
        }
        RETURN_IF_NTSTATUS_FAILED(BCryptHashData(hash.get(), block.data(), read, 0));
    }
    uint8_t digest[32];
    RETURN_IF_NTSTATUS_FAILED(BCryptFinishHash(hash.get(), digest, sizeof(digest), 0));
    out = 0;
    for (int i = 0; i < 8; ++i) {
        out = (out << 8) | digest[i];
    }
    complete = true;
    return S_OK;
}

uint64_t text_fingerprint(std::wstring_view text) {
    fnv1a hash;
    for (wchar_t c : text) {
        const auto lower = static_cast<wchar_t>(towlower(c));
        hash.add(reinterpret_cast<const uint8_t*>(&lower), sizeof(lower));
    }
    return spread(hash.hash);
}

}  // namespace skyggn
