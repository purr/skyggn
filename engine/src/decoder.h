#pragma once

#include "image.h"
#include "settings.h"

#include <objidl.h>

namespace skyggn {

// makes a thumbnail from a media file, at most `size` pixels on its longer side, with its badge.
// the picture is embedded cover art, a video frame, an image or a camera's raw preview. `damage`,
// when given, gets what the file's structure proves wrong with it (damage.h), which the thumbnail
// shows.
HRESULT make_thumbnail_image(IStream* stream, UINT size, const settings& options, image& picture,
                             skyggn_damage* damage = nullptr);

// the same, as the bitmap windows asks thumbnail providers for (premultiplied 32-bit). `transparent`
// tells whether it has transparent parts, the room around a badge.
HRESULT make_thumbnail(IStream* stream, UINT size, const settings& options, HBITMAP* bitmap, bool* transparent);

}  // namespace skyggn
