#pragma once

#include <windows.h>

namespace skyggn {

// loads the ffmpeg dlls from the engine's folder. call before any ffmpeg function; a failure means
// the install is incomplete.
HRESULT load_ffmpeg();

HMODULE engine_module();

}  // namespace skyggn
