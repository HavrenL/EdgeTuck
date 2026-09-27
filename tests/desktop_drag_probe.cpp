// Manual native desktop regression: drag only an owned, empty fixture. This
// intentionally does not refresh Explorer, change its flags, or move user icons.
#include "desktop_icons.hpp"
#include "archive.hpp"
#include "shell_files.hpp"
#include <exdisp.h>
#include <servprov.h>
#include <algorithm>
#include <iostream>
#include <map>
#include <io.h>
#include <fcntl.h>
using namespace edge;
static HWND list{};
static ULONGLONG started{};
static void check(HRESULT hr) { if(FAILED(hr)) throw std::runtime_error("Shell drag probe failed"); }
static void pump(unsigned ms) {
    const auto end=GetTickCount64()+ms;
    do { MSG m{}; while(PeekMessageW(&m,nullptr,0,0,PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
        MsgWaitForMultipleObjects(0,nullptr,FALSE,5,QS_ALLINPUT);
    } while(GetTickCount64()<end);
}
static void CALLBACK event_proc(HWINEVENTHOOK,DWORD event,HWND hwnd,LONG object,LONG child,DWORD,DWORD time) {
    if(hwnd!=list && event>=EVENT_OBJECT_CREATE) return;
    wchar_t name[128]{}; GetClassNameW(hwnd,name,128);
    std::wcout<<L"event t="<<GetTickCount64()-started<<L" sent="<<static_cast<DWORD>(time-static_cast<DWORD>(started))
        <<L" code="<<std::hex<<event<<std::dec<<L" class="<<name<<L" obj="<<object<<L" child="<<child<<L'\n';
}
static std::map<std::wstring,POINT> snapshot(IFolderView2* view,UINT kind=SVGIO_ALLVIEW) {
    std::map<std::wstring,POINT> result; ComPtr<IShellItemArray> items;
    const auto hr=view->Items(kind,IID_PPV_ARGS(&items));
    if(kind==SVGIO_SELECTION && FAILED(hr)) return result;
    check(hr); DWORD count{}; items->GetCount(&count);
    for(DWORD i=0;i<count;++i) {
        ComPtr<IShellItem> item; PWSTR name{}; PIDLIST_ABSOLUTE pidl{}; POINT p{};
        if(SUCCEEDED(items->GetItemAt(i,&item)) && SUCCEEDED(item->GetDisplayName(SIGDN_DESKTOPABSOLUTEPARSING,&name))) {
            if(SUCCEEDED(SHGetIDListFromObject(item.Get(),&pidl)) && SUCCEEDED(view->GetItemPosition(ILFindLastID(pidl),&p))) result[name]=p;
            CoTaskMemFree(name); CoTaskMemFree(pidl);
        }
    }
    return result;
}
static void position(IFolderView2* view,const std::wstring& path,POINT p) {
    PIDLIST_ABSOLUTE pidl{}; check(SHParseDisplayName(path.c_str(),nullptr,&pidl,0,nullptr));
    PCUITEMID_CHILD item=ILFindLastID(pidl);
    auto hr=view->SelectAndPositionItems(1,&item,&p,SVSI_POSITIONITEM|SVSI_NOSTATECHANGE);
    CoTaskMemFree(pidl); check(hr);
}
static void button(DWORD flag) { INPUT input{}; input.type=INPUT_MOUSE; input.mi.dwFlags=flag; if(SendInput(1,&input,sizeof(input))!=1) throw std::runtime_error("Mouse input failed"); }
int main(int argc,char** argv) {
    _setmode(_fileno(stdout),_O_U8TEXT); _setmode(_fileno(stderr),_O_U8TEXT);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2); OleInitialize(nullptr); started=GetTickCount64();
    DrawerModel fixture{901,L"drag_probe_"+std::to_wstring(GetCurrentProcessId()),Edge::Right,0,3,3,{}};
    ComPtr<IFolderView2> view; std::map<std::wstring,POINT> before,selected; DWORD flags{}; POINT pitch{},cursor{}; GetCursorPos(&cursor);
    HWINEVENTHOOK hook{}; bool down=false; int result=0;
    try {
        ComPtr<IShellWindows> windows; check(CoCreateInstance(CLSID_ShellWindows,nullptr,CLSCTX_LOCAL_SERVER,IID_PPV_ARGS(&windows)));
        VARIANT loc{},root{}; long handle{}; ComPtr<IDispatch> dispatch; check(windows->FindWindowSW(&loc,&root,SWC_DESKTOP,&handle,SWFO_NEEDDISPATCH,&dispatch));
        ComPtr<IServiceProvider> provider; ComPtr<IShellBrowser> browser; ComPtr<IShellView> shell;
        check(dispatch.As(&provider)); check(provider->QueryService(SID_STopLevelBrowser,IID_PPV_ARGS(&browser))); check(browser->QueryActiveShellView(&shell)); check(shell.As(&view));
        HWND host{}; check(shell->GetWindow(&host)); list=FindWindowExW(host,nullptr,L"SysListView32",nullptr);
        check(view->GetCurrentFolderFlags(&flags)); check(view->GetSpacing(&pitch));
        if(!list || (flags&FWF_AUTOARRANGE)) throw std::runtime_error("Requires manual desktop arrangement");
        before=snapshot(view.Get()); selected=snapshot(view.Get(),SVGIO_SELECTION);
        for(const auto& [path,p]:before) std::wcout<<L"before "<<p.x<<L','<<p.y<<L' '<<path<<L'\n';
        POINT origin{}; ClientToScreen(list,&origin); RECT work{}; SystemParametersInfoW(SPI_GETWORKAREA,0,&work,0);
        POINT start{},finish{}; bool found=false;
        auto free=[&](POINT p) { return std::none_of(before.begin(),before.end(),[&](const auto& item){return abs(item.second.x-p.x)<pitch.x/2 && abs(item.second.y-p.y)<pitch.y/2;}); };
        const POINT phase=before.begin()->second;
        for(LONG x=(phase.x%pitch.x+pitch.x)%pitch.x;!found && x<work.right-origin.x-pitch.x;x+=pitch.x)
            for(LONG y=(phase.y%pitch.y+pitch.y)%pitch.y;!found && y<work.bottom-origin.y-2*pitch.y;y+=pitch.y) {
                if(!free({x,y}) || !free({x,y+pitch.y})) continue;
                bool exposed=true;
                for(LONG dy=0;dy<=pitch.y;dy+=8) if(WindowFromPoint({origin.x+x+24,origin.y+y+24+dy})!=list) exposed=false;
                if(exposed) { start={x,y}; finish={x,y+pitch.y}; found=true; }
            }
        if(!found) throw std::runtime_error("No exposed pair of free desktop cells; no UI changed");
        std::wstring error; if(!bind_new_folder(fixture,desktop_directory(),error)) throw std::runtime_error("Fixture creation failed");
        pump(1200); position(view.Get(),fixture.folder,start); pump(700);
        DWORD pid{}; GetWindowThreadProcessId(list,&pid);
        hook=SetWinEventHook(EVENT_MIN,EVENT_MAX,nullptr,event_proc,pid,0,WINEVENT_OUTOFCONTEXT|WINEVENT_SKIPOWNPROCESS);
        if(!hook) throw std::runtime_error("Event probe hook failed");
        auto last=snapshot(view.Get());
        const int cycles=argc>1?std::clamp(atoi(argv[1]),1,8):1;
        const bool expect=argc>2 && std::string(argv[2])=="--expect-repair";
        for(int cycle=0;cycle<cycles;++cycle) {
            const POINT a=cycle%2?finish:start,b=cycle%2?start:finish;
            if(WindowFromPoint({a.x+origin.x+24,a.y+origin.y+24})!=list || WindowFromPoint({b.x+origin.x+24,b.y+origin.y+24})!=list) throw std::runtime_error("Desktop became covered; drag stopped");
            SetCursorPos(a.x+origin.x+24,a.y+origin.y+24); button(MOUSEEVENTF_LEFTDOWN); down=true; pump(80);
            for(int step=1;step<=12;++step) { SetCursorPos(a.x+origin.x+24+(b.x-a.x)*step/12,a.y+origin.y+24+(b.y-a.y)*step/12); pump(25); }
            button(MOUSEEVENTF_LEFTUP); down=false;
            std::wcout<<L"drop cycle="<<cycle<<L" t="<<GetTickCount64()-started<<L'\n';
            for(int tick=0;tick<100;++tick) {
                pump(20); const auto now=snapshot(view.Get());
                for(const auto& [path,p]:now) if(!last.contains(path)||last[path].x!=p.x||last[path].y!=p.y)
                    std::wcout<<L"position t="<<GetTickCount64()-started<<L' '<<p.x<<L','<<p.y<<L' '<<path<<L'\n';
                last=now;
            }
            const auto p=last.at(fixture.folder);
            if(p.x!=b.x||p.y!=b.y) throw std::runtime_error("Fixture did not land in requested grid cell");
            int visible=0; for(const auto& [path,old]:before) if(old.x>=work.right-origin.x+pitch.x && last.contains(path) && last[path].x<work.right-origin.x+pitch.x) ++visible;
            std::wcout<<L"cycle="<<cycle<<L" returned_visible="<<visible<<L'\n';
            if(expect && visible) throw std::runtime_error("Parked folders remained visible after native drag");
        }
    } catch(const std::exception& e) { std::wcerr<<e.what()<<L'\n'; result=1; }
    if(down) button(MOUSEEVENTF_LEFTUP);
    if(hook) UnhookWinEvent(hook);
    // Ask the running app to restore its own bindings after the observation;
    // no global Refresh and no direct positioning of pre-existing user items.
    if(HWND broker=FindWindowW(L"EdgeTuck.Broker",nullptr)) PostMessageW(broker,WM_APP+11,0,0);
    if(!fixture.folder.empty() && folder_available(fixture) && RemoveDirectoryW(fixture.folder.c_str())) SHChangeNotify(SHCNE_RMDIR,SHCNF_PATHW,fixture.folder.c_str(),nullptr);
    pump(800);
    if(view && !before.empty()) {
        const auto after=snapshot(view.Get()); int moved=0;
        for(const auto& [path,p]:before) if(!after.contains(path)||after.at(path).x!=p.x||after.at(path).y!=p.y) ++moved;
        DWORD afterflags{}; POINT afterpitch{}; view->GetCurrentFolderFlags(&afterflags); view->GetSpacing(&afterpitch);
        std::wcout<<L"original_icons_moved="<<moved<<L" flags_unchanged="<<(flags==afterflags)<<L" spacing_unchanged="<<(pitch.x==afterpitch.x&&pitch.y==afterpitch.y)<<L'\n';
        if(moved||flags!=afterflags||pitch.x!=afterpitch.x||pitch.y!=afterpitch.y) result=1;
        view->SelectItem(-1,SVSI_DESELECTOTHERS);
        for(const auto& [path,p]:selected) {
            PIDLIST_ABSOLUTE pidl{}; if(SUCCEEDED(SHParseDisplayName(path.c_str(),nullptr,&pidl,0,nullptr))) { PCUITEMID_CHILD item=ILFindLastID(pidl); view->SelectAndPositionItems(1,&item,nullptr,SVSI_SELECT); CoTaskMemFree(pidl); }
        }
    }
    SetCursorPos(cursor.x,cursor.y); view.Reset(); OleUninitialize(); return result;
}
