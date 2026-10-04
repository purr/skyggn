#include "byte_reader.h"

#include <wil/result.h>

namespace skyggn {

HRESULT read_at(IStream* stream, uint64_t offset, void* buffer, ULONG count, ULONG& got) {
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(offset);
    RETURN_IF_FAILED(stream->Seek(position, STREAM_SEEK_SET, nullptr));
    got = 0;
    // a stream may hand out less than asked for before its end
    while (got < count) {
        ULONG part = 0;
        RETURN_IF_FAILED(stream->Read(static_cast<uint8_t*>(buffer) + got, count - got, &part));
        if (part == 0) {
            break;
        }
        got += part;
    }
    return S_OK;
}

}  // namespace skyggn
