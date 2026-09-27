#include "desktop_icons.hpp"
#include "archive.hpp"
#include "shell_files.hpp"
#include <exdisp.h>
#include <servprov.h>
#include <map>
#include <iostream>
using namespace edge;
static void check(HRESULT hr) { if(FAILED(hr)) throw std::runtime_error("Shell probe failed"); }
static void pump(unsigned ms) { auto end=GetTickCount64()+ms; do { MSG m{}; while(PeekMessageW(&m,nullptr,0,0,PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); } MsgWaitForMultipleObjects(0,nullptr,FALSE,20,QS_ALLINPUT); }while(GetTickCount64()<end); }
static std::map<std::wstring,POINT> snapshot(IFolderView2* view) {
    std::map<std::wstring,POINT> result; int count{}; view->ItemCount(SVGIO_ALLVIEW,&count);
    for(int i=0;i<count;++i) {
        ComPtr<IShellItem> item; PWSTR name{}; PITEMID_CHILD pidl{}; POINT p{};
        if(SUCCEEDED(view->GetItem(i,IID_PPV_ARGS(&item))) && SUCCEEDED(item->GetDisplayName(SIGDN_DESKTOPABSOLUTEPARSING,&name))) {
            if(SUCCEEDED(view->Item(i,&pidl)) && SUCCEEDED(view->GetItemPosition(pidl,&p))) result[name]=p;
            CoTaskMemFree(name); CoTaskMemFree(pidl);
        }
    }
    return result;
}
int main() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2); OleInitialize(nullptr);
    DrawerModel a{101,L"parking_probe_"+std::to_wstring(GetCurrentProcessId()),Edge::Right,0,3,3,{}},b{102,L"parking_probe_neighbor_"+std::to_wstring(GetCurrentProcessId()),Edge::Right,3,3,3,{}};
    int result=0;
    try {
        ComPtr<IShellWindows> windows; check(CoCreateInstance(CLSID_ShellWindows,nullptr,CLSCTX_LOCAL_SERVER,IID_PPV_ARGS(&windows)));
        VARIANT loc{},root{}; long handle{}; ComPtr<IDispatch> dispatch; check(windows->FindWindowSW(&loc,&root,SWC_DESKTOP,&handle,SWFO_NEEDDISPATCH,&dispatch));
        ComPtr<IServiceProvider> provider; ComPtr<IShellBrowser> browser; ComPtr<IShellView> shell; ComPtr<IFolderView2> view;
        check(dispatch.As(&provider)); check(provider->QueryService(SID_STopLevelBrowser,IID_PPV_ARGS(&browser))); check(browser->QueryActiveShellView(&shell)); check(shell.As(&view));
        DWORD flags{}; POINT spacing{}; view->GetCurrentFolderFlags(&flags); view->GetSpacing(&spacing);
        const auto before=snapshot(view.Get());
        std::wstring error;
        if(!bind_new_folder(a,desktop_directory(),error) || !bind_new_folder(b,desktop_directory(),error)) throw std::runtime_error("fixture creation failed");
        pump(1000);
        auto position=[&](const DrawerModel& d) { PIDLIST_ABSOLUTE pidl{}; check(SHParseDisplayName(d.folder.c_str(),nullptr,&pidl,0,nullptr)); POINT p{}; auto hr=view->GetItemPosition(ILFindLastID(pidl),&p); CoTaskMemFree(pidl); check(hr); return p; };
        std::cout<<"snap="<<!!(flags&FWF_SNAPTOGRID)<<" auto="<<!!(flags&FWF_AUTOARRANGE)<<" spacing="<<spacing.x<<','<<spacing.y<<'\n';
        for(int stage=0;stage<4;++stage) {
            if(stage==1) { shell->Refresh(); pump(1400); }
            if(stage==2) { park_folder_icon(b); restore_folder_icon(b); pump(1400); }
            if(stage==3) { restore_folder_icon(a); pump(300); }
            const auto status=park_folder_icon(a); pump(1700); const auto p=position(a);
            std::cout<<"stage="<<stage<<" status="<<static_cast<int>(status)<<" after_wait="<<p.x<<','<<p.y<<'\n';
            if(!(flags&FWF_AUTOARRANGE) && (status!=ParkResult::Parked || p.x<GetSystemMetrics(SM_CXVIRTUALSCREEN))) throw std::runtime_error("parking did not persist");
        }
        if(!restore_folder_icon(a) || !restore_folder_icon(b)) throw std::runtime_error("restore failed");
        pump(400);
        DWORD afterflags{}; POINT afterspacing{}; view->GetCurrentFolderFlags(&afterflags); view->GetSpacing(&afterspacing);
        const auto after=snapshot(view.Get()); int moved=0;
        for(const auto& [path,p]:before) if(auto it=after.find(path);it!=after.end() && (p.x!=it->second.x || p.y!=it->second.y)) ++moved;
        std::cout<<"original_icons_moved="<<moved<<" flags_unchanged="<<(flags==afterflags)<<" spacing_unchanged="<<(spacing.x==afterspacing.x && spacing.y==afterspacing.y)<<" restored="<<position(a).x<<','<<position(a).y<<'\n';
        if(moved || flags!=afterflags || spacing.x!=afterspacing.x || spacing.y!=afterspacing.y) throw std::runtime_error("desktop state changed during probe");
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; result=1; }
    for(auto* d:{&a,&b}) if(!d->folder.empty()) {
        restore_folder_icon(*d);
        // Only empty directories created with unique names by this probe can be removed.
        if(folder_available(*d) && RemoveDirectoryW(d->folder.c_str())) SHChangeNotify(SHCNE_RMDIR,SHCNF_PATHW,d->folder.c_str(),nullptr);
    }
    OleUninitialize(); return result;
}
