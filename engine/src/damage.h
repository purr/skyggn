#pragma once

#include "deadline.h"

#include <skyggn/skyggn.h>

#include <objidl.h>

namespace skyggn {

// what is proven wrong with a file, from its own structure alone. a file is never called damaged
// because it could not be read: an unknown codec, copy protection, a password or the time limit
// fail just the same. only what a healthy file cannot show counts (skyggn_damage):
//   empty       no bytes, or nothing but zero bytes
//   incomplete  it ends before its own structure says it does (a cut download or copy, a recording
//               that stopped), or lacks a part every finished file of its format has
//   corrupted   a checksum over its header fails, or it points past its own end
// the format is told by the file's first bytes, not its name. formats whose writers leave wrong
// sizes in healthy files are not judged: wav and avi written through a pipe keep placeholder sizes,
// and phones put a video after a jpeg's end.
//
// `picture_found` says whether a picture was read. two checks only hold for a file nothing could be
// read from, and run only without one: all zeros (read to the end, so only within the time limit),
// and a pdf's end (some pdfs that windows draws lack theirs).
HRESULT find_damage(IStream* stream, bool picture_found, const deadline& limit, skyggn_damage& out);

}  // namespace skyggn
