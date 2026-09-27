#include "shell_files.hpp"
#include <shellapi.h>
#include <shlwapi.h>
#include <algorithm>
#include <set>

namespace edge {
ComPtr<IShellItemArray> shell_items(const std::vector<std::wstring>& paths,HRESULT* status) {
    ComPtr<IShellItemArray> result;
    HRESULT hr=E_INVALIDARG;
    if(status) *status=hr;
    if(paths.empty() || paths.size()>1000) return result;
    std::vector<PIDLIST_ABSOLUTE> pidls;
    for(const auto& p:paths) { PIDLIST_ABSOLUTE pidl{}; hr=SHParseDisplayName(p.c_str(),nullptr,&pidl,0,nullptr); if(FAILED(hr)) break; pidls.push_back(pidl); }
    if(pidls.size()==paths.size()) hr=SHCreateShellItemArrayFromIDLists(static_cast<UINT>(pidls.size()),const_cast<PCIDLIST_ABSOLUTE*>(pidls.data()),&result);
    for(auto p:pidls) CoTaskMemFree(p);
    if(status) *status=hr;
    return result;
}
ComPtr<IDataObject> file_data(const std::vector<std::wstring>& paths) {
    auto items=shell_items(paths); ComPtr<IDataObject> data;
    if(items) items->BindToHandler(nullptr,BHID_DataObject,IID_PPV_ARGS(&data));
    return data;
}
std::vector<std::wstring> data_paths(IDataObject* data) {
    std::vector<std::wstring> result;
    FORMATETC format{CF_HDROP,nullptr,DVASPECT_CONTENT,-1,TYMED_HGLOBAL}; STGMEDIUM medium{};
    if(!data || FAILED(data->GetData(&format,&medium))) return result;
    if(medium.tymed==TYMED_HGLOBAL && medium.hGlobal) {
        auto drop=static_cast<HDROP>(medium.hGlobal);
        const UINT count=DragQueryFileW(drop,0xFFFFFFFF,nullptr,0);
        if(count>1000) { ReleaseStgMedium(&medium); return {}; }
        for(UINT i=0;i<count;++i) {
            const UINT length=DragQueryFileW(drop,i,nullptr,0);
            if(!length || length>32767) continue;
            std::wstring path(length+1,L'\0'); DragQueryFileW(drop,i,path.data(),length+1); path.resize(length);
            if(std::filesystem::path(path).is_absolute()) result.push_back(std::move(path));
        }
    }
    ReleaseStgMedium(&medium); return result;
}
bool clipboard_files(HWND, const std::vector<std::wstring>& paths) {
    auto data=file_data(paths);
    if(!data || FAILED(OleSetClipboard(data.Get()))) return false;
    // Materialize the clipboard so a subsequent quit does not lose the selection.
    return SUCCEEDED(OleFlushClipboard());
}
DWORD preferred_drop_effect(IDataObject* data) {
    FORMATETC format{static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT)),nullptr,DVASPECT_CONTENT,-1,TYMED_HGLOBAL};
    STGMEDIUM medium{}; DWORD effect=DROPEFFECT_COPY;
    if(data && SUCCEEDED(data->GetData(&format,&medium))) {
        if(medium.tymed==TYMED_HGLOBAL && GlobalSize(medium.hGlobal)>=sizeof(DWORD)) {
            if(auto* value=static_cast<DWORD*>(GlobalLock(medium.hGlobal))) { effect=*value; GlobalUnlock(medium.hGlobal); }
        }
        ReleaseStgMedium(&medium);
    }
    return effect;
}
void completed_file_move(IDataObject* data) {
    // Optimized move: we already moved files. Never ask the source to delete them.
    FORMATETC format{static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_PERFORMEDDROPEFFECT)),nullptr,DVASPECT_CONTENT,-1,TYMED_HGLOBAL};
    STGMEDIUM medium{}; medium.tymed=TYMED_HGLOBAL; medium.hGlobal=GlobalAlloc(GHND,sizeof(DWORD));
    if(!medium.hGlobal) return;
    if(auto* effect=static_cast<DWORD*>(GlobalLock(medium.hGlobal))) { *effect=DROPEFFECT_NONE; GlobalUnlock(medium.hGlobal); }
    if(!data || FAILED(data->SetData(&format,&medium,TRUE))) ReleaseStgMedium(&medium);
}
void reveal_file(const std::wstring& path) {
    PIDLIST_ABSOLUTE pidl{};
    if(SUCCEEDED(SHParseDisplayName(path.c_str(),nullptr,&pidl,0,nullptr))) { SHOpenFolderAndSelectItems(pidl,0,nullptr,0); CoTaskMemFree(pidl); }
}
bool desktop_at(POINT point) {
    HWND window=WindowFromPoint(point);
    if(!window) return false;
    HWND root=GetAncestor(window,GA_ROOT); wchar_t name[128]{};
    GetClassNameW(root,name,128);
    return wcscmp(name,L"Progman")==0 || (wcscmp(name,L"WorkerW")==0 &&
        (FindWindowExW(root,nullptr,L"SHELLDLL_DefView",nullptr) || FindWindowExW(window,nullptr,L"SHELLDLL_DefView",nullptr)));
}
std::filesystem::path desktop_directory() {
    PWSTR path{}; std::filesystem::path result;
    if(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop,0,nullptr,&path))) { result=path; CoTaskMemFree(path); }
    return result;
}
std::filesystem::path create_desktop_item(const std::filesystem::path& directory, bool folder, DWORD& error) {
    error=ERROR_PATH_NOT_FOUND;
    std::error_code ec;
    if(!directory.is_absolute() || !std::filesystem::is_directory(directory,ec)) return {};
    for(unsigned i=1;i<=10000;++i) {
        std::wstring name=folder?L"新建文件夹":L"新建文本文档";
        if(i>1) name+=L" ("+std::to_wstring(i)+L")";
        if(!folder) name+=L".txt";
        const auto path=directory/name;
        bool okay=false;
        if(folder) okay=CreateDirectoryW(path.c_str(),nullptr)!=FALSE;
        else {
            HANDLE file=CreateFileW(path.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
            if(file!=INVALID_HANDLE_VALUE) { CloseHandle(file); okay=true; }
        }
        if(okay) { error=0; SHChangeNotify(folder?SHCNE_MKDIR:SHCNE_CREATE,SHCNF_PATHW,path.c_str(),nullptr); return path; }
        error=GetLastError(); if(error!=ERROR_ALREADY_EXISTS && error!=ERROR_FILE_EXISTS) return {};
    }
    error=ERROR_FILE_EXISTS; return {};
}
HRESULT rename_file(HWND owner, const std::wstring& path, const std::wstring& name) {
    if(name.empty() || name==L"." || name==L".." || name.find_first_of(L"\\/:*?\"<>|")!=std::wstring::npos || name.back()==L'.' || name.back()==L' ')
        return E_INVALIDARG;
    ComPtr<IFileOperation> operation; ComPtr<IShellItem> item;
    HRESULT hr=CoCreateInstance(CLSID_FileOperation,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&operation));
    if(SUCCEEDED(hr)) hr=SHCreateItemFromParsingName(path.c_str(),nullptr,IID_PPV_ARGS(&item));
    if(FAILED(hr)) return hr;
    operation->SetOwnerWindow(owner);
    operation->SetOperationFlags(FOF_ALLOWUNDO | FOF_NOCONFIRMMKDIR | FOFX_ADDUNDORECORD);
    hr=operation->RenameItem(item.Get(),name.c_str(),nullptr);
    if(SUCCEEDED(hr)) hr=operation->PerformOperations();
    BOOL aborted=FALSE; operation->GetAnyOperationsAborted(&aborted);
    return aborted?HRESULT_FROM_WIN32(ERROR_CANCELLED):hr;
}
bool ShellMenu::fill(HWND owner, HMENU popup, const std::vector<std::wstring>& paths,DWORD keys,ShellMenuLoad* output) {
    ShellMenuLoad local; auto& load=output?*output:local;
    load.status=S_OK; load.resolve_ms=load.bind_ms=load.query_ms=0;
    const auto stage=[&](MenuLoadStage value) { if(load.progress) load.progress(load.context,value); };
    if(keys==MAXDWORD) keys=GetKeyState(VK_SHIFT)<0?MK_SHIFT:0;
    menu.Reset(); menu2.Reset(); menu3.Reset();
    stage(MenuLoadStage::Resolving); auto began=GetTickCount64();
    auto items=shell_items(paths,&load.status); load.resolve_ms=static_cast<DWORD>(GetTickCount64()-began);
    if(!items) return false;
    stage(MenuLoadStage::Binding); began=GetTickCount64();
    load.status=items->BindToHandler(nullptr,BHID_SFUIObject,IID_PPV_ARGS(&menu)); load.bind_ms=static_cast<DWORD>(GetTickCount64()-began);
    if(FAILED(load.status)) return false;
    stage(MenuLoadStage::Querying); began=GetTickCount64();
    // Let supported Shell handlers evaluate expensive verb state asynchronously.
    // Keep the full menu and Shift's extended verbs; do not use DEFAULTONLY.
    load.status=menu->QueryContextMenu(popup,0,1,0x6FFF,CMF_NORMAL | CMF_CANRENAME | (load.asynchronous?CMF_ASYNCVERBSTATE:0) | ((keys&MK_SHIFT)?CMF_EXTENDEDVERBS:0));
    load.query_ms=static_cast<DWORD>(GetTickCount64()-began);
    if(FAILED(load.status)) { menu.Reset(); return false; }
    menu.As(&menu2); menu.As(&menu3); (void)owner; return true;
}
bool ShellMenu::message(UINT msg, WPARAM wp, LPARAM lp, LRESULT& result) {
    if(msg!=WM_INITMENUPOPUP && msg!=WM_DRAWITEM && msg!=WM_MEASUREITEM && msg!=WM_MENUCHAR) return false;
    if((msg==WM_DRAWITEM || msg==WM_MEASUREITEM) && wp!=0) return false;
    if(menu3 && SUCCEEDED(menu3->HandleMenuMsg2(msg,wp,lp,&result))) return true;
    if(menu2 && SUCCEEDED(menu2->HandleMenuMsg(msg,wp,lp))) { result=0; return true; }
    return false;
}
HRESULT ShellMenu::invoke(HWND owner, UINT command, POINT point,DWORD keys) {
    if(!menu || command<1 || command>0x6FFF) return E_INVALIDARG;
    CMINVOKECOMMANDINFOEX info{sizeof(info)};
    info.fMask=CMIC_MASK_UNICODE | CMIC_MASK_PTINVOKE;
    if(keys==MAXDWORD) keys=(GetKeyState(VK_SHIFT)<0?MK_SHIFT:0)|(GetKeyState(VK_CONTROL)<0?MK_CONTROL:0);
    if(keys&MK_CONTROL) info.fMask|=CMIC_MASK_CONTROL_DOWN;
    if(keys&MK_SHIFT) info.fMask|=CMIC_MASK_SHIFT_DOWN;
    info.hwnd=owner; info.lpVerb=MAKEINTRESOURCEA(command-1); info.lpVerbW=MAKEINTRESOURCEW(command-1);
    info.nShow=SW_SHOWNORMAL; info.ptInvoke=point;
    return menu->InvokeCommand(reinterpret_cast<CMINVOKECOMMANDINFO*>(&info));
}
std::wstring ShellMenu::verb(UINT command) const {
    wchar_t text[128]{};
    if(menu && command>0 && command<=0x6FFF) menu->GetCommandString(command-1,GCS_VERBW,nullptr,reinterpret_cast<char*>(text),128);
    return text;
}
FileWatch::~FileWatch() { clear(); }
void FileWatch::clear() { for(const auto& [path,id]:registrations) SHChangeNotifyDeregister(id); registrations.clear(); }
void FileWatch::update(HWND window, UINT message, const std::vector<std::wstring>& paths) {
    std::set<std::wstring> parents;
    for(const auto& path:paths) {
        // Ancestors catch an externally renamed/deleted containing folder, without
        // recursively subscribing to the whole disk or starting polling workers.
        auto parent=std::filesystem::path(path).parent_path();
        while(!parent.empty()) {
            parents.insert(parent.wstring()); const auto next=parent.parent_path(); if(next==parent) break; parent=next;
        }
    }
    for(auto it=registrations.begin();it!=registrations.end();) {
        if(!parents.contains(it->first)) { SHChangeNotifyDeregister(it->second); it=registrations.erase(it); } else ++it;
    }
    for(const auto& parent:parents) if(!registrations.contains(parent)) {
        PIDLIST_ABSOLUTE pidl{};
        if(FAILED(SHParseDisplayName(parent.c_str(),nullptr,&pidl,0,nullptr))) continue;
        SHChangeNotifyEntry entry{pidl,FALSE};
        ULONG id=SHChangeNotifyRegister(window,SHCNRF_ShellLevel | SHCNRF_InterruptLevel | SHCNRF_NewDelivery,
            SHCNE_CREATE | SHCNE_MKDIR | SHCNE_RENAMEITEM | SHCNE_RENAMEFOLDER | SHCNE_DELETE | SHCNE_RMDIR | SHCNE_UPDATEITEM | SHCNE_UPDATEDIR | SHCNE_ATTRIBUTES,
            message,1,&entry);
        CoTaskMemFree(pidl); if(id) registrations.emplace(parent,id);
    }
}
FileChange FileWatch::read(WPARAM wp, LPARAM lp) {
    PIDLIST_ABSOLUTE* pidls{}; FileChange change;
    HANDLE lock=SHChangeNotification_Lock(reinterpret_cast<HANDLE>(wp),static_cast<DWORD>(lp),&pidls,&change.event);
    if(!lock) return change;
    auto read_path=[](PCIDLIST_ABSOLUTE pidl) { wchar_t path[32768]{}; return pidl && SHGetPathFromIDListEx(pidl,path,32768,GPFIDL_DEFAULT)?std::wstring(path):std::wstring(); };
    if(pidls) {
        change.from=read_path(pidls[0]);
        if(change.event & (SHCNE_RENAMEITEM | SHCNE_RENAMEFOLDER)) change.to=read_path(pidls[1]);
    }
    SHChangeNotification_Unlock(lock); return change;
}
}
