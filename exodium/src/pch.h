// Precompiled header: the system and library headers most of the plugin uses. It is
// force-included by the project, so no source file names it, and every source file
// still includes what it uses itself.
#pragma once

#include <windows.h>

#include <commctrl.h>
#include <dwmapi.h>
#include <objbase.h>
#include <objidl.h>
#include <shlwapi.h>
#include <windowsx.h>

#include <WebView2.h>
#include <wrl/client.h>
#include <wrl/event.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
