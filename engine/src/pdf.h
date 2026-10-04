#pragma once

#include "deadline.h"
#include "image.h"

#include <objidl.h>

namespace skyggn {

// a pdf's first page, drawn to fit `size` by windows' own pdf renderer (Windows.Data.Pdf)
HRESULT pdf_image(IStream* stream, UINT size, const deadline& limit, image& out);

}  // namespace skyggn
