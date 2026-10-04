#pragma once

#include "deadline.h"

#include <objidl.h>

#include <cstdint>
#include <string_view>

namespace skyggn {

// the largest file content_fingerprint reads: a long song, not a film
inline constexpr uint64_t kLargestFingerprinted = 64 * 1024 * 1024;

// a number that is the same for files with the same contents, whatever their names, and differs
// for any other file: the first 64 bits of a sha-256 of the whole file. `complete` is false when
// the file is larger than kLargestFingerprinted or could not be read to its end within the time
// limit, and `out` is then not set.
HRESULT content_fingerprint(IStream* stream, const deadline& limit, uint64_t& out, bool& complete);

// the same for text, case aside (an album's name)
uint64_t text_fingerprint(std::wstring_view text);

}  // namespace skyggn
