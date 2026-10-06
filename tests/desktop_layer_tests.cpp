#include "desktop_layer.hpp"
#include <array>
#include <iostream>
#include <stdexcept>

static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
struct Window {
    HWND handle = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC", L"Desktop layer fixture",
        WS_POPUP, -32000, -32000, 1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Window() {
        require(handle != nullptr, "create isolated window");
        ShowWindow(handle, SW_SHOWNOACTIVATE);
    }
    ~Window() { if (handle) DestroyWindow(handle); }
    void above(HWND after) {
        require(SetWindowPos(handle, after, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE) != FALSE, "order isolated window");
    }
};
int main() {
    try {
        // All fixtures stay offscreen. No real desktop or application window is
        // reordered, activated or minimized, and no user profile is loaded.
        Window desktop, first, second, application;
        const std::array<HWND, 2> panels{first.handle, second.handle};
        desktop.above(HWND_BOTTOM);
        second.above(HWND_TOP);
        first.above(HWND_TOP);
        application.above(HWND_TOP);
        require(!edge::drawers_below_desktop(desktop.handle, panels), "ordinary app above drawers needs no repair");
        desktop.above(HWND_TOP);
        require(edge::drawers_below_desktop(desktop.handle, panels), "desktop raised after foreground needs repair");
        first.above(HWND_TOP);
        require(edge::drawers_below_desktop(desktop.handle, panels), "one remaining covered drawer still needs repair");
        second.above(HWND_TOP);
        require(!edge::drawers_below_desktop(desktop.handle, panels), "completed repair suppresses recursive reorder events");
        application.above(HWND_TOP);
        require(!edge::drawers_below_desktop(desktop.handle, panels), "application activation does not lift drawers again");
        require(!edge::drawers_below_desktop(nullptr, panels), "missing desktop is ignored");
        require(!edge::drawers_below_desktop(desktop.handle, {}), "empty drawer set is ignored");
        DestroyWindow(desktop.handle);
        const auto stale = desktop.handle; desktop.handle = nullptr;
        require(!edge::drawers_below_desktop(stale, panels), "destroyed Explorer host is ignored");
        std::cout << "Desktop layer checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
