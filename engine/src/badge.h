#pragma once

#include "image.h"
#include "settings.h"

#include <skyggn/skyggn.h>

#include <cstdint>
#include <string_view>

namespace skyggn {

// the badge sits on one of the picture's corners, mostly on the picture, hanging out into a
// transparent room around it but short of the thumbnail's edge, where its soft shadow fades. the
// picture keeps its shape and stays centred: the room is the same on all four sides (a quarter of
// the badge), whichever corner the badge is on.
//
// the badge is the same share of every thumbnail. it is measured on the finished thumbnail, not on
// the size windows asked for: a picture smaller than that (a 600 px cover asked for at 1280) is not
// enlarged, and a badge sized for 1280 would dwarf it once windows scales the thumbnail to its view.

// the room to leave around the picture in a thumbnail of `size`; the picture fits inside the rest
UINT badge_room(UINT size, const badge_look& look);

// lets go of what drawing keeps between thumbnails (the direct2d factory). for com's unload of
// the dll, when no thumbnail is being made; the next one sets it up again.
void release_drawing_resources();

// puts the picture in the middle of a canvas with the badge's room around it, and draws the badge
// over its corner. `size` is the thumbnail size windows asked for, `extension` (with the dot) the
// labelled style's text. `warned` adds an amber warning mark to the badge's corner, for a file
// proven damaged (damage.h) that still gave a picture.
HRESULT draw_badge(image& picture, UINT size, std::wstring_view extension, skyggn_category category,
                   const badge_look& look, bool warned);

// a stand-in for a file without a picture (a song without cover art, a damaged video): a tile in
// the picture's place with the kind's symbol, big, and the file type below it. its colour is the
// kind's, or one made from `colour_seed` (see fingerprint.h), so a file's colour never changes and
// files with the same seed share it. it has the same room around it as a thumbnail with a badge,
// but no badge: the tile itself says what the file is. a proven `damage` (empty, incomplete,
// corrupted) is named under the file type, and the tile gets the badge's warning mark on its corner.
HRESULT draw_placeholder(UINT size, std::wstring_view extension, uint64_t colour_seed,
                         skyggn_category category, skyggn_placeholder style, const badge_look& badge,
                         skyggn_damage damage, image& out);

}  // namespace skyggn
