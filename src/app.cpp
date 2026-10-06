#include "app.hpp"
#include "desktop_layer.hpp"
#include "drop_target.hpp"
#include "desktop_grid.hpp"
#include "diagnostics.hpp"
#include "storage.hpp"
#include "archive_rename.hpp"
#include <windowsx.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <shlobj.h>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <sstream>

namespace edge {
static App* active_app = nullptr;
static constexpr UINT_PTR smoke_timer = 70;
static constexpr wchar_t control_class[] = L"EdgeTuck.Control";
static constexpr wchar_t broker_class[] = L"EdgeTuck.Broker";

static HICON make_icon() {
    constexpr int n = 32;
    BITMAPV5HEADER header{}; header.bV5Size = sizeof(header); header.bV5Width = n; header.bV5Height = -n;
    header.bV5Planes = 1; header.bV5BitCount = 32; header.bV5Compression = BI_BITFIELDS;
    header.bV5RedMask = 0x00FF0000; header.bV5GreenMask = 0x0000FF00; header.bV5BlueMask = 0x000000FF; header.bV5AlphaMask = 0xFF000000;
    void* pixels = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP bitmap = CreateDIBSection(dc, reinterpret_cast<BITMAPINFO*>(&header), DIB_RGB_COLORS, &pixels, nullptr, 0);
    ReleaseDC(nullptr, dc);
    if (!bitmap) return CopyIcon(LoadIconW(nullptr, IDI_APPLICATION));
    auto* data = static_cast<DWORD*>(pixels);
    for (int y = 0; y < n; ++y) for (int x = 0; x < n; ++x) {
        const bool rounded = (x >= 7 && x <= 24) || (y >= 7 && y <= 24) || (std::pow(x < 16 ? x - 7 : x - 24, 2) + std::pow(y < 16 ? y - 7 : y - 24, 2) < 49);
        DWORD color = rounded ? 0xFF287556 : 0;
        if (x >= 7 && x <= 24 && ((y == 8 || y == 15 || y == 23) || x == 7 || x == 24)) color = 0xFFE7F5EC;
        if (x >= 14 && x <= 17 && (y == 11 || y == 19)) color = 0xFFE7F5EC;
        data[y * n + x] = color;
    }
    HBITMAP mask = CreateBitmap(n, n, 1, 1, nullptr);
    ICONINFO info{TRUE, 0, 0, mask, bitmap};
    HICON result = CreateIconIndirect(&info);
    DeleteObject(mask); DeleteObject(bitmap);
    return result;
}

App::App(HINSTANCE instance) : instance(instance), settings(defaults()) { active_app = this; }
App::~App() {
    quitting = true;
    sort_queue.stop();
    file_watch.clear();
    if(broker) KillTimer(broker,folder_timer);
    if(reorder_hook) UnhookWinEvent(reorder_hook);
    if(!smoke && save_allowed) for(auto& d:settings.drawers) restore_folder_icon(d);
    if(graphics.composition) graphics.composition->live_active(false,broker,WM_EDGE_LIVE_FRAME);
    if(test_background) DestroyWindow(test_background);
    if (foreground_hook) UnhookWinEvent(foreground_hook);
    if (desktop_hook) UnhookWinEvent(desktop_hook);
    if (destroy_hook) UnhookWinEvent(destroy_hook);
    if (location_hook) UnhookWinEvent(location_hook);
    if (broker) tray(true);
    if (material_window) DestroyWindow(material_window);
    drawers.clear();
    if (control && IsWindow(control)) DestroyWindow(control);
    if (broker && IsWindow(broker)) DestroyWindow(broker);
    if (icon) DestroyIcon(icon);
    active_app = nullptr;
}
int App::run(bool smoke_test, bool start_hidden, bool force_fallback, bool material_preview, bool live_preview, bool test_live) {
    smoke = smoke_test || material_preview || live_preview || test_live;
    live_test=test_live;
    automated_test=smoke_test || test_live;
    config_path = default_config_path();
    if (!smoke) {
        std::wstring recovery;
        if(!recover_storage(config_path,recovery)) save_allowed=false;
        if (!load_settings(config_path, settings, notice)) { settings = defaults(); save_allowed = false; }
        if(!recovery.empty()) notice=recovery;
        DWORD error{};
        if(!set_responsive_priority(settings.responsive_priority,error)) { notice=L"未能应用运行优先级（错误 "+std::to_wstring(error)+L"）。"; diagnostic("priority.apply",HRESULT_FROM_WIN32(error)); }
        startup=startup_status();
    }
    else settings.live_background=live_preview || test_live;
    if(archive_test) {
        archive_fixture=std::filesystem::temp_directory_path()/(L"EdgeTuck-archive-ui-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
        if(!std::filesystem::create_directory(archive_fixture)) throw std::runtime_error("Cannot create archive fixture");
    }
    // Optional read-only fixtures for isolated visual previews. The ordinary
    // application never reads this variable or adds these references to settings.
    if(material_preview || live_preview) {
        const DWORD length=GetEnvironmentVariableW(L"EDGETUCK_PREVIEW_ITEMS",nullptr,0);
        if(length>1 && length<32768) {
            std::wstring value(length,L'\0');
            const DWORD read=GetEnvironmentVariableW(L"EDGETUCK_PREVIEW_ITEMS",value.data(),length);
            if(read>0 && read<length) {
                value.resize(read); std::wistringstream lines(value); std::wstring path;
                auto& items=settings.drawers.front().items;
                while(items.size()<32 && std::getline(lines,path)) {
                    if(!path.empty() && path.back()==L'\r') path.pop_back();
                    std::error_code error;
                    if(std::filesystem::path(path).is_absolute() && std::filesystem::is_regular_file(path,error)) items.push_back(path);
                }
            }
        }
    }
    graphics.initialize();
    if (smoke && !force_fallback && !graphics.composition) smoke_ok = false;
    if (smoke && force_fallback) graphics.composition.reset();
    if(graphics.composition) graphics.composition->live_mode(settings.live_background);
    refresh_metrics(); refresh_theme(); create_windows(); create_drawers();
    initialize_folders();
    sync_explorer();
    for(auto& drawer:drawers) drawer->prepare_images(ImagePriority::Background);
    if (!smoke) tray();
    refresh_file_watch();
    // Reuse this hook for desktop capture completion as well as foreground
    // changes. The intervening menu events are discarded in event_proc.
    foreground_hook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_CAPTUREEND, nullptr, event_proc, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    desktop_hook = SetWinEventHook(EVENT_SYSTEM_DESKTOPSWITCH, EVENT_SYSTEM_DESKTOPSWITCH, nullptr, event_proc, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    destroy_hook = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_HIDE, nullptr, event_proc, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    location_hook = SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE, nullptr, event_proc, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    // Also observe the final Show Desktop ordering in isolated previews. Those
    // profiles still ignore icon notifications and never park user folders.
    reorder_hook=SetWinEventHook(EVENT_OBJECT_REORDER,EVENT_OBJECT_REORDER,nullptr,event_proc,0,0,WINEVENT_OUTOFCONTEXT|WINEVENT_SKIPOWNPROCESS);
    desktop_order();
    if (smoke_test || test_live) SetTimer(broker, smoke_timer, test_live?500:350, nullptr);
    else if (!start_hidden) show_control();
    if (material_preview || live_preview) show_material();
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (material_window && IsDialogMessageW(material_window, &message)) continue;
        TranslateMessage(&message); DispatchMessageW(&message);
    }
    if (!smoke) save();
    return smoke && !smoke_ok ? 2 : static_cast<int>(message.wParam);
}
void App::refresh_metrics() {
    const HMONITOR monitor = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO info{sizeof(info)}; GetMonitorInfoW(monitor, &info); work = info.rcWork;
    const UINT dpi = GetDpiForSystem(); scale = static_cast<float>(dpi) / 96.0f;
    ICONMETRICSW metrics{sizeof(metrics)};
    if (SystemParametersInfoForDpi(SPI_GETICONMETRICS, sizeof(metrics), &metrics, 0, dpi)) {
        grid_x = std::max(48, metrics.iHorzSpacing); grid_y = std::max(48, metrics.iVertSpacing);
    } else { grid_x = static_cast<int>(80 * scale); grid_y = static_cast<int>(80 * scale); }
    POINT spacing{};
    if (desktop_spacing(spacing)) { grid_x=spacing.x; grid_y=spacing.y; }
}
void App::refresh_theme() {
    DWORD light = 1, transparent = 1, bytes = sizeof(DWORD);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &bytes);
    bytes = sizeof(DWORD);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"EnableTransparency", RRF_RT_REG_DWORD, nullptr, &transparent, &bytes);
    HIGHCONTRASTW contrast{sizeof(contrast)}; SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0);
    BOOL animation = TRUE; SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animation, 0);
    dark = settings.theme == Theme::Dark || (settings.theme == Theme::System && light == 0);
    transparency = settings.glass && transparent && !(contrast.dwFlags & HCF_HIGHCONTRASTON);
    animations = settings.motion && animation;
    if (control) backdrop(control, false);
    for (auto& drawer : drawers) drawer->apply_theme();
    sync_live_capture();
    invalidate();
}
bool App::backdrop(HWND hwnd, bool glass) {
    const BOOL use_dark = dark;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &use_dark, sizeof(use_dark));
    const DWM_WINDOW_CORNER_PREFERENCE corners = DWMWCP_ROUND;
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof(corners));
    const DWM_SYSTEMBACKDROP_TYPE type = glass && transparency ? DWMSBT_TRANSIENTWINDOW : DWMSBT_NONE;
    const HRESULT result = DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &type, sizeof(type));
    const bool active = SUCCEEDED(result) && type == DWMSBT_TRANSIENTWINDOW;
    const MARGINS margins = active ? MARGINS{-1, -1, -1, -1} : MARGINS{0, 0, 0, 0};
    DwmExtendFrameIntoClientArea(hwnd, &margins);
    return active;
}
void App::create_windows() {
    icon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR));
    if (!icon) icon = make_icon();
    WNDCLASSEXW wc{sizeof(wc)}; wc.hInstance = instance; wc.lpfnWndProc = window_proc;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW); wc.hIcon = icon; wc.hIconSm = icon;
    wc.lpszClassName = control_class; check(RegisterClassExW(&wc) ? S_OK : HRESULT_FROM_WIN32(GetLastError()));
    wc.lpszClassName = broker_class; check(RegisterClassExW(&wc) ? S_OK : HRESULT_FROM_WIN32(GetLastError()));
    broker = CreateWindowExW(WS_EX_TOOLWINDOW, broker_class, L"EdgeTuck Broker", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance, this);
    if (!broker) throw std::runtime_error("Cannot create controller");
    RECT bounds{0, 0, static_cast<LONG>(940 * scale), static_cast<LONG>(680 * scale)};
    const DWORD style = WS_OVERLAPPEDWINDOW;
    AdjustWindowRectExForDpi(&bounds, style, FALSE, 0, GetDpiForSystem());
    const int width = bounds.right - bounds.left, height = bounds.bottom - bounds.top;
    control = CreateWindowExW(0, control_class, L"轻屉 · EdgeTuck", style, work.left + ((work.right - work.left) - width) / 2, std::max(work.top, work.top + ((work.bottom - work.top) - height) / 2), width, height, nullptr, nullptr, instance, this);
    if (!control) throw std::runtime_error("Cannot create settings window");
    taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
    backdrop(control, false);
}
int App::pitch(Edge edge) const { return edge == Edge::Top ? grid_x : grid_y; }
int App::origin(Edge edge) const { return edge == Edge::Top ? work.left : work.top; }
int App::capacity(Edge edge) const {
    const int length = edge == Edge::Top ? work.right - work.left : work.bottom - work.top;
    return std::max(2, grid_cells(length, pitch(edge)));
}
int App::maximum_depth(Edge edge) const {
    const int length=edge==Edge::Top ? work.bottom-work.top : work.right-work.left;
    return std::max(3,std::min(20,grid_cells(length,edge==Edge::Top ? grid_y : grid_x)));
}
int App::minimum_span(Edge edge) const {
    return std::min(capacity(edge), std::max(2, static_cast<int>(std::ceil(222 * scale / pitch(edge)))));
}
std::vector<Slot> App::slots(Edge side) const {
    std::vector<Slot> result;
    for (const auto& d : settings.drawers) if (d.edge == side) result.push_back({d.id, d.start, d.span});
    std::sort(result.begin(), result.end(), [](auto a, auto b) { return a.start == b.start ? a.id < b.id : a.start < b.start; });
    return result;
}
void App::apply_slots(Edge side, const std::vector<Slot>& layout) {
    for (const auto& slot : layout) if (auto* d = find(slot.id); d && d->edge == side) { d->start = slot.start; d->span = slot.span; }
    bool changed = false;
    for (auto& drawer : drawers) { const int before = drawer->geometry_changes; drawer->update_geometry(); changed |= before != drawer->geometry_changes; }
    if (changed) invalidate();
}
DrawerModel* App::find(int id) {
    for (auto& d : settings.drawers) if (d.id == id) return &d;
    return nullptr;
}
void App::create_drawers() {
    drawers.clear();
    // Adapt to a changed display without dropping references from configuration.
    // Overflow drawers remain listed in settings; only fitting handles are shown.
    for (Edge side : {Edge::Left, Edge::Right, Edge::Top}) {
        int end = 0;
        for (const auto& slot : slots(side)) {
            auto* d = find(slot.id);
            d->span = std::clamp(d->span, minimum_span(side), capacity(side));
            d->depth = std::clamp(d->depth, 3, maximum_depth(side));
            d->start = std::max(end, std::min(d->start, capacity(side) - d->span));
            end = d->start + d->span;
        }
    }
    for (auto& d : settings.drawers) {
        if (d.start + d.span > capacity(d.edge)) continue;
        auto drawer = std::make_unique<Drawer>(*this, d.id); drawer->create(); drawers.push_back(std::move(drawer));
    }
}
bool App::save() {
    if (smoke) return true;
    if (storage_migrating) return false; // Worker owns the durable migration commit.
    if (!save_allowed) return false;
    std::wstring error;
    if (!save_settings(config_path, settings, error)) { notice = error; invalidate(); return false; }
    return true;
}
void App::invalidate() { if (control) InvalidateRect(control, nullptr, FALSE); }
LSTATUS App::set_explorer_visible(bool enable) {
    if(smoke || storage_migrating || (enable && !save_allowed)) return ERROR_BUSY;
    const auto root=archive_root();
    if(enable) {
        const auto attrs=GetFileAttributesW(root.c_str());
        if(attrs==INVALID_FILE_ATTRIBUTES) return GetLastError();
        if(!(attrs&FILE_ATTRIBUTE_DIRECTORY)) return ERROR_DIRECTORY;
    }
    return set_explorer_entry(enable,root,executable_path());
}
void App::sync_explorer() {
    if(smoke || !save_allowed || storage_migrating) return;
    const auto entry=explorer_entry_status();
    if(!entry.enabled || entry.error) return;
    const auto error=set_explorer_visible(true);
    if(error) { diagnostic("explorer.entry.update",HRESULT_FROM_WIN32(error)); notice=L"此电脑中的轻屉入口未能更新，可在存放位置设置中重新开启。"; invalidate(); }
}
void App::refresh_file_watch() {
    if(automated_test && !archive_test) return;
    std::vector<std::wstring> paths;
    for(const auto& d:settings.drawers) paths.insert(paths.end(),d.items.begin(),d.items.end());
    for(const auto& d:settings.drawers) if(!d.folder.empty()) {
        paths.push_back(d.folder);
        paths.push_back((std::filesystem::path(d.folder)/L".__watch_children__").wstring());
    }
    file_watch.update(broker,WM_EDGE_FILES,paths);
}
void App::references_changed() {
    for(auto& model:settings.drawers) prune_recent_uses(model);
    for(auto& d:drawers) {
        d->selection.clear(); d->focused=0; d->anchor=0; d->selected=-1;
        d->panel_canvas.icons.clear(); InvalidateRect(d->panel,nullptr,FALSE);
        d->prepare_images(d->target>0?ImagePriority::Visible:ImagePriority::Background);
        d->request_sort();
    }
    refresh_file_watch(); save(); invalidate();
}
void App::file_changed(const FileChange& change) {
    if(change.from.empty()) return;
    if(interaction_depth) { pending_changes.push_back(change); return; }
    bool relevant=false, changed=false;
    bool archive_change=false;
    for(auto& d:settings.drawers) if(!d.folder.empty()) {
        if(path_within(change.from,d.folder) || path_within(d.folder,change.from) || (!change.to.empty() && path_within(change.to,d.folder))) archive_change=true;
        if((change.event&(SHCNE_RENAMEITEM|SHCNE_RENAMEFOLDER)) && path_within(d.folder,change.from)) {
            const auto next=change.to+d.folder.substr(change.from.size());
            changed|=rebind_folder(d,next);
        }
        for(auto& p:d.legacy_items) if((change.event&(SHCNE_RENAMEITEM|SHCNE_RENAMEFOLDER)) && path_within(p,change.from)) { p=change.to+p.substr(change.from.size()); changed=true; }
        if(change.event&(SHCNE_DELETE|SHCNE_RMDIR)) std::erase_if(d.legacy_items,[&](const auto& p){return path_within(p,change.from) && GetFileAttributesW(p.c_str())==INVALID_FILE_ATTRIBUTES;});
    }
    if(archive_change) schedule_folders(true);
    for(auto& d:settings.drawers) for(const auto& p:d.items) if(path_within(p,change.from)) {
        graphics.file_images.forget(p); relevant=true;
    }
    if(!relevant && !changed) return;
    if(change.event & (SHCNE_RENAMEITEM | SHCNE_RENAMEFOLDER)) changed|=rename_references(settings,change.from,change.to);
    else if(change.event & (SHCNE_DELETE | SHCNE_RMDIR)) {
        // A replacement save can deliver a delayed delete after recreation.
        for(auto& d:settings.drawers) changed |= std::erase_if(d.items,[&](const auto& p) {
            if(!path_within(p,change.from)) return false;
            const DWORD attrs=GetFileAttributesW(p.c_str()), error=GetLastError();
            return attrs==INVALID_FILE_ATTRIBUTES && (error==ERROR_FILE_NOT_FOUND || error==ERROR_PATH_NOT_FOUND);
        })>0;
    }
    if(changed) references_changed();
    else for(auto& d:drawers) {
        d->panel_canvas.icons.clear();
        d->prepare_images(d->target>0?ImagePriority::Visible:ImagePriority::Background);
        InvalidateRect(d->panel,nullptr,FALSE);
        if(d->model().sort==SortMode::Size || d->model().sort==SortMode::Modified) d->request_sort();
    }
}
void App::finish_interaction() {
    if(interaction_depth) return;
    for(auto& d:drawers) d->apply_sort();
    auto changes=std::move(pending_changes); pending_changes.clear();
    for(const auto& change:changes) file_changed(change);
    if(!folder_sync_pending) schedule_folders(folders_dirty);
    if(metrics_pending) { metrics_pending=false; refresh_metrics(); create_drawers(); desktop_order(); schedule_folders(); invalidate(); }
    if(shutdown_pending) { quitting=true; PostQuitMessage(0); }
}
void App::tray(bool remove) {
    NOTIFYICONDATAW data{sizeof(data)}; data.hWnd = broker; data.uID = 1;
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP; data.uCallbackMessage = WM_EDGE_TRAY; data.hIcon = icon;
    wcscpy_s(data.szTip, L"轻屉 · EdgeTuck\n双击打开设置");
    if (remove) Shell_NotifyIconW(NIM_DELETE, &data);
    else { Shell_NotifyIconW(NIM_ADD, &data); data.uVersion = NOTIFYICON_VERSION_4; Shell_NotifyIconW(NIM_SETVERSION, &data); }
}
void App::show_control() {
    if(!smoke) startup=startup_status();
    ShowWindow(control, SW_RESTORE); SetForegroundWindow(control);
    for (auto& d : drawers) { d->preview = false; if (!d->pinned) d->set_open(false); }
    desktop_order();
}
static bool own_window(HWND hwnd) {
    DWORD pid = 0; GetWindowThreadProcessId(hwnd, &pid); return pid == GetCurrentProcessId();
}
void App::desktop_order() {
    if (quitting) return;
    HWND desktop = nullptr;
    EnumWindows([](HWND hwnd, LPARAM param) -> BOOL {
        if (FindWindowExW(hwnd, nullptr, L"SHELLDLL_DefView", nullptr)) { *reinterpret_cast<HWND*>(param) = hwnd; return FALSE; }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&desktop));
    if (!desktop) desktop = GetShellWindow();
    desktop_host = desktop;
    if (!desktop) { for (auto& d : drawers) ShowWindow(d->panel, SW_HIDE); return; }
    HWND anchor = GetWindow(desktop, GW_HWNDPREV);
    while (anchor && own_window(anchor)) anchor = GetWindow(anchor, GW_HWNDPREV);
    // Insert immediately above the Explorer desktop, below ordinary app windows.
    // No Explorer parenting, injection, global icon toggles, or always-on-top flag.
    std::vector<Drawer*> ordered;
    for (auto& d : drawers) ordered.push_back(d.get());
    std::stable_sort(ordered.begin(),ordered.end(),[](const Drawer* a,const Drawer* b) { return (a->target>0) > (b->target>0); });
    for (auto* d : ordered) {
        ShowWindow(d->panel, SW_SHOWNOACTIVATE);
        SetWindowPos(d->panel, anchor ? anchor : HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        anchor = d->panel;
        if (d->preview && IsWindowVisible(control)) {
            SetWindowPos(d->panel, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
    }
    sync_live_capture();
}
void App::sync_live_capture() {
    if(!graphics.composition || !broker) return;
    bool visible=false;
    if(settings.live_background && transparency && !quitting) {
        for(const auto& d:drawers) if(IsWindowVisible(d->panel)) {
            bool desktop_covered=false;
            for(HWND window=GetWindow(d->panel,GW_HWNDPREV);window;window=GetWindow(window,GW_HWNDPREV)) {
                if(window==test_background || !IsWindowVisible(window) || IsIconic(window)) continue;
                RECT r{}; GetWindowRect(window,&r);
                if(r.left>work.left || r.top>work.top || r.right<work.right || r.bottom<work.bottom) continue;
                if(GetWindowLongPtrW(window,GWL_EXSTYLE)&WS_EX_LAYERED) continue;
                DWORD cloaked{}; DwmGetWindowAttribute(window,DWMWA_CLOAKED,&cloaked,sizeof(cloaked));
                if(!cloaked) { desktop_covered=true; break; }
            }
            if(!desktop_covered) { visible=true; break; }
        }
    }
    graphics.composition->live_active(visible,broker,WM_EDGE_LIVE_FRAME);
}
void App::apply_live_mode() {
    std::vector<int> opened;
    for(auto& d:drawers) {
        if(d->target>0) opened.push_back(d->id);
        d->set_open(false); d->finish_animation(d->motion_serial);
    }
    if(graphics.composition) graphics.composition->live_mode(settings.live_background);
    for(auto& d:drawers) if(std::find(opened.begin(),opened.end(),d->id)!=opened.end()) d->set_open(true);
    sync_live_capture(); save(); invalidate();
}
void App::event_proc(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG object, LONG child, DWORD thread, DWORD) {
    if (!active_app || active_app->quitting) return;
    if (event == EVENT_OBJECT_REORDER && hwnd == GetDesktopWindow() &&
        object == OBJID_CLIENT && child == CHILDID_SELF) {
        // Show Desktop raises Explorer after its foreground event. Observe the
        // completed top-level reorder using the existing icon-event hook. Only
        // repair drawers left below the desktop, so our own SetWindowPos calls
        // cannot create an event loop or pull drawers over application windows.
        if (!active_app->desktop_order_pending && active_app->desktop_host) {
            std::vector<HWND> panels;
            panels.reserve(active_app->drawers.size());
            for (const auto& drawer : active_app->drawers) panels.push_back(drawer->panel);
            if (drawers_below_desktop(active_app->desktop_host, panels))
                active_app->desktop_order_pending = PostMessageW(active_app->broker, WM_EDGE_SYNC, 0, 0) != FALSE;
        }
        return;
    }
    if(event>=EVENT_SYSTEM_MENUSTART && event<=EVENT_SYSTEM_MENUPOPUPEND) return;
    if(event==EVENT_SYSTEM_CAPTURESTART || event==EVENT_SYSTEM_CAPTUREEND) {
        if(active_app->smoke || object!=OBJID_WINDOW || child!=CHILDID_SELF) return;
        const bool desktop=desktop_icon_event(hwnd,OBJID_CLIENT);
        const HWND source=active_app->desktop_capture_source;
        wchar_t name[64]{}; if(hwnd) GetClassNameW(hwnd,name,64);
        // Explorer hands capture from its desktop list to OLE's hidden window
        // once dragging starts. Track that handoff only on the source thread.
        const bool ole=source && desktop_icon_event(source,OBJID_CLIENT) &&
            GetWindowThreadProcessId(source,nullptr)==thread && wcscmp(name,L"CLIPBRDWNDCLASS")==0;
        if(event==EVENT_SYSTEM_CAPTURESTART) {
            if(desktop) active_app->desktop_capture_source=hwnd;
            else if(!ole) active_app->desktop_capture_source=nullptr;
        } else {
            // Native drops can clamp parked icons without REORDER/LOCATIONCHANGE
            // and without a new focus event. Check after capture is released;
            // the existing bounded timer lets Explorer finish the layout first.
            if(desktop || ole) PostMessageW(active_app->broker,WM_EDGE_FOLDERS,0,0);
            if(!desktop) active_app->desktop_capture_source=nullptr;
        }
        return;
    }
    if(desktop_icon_event(hwnd,object)) {
        if(!active_app->smoke) PostMessageW(active_app->broker,WM_EDGE_FOLDERS,0,0);
        return;
    }
    if (event >= EVENT_OBJECT_CREATE && (object != OBJID_WINDOW || child != CHILDID_SELF || !hwnd || GetAncestor(hwnd, GA_ROOT) != hwnd)) return;
    if(event==EVENT_OBJECT_LOCATIONCHANGE) {
        // A maximized foreground window can be restored without changing the
        // foreground HWND. Wake a paused capture without adding an idle timer.
        if(active_app->capture_sync_pending || !active_app->settings.live_background ||
           !std::any_of(active_app->drawers.begin(),active_app->drawers.end(),[](const auto& d) { return d->target>0 || d->animating; })) return;
        active_app->capture_sync_pending=PostMessageW(active_app->broker,WM_EDGE_CAPTURE_SYNC,0,0)!=FALSE;
        return;
    }
    PostMessageW(active_app->broker, WM_EDGE_SYNC, 0, 0);
}
void App::preview_drawer(int id) {
    for (auto& d : drawers) {
        d->preview = d->id == id;
        d->set_open(d->id == id);
    }
    notice = L"正在预览抽屉；移开鼠标后会收起。回到桌面可直接触碰边缘把手。";
    desktop_order(); invalidate();
}
void App::add_drawer(Edge side) {
    if (settings.drawers.size() >= 32) { notice = L"这个版本最多保留 32 个抽屉。"; invalidate(); return; }
    const auto layout = slots(side);
    int span = std::min(4, capacity(side));
    int start = first_gap(layout, span, capacity(side));
    if (start < 0) { span = minimum_span(side); start = first_gap(layout, span, capacity(side)); }
    if (start < 0) { notice = L"这一侧没有足够空位了。可以缩小现有抽屉，或换一个边缘。"; invalidate(); return; }
    int id = 1; for (const auto& d : settings.drawers) id = std::max(id, d.id + 1);
    if(undo) id=std::max(id,undo->id+1);
    if (id > 1000000) return;
    std::wstring name=L"新抽屉";
    std::wstring alias;
    if(!smoke) { const auto chosen=ask_name(control,L"新建抽屉",name,64,&alias); if(!chosen) return; name=*chosen; }
    DrawerModel model{id,name,side,start,span,5,{}};
    model.english_folder=true;
    if(!smoke) {
        std::wstring error;
        if(!save_allowed || !bind_new_folder(model,archive_root(),error,alias)) { notice=error; invalidate(); return; }
    }
    settings.drawers.push_back(std::move(model));
    auto drawer = std::make_unique<Drawer>(*this, id); drawer->create(); drawers.push_back(std::move(drawer));
    refresh_file_watch(); if(save()) schedule_folders(); desktop_order(); invalidate(); preview_drawer(id);
}
void App::remove_drawer(int id) {
    if (auto* d = find(id)) undo = *d; else return;
    if(!smoke && undo) restore_folder_icon(*undo);
    folder_positions.erase(id);
    std::erase_if(drawers, [id](const auto& d) { return d->id == id; });
    std::erase_if(settings.drawers, [id](const auto& d) { return d.id == id; });
    create_drawers(); desktop_order();
    notice = L"抽屉已移除，文件夹和内容已保留在桌面。可撤销这次操作。";
    refresh_file_watch(); save(); invalidate();
}
void App::restore_drawer() {
    if (!undo) return;
    if(settings.drawers.size()>=32 || find(undo->id)) { notice=L"当前抽屉数量已满，暂时无法恢复。"; invalidate(); return; }
    auto restored = *undo;
    const auto layout = slots(restored.edge);
    if (first_gap(layout, restored.span, capacity(restored.edge)) < 0) { notice = L"原来的边缘没有足够空位，暂时无法恢复。"; invalidate(); return; }
    restored.start = first_gap(layout, restored.span, capacity(restored.edge));
    settings.drawers.push_back(std::move(restored));
    auto d = std::make_unique<Drawer>(*this, undo->id); d->create(); drawers.push_back(std::move(d));
    undo.reset(); notice = L"抽屉已恢复。"; refresh_file_watch(); if(save()) schedule_folders(true); desktop_order(); invalidate();
}
void App::move_drawer_edge(int id, Edge side) {
    auto* d = find(id); if (!d || d->edge == side) return;
    const int span = std::clamp(d->span, minimum_span(side), capacity(side));
    const int start = first_gap(slots(side), span, capacity(side));
    if (start < 0) { notice = L"目标边缘没有足够空位。"; invalidate(); return; }
    d->edge = side; d->start = start; d->span = span;
    create_drawers(); save(); desktop_order(); invalidate();
}

struct RenameState { HWND edit{},alias{},validation{}; HFONT font{}; std::wstring name; std::wstring* directory_alias{}; bool done{}, accepted{}; };
static LRESULT CALLBACK rename_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* state = reinterpret_cast<RenameState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) { state = static_cast<RenameState*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams); SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state)); }
    if (!state) return DefWindowProcW(hwnd, message, wparam, lparam);
    if (message == WM_COMMAND && (LOWORD(wparam) == IDOK || LOWORD(wparam) == IDCANCEL)) {
        if (LOWORD(wparam) == IDOK) {
            wchar_t name[256]{}; GetWindowTextW(state->edit, name, 256);
            std::wstring value = name;
            const auto first = value.find_first_not_of(L" \t\r\n");
            if (first == std::wstring::npos) { if(state->validation) SetWindowTextW(state->validation,L"请填写抽屉标题。"); SetFocus(state->edit); return 0; }
            value = value.substr(first, value.find_last_not_of(L" \t\r\n") - first + 1);
            if(state->directory_alias) {
                wchar_t alias[256]{}; GetWindowTextW(state->alias,alias,256);
                if(!valid_english_archive_alias(alias)) {
                    SetWindowTextW(state->validation,L"英文目录名必填：以英文字母开头，只能包含字母和数字。");
                    SetFocus(state->alias); return 0;
                }
                *state->directory_alias=alias;
            }
            state->name = value; state->accepted = true;
        }
        DestroyWindow(hwnd); return 0;
    }
    if (message == WM_CLOSE) { DestroyWindow(hwnd); return 0; }
    if (message == WM_DESTROY) { state->done = true; return 0; }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}
