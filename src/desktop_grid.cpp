#include "desktop_grid.hpp"
#include <shlobj.h>
#include <exdisp.h>
#include <servprov.h>
#include <wrl/client.h>

namespace edge {
bool desktop_spacing(POINT& spacing) {
    // Read-only Shell interfaces: no injection, desktop selection or layout edits.
    using Microsoft::WRL::ComPtr;
    ComPtr<IShellWindows> windows;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows,nullptr,CLSCTX_LOCAL_SERVER,IID_PPV_ARGS(&windows)))) return false;
    VARIANT location{}, root{}; VariantInit(&location); VariantInit(&root);
    long hwnd{}; ComPtr<IDispatch> dispatch;
    if (FAILED(windows->FindWindowSW(&location,&root,SWC_DESKTOP,&hwnd,SWFO_NEEDDISPATCH,&dispatch)) || !dispatch) return false;
    ComPtr<IServiceProvider> provider; ComPtr<IShellBrowser> browser; ComPtr<IShellView> view; ComPtr<IFolderView> folder;
    if (FAILED(dispatch.As(&provider)) || FAILED(provider->QueryService(SID_STopLevelBrowser,IID_PPV_ARGS(&browser)))
        || FAILED(browser->QueryActiveShellView(&view)) || FAILED(view.As(&folder))) return false;
    POINT value{};
    if (FAILED(folder->GetSpacing(&value)) || value.x<32 || value.y<32 || value.x>1024 || value.y>1024) return false;
    spacing=value; return true;
}
}
