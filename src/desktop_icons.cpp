#include "desktop_icons.hpp"
#include "archive.hpp"
#include "references.hpp"
#include "shell_files.hpp"
#include <exdisp.h>
#include <servprov.h>
#include <algorithm>

namespace edge {
struct DesktopView {
    ComPtr<IShellView> view; ComPtr<IFolderView2> folder;
    HWND window{}; POINT origin{},pitch{}; RECT work{}; DWORD flags{};
    bool open() {
        ComPtr<IShellWindows> windows;
        if(FAILED(CoCreateInstance(CLSID_ShellWindows,nullptr,CLSCTX_LOCAL_SERVER,IID_PPV_ARGS(&windows)))) return false;
        VARIANT location{},root{}; long desktop{}; ComPtr<IDispatch> dispatch;
        if(FAILED(windows->FindWindowSW(&location,&root,SWC_DESKTOP,&desktop,SWFO_NEEDDISPATCH,&dispatch)) || !dispatch) return false;
        ComPtr<IServiceProvider> provider; ComPtr<IShellBrowser> browser;
        if(FAILED(dispatch.As(&provider)) || FAILED(provider->QueryService(SID_STopLevelBrowser,IID_PPV_ARGS(&browser)))
            || FAILED(browser->QueryActiveShellView(&view)) || FAILED(view.As(&folder))) return false;
        if(FAILED(view->GetWindow(&window)) || FAILED(folder->GetCurrentFolderFlags(&flags)) || FAILED(folder->GetSpacing(&pitch)) || pitch.x<16 || pitch.y<16) return false;
        HWND list=FindWindowExW(window,nullptr,L"SysListView32",nullptr); if(list) window=list;
        ClientToScreen(window,&origin);
        MONITORINFO info{sizeof(info)}; GetMonitorInfoW(MonitorFromPoint({0,0},MONITOR_DEFAULTTOPRIMARY),&info); work=info.rcWork;
        OffsetRect(&work,-origin.x,-origin.y); return true;
    }
};
bool desktop_icon_event(HWND window,LONG object) {
    if(!window || object!=OBJID_CLIENT) return false;
    wchar_t name[64]{}; GetClassNameW(window,name,64);
    if(wcscmp(name,L"SysListView32")!=0) return false;
    GetClassNameW(GetParent(window),name,64);
    return wcscmp(name,L"SHELLDLL_DefView")==0;
}
static bool on_desktop(const DrawerModel& d) {
    return !d.folder.empty() && same_path(std::filesystem::path(d.folder).parent_path().wstring(),desktop_directory().wstring());
}
ParkResult park_folder_icon(DrawerModel& d) {
    if(!on_desktop(d)) return ParkResult::NotOnDesktop;
    if(!folder_available(d)) return ParkResult::Unavailable;
    DesktopView desktop; if(!desktop.open()) return ParkResult::Unavailable;
    // Never alter global auto-arrange, snap-to-grid, or desktop visibility.
    if(desktop.flags&FWF_AUTOARRANGE) return ParkResult::AutoArrange;
    PIDLIST_ABSOLUTE absolute{};
    if(FAILED(SHParseDisplayName(d.folder.c_str(),nullptr,&absolute,0,nullptr))) return ParkResult::Unavailable;
    PCUITEMID_CHILD item=ILFindLastID(absolute); POINT current{};
    if(FAILED(desktop.folder->GetItemPosition(item,&current))) { CoTaskMemFree(absolute); return ParkResult::Unavailable; }
    const LONG right=GetSystemMetrics(SM_XVIRTUALSCREEN)+GetSystemMetrics(SM_CXVIRTUALSCREEN)-desktop.origin.x;
    if(!d.restore_position && PtInRect(&desktop.work,current)) { d.restore_position=true; d.restore_x=current.x; d.restore_y=current.y; }
    HRESULT hr=S_OK;
    if(current.x<right+desktop.pitch.x) {
        // Positive coordinates avoid Explorer's negative-coordinate normalization.
        const LONG phase=d.restore_position?((d.restore_x%desktop.pitch.x)+desktop.pitch.x)%desktop.pitch.x:17;
        POINT requested{phase+((right/desktop.pitch.x)+100+(d.id%64))*desktop.pitch.x,
            d.restore_position?d.restore_y:desktop.work.top+2};
        hr=desktop.folder->SelectAndPositionItems(1,&item,&requested,SVSI_POSITIONITEM|SVSI_NOSTATECHANGE);
        if(SUCCEEDED(hr)) hr=desktop.folder->GetItemPosition(item,&current);
    }
    CoTaskMemFree(absolute);
    return SUCCEEDED(hr) && current.x>=right+desktop.pitch.x?ParkResult::Parked:ParkResult::Unavailable;
}
bool restore_folder_icon(DrawerModel& d) {
    if(!on_desktop(d) || !folder_available(d)) return false;
    DesktopView desktop; if(!desktop.open()) return false;
    PIDLIST_ABSOLUTE absolute{};
    if(FAILED(SHParseDisplayName(d.folder.c_str(),nullptr,&absolute,0,nullptr))) return false;
    PCUITEMID_CHILD item=ILFindLastID(absolute); POINT current{};
    if(FAILED(desktop.folder->GetItemPosition(item,&current))) { CoTaskMemFree(absolute); return false; }
    if(PtInRect(&desktop.work,current) || (desktop.flags&FWF_AUTOARRANGE)) { CoTaskMemFree(absolute); return true; }
    std::vector<POINT> occupied; int count{}; desktop.folder->ItemCount(SVGIO_ALLVIEW,&count);
    for(int i=0;i<count && i<10000;++i) {
        PITEMID_CHILD p{}; POINT position{};
        if(SUCCEEDED(desktop.folder->Item(i,&p))) {
            if(SUCCEEDED(desktop.folder->GetItemPosition(p,&position)) && PtInRect(&desktop.work,position)) occupied.push_back(position);
            CoTaskMemFree(p);
        }
    }
    auto free=[&](POINT p) { return PtInRect(&desktop.work,p) && std::none_of(occupied.begin(),occupied.end(),[&](POINT q){return abs(p.x-q.x)<desktop.pitch.x/2 && abs(p.y-q.y)<desktop.pitch.y/2;}); };
    POINT target{d.restore_x,d.restore_y}; bool found=d.restore_position && free(target);
    POINT phase=occupied.empty()?POINT{17,2}:occupied.front();
    const LONG x0=desktop.work.left+((phase.x-desktop.work.left)%desktop.pitch.x+desktop.pitch.x)%desktop.pitch.x;
    const LONG y0=desktop.work.top+((phase.y-desktop.work.top)%desktop.pitch.y+desktop.pitch.y)%desktop.pitch.y;
    for(LONG x=x0;!found && x<desktop.work.right-desktop.pitch.x/2;x+=desktop.pitch.x)
        for(LONG y=y0;!found && y<desktop.work.bottom-desktop.pitch.y/2;y+=desktop.pitch.y) if(free({x,y})) { target={x,y}; found=true; }
    if(!found) target={x0,y0}; // Full desktop: accessible is preferable to off-screen.
    const auto hr=desktop.folder->SelectAndPositionItems(1,&item,&target,SVSI_POSITIONITEM|SVSI_NOSTATECHANGE);
    CoTaskMemFree(absolute); return SUCCEEDED(hr);
}
}