std::optional<std::wstring> App::ask_name(HWND owner, const std::wstring& title, const std::wstring& value, int limit, std::wstring* directory_alias) {
    Interaction interaction(*this);
    WNDCLASSW wc{}; wc.hInstance = instance; wc.lpfnWndProc = rename_proc; wc.lpszClassName = L"EdgeTuck.Rename"; wc.hCursor = LoadCursorW(nullptr, IDC_ARROW); wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1); RegisterClassW(&wc);
    RenameState state; state.name = value; state.directory_alias=directory_alias;
    RECT parent{}; GetWindowRect(owner, &parent);
    const auto px = [this](int value) { return static_cast<int>(value * scale); };
    const int width=directory_alias?480:390,height=directory_alias?352:172;
    HWND dialog = CreateWindowExW(WS_EX_DLGMODALFRAME, wc.lpszClassName, title.c_str(), WS_CAPTION | WS_SYSMENU | WS_POPUP,
        std::clamp(parent.left+(parent.right-parent.left-px(width))/2,work.left,work.right-px(width)),
        std::clamp(parent.top+(parent.bottom-parent.top-px(height))/2,work.top,work.bottom-px(height)),px(width),px(height),owner,nullptr,instance,&state);
    if (!dialog) return {};
    state.font = CreateFontW(-px(15), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
    state.edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", state.name.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, px(20), px(directory_alias?40:20), px(directory_alias?435:335), px(32), dialog, reinterpret_cast<HMENU>(100), instance, nullptr);
    HWND okay = CreateWindowExW(0, L"BUTTON", directory_alias?L"创建":L"保存", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, px(directory_alias?269:174), px(directory_alias?262:76), px(86), px(32), dialog, reinterpret_cast<HMENU>(IDOK), instance, nullptr);
    HWND cancel = CreateWindowExW(0, L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE | WS_TABSTOP, px(directory_alias?365:270), px(directory_alias?262:76), px(86), px(32), dialog, reinterpret_cast<HMENU>(IDCANCEL), instance, nullptr);
    if(directory_alias) {
        const auto label=[&](LPCWSTR text,int y,int h) {
            HWND child=CreateWindowExW(0,L"STATIC",text,WS_CHILD|WS_VISIBLE,px(20),px(y),px(435),px(h),dialog,nullptr,instance,nullptr);
            SendMessageW(child,WM_SETFONT,reinterpret_cast<WPARAM>(state.font),TRUE); return child;
        };
        label(L"抽屉标题（可以使用中文）",14,23);
        label(L"英文目录名（必填，例如 Development）",84,23);
        state.alias=CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",directory_alias->c_str(),WS_CHILD|WS_VISIBLE|WS_TABSTOP|ES_AUTOHSCROLL,
            px(20),px(110),px(435),px(32),dialog,reinterpret_cast<HMENU>(101),instance,nullptr);
        SendMessageW(state.alias,WM_SETFONT,reinterpret_cast<WPARAM>(state.font),TRUE);
        SendMessageW(state.alias,EM_SETLIMITTEXT,80,0);
        SendMessageW(state.alias,EM_SETCUEBANNER,FALSE,reinterpret_cast<LPARAM>(L"Development"));
        label(L"将创建 ET + 英文目录名，改标题时路径保持不变。\r\n需要全英文路径时，请同时选择英文的存放位置。",153,55);
        state.validation=label(L"",213,40);
        // Controls were created in layout order rather than tab order.
        SetWindowPos(state.alias,state.edit,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
    }
    for (HWND child : {state.edit, okay, cancel}) SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(state.font), TRUE);
    SendMessageW(state.edit, EM_SETLIMITTEXT, limit, 0); SendMessageW(state.edit, EM_SETSEL, 0, -1);
    EnableWindow(owner, FALSE); ShowWindow(dialog, SW_SHOW); SetForegroundWindow(dialog); SetFocus(state.edit);
    MSG message{};
    while (!state.done && GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (message.message == WM_KEYDOWN && (message.wParam == VK_RETURN || message.wParam == VK_ESCAPE)) SendMessageW(dialog, WM_COMMAND, message.wParam == VK_RETURN ? IDOK : IDCANCEL, 0);
        else if (!IsDialogMessageW(dialog, &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
    }
    if(!state.done) { DestroyWindow(dialog); PostQuitMessage(static_cast<int>(message.wParam)); }
    EnableWindow(owner, TRUE); SetForegroundWindow(owner); DeleteObject(state.font);
    return state.accepted?std::optional<std::wstring>(state.name):std::nullopt;
}
void App::rename_drawer(int id) {
    auto* d=find(id); if(!d || automated_test) return;
    Interaction interaction(*this);
    show_control(); const auto name=ask_name(control,d->english_folder?L"抽屉改名（文件夹路径保持不变）":L"抽屉改名（同时修改分类文件夹名称）",d->name);
    if(name) rename_drawer_to(id,*name);
}
bool App::rename_drawer_to(int id,const std::wstring& name) {
    auto* d=find(id); if(!d || storage_migrating || !save_allowed) return false;
    if(smoke && !archive_test) { d->name=name; invalidate(); return true; }
    Interaction interaction(*this);
    const auto profile=archive_test?archive_fixture/L"rename-settings.dat":config_path;
    if(archive_test) {std::wstring error; if(!save_settings(profile,settings,error)) return false;}
    else if(!save()) return false;
    auto result=rename_archive(settings,id,name,profile);
    const bool saved=result.saved;
    settings=std::move(result.settings);
    if(result.recovery_required) save_allowed=false;
    if(saved || result.recovery_required) { references_changed(); schedule_folders(true); }
    notice=result.message; invalidate(); return saved;
}
void App::rename_drawer_folder(int id) {
    auto* d=find(id); if(!d || smoke || automated_test || storage_migrating || !save_allowed) return;
    Interaction interaction(*this); show_control();
    auto alias=L"Drawer"+std::to_wstring(id);
    const auto filename=std::filesystem::path(d->folder).filename().wstring();
    if(d->english_folder && filename.starts_with(L"ET") && valid_english_archive_alias(filename.substr(2))) alias=filename.substr(2);
    for(;;) {
        const auto chosen=ask_name(control,L"英文目录名（仅字母和数字，不含 ET 前缀）",alias,80);
        if(!chosen) return;
        alias=*chosen;
        if(valid_english_archive_alias(alias)) break;
        MessageBoxW(control,L"目录名必须以英文字母开头，只能包含英文字母和数字。",L"英文目录名",MB_OK|MB_ICONINFORMATION);
    }
    d=find(id); if(!d) return;
    const auto target=std::filesystem::path(d->folder).parent_path()/(L"ET"+alias);
    const auto text=L"将分类目录设为：\r\n"+target.wstring()+L"\r\n\r\n抽屉标题保持不变。改名会改变实际路径，外部软件的配置和快捷方式可能需要手动更新。\r\n同名时不会覆盖或合并。是否继续？";
    if(MessageBoxW(control,text.c_str(),L"设置英文目录名",MB_YESNO|MB_ICONQUESTION|MB_DEFBUTTON2)!=IDYES || !save()) return;
    auto result=rename_archive_folder(settings,id,alias,config_path);
    settings=std::move(result.settings);
    if(result.recovery_required) save_allowed=false;
    if(result.saved || result.recovery_required) {references_changed(); schedule_folders(true);}
    notice=result.message; invalidate();
}
void App::sync_folder_names(HWND owner) {
    if(smoke || storage_migrating || !save_allowed) return;
    std::vector<std::pair<int,std::wstring>> changes; std::wstring preview;
    for(const auto& d:settings.drawers) if(!d.folder.empty() && !d.english_folder) {
        const auto old=std::filesystem::path(d.folder).filename().wstring(),next=archive_folder_name(d.name);
        if(old!=next) {changes.emplace_back(d.id,d.name); preview+=old+L" → "+next+L"\r\n";}
    }
    if(changes.empty()) {notice=L"需要同步的文件夹名已经对应；独立英文目录不会跟随标题改名。"; invalidate(); return;}
    Interaction interaction(*this);
    const auto text=L"按当前抽屉名称修改以下文件夹：\r\n\r\n"+preview+L"\r\n这会改变实际文件路径，指向旧路径的外部配置可能需要更新。重名时不覆盖。是否继续？";
    if(MessageBoxW(owner,text.c_str(),L"同步分类文件夹名称",MB_YESNO|MB_ICONQUESTION|MB_DEFBUTTON2)!=IDYES) return;
    int completed=0;
    for(const auto& [id,name]:changes) {if(!rename_drawer_to(id,name)) break; ++completed;}
    notice=completed==static_cast<int>(changes.size())?L"已同步 "+std::to_wstring(completed)+L" 个分类文件夹。":
        L"已同步 "+std::to_wstring(completed)+L" 个，其余未修改。"+notice;
    invalidate();
}

void App::action(int id) {
    if(interaction_depth) return;
    if(id>=200 && id<=202) { control_page_to(static_cast<ControlPage>(id-200)); return; }
    if(id==210 || id==211) { drawer_scroll+=id==210?-1:1; control_page_to(ControlPage::Drawers); return; }
    if(id==52) { show_storage(); return; }
    if(id==50) {
        if(smoke) { notice=L"隔离预览不更改系统自启动设置。"; invalidate(); return; }
        startup=startup_status(); const bool enable=startup.state!=StartupState::On;
        const auto error=set_startup_enabled(enable); startup=startup_status();
        notice=error==ERROR_SUCCESS?(enable?L"已开启开机启动，登录 Windows 后在托盘运行。":L"已关闭开机启动。")
            :L"未能修改自启动设置（错误 "+std::to_wstring(error)+L"）。";
        diagnostic("startup.change",HRESULT_FROM_WIN32(error)); invalidate(); return;
    }
    if(id==51) {
        if(smoke) { notice=L"隔离预览不更改运行优先级。"; invalidate(); return; }
        const bool before=settings.responsive_priority; DWORD error{};
        if(!set_responsive_priority(!before,error)) { notice=L"优先级调整失败（错误 "+std::to_wstring(error)+L"）。"; invalidate(); return; }
        settings.responsive_priority=!before;
        if(!save()) { settings.responsive_priority=before; set_responsive_priority(before,error); }
        else notice=settings.responsive_priority?L"运行优先级已设为高于正常，立即生效。":L"运行优先级已恢复正常。";
        invalidate(); return;
    }
    if (id == 40) { show_material(); return; }
    if (id >= 1000) {
        const int drawer_id = (id - 1000) / 10, command = (id - 1000) % 10;
        if (command == 0) preview_drawer(drawer_id);
        if (command == 1) rename_drawer(drawer_id);
        if (command == 2) remove_drawer(drawer_id);
        if (command == 3) control_drawer_menu(drawer_id);
        return;
    }
    if (id >= 10 && id <= 12) settings.theme = static_cast<Theme>(id - 10);
    if (id == 20 || id == 21) settings.glass = id == 20;
    if (id == 30) settings.motion = !settings.motion;
    if (id == 100) {
        HMENU choices = CreatePopupMenu(); AppendMenuW(choices, MF_STRING, 1, L"左侧抽屉"); AppendMenuW(choices, MF_STRING, 2, L"右侧抽屉"); AppendMenuW(choices, MF_STRING, 3, L"顶部抽屉");
        POINT point{}; GetCursorPos(&point);
        const int chosen = TrackPopupMenu(choices, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, 0, control, nullptr);
        DestroyMenu(choices); if (chosen) add_drawer(static_cast<Edge>(chosen - 1));
    }
    if (id == 101) { for (auto& d : drawers) { d->preview = false; d->set_open(false); } ShowWindow(control, SW_HIDE); desktop_order(); }
    if (id == 103) restore_drawer();
    if (id == 104) { quitting = true; PostQuitMessage(0); return; }
    save(); refresh_theme();
}
void App::menu(HWND owner, POINT point) {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, 1, L"打开轻屉设置");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 2, L"新建左侧抽屉"); AppendMenuW(menu, MF_STRING, 3, L"新建右侧抽屉"); AppendMenuW(menu, MF_STRING, 4, L"新建顶部抽屉");
    if (undo) AppendMenuW(menu, MF_STRING, 5, L"撤销解散抽屉");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr); AppendMenuW(menu, MF_STRING, 6, L"退出轻屉");
    SetForegroundWindow(owner);
    const int choice = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, 0, owner, nullptr);
    DestroyMenu(menu); PostMessageW(owner, WM_NULL, 0, 0);
    if (choice == 1) show_control();
    if (choice >= 2 && choice <= 4) add_drawer(static_cast<Edge>(choice - 2));
    if (choice == 5) restore_drawer();
    if (choice == 6) { quitting = true; PostQuitMessage(0); }
}
void App::smoke_tick() {
    smoke_dispatch=true;
    const bool passed_before = smoke_ok;
    ++smoke_step;
    if (smoke_step == 1) {
        show_control(); smoke_ok &= drawers.size() == 3;
        for (auto& d : drawers) smoke_ok &= IsWindow(d->panel) && IsWindowVisible(d->panel) && d->collapsed();
        auto& first = drawers.front(); first->pinned = true;
        smoke_geometry_before = first->geometry_changes;
        const int first_x = first->model().edge == Edge::Right ? first->bounds.right-first->bounds.left-7 : 7;
        const int first_y = first->model().edge == Edge::Top ? 7 : 50;
        SendMessageW(first->panel, WM_MOUSEMOVE, 0, MAKELPARAM(first_x, first_y));
        // Immediate response, no hover timer and no real cursor movement.
        smoke_ok &= first->target == 1;
    }
    if (smoke_step == 2) {
        if (graphics.composition && animations) smoke_ok &= animation_submissions > 0;
        smoke_ok &= drawers.front()->target == 1 && drawers.front()->progress > .8f;
        if (drawers.front()->panel_canvas.surface) smoke_ok &= drawers.front()->geometry_changes == smoke_geometry_before;
        SendMessageW(drawers.front()->panel, WM_MOUSELEAVE, 0, 0);
        for (auto& d : drawers) { d->pinned = true; d->set_open(true); smoke_ok &= !(GetWindowLongPtrW(d->panel, GWL_EXSTYLE) & WS_EX_TOPMOST); }
    }
    if (smoke_step == 3) {
        for (auto& d : drawers) {
            const auto& canvas=d->panel_canvas;
            smoke_ok &= d->progress > .99f && IsWindowVisible(d->panel);
            smoke_ok &= canvas.surface ? (canvas.successful_draws>0 && !canvas.target) : canvas.target!=nullptr;
            if (transparency && graphics.composition) smoke_ok &= d->panel_canvas.surface && d->panel_canvas.surface->has_refraction() && !d->panel_canvas.surface->has_live_blur();
        }
        const unsigned samples = graphics.composition ? graphics.composition->scene_capture_count() : 0;
        const bool before_geometry = smoke_ok;
        // Exercise the real mouse-message path on all docking sides, including
        // geometry changes while open. All changes belong to this temporary run.
        for (auto& d : drawers) {
            const auto side = d->model().edge;
            for (int kind : {4, 1, 2, 3}) {
                const RECT initial = d->bounds;
                const int w = initial.right-initial.left, h = initial.bottom-initial.top;
                POINT local{static_cast<LONG>(80*scale),static_cast<LONG>(30*scale)};
                if (kind == 1) local = side == Edge::Top ? POINT{1,h/2} : POINT{w/2,1};
                if (kind == 2) local = side == Edge::Top ? POINT{w-2,h/2} : POINT{w/2,h-2};
                if (kind == 3) local = side == Edge::Top ? POINT{w/2,h-2} : POINT{side==Edge::Left?w-2:1,h/2};
                POINT anchor = local; ClientToScreen(d->panel,&anchor);
                SendMessageW(d->panel,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(local.x,local.y));
                smoke_ok &= d->resize_kind == kind;
                const auto send_delta = [&](int cells, bool subcell) {
                    POINT point = anchor;
                    const int step = kind == 3 ? (side == Edge::Top ? grid_y : grid_x) : pitch(side);
                    const int delta = subcell ? step/3 : cells*step;
                    if (kind == 3) { if(side==Edge::Top) point.y+=delta; else point.x+=side==Edge::Left?delta:-delta; }
                    else if (side==Edge::Top) point.x+=delta; else point.y+=delta;
                    ScreenToClient(d->panel,&point);
                    SendMessageW(d->panel,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(point.x,point.y));
                };
                const int geometry = d->geometry_changes, paints = paint_count;
                send_delta(0,true);
                smoke_ok &= d->geometry_changes == geometry && paint_count == paints;
                for (int delta : {1,2,-1,100,0}) {
                    send_delta(delta,false);
                    RECT rect{}; GetWindowRect(d->panel,&rect);
                    // A right-docked drawer includes the screen's trailing
                    // remainder. Its free edge, not total width, must align.
                    smoke_ok &= (rect.left-work.left)%grid_x==0 && (rect.bottom-rect.top)%grid_y==0;
                    if(side==Edge::Right) {
                        smoke_ok &= rect.right==work.right;
                        smoke_ok &= grid_cells(rect.right-rect.left,grid_x)==d->model().depth;
                        smoke_ok &= (rect.right-rect.left)%grid_x==(work.right-work.left)%grid_x;
                    } else smoke_ok &= (rect.right-work.left)%grid_x==0;
                    smoke_ok &= rect.left>=work.left && rect.top>=work.top && rect.right<=work.right && rect.bottom<=work.bottom;
                    smoke_ok &= (side==Edge::Top ? rect.left-work.left : rect.top-work.top)%pitch(side)==0;
                    const auto grid=d->content_grid();
                    smoke_ok &= grid.top()==static_cast<int>(std::lround(38*scale)) && grid.bottom()<=rect.bottom-rect.top;
                    smoke_ok &= rect.bottom-rect.top-grid.bottom()<grid.cell_height;
                    if (transparency && graphics.composition) smoke_ok &= d->panel_canvas.surface->has_refraction() && !d->panel_canvas.surface->has_live_blur();
                }
                SendMessageW(d->panel,WM_LBUTTONUP,0,0);
                smoke_ok &= EqualRect(&initial,&d->bounds) && d->resize_kind==0;
            }
        }
        // The neighbor moved by a resize must retain its own optical crop too.
        Drawer* host = drawers.front().get();
        const DrawerModel saved = host->model();
        const int span = minimum_span(saved.edge);
        if (capacity(saved.edge) >= 2*span+1) {
            host->model().start=0; host->model().span=span; host->update_geometry();
            DrawerModel neighbor{100,L"Grid neighbor",saved.edge,span,span,3,{}};
            settings.drawers.push_back(neighbor);
            auto added=std::make_unique<Drawer>(*this,neighbor.id); added->create();
            added->pinned=true; added->set_open(true); added->finish_animation(added->motion_serial);
            drawers.push_back(std::move(added));
            auto layout=slots(saved.edge);
            smoke_ok &= resize_end(layout,host->id,span+1,capacity(saved.edge),span);
            apply_slots(saved.edge,layout);
            smoke_ok &= host->model().span==span+1 && drawers.back()->model().start==span+1;
            if (transparency && graphics.composition) smoke_ok &= host->panel_canvas.surface->has_refraction() && drawers.back()->panel_canvas.surface->has_refraction();
            drawers.pop_back(); settings.drawers.pop_back();
            host->model()=saved; host->update_geometry();
        }
        if (graphics.composition) smoke_ok &= samples==graphics.composition->scene_capture_count() && graphics.composition->has_retained_scene();
        std::cout << "Grid geometry: " << (smoke_ok && before_geometry ? "PASS" : "FAIL") << "; spacing=" << grid_x << 'x' << grid_y
            << "; right remainder="<<(work.right-work.left)%grid_x
            << "; three sides share grid origin, four gestures, subcell no-op, growth/shrink/bounds, last row, neighbor push, retained optical scene, no additional capture\n";
        // Use this test executable as a read-only Shell data object. No real desktop
        // file is created or altered, and smoke mode never saves these references.
        wchar_t path[32768]{}; GetModuleFileNameW(nullptr, path, 32768);
        const DWORD before = GetFileAttributesW(path);
        PIDLIST_ABSOLUTE absolute = nullptr;
        if (SUCCEEDED(SHParseDisplayName(path, nullptr, &absolute, 0, nullptr))) {
            auto parent = ILCloneFull(absolute); ILRemoveLastID(parent);
            PCUITEMID_CHILD child = ILFindLastID(absolute);
            ComPtr<IDataObject> data;
            if (SUCCEEDED(SHCreateDataObject(parent, 1, &child, nullptr, IID_PPV_ARGS(&data)))) {
                auto& d = drawers.front(); DWORD effect = DROPEFFECT_MOVE;
                d->panel_drop->DragEnter(data.Get(), 0, {}, &effect);
                smoke_ok &= effect == DROPEFFECT_NONE; d->panel_drop->DragLeave();
                effect = DROPEFFECT_LINK | DROPEFFECT_COPY;
                d->panel_drop->DragEnter(data.Get(), 0, {}, &effect); smoke_ok &= effect == DROPEFFECT_LINK;
                d->panel_drop->Drop(data.Get(), 0, {}, &effect);
                smoke_ok &= effect == DROPEFFECT_LINK && d->model().items.size() == 1 && d->model().items[0] == path;
                smoke_ok &= GetFileAttributesW(path) == before;
            } else smoke_ok = false;
            CoTaskMemFree(parent); CoTaskMemFree(absolute);
        } else smoke_ok = false;
        // Exercise metadata transfer through the actual OLE target, selection,
        // and the modal-loop lifetime guard with test-owned executable paths.
        {
            auto& source=*drawers.front(); auto& dest=*drawers.back();
            const auto old_source=source.model().items, old_dest=dest.model().items;
            const auto second=(std::filesystem::path(path).parent_path()/L"EdgeTuckTests.exe").wstring();
            source.add_paths({second}); source.select_item(0,false,false); source.select_item(1,false,true);
            smoke_ok &= source.selected_paths().size()==2;
            source.select_item(0,true,false); smoke_ok &= source.selected_paths()==std::vector<std::wstring>{second};
            source.select_item(0,false,false);
            auto data=file_data(source.selected_paths());
            smoke_ok &= data!=nullptr;
            if(data) {
                drag_data=data.Get(); drag_source=source.id; drag_paths=source.selected_paths();
                DWORD effect=DROPEFFECT_COPY|DROPEFFECT_LINK;
                dest.panel_drop->DragEnter(data.Get(),0,{},&effect);
                dest.panel_drop->Drop(data.Get(),0,{},&effect);
                smoke_ok &= effect==DROPEFFECT_LINK && source.model().items==std::vector<std::wstring>{second};
                smoke_ok &= std::any_of(dest.model().items.begin(),dest.model().items.end(),[&](const auto& p){return same_path(p,path);});
                drag_data=nullptr; drag_source=0; drag_paths.clear();
            }
            source.select_item(0,false,false); source.remove_selected(); smoke_ok &= source.model().items.empty();
            smoke_ok &= GetFileAttributesW(path)==before && std::filesystem::exists(second);
            const HWND original=source.panel;
            {
                Interaction operation(*this); SendMessageW(broker,WM_DISPLAYCHANGE,0,0);
                smoke_ok &= metrics_pending && IsWindow(original) && drawers.front()->panel==original;
                metrics_pending=false; // This test did not change the real display.
            }
            source.model().items=old_source; dest.model().items=old_dest; references_changed();
            std::cout<<"File interaction: "<<(smoke_ok?"PASS":"FAIL")<<"; multi-selection, internal OLE transfer, reference removal, original attributes, deferred display reconstruction\n";
        }
        {
            auto& drawer=*drawers.front(); const auto saved_sort_model=drawer.model();
            const auto second=(std::filesystem::path(path).parent_path()/L"EdgeTuckTests.exe").wstring();
            drawer.model().items={path,second}; drawer.model().recent_uses.clear();
            drawer.pinned=true; drawer.set_open(true); drawer.finish_animation(drawer.motion_serial);
            drawer.choose_sort(SortMode::Recent,true);
            drawer.select_item(0,false,false);
            smoke_ok &= drawer.item_count()==2 && drawer.selected_paths()==std::vector<std::wstring>{path};
            smoke_ok &= drawer.hit_item(grid_x/2/scale,(drawer.content_grid().top()+10)/scale)==0;
            smoke_ok &= drawer.resize_hit({static_cast<LONG>(drawer.bounds.right-drawer.bounds.left-60*scale),static_cast<LONG>(18*scale)})==0;
            drawer.opened_item(second,false,true); drawer.opened_item(second,true,false);
            smoke_ok &= drawer.model().recent_uses.empty();
            drawer.opened_item(second,true,true);
            smoke_ok &= recent_use(drawer.model(),second)>0 && drawer.model().items==std::vector<std::wstring>{path,second};
            drawer.pinned=false; drawer.set_open(false);
            if(drawer.animating) smoke_ok &= drawer.model().items.front()==path;
            drawer.finish_animation(drawer.motion_serial);
            smoke_ok &= drawer.model().items==std::vector<std::wstring>{second,path};
            drawer.set_open(true); drawer.finish_animation(drawer.motion_serial);
            smoke_ok &= drawer.model().items.front()==second;
            drawer.model()=saved_sort_model; drawer.request_sort(); drawer.pinned=true;
            InvalidateRect(drawer.panel,nullptr,FALSE);
            std::cout<<"Drawer sorting: "<<(smoke_ok?"PASS":"FAIL")<<"; first file in cell zero, header sort hit region, successful-double-click-only usage, stable open view, reorder at close and next open\n";
        }
        settings.theme = Theme::Dark; refresh_theme();
    }
    if (smoke_step == 4) {
        // Leave only the first drawer's clean scene, then enter another drawer
        // during its closing animation. The old implementation re-captured the
        // visible outgoing body at this exact point.
        auto& outgoing=drawers.front(); auto& incoming=drawers.back();
        for (size_t i=1;i<drawers.size();++i) { drawers[i]->pinned=false; drawers[i]->set_open(false); drawers[i]->finish_animation(drawers[i]->motion_serial); }
        incoming->model().start=capacity(Edge::Top)-incoming->model().span;
        incoming->update_geometry();
        const unsigned captures=graphics.composition?graphics.composition->scene_capture_count():0;
        outgoing->pinned=false; outgoing->set_open(false);
        if (graphics.composition && animations) smoke_ok &= outgoing->animating && graphics.composition->has_retained_scene();
        incoming->set_open(true);
        if (transparency && graphics.composition) smoke_ok &= incoming->panel_canvas.surface->has_refraction() && graphics.composition->scene_capture_count()==captures;
        smoke_ok &= outgoing->collapsed(); // Intersecting corner switches atomically.
        InvalidateRect(outgoing->panel,nullptr,FALSE); UpdateWindow(outgoing->panel);
        if (outgoing->panel_canvas.surface) smoke_ok &= !outgoing->panel_canvas.surface->has_refraction();
        for (auto& d:drawers) {
            DWORD affinity{}; smoke_ok &= GetWindowDisplayAffinity(d->panel,&affinity) && affinity==WDA_NONE;
            d->pinned=true;
        }
        std::cout << "Rapid corner handoff: " << (smoke_ok?"PASS":"FAIL") << "; outgoing scene retained, no contaminated re-capture, screenshot affinity unchanged\n";
        for(auto& d:drawers) { d->set_open(true); d->finish_animation(d->motion_serial); }
        POINT pointer{}; GetCursorPos(&pointer);
        const HWND hovered = GetAncestor(WindowFromPoint(pointer), GA_ROOT);
        for (auto& d : drawers) if (d->panel != hovered) {
            d->pinned = false; d->preview_deadline = 0; d->menu_open = true;
            SendMessageW(d->panel, WM_MOUSELEAVE, 0, 0);
            smoke_ok &= d->target == 1; // Popup interaction protects the drawer.
            d->menu_open = false;
            SendMessageW(d->panel, WM_MOUSELEAVE, 0, 0);
            smoke_ok &= d->target == 0; // Same message dispatch; no timer wait.
            if (d->panel_canvas.surface) smoke_ok &= !d->panel_canvas.surface->has_refraction();
            d->pinned = true; d->set_open(true);
            break;
        }
        auto& first = drawers.front(); first->pinned=true; first->set_open(false);
        if (first->panel_canvas.surface) smoke_ok &= !first->panel_canvas.surface->has_refraction();
        const auto stale = first->motion_serial;
        first->set_open(true); first->finish_animation(stale);
        smoke_ok &= first->target == 1 && first->motion_serial != stale;
        if (animations && first->panel_canvas.surface) smoke_ok &= first->animating;
        settings.glass = false; refresh_theme();
    }
    if (smoke_step == 5) {
        // The previous stage switched material in the middle of a slide.
        // Completion must arrive normally, before the 480 ms recovery deadline.
        smoke_ok &= !drawers.front()->animating && drawers.front()->progress==1;
        settings.motion = false; refresh_theme(); for (auto& d : drawers) { d->pinned = false; d->set_open(false); }
    }
    if (smoke_step == 6) {
        for (auto& d : drawers) {
            smoke_ok &= IsWindowVisible(d->panel) && d->collapsed();
            HRGN region = CreateRectRgn(0, 0, 0, 0); RECT hit{};
            GetWindowRgn(d->panel, region); GetRgnBox(region, &hit); DeleteObject(region);
            if (d->panel_canvas.surface) smoke_ok &= d->model().edge == Edge::Top ? hit.bottom-hit.top == static_cast<int>(14*scale) : hit.right-hit.left == static_cast<int>(14*scale);
            else {
                RECT rect{}, visible{}; GetWindowRect(d->panel, &rect); IntersectRect(&visible, &rect, &work);
                smoke_ok &= d->model().edge == Edge::Top ? visible.bottom-visible.top == static_cast<int>(14*scale) : visible.right-visible.left == static_cast<int>(14*scale);
            }
        }
        const int id = drawers.back()->id; remove_drawer(id); smoke_ok &= drawers.size() == 2; restore_drawer(); smoke_ok &= drawers.size() == 3;
        // Repainting closed glass must not resurrect the previous edge sample.
        settings.glass = true; refresh_theme();
    }
    if (smoke_step == 7) {
        if (graphics.composition) smoke_ok &= !graphics.composition->has_retained_scene();
        for (auto& d : drawers) if (d->panel_canvas.surface) {
            smoke_ok &= !d->panel_canvas.surface->has_refraction();
            if (transparency) smoke_ok &= d->panel_canvas.surface->has_live_blur();
        }
        smoke_ok &= paint_count > 0;
        std::cout << "Native smoke: " << (smoke_ok ? "PASS" : "FAIL") << "; paints=" << paint_count << "; animation_submissions=" << animation_submissions << "; composition=" << (graphics.composition ? "available" : "fallback") << "; immediate hover, fixed window geometry (composition), stale completion, edge refraction (glass), collapsed hit region, OLE reference-only drop checked\n";
        KillTimer(broker, smoke_timer); quitting = true; PostQuitMessage(smoke_ok ? 0 : 2);
    }
    if (passed_before && !smoke_ok) {
        std::cout << "First failing native smoke stage: " << smoke_step << '\n';
        for (auto& d : drawers) std::cout << "drawer=" << d->id << " progress=" << d->progress << " target=" << d->target << " animating=" << d->animating << " geometry=" << d->geometry_changes << " refraction=" << (d->panel_canvas.surface && d->panel_canvas.surface->has_refraction()) << '\n';
    }
    smoke_dispatch=false;
}
LRESULT App::window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) { app = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams); SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app)); }
    if (!app) return DefWindowProcW(hwnd, message, wparam, lparam);
    if(hwnd==app->control && app->control_input(message,wparam,lparam)) return 0;
    if (app->taskbar_created && message == app->taskbar_created) { if (!app->smoke) app->tray(); app->desktop_order(); app->schedule_folders(true); return 0; }
    switch (message) {
    case WM_ACTIVATE:
        if(hwnd==app->control && LOWORD(wparam)!=WA_INACTIVE && !app->smoke) { app->startup=startup_status(); app->invalidate(); }
        break;
    case WM_PAINT: if (hwnd == app->control) { app->paint_control(); return 0; } break;
    case WM_ERASEBKGND: return 1;
    case WM_CLOSE: ShowWindow(hwnd, SW_HIDE); return 0;
    case WM_EDGE_SHOW: app->show_control(); return 0;
    case WM_EDGE_EXPLORER_ENTRY: return app->set_explorer_visible(wparam!=0);
    case WM_EDGE_SHUTDOWN:
        if(app->interaction_depth) { app->shutdown_pending=true; return 0; }
        app->quitting = true; PostQuitMessage(0); return 0;
    case WM_EDGE_FILES: app->file_changed(FileWatch::read(wparam,lparam)); return 0;
    case WM_EDGE_SORTED:
        for(auto& result:app->sort_queue.take()) for(auto& drawer:app->drawers) if(drawer->id==result.drawer && drawer->sort_ticket==result.ticket) {
            drawer->pending_sort=std::move(result); drawer->apply_sort(); break;
        }
        return 0;
    case WM_EDGE_FOLDERS: app->schedule_folders(); return 0;
    case WM_QUERYENDSESSION: return app->storage_migrating?FALSE:TRUE;
    case WM_ENDSESSION:
        if(wparam) { app->quitting=true; app->save(); if(!app->smoke) for(auto& d:app->settings.drawers) restore_folder_icon(d); PostQuitMessage(0); }
        return 0;
    case WM_EDGE_DEFERRED: app->finish_interaction(); return 0;
    case WM_EDGE_SYNC:
        app->desktop_order_pending = false;
        for (auto& d : app->drawers) if (d->preview && !own_window(GetForegroundWindow())) { d->preview = false; d->set_open(false); }
        app->desktop_order(); return 0;
    case WM_EDGE_REMOVE: if(!app->interaction_depth) app->remove_drawer(static_cast<int>(wparam)); return 0;
    case WM_EDGE_CAPTURE_SYNC: app->capture_sync_pending=false; app->sync_live_capture(); return 0;
    case WM_APP + 20: if(!app->interaction_depth) app->move_drawer_edge(static_cast<int>(wparam), static_cast<Edge>(lparam)); return 0;
    case WM_APP + 21: if(!app->interaction_depth) app->rename_drawer(static_cast<int>(wparam)); return 0;
    case WM_APP + 22: if(!app->interaction_depth) app->rename_drawer_folder(static_cast<int>(wparam)); return 0;
    case WM_EDGE_TRAY:
        if(app->interaction_depth) return 0;
        if (LOWORD(lparam) == WM_LBUTTONDBLCLK || LOWORD(lparam) == NIN_KEYSELECT) app->show_control();
        if (LOWORD(lparam) == WM_CONTEXTMENU || LOWORD(lparam) == WM_RBUTTONUP) { POINT point{}; GetCursorPos(&point); app->menu(hwnd, point); }
        return 0;
    case WM_TIMER:
        if(wparam==folder_timer) { app->sync_folders(); return 0; }
        if (wparam == smoke_timer) { if(app->archive_test) app->archive_test_tick(); else if(app->live_test) app->live_test_tick(); else app->smoke_tick(); } return 0;
    case WM_EDGE_LIVE_FRAME:
        app->sync_live_capture();
        if(app->graphics.composition) {
            try { app->graphics.composition->live_frame(); }
            catch(...) { app->graphics.composition->live_mode(false); app->notice=L"实时背景暂不可用，已恢复系统透明效果。"; app->invalidate(); }
        }
        return 0;
    case WM_DISPLAYCHANGE:
        if(app->interaction_depth) { app->metrics_pending=true; return 0; }
        app->refresh_metrics(); app->create_drawers(); app->desktop_order(); app->schedule_folders(); app->invalidate(); return 0;
    case WM_SETTINGCHANGE:
        if(app->interaction_depth) { app->metrics_pending=true; return 0; }
        if (wparam == SPI_SETWORKAREA || wparam == SPI_SETICONMETRICS || wparam == SPI_ICONHORIZONTALSPACING || wparam == SPI_ICONVERTICALSPACING) { app->refresh_metrics(); app->create_drawers(); app->desktop_order(); }
        app->refresh_theme(); app->schedule_folders(); return 0;
    case WM_THEMECHANGED: app->refresh_theme(); return 0;
    case WM_DPICHANGED: {
        const auto* rect = reinterpret_cast<RECT*>(lparam); SetWindowPos(hwnd, nullptr, rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top, SWP_NOZORDER | SWP_NOACTIVATE); return 0;
    }

    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}
}
