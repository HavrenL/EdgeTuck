#include <windows.h>
#include <shlobj.h>
#include <exdisp.h>
#include <servprov.h>
#include <wrl/client.h>
#include <filesystem>
#include <iostream>
#include <stdexcept>
using Microsoft::WRL::ComPtr;
static void check(HRESULT hr) { if(FAILED(hr)) throw std::runtime_error("Shell positioning failed"); }
static void pump(unsigned ms) {
    const auto until=GetTickCount64()+ms;
    while(GetTickCount64()<until) {
        MSG m{}; while(PeekMessageW(&m,nullptr,0,0,PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
        MsgWaitForMultipleObjects(0,nullptr,FALSE,25,QS_ALLINPUT);
    }
}
int main() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    OleInitialize(nullptr); std::filesystem::path fixture;
    int result=0;
    try {
        PWSTR desktop{}; check(SHGetKnownFolderPath(FOLDERID_Desktop,0,nullptr,&desktop));
        fixture=std::filesystem::path(desktop)/(L"ET_position_test_"+std::to_wstring(GetCurrentProcessId())+L"_"+std::to_wstring(GetTickCount64()));
        CoTaskMemFree(desktop);
        if(!CreateDirectoryW(fixture.c_str(),nullptr)) throw std::runtime_error("Cannot create owned fixture");
        SHChangeNotify(SHCNE_MKDIR,SHCNF_PATHW|SHCNF_FLUSH,fixture.c_str(),nullptr); pump(500);
        ComPtr<IShellWindows> windows; check(CoCreateInstance(CLSID_ShellWindows,nullptr,CLSCTX_LOCAL_SERVER,IID_PPV_ARGS(&windows)));
        VARIANT location{},root{}; long desktop_hwnd{}; ComPtr<IDispatch> dispatch;
        check(windows->FindWindowSW(&location,&root,SWC_DESKTOP,&desktop_hwnd,SWFO_NEEDDISPATCH,&dispatch));
        ComPtr<IServiceProvider> provider; ComPtr<IShellBrowser> browser; ComPtr<IShellView> view; ComPtr<IFolderView2> folder;
        check(dispatch.As(&provider)); check(provider->QueryService(SID_STopLevelBrowser,IID_PPV_ARGS(&browser)));
        check(browser->QueryActiveShellView(&view)); check(view.As(&folder));
        DWORD flags{}; check(folder->GetCurrentFolderFlags(&flags));
        POINT pitch{}; check(folder->GetSpacing(&pitch));
        PIDLIST_ABSOLUTE pidl{}; check(SHParseDisplayName(fixture.c_str(),nullptr,&pidl,0,nullptr));
        PCUITEMID_CHILD child=ILFindLastID(pidl); POINT original{};
        check(folder->GetItemPosition(child,&original));
        std::cout<<"flags="<<std::hex<<flags<<std::dec<<" snap="<<!!(flags&FWF_SNAPTOGRID)<<" auto="<<!!(flags&FWF_AUTOARRANGE)<<" pitch="<<pitch.x<<','<<pitch.y<<" original="<<original.x<<','<<original.y<<'\n';
        // Change ONLY the test folder's position. Never change view flags or user icons.
        for(POINT requested : {POINT{-2000,-2000},POINT{original.x+pitch.x*100,original.y},POINT{original.x,original.y+pitch.y*100}}) {
            HRESULT hr=folder->SelectAndPositionItems(1,&child,&requested,SVSI_POSITIONITEM|SVSI_NOSTATECHANGE);
            pump(150); POINT actual{}; folder->GetItemPosition(child,&actual);
            std::cout<<"requested="<<requested.x<<','<<requested.y<<" hr="<<std::hex<<hr<<std::dec<<" actual="<<actual.x<<','<<actual.y;
            pump(1500); folder->GetItemPosition(child,&actual);
            std::cout<<" after1500ms="<<actual.x<<','<<actual.y<<'\n';
        }
        folder->SelectAndPositionItems(1,&child,&original,SVSI_POSITIONITEM|SVSI_NOSTATECHANGE);
        DWORD after{}; folder->GetCurrentFolderFlags(&after);
        std::cout<<"flags_unchanged="<<(after==flags)<<" original_position_restored\n";
        CoTaskMemFree(pidl);
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; result=1; }
    // RemoveDirectory only removes our newly-created EMPTY fixture, never contents.
    if(!fixture.empty() && RemoveDirectoryW(fixture.c_str())) SHChangeNotify(SHCNE_RMDIR,SHCNF_PATHW,fixture.c_str(),nullptr);
    OleUninitialize(); return result;
}
