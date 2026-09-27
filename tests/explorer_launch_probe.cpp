#include <windows.h>
#include <shlobj.h>
#include <exdisp.h>
#include <shldisp.h>
#include <servprov.h>
#include <wrl/client.h>
#include <iostream>
using Microsoft::WRL::ComPtr;
static void check(HRESULT hr) { if(FAILED(hr)) { std::cerr<<"HRESULT="<<std::hex<<static_cast<unsigned long>(hr)<<'\n'; throw hr; } }
int wmain(int argc,wchar_t** argv) {
    if(argc<2 || argc>3) return 2;
    CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    try {
        ComPtr<IShellWindows> windows; check(CoCreateInstance(CLSID_ShellWindows,nullptr,CLSCTX_LOCAL_SERVER,IID_PPV_ARGS(&windows)));
        VARIANT location{},root{}; long desktop{}; ComPtr<IDispatch> dispatch;
        check(windows->FindWindowSW(&location,&root,SWC_DESKTOP,&desktop,SWFO_NEEDDISPATCH,&dispatch));
        ComPtr<IServiceProvider> provider; check(dispatch.As(&provider)); ComPtr<IShellBrowser> browser;
        check(provider->QueryService(SID_STopLevelBrowser,IID_PPV_ARGS(&browser)));
        ComPtr<IShellView> view; check(browser->QueryActiveShellView(&view));
        ComPtr<IDispatch> background; check(view->GetItemObject(SVGIO_BACKGROUND,IID_PPV_ARGS(&background)));
        ComPtr<IShellFolderViewDual> folder; check(background.As(&folder));
        ComPtr<IDispatch> application; check(folder->get_Application(&application));
        ComPtr<IShellDispatch2> shell; check(application.As(&shell));
        VARIANT args{},directory{},verb{},show{};
        args.vt=VT_BSTR; args.bstrVal=SysAllocString(argc==3?argv[2]:L"");
        show.vt=VT_I4; show.lVal=argc==3?SW_HIDE:SW_SHOWNORMAL;
        BSTR path=SysAllocString(argv[1]);
        const auto hr=shell->ShellExecute(path,args,directory,verb,show);
        SysFreeString(path); VariantClear(&args); check(hr);
        std::cout<<"Launched through the existing Explorer desktop.\n";
    } catch(...) { CoUninitialize(); return 1; }
    CoUninitialize(); return 0;
}
