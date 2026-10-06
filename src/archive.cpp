#include "archive.hpp"
#include "references.hpp"
#include "shell_files.hpp"
#include <algorithm>
#include <shlwapi.h>
#include <shellapi.h>

namespace edge {
std::wstring folder_identity(const std::filesystem::path& path) {
    const DWORD attributes=GetFileAttributesW(path.c_str());
    if(attributes==INVALID_FILE_ATTRIBUTES || !(attributes&FILE_ATTRIBUTE_DIRECTORY) || (attributes&FILE_ATTRIBUTE_REPARSE_POINT)) return {};
    HANDLE file=CreateFileW(path.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    if(file==INVALID_HANDLE_VALUE) return {};
    BY_HANDLE_FILE_INFORMATION info{}; const bool okay=GetFileInformationByHandle(file,&info)!=FALSE; CloseHandle(file);
    return okay?std::to_wstring(info.dwVolumeSerialNumber)+L":"+std::to_wstring(info.nFileIndexHigh)+L":"+std::to_wstring(info.nFileIndexLow):L"";
}
bool folder_available(const DrawerModel& d) { return !d.folder.empty() && !d.folder_identity.empty() && folder_identity(d.folder)==d.folder_identity; }
std::wstring english_archive_name(int id) { return L"ETDrawer"+std::to_wstring(id); }
bool valid_english_archive_alias(const std::wstring& alias) {
    const auto alnum=[](wchar_t c) {return (c>=L'A' && c<=L'Z') || (c>=L'a' && c<=L'z') || (c>=L'0' && c<=L'9');};
    return !alias.empty() && alias.size()<=80 && ((alias.front()>=L'A' && alias.front()<=L'Z') || (alias.front()>=L'a' && alias.front()<=L'z')) &&
        std::all_of(alias.begin(),alias.end(),alnum);
}
bool ascii_path(const std::filesystem::path& path) {
    const auto text=path.wstring();
    return std::all_of(text.begin(),text.end(),[](wchar_t c){return c>=32 && c<=126;});
}
std::wstring archive_folder_name(const std::wstring& title) {
    auto name=title;
    for(auto& c:name) if(c<32 || std::wstring(L"\\/:*?\"<>|").find(c)!=std::wstring::npos) c=L'_';
    if(name.size()>100) name.resize(100);
    if(!name.empty() && name.back()>=0xD800 && name.back()<=0xDBFF) name.pop_back();
    while(!name.empty() && (name.back()==L'.' || name.back()==L' ')) name.pop_back();
    return L"ET_"+(name.empty()?L"抽屉":name);
}
bool rebind_folder(DrawerModel& d,const std::filesystem::path& path) {
    if(d.folder.empty() || d.folder_identity.empty() || folder_identity(path)!=d.folder_identity || d.folder==path.wstring()) return false;
    const auto old=d.folder; const auto previous=std::filesystem::path(old).filename().wstring(),next=path.filename().wstring();
    if(!d.english_folder && previous!=next && archive_folder_name(d.name)!=next) {
        auto title=next.starts_with(L"ET_")?next.substr(3):next;
        if(title.empty()) title=next;
        if(title.size()>64) title.resize(64);
        if(!title.empty() && title.back()>=0xD800 && title.back()<=0xDBFF) title.pop_back();
        d.name=std::move(title);
    }
    d.folder=path.wstring();
    for(auto& p:d.items) if(path_within(p,old)) p=d.folder+p.substr(old.size());
    for(auto& p:d.legacy_items) if(path_within(p,old)) p=d.folder+p.substr(old.size());
    for(auto& use:d.recent_uses) if(path_within(use.path,old)) use.path=d.folder+use.path.substr(old.size());
    return true;
}
bool bind_new_folder(DrawerModel& d,const std::filesystem::path& desktop,std::wstring& error,const std::wstring& english_alias) {
    error.clear();
    if(!d.folder.empty()) { if(folder_available(d)) return true; error=L"抽屉文件夹已移动、删除或不可用，请先在资源管理器中恢复。"; return false; }
    if(!english_alias.empty() && (!d.english_folder || !valid_english_archive_alias(english_alias))) {error=L"目录名必须以英文字母开头，只能包含英文字母和数字。"; return false;}
    if(!desktop.is_absolute()) { error=L"无法找到收纳目录。"; return false; }
    // A missing removable drive must not be replaced by an unrelated directory.
    std::error_code ec;
    if(!std::filesystem::is_directory(desktop.root_path(),ec)) { error=L"收纳目录所在的磁盘不可用。"; return false; }
    std::filesystem::create_directories(desktop,ec);
    if(ec || folder_identity(desktop).empty()) { error=L"无法使用收纳目录，请检查位置和写入权限。"; return false; }
    const auto name=d.english_folder?(english_alias.empty()?english_archive_name(d.id):L"ET"+english_alias):archive_folder_name(d.name);
    for(int i=1;i<=10000;++i) {
        auto title=d.name;
        if(i>1 && !d.english_folder) {
            const auto suffix=L" ("+std::to_wstring(i)+L")";
            if(title.size()+suffix.size()>64) title.resize(64-suffix.size());
            if(!title.empty() && title.back()>=0xD800 && title.back()<=0xDBFF) title.pop_back();
            title+=suffix;
        }
        auto path=desktop/(d.english_folder?(i==1?name:name+std::to_wstring(i)):(i==1?name:archive_folder_name(title)));
        if(CreateDirectoryW(path.c_str(),nullptr)) {
            const auto identity=folder_identity(path);
            if(identity.empty()) { RemoveDirectoryW(path.c_str()); error=L"无法记录抽屉文件夹的身份。"; return false; }
            d.name=std::move(title); d.folder=path.wstring(); d.folder_identity=identity; d.legacy_items=d.items;
            SHChangeNotify(SHCNE_MKDIR,SHCNF_PATHW,path.c_str(),nullptr); return true;
        }
        if(GetLastError()!=ERROR_ALREADY_EXISTS) break;
    }
    error=L"无法创建抽屉文件夹，请检查收纳目录的写入权限。"; return false;
}
bool reconnect_folder(DrawerModel& d,const std::filesystem::path& desktop) {
    if(d.folder.empty() || d.folder_identity.empty() || folder_available(d)) return false;
    std::error_code ec;
    for(std::filesystem::directory_iterator it(desktop,ec),end;!ec && it!=end;it.increment(ec)) {
        if(folder_identity(it->path())==d.folder_identity) {
            return rebind_folder(d,it->path());
        }
    }
    return false;
}
bool refresh_folder(DrawerModel& d,std::wstring& error) {
    error.clear(); if(d.folder.empty()) return false;
    if(!folder_available(d)) { error=L"抽屉文件夹不可用；未创建替代文件夹，也未移动任何文件。"; return false; }
    std::vector<std::wstring> available; std::error_code ec;
    for(std::filesystem::directory_iterator it(d.folder,ec),end;!ec && it!=end;it.increment(ec)) {
        const auto path=it->path().wstring(); const DWORD attrs=GetFileAttributesW(path.c_str());
        if(attrs==INVALID_FILE_ATTRIBUTES || (attrs&(FILE_ATTRIBUTE_HIDDEN|FILE_ATTRIBUTE_SYSTEM))) continue;
        available.push_back(path);
        if(available.size()>10000) { error=L"文件夹内容过多，请在资源管理器中查看。"; return false; }
    }
    if(ec) { error=L"无法读取抽屉文件夹，保留上次显示的内容。"; return false; }
    for(const auto& p:d.legacy_items) if(std::none_of(available.begin(),available.end(),[&](const auto& x){return same_path(x,p);})) available.push_back(p);
    std::stable_sort(available.begin(),available.end(),[](const auto& a,const auto& b){return StrCmpLogicalW(std::filesystem::path(a).filename().c_str(),std::filesystem::path(b).filename().c_str())<0;});
    std::vector<std::wstring> next;
    for(const auto& p:d.items) {
        auto it=std::find_if(available.begin(),available.end(),[&](const auto& x){return same_path(x,p);});
        if(it!=available.end()) { next.push_back(*it); available.erase(it); }
    }
    next.insert(next.end(),available.begin(),available.end());
    if(next.size()>1000) { next.resize(1000); error=L"抽屉显示前 1000 项；全部内容可在资源管理器中查看。"; }
    if(next==d.items) return false;
    d.items=std::move(next); return true;
}
static std::wstring item_path(IShellItem* item) {
    PWSTR value{}; std::wstring path;
    if(item && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH,&value))) { path=value; CoTaskMemFree(value); }
    return path;
}
class TransferSink final : public IFileOperationProgressSink {
    LONG refs{1}; TransferResult& result;
public:
    bool recycle_only{};
    explicit TransferSink(TransferResult& r):result(r){}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out) override {
        if(!out) return E_POINTER; *out=nullptr;
        if(iid!=IID_IUnknown && iid!=IID_IFileOperationProgressSink) return E_NOINTERFACE;
        *out=static_cast<IFileOperationProgressSink*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs); }
    ULONG STDMETHODCALLTYPE Release() override { const auto n=InterlockedDecrement(&refs); if(!n) delete this; return n; }
    HRESULT record(IShellItem* from,HRESULT hr,IShellItem* to) {
        if(SUCCEEDED(hr) && to) { auto a=item_path(from),b=item_path(to); if(!a.empty() && !b.empty()) result.completed.push_back({a,b}); }
        else if(FAILED(hr)) result.status=hr;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE StartOperations() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE FinishOperations(HRESULT hr) override { if(FAILED(hr)) result.status=hr; return S_OK; }
    HRESULT STDMETHODCALLTYPE PreRenameItem(DWORD,IShellItem*,LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PostRenameItem(DWORD,IShellItem*,LPCWSTR,HRESULT,IShellItem*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PreMoveItem(DWORD,IShellItem*,IShellItem*,LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PostMoveItem(DWORD,IShellItem* a,IShellItem*,LPCWSTR,HRESULT hr,IShellItem* b) override { return record(a,hr,b); }
    HRESULT STDMETHODCALLTYPE PreCopyItem(DWORD,IShellItem*,IShellItem*,LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PostCopyItem(DWORD,IShellItem* a,IShellItem*,LPCWSTR,HRESULT hr,IShellItem* b) override { return record(a,hr,b); }
    HRESULT STDMETHODCALLTYPE PreDeleteItem(DWORD flags,IShellItem*) override { return recycle_only && !(flags&TSF_DELETE_RECYCLE_IF_POSSIBLE)?E_ABORT:S_OK; }
    HRESULT STDMETHODCALLTYPE PostDeleteItem(DWORD,IShellItem*,HRESULT,IShellItem*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PreNewItem(DWORD,IShellItem*,LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PostNewItem(DWORD,IShellItem*,LPCWSTR,LPCWSTR,DWORD,HRESULT,IShellItem*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE UpdateProgress(UINT,UINT) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE ResetTimer() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PauseTimer() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE ResumeTimer() override { return S_OK; }
};
TransferResult transfer_files(HWND owner,const std::vector<std::wstring>& paths,const std::filesystem::path& directory,bool copy,const std::vector<std::wstring>& protected_folders) {
    TransferResult result;
    if(paths.empty() || paths.size()>1000 || !directory.is_absolute() || folder_identity(directory).empty()) { result.status=E_INVALIDARG; return result; }
    const auto destination=directory.lexically_normal().wstring();
    std::vector<std::wstring> unique;
    for(const auto& raw:paths) {
        const std::filesystem::path input(raw); const auto path=input.lexically_normal().wstring();
        if(raw.find(L'\0')!=std::wstring::npos || !input.is_absolute() || path_within(destination,path) || GetFileAttributesW(path.c_str())==INVALID_FILE_ATTRIBUTES
            || std::any_of(protected_folders.begin(),protected_folders.end(),[&](const auto& p){return path_within(p,path);})) { result.status=E_INVALIDARG; return result; }
        if(std::none_of(unique.begin(),unique.end(),[&](const auto& p){return same_path(p,path);})) unique.push_back(path);
    }
    for(const auto& a:unique) for(const auto& b:unique) if(!same_path(a,b) && path_within(a,b)) { result.status=E_INVALIDARG; return result; }
    ComPtr<IFileOperation> operation; ComPtr<IShellItem> target;
    result.status=CoCreateInstance(CLSID_FileOperation,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&operation));
    if(SUCCEEDED(result.status)) result.status=SHCreateItemFromParsingName(destination.c_str(),nullptr,IID_PPV_ARGS(&target));
    if(FAILED(result.status)) return result;
    operation->SetOwnerWindow(owner);
    result.status=operation->SetOperationFlags(FOF_ALLOWUNDO|FOF_RENAMEONCOLLISION|FOF_NOCONFIRMMKDIR|FOFX_ADDUNDORECORD);
    if(FAILED(result.status)) return result;
    ComPtr<TransferSink> sink; sink.Attach(new TransferSink(result));
    for(const auto& path:unique) {
        if(!copy && same_path(std::filesystem::path(path).parent_path().wstring(),destination)) { result.completed.push_back({path,path}); continue; }
        ComPtr<IShellItem> item; result.status=SHCreateItemFromParsingName(path.c_str(),nullptr,IID_PPV_ARGS(&item));
        if(FAILED(result.status)) return result;
        result.status=copy?operation->CopyItem(item.Get(),target.Get(),nullptr,sink.Get()):operation->MoveItem(item.Get(),target.Get(),nullptr,sink.Get());
        if(FAILED(result.status)) return result;
    }
    const auto hr=operation->PerformOperations(); if(FAILED(hr)) result.status=hr;
    BOOL aborted{}; operation->GetAnyOperationsAborted(&aborted); result.aborted=aborted!=FALSE;
    return result;
}
bool recycle_archive(HWND owner,const DrawerModel& drawer) {
    if(!folder_available(drawer)) return false;
    ComPtr<IFileOperation> operation; ComPtr<IShellItem> item;
    if(FAILED(CoCreateInstance(CLSID_FileOperation,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&operation))) ||
        FAILED(SHCreateItemFromParsingName(drawer.folder.c_str(),nullptr,IID_PPV_ARGS(&item)))) return false;
    operation->SetOwnerWindow(owner);
    if(FAILED(operation->SetOperationFlags(FOF_ALLOWUNDO|FOFX_RECYCLEONDELETE|FOFX_ADDUNDORECORD|
        FOF_NOCONFIRMATION|FOF_WANTNUKEWARNING|FOF_NOERRORUI|FOFX_EARLYFAILURE))) return false;
    TransferResult result; ComPtr<TransferSink> sink; sink.Attach(new TransferSink(result)); sink->recycle_only=true;
    if(!folder_available(drawer) || FAILED(operation->DeleteItem(item.Get(),sink.Get()))) return false;
    const auto hr=operation->PerformOperations(); BOOL aborted=TRUE;
    operation->GetAnyOperationsAborted(&aborted);
    return SUCCEEDED(hr) && SUCCEEDED(result.status) && !aborted && GetFileAttributesW(drawer.folder.c_str())==INVALID_FILE_ATTRIBUTES;
}
}
