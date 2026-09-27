#include <windows.h>
#include <shlobj.h>
#include <exdisp.h>
#include <servprov.h>
#include <wrl/client.h>
#include <map>
#include <iostream>
#include <stdexcept>
using Microsoft::WRL::ComPtr;
static void check(HRESULT value) { if(FAILED(value)) throw std::runtime_error("Shell grid inspection failed"); }
int main() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const HRESULT initialized=OleInitialize(nullptr);
    int result=0;
    try {
        check(initialized);
        ComPtr<IShellWindows> windows; check(CoCreateInstance(CLSID_ShellWindows,nullptr,CLSCTX_LOCAL_SERVER,IID_PPV_ARGS(&windows)));
        VARIANT location{},root{}; long desktop{}; ComPtr<IDispatch> dispatch;
        check(windows->FindWindowSW(&location,&root,SWC_DESKTOP,&desktop,SWFO_NEEDDISPATCH,&dispatch));
        ComPtr<IServiceProvider> provider; ComPtr<IShellBrowser> browser; ComPtr<IShellView> view; ComPtr<IFolderView> folder;
        check(dispatch.As(&provider)); check(provider->QueryService(SID_STopLevelBrowser,IID_PPV_ARGS(&browser)));
        check(browser->QueryActiveShellView(&view)); check(view.As(&folder));
        ComPtr<IFolderFilterSite> filter;
        ComPtr<IShellFolderView> mutable_view;
        std::cout<<"IFolderFilterSite="<<std::hex<<static_cast<unsigned long>(view.As(&filter))
            <<" IShellFolderView="<<static_cast<unsigned long>(view.As(&mutable_view))<<std::dec<<'\n';
        POINT pitch{}; check(folder->GetSpacing(&pitch));
        HWND host{}; check(view->GetWindow(&host));
        const HWND list=FindWindowExW(host,nullptr,L"SysListView32",nullptr);
        std::cout<<"list_style="<<std::hex<<GetWindowLongPtrW(list,GWL_STYLE)<<std::dec<<'\n';
        POINT origin{}; ClientToScreen(list?list:host,&origin);
        MONITORINFO monitor{sizeof(monitor)}; GetMonitorInfoW(MonitorFromPoint({0,0},MONITOR_DEFAULTTOPRIMARY),&monitor);
        const RECT work=monitor.rcWork;
        std::cout<<"work="<<work.left<<','<<work.top<<','<<work.right<<','<<work.bottom
            <<" spacing="<<pitch.x<<'x'<<pitch.y<<" view_origin="<<origin.x<<','<<origin.y
            <<" right_remainder="<<(work.right-work.left)%pitch.x<<'\n';
        int count{}; check(folder->ItemCount(SVGIO_ALLVIEW,&count));
        std::map<int,int> xphases,yphases; int checked=0;
        for(int i=0;i<count && i<4096;++i) {
            PITEMID_CHILD item{};
            if(FAILED(folder->Item(i,&item))) continue;
            POINT point{}; const HRESULT hr=folder->GetItemPosition(item,&point); CoTaskMemFree(item);
            if(FAILED(hr)) continue;
            point.x+=origin.x; point.y+=origin.y;
            if(!PtInRect(&work,point)) continue;
            ++xphases[(point.x-work.left)%pitch.x]; ++yphases[(point.y-work.top)%pitch.y];
            if(checked++<8) std::cout<<"sample="<<point.x<<','<<point.y<<'\n';
        }
        std::cout<<"items="<<count<<" inspected="<<checked<<" x_phases=";
        for(auto [phase,n]:xphases) std::cout<<phase<<':'<<n<<' ';
        std::cout<<" y_phases="; for(auto [phase,n]:yphases) std::cout<<phase<<':'<<n<<' ';
        std::cout<<"\nRead only: no selection, positioning or file changes.\n";
    } catch(std::exception const& error) { std::cerr<<error.what()<<'\n'; result=1; }
    if(SUCCEEDED(initialized)) OleUninitialize();
    return result;
}
