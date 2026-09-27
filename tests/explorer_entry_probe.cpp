#include "explorer_entry.hpp"
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>
#include <fstream>
#include <iostream>
using Microsoft::WRL::ComPtr;
static void check(HRESULT hr) {if(FAILED(hr)) throw hr;}
static std::wstring path_of(IShellItem* item) {
    PWSTR text{}; check(item->GetDisplayName(SIGDN_FILESYSPATH,&text)); std::wstring path(text); CoTaskMemFree(text); return path;
}
// The common dialog browses the real namespace folder, enumerates it through
// its normal callback, then cancels. No file is opened or modified.
class DialogEvents : public IFileDialogEvents {
    LONG refs{1};
public:
    bool visited{}; std::wstring path;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out) override {
        if(!out) return E_POINTER; *out=nullptr;
        if(iid!=IID_IUnknown && iid!=IID_IFileDialogEvents) return E_NOINTERFACE;
        *out=static_cast<IFileDialogEvents*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {return InterlockedIncrement(&refs);}
    ULONG STDMETHODCALLTYPE Release() override {auto n=InterlockedDecrement(&refs); if(!n) delete this; return n;}
    HRESULT STDMETHODCALLTYPE OnFileOk(IFileDialog*) override {return S_FALSE;}
    HRESULT STDMETHODCALLTYPE OnFolderChanging(IFileDialog*,IShellItem*) override {return S_OK;}
    HRESULT STDMETHODCALLTYPE OnFolderChange(IFileDialog* dialog) override {
        try {ComPtr<IShellItem> folder; check(dialog->GetFolder(&folder)); path=path_of(folder.Get()); visited=true;} catch(...) {}
        dialog->Close(HRESULT_FROM_WIN32(ERROR_CANCELLED)); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnSelectionChange(IFileDialog*) override {return S_OK;}
    HRESULT STDMETHODCALLTYPE OnShareViolation(IFileDialog*,IShellItem*,FDE_SHAREVIOLATION_RESPONSE* reply) override {*reply=FDESVR_DEFAULT; return S_OK;}
    HRESULT STDMETHODCALLTYPE OnTypeChange(IFileDialog*) override {return S_OK;}
    HRESULT STDMETHODCALLTYPE OnOverwrite(IFileDialog*,IShellItem*,FDE_OVERWRITE_RESPONSE* reply) override {*reply=FDEOR_DEFAULT; return S_OK;}
};
int wmain(int argc,wchar_t** argv) {
    // Optional output path is an artifact, never the normal app's config.
    if(argc!=2) return 2;
    std::ofstream out(std::filesystem::path(argv[1]),std::ios::binary);
    if(!out) return 2;
    const auto initialized=OleInitialize(nullptr); if(FAILED(initialized)) return 2;
    int result=1;
    try {
        const auto entry=edge::explorer_entry_status();
        check(entry.enabled && !entry.error?S_OK:E_FAIL);
        const auto name=std::wstring(L"::{20D04FE0-3AEA-1069-A2D8-08002B30309D}\\::")+edge::explorer_entry_id;
        ComPtr<IShellItem> root; check(SHCreateItemFromParsingName(name.c_str(),nullptr,IID_PPV_ARGS(&root)));
        check(path_of(root.Get())==entry.target?S_OK:E_FAIL);
        out<<"PASS: This PC namespace resolves to the registered filesystem root\n";
        ComPtr<IShellItem> computer; check(SHCreateItemFromParsingName(L"::{20D04FE0-3AEA-1069-A2D8-08002B30309D}",nullptr,IID_PPV_ARGS(&computer)));
        ComPtr<IEnumShellItems> places; check(computer->BindToHandler(nullptr,BHID_EnumItems,IID_PPV_ARGS(&places)));
        ULONG count{}; bool listed=false;
        for(ComPtr<IShellItem> place;places->Next(1,&place,&count)==S_OK;place.Reset()) {
            PWSTR parsing{};
            if(SUCCEEDED(place->GetDisplayName(SIGDN_DESKTOPABSOLUTEPARSING,&parsing))) {
                listed|=std::wstring(parsing).find(edge::explorer_entry_id)!=std::wstring::npos; CoTaskMemFree(parsing);
            }
            if(!listed) {try {listed=path_of(place.Get())==entry.target;}catch(...) {}}
            if(listed) break;
        }
        check(listed?S_OK:E_FAIL); out<<"PASS: This PC enumerates the entry alongside its existing items\n";
        ComPtr<IEnumShellItems> children; check(root->BindToHandler(nullptr,BHID_EnumItems,IID_PPV_ARGS(&children)));
        ULONG fetched{}; unsigned folders{};
        for(ComPtr<IShellItem> child;children->Next(1,&child,&fetched)==S_OK;child.Reset()) {
            SFGAOF attrs{}; check(child->GetAttributes(SFGAO_FOLDER,&attrs));
            if(!(attrs&SFGAO_FOLDER)) continue;
            const auto path=path_of(child.Get());
            check(std::filesystem::path(path).parent_path()==std::filesystem::path(entry.target)?S_OK:E_FAIL);
            ComPtr<IEnumShellItems> contents; check(child->BindToHandler(nullptr,BHID_EnumItems,IID_PPV_ARGS(&contents))); ++folders;
        }
        out<<"PASS: "<<folders<<" child folders enumerate and open with real filesystem paths\n"; out.flush();
        ComPtr<IFileOpenDialog> dialog; check(CoCreateInstance(CLSID_FileOpenDialog,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&dialog)));
        FILEOPENDIALOGOPTIONS options{}; check(dialog->GetOptions(&options));
        check(dialog->SetOptions(options|FOS_FORCEFILESYSTEM|FOS_FILEMUSTEXIST|FOS_NOCHANGEDIR|FOS_DONTADDTORECENT));
        check(dialog->SetTitle(L"轻屉入口检查（自动关闭）"));
        check(dialog->SetFolder(root.Get()));
        ComPtr<DialogEvents> events; events.Attach(new DialogEvents); DWORD cookie{}; check(dialog->Advise(events.Get(),&cookie));
        const auto shown=dialog->Show(nullptr); dialog->Unadvise(cookie);
        check(shown==HRESULT_FROM_WIN32(ERROR_CANCELLED) && events->visited && events->path==entry.target?S_OK:E_FAIL);
        out<<"PASS: standard filesystem-only Open dialog browses the namespace and returns the real root; canceled without opening a file\n";
        result=0;
    } catch(HRESULT error) {out<<"FAIL HRESULT=0x"<<std::hex<<static_cast<unsigned long>(error)<<'\n';}
      catch(...) {out<<"FAIL: unexpected probe error\n";}
    OleUninitialize(); return result;
}
