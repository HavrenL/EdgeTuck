#pragma once
#include <windows.h>
#include <algorithm>
#include <span>

namespace edge {
inline bool drawers_below_desktop(HWND desktop, std::span<const HWND> panels) {
    if (!desktop || !IsWindow(desktop) || panels.empty()) return false;
    for (HWND window = GetWindow(desktop, GW_HWNDNEXT); window;
         window = GetWindow(window, GW_HWNDNEXT)) {
        if (std::find(panels.begin(), panels.end(), window) != panels.end()) return true;
    }
    return false;
}
}
