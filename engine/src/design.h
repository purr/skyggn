#pragma once

#include "deadline.h"

#include <objidl.h>

#include <cstdint>
#include <string>
#include <vector>

namespace skyggn {

// a preview a design file carries, still encoded; its extension says how to decode it. its size
// and whether it has colour are read from its own header (0 and false when it does not say).
struct embedded_preview {
    std::wstring extension;  // L".jpg" or L".tif"
    std::vector<uint8_t> data;
    uint32_t width = 0;
    uint32_t height = 0;
    bool colour = false;
};

// the previews an adobe design file carries, best first: in colour before black and white (an eps
// file's tiff preview often is), then the most pixels. they are:
// - the tiff preview behind an eps file's binary header (encapsulated postscript 3.0, 5.2)
// - the jpeg thumbnails in its xmp metadata (xmpGImg:image, in base64): an illustrator file's
//   picture of its artwork (illustrator stores up to 256 px), an indesign file's one per page, of
//   which the first page's are taken
// indesign keeps several copies, some of them broken; a copy that is not clean base64 is left out,
// and the caller drops any that does not decode. the format is told by the file's contents: an .ai
// file can be a pdf, an eps file or postscript, depending on the illustrator version that saved it.
HRESULT find_design_previews(IStream* stream, const deadline& limit, std::vector<embedded_preview>& out);

// whether the file is a pdf, as illustrator files saved with pdf content are
HRESULT is_pdf(IStream* stream, bool& pdf);

}  // namespace skyggn
