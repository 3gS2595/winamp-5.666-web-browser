// <gdiplus.h>, which still expects the min and max macros that NOMINMAX takes away
// and the COM declarations that WIN32_LEAN_AND_MEAN leaves out.
#pragma once

#include <windows.h>

#include <objidl.h>

#include <algorithm>

namespace Gdiplus {
using std::max;
using std::min;
} // namespace Gdiplus

#include <gdiplus.h>
