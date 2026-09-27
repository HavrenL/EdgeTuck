#include "explorer_entry.hpp"
#include <shlobj.h>
#include <shellapi.h>
#include <vector>

namespace edge {
namespace {
constexpr wchar_t owner_name[]=L"EdgeTuck.Owner";
constexpr wchar_t owner_value[]=L"EdgeTuck.ExplorerEntry.1";
const std::wstring class_key=std::wstring(L"Software\\Classes\\CLSID\\")+explorer_entry_id;
const std::wstring namespace_key=std::wstring(L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\MyComputer\\NameSpace\\")+explorer_entry_id;
struct Key { HKEY value{}; ~Key(){if(value) RegCloseKey(value);} };
std::wstring read_string(HKEY key,const wchar_t* name,LSTATUS& error) {
    DWORD size{}; error=RegGetValueW(key,nullptr,name,RRF_RT_REG_SZ,nullptr,nullptr,&size);
    if(error) return {};
    if(size<2 || size>65536 || size%2) {error=ERROR_INVALID_DATA; return {};}
    std::vector<wchar_t> value(size/2+1);
    error=RegGetValueW(key,nullptr,name,RRF_RT_REG_SZ,nullptr,value.data(),&size);
    return error?std::wstring{}:std::wstring(value.data());
}
LSTATUS ownership(HKEY root,const std::wstring& path,REGSAM view,bool& exists) {
    Key key; auto error=RegOpenKeyExW(root,path.c_str(),0,KEY_QUERY_VALUE|view,&key.value);
    exists=error!=ERROR_FILE_NOT_FOUND;
    if(error==ERROR_FILE_NOT_FOUND) return ERROR_SUCCESS;
    if(error) return error;
    const auto owner=read_string(key.value,owner_name,error);
    return error || owner!=owner_value?ERROR_ALREADY_EXISTS:ERROR_SUCCESS;
}
LSTATUS set_value(HKEY root,const std::wstring& path,REGSAM view,const wchar_t* name,DWORD type,const void* data,DWORD bytes) {
    Key key; auto error=RegCreateKeyExW(root,path.c_str(),0,nullptr,0,KEY_SET_VALUE|view,nullptr,&key.value,nullptr);
    return error?error:RegSetValueExW(key.value,name,0,type,static_cast<const BYTE*>(data),bytes);
}
LSTATUS remove_key(HKEY root,const std::wstring& path,REGSAM view) {
    const auto slash=path.find_last_of(L'\\'); Key parent;
    auto error=RegOpenKeyExW(root,path.substr(0,slash).c_str(),0,KEY_READ|KEY_WRITE|view,&parent.value);
    if(error==ERROR_FILE_NOT_FOUND) return ERROR_SUCCESS;
    if(error) return error;
    error=RegDeleteTreeW(parent.value,path.substr(slash+1).c_str());
    return error==ERROR_FILE_NOT_FOUND?ERROR_SUCCESS:error;
}
bool valid_path(const std::filesystem::path& path) {
    const auto value=path.wstring();
    return path.is_absolute() && value.size()<32760 && value.find_first_of(L"\"\r\n") == std::wstring::npos && value.find(L'\0')==std::wstring::npos;
}
std::wstring icon_path(const std::filesystem::path& exe) {return L"\""+exe.wstring()+L"\",-1";}
}
ExplorerEntryStatus read_explorer_entry(HKEY root,REGSAM view) {
    ExplorerEntryStatus result; bool exists{};
    result.error=ownership(root,namespace_key,view,exists);
    if(result.error || !exists) return result;
    result.enabled=true;
    result.error=ownership(root,class_key,view,exists);
    if(result.error || !exists) {if(!result.error) result.error=ERROR_FILE_NOT_FOUND; return result;}
    Key bag,icon;
    result.error=RegOpenKeyExW(root,(class_key+L"\\Instance\\InitPropertyBag").c_str(),0,KEY_QUERY_VALUE|view,&bag.value);
    if(result.error) return result;
    result.target=read_string(bag.value,L"TargetFolderPath",result.error); if(result.error) return result;
    result.error=RegOpenKeyExW(root,(class_key+L"\\DefaultIcon").c_str(),0,KEY_QUERY_VALUE|view,&icon.value);
    if(!result.error) result.icon=read_string(icon.value,nullptr,result.error);
    return result;
}
LSTATUS write_explorer_entry(HKEY root,REGSAM view,bool enable,const std::filesystem::path& target,const std::filesystem::path& executable) {
    if(enable && (!valid_path(target) || !valid_path(executable))) return ERROR_INVALID_PARAMETER;
    bool had_class{},had_namespace{};
    auto error=ownership(root,class_key,view,had_class); if(error) return error;
    error=ownership(root,namespace_key,view,had_namespace); if(error) return error;
    if(!enable) {
        if(had_namespace) { error=remove_key(root,namespace_key,view); if(error) return error; }
        return had_class?remove_key(root,class_key,view):ERROR_SUCCESS;
    }
    auto string=[&](const std::wstring& path,const wchar_t* name,const std::wstring& value,DWORD type=REG_SZ) {
        if(!error) error=set_value(root,path,view,name,type,value.c_str(),static_cast<DWORD>((value.size()+1)*sizeof(wchar_t)));
    };
    auto number=[&](const std::wstring& path,const wchar_t* name,DWORD value) {
        if(!error) error=set_value(root,path,view,name,REG_DWORD,&value,sizeof(value));
    };
    string(class_key,owner_name,owner_value);
    string(class_key,nullptr,L"轻屉"); string(class_key,L"InfoTip",L"浏览轻屉分类文件夹");
    string(class_key+L"\\DefaultIcon",nullptr,icon_path(executable));
    number(class_key,L"System.IsPinnedToNameSpaceTree",1); number(class_key,L"SortOrderIndex",0x48);
    // Microsoft's built-in folder instance supplies Explorer and file-dialog
    // navigation. EdgeTuck does not inject a DLL or implement file operations.
    string(class_key+L"\\InProcServer32",nullptr,L"%SystemRoot%\\System32\\shell32.dll",REG_EXPAND_SZ);
    string(class_key+L"\\Instance",L"CLSID",L"{0E5AAE11-A475-4C5B-AB00-C66DE400274E}");
    number(class_key+L"\\Instance\\InitPropertyBag",L"Attributes",0x11);
    string(class_key+L"\\Instance\\InitPropertyBag",L"TargetFolderPath",target.wstring());
    number(class_key+L"\\ShellFolder",L"FolderValueFlags",0x28);
    number(class_key+L"\\ShellFolder",L"Attributes",0xF080004D);
    // Publish only after the class is ready. Both keys carry an ownership marker.
    string(namespace_key,owner_name,owner_value); string(namespace_key,nullptr,L"轻屉");
    if(error) {
        if(!had_namespace) remove_key(root,namespace_key,view);
        if(!had_class) remove_key(root,class_key,view);
    }
    return error;
}
ExplorerEntryStatus explorer_entry_status() {return read_explorer_entry(HKEY_CURRENT_USER,KEY_WOW64_64KEY);}
LSTATUS set_explorer_entry(bool enable,const std::filesystem::path& target,const std::filesystem::path& executable) {
    if(enable) {
        const auto x64=read_explorer_entry(HKEY_CURRENT_USER,KEY_WOW64_64KEY),x86=read_explorer_entry(HKEY_CURRENT_USER,KEY_WOW64_32KEY);
        if(!x64.error && !x86.error && x64.enabled && x86.enabled && x64.target==target.wstring() && x86.target==target.wstring() && x64.icon==icon_path(executable) && x86.icon==icon_path(executable)) return ERROR_SUCCESS;
    }
    auto error=write_explorer_entry(HKEY_CURRENT_USER,KEY_WOW64_64KEY,enable,target,executable);
    if(!error) error=write_explorer_entry(HKEY_CURRENT_USER,KEY_WOW64_32KEY,enable,target,executable);
    SHChangeNotify(SHCNE_ASSOCCHANGED,SHCNF_IDLIST|SHCNF_FLUSHNOWAIT,nullptr,nullptr);
    PIDLIST_ABSOLUTE computer{};
    if(SUCCEEDED(SHGetKnownFolderIDList(FOLDERID_ComputerFolder,0,nullptr,&computer))) {
        SHChangeNotify(SHCNE_UPDATEDIR,SHCNF_IDLIST|SHCNF_FLUSHNOWAIT,computer,nullptr); CoTaskMemFree(computer);
    }
    return error;
}
void open_explorer_entry(HWND owner) {
    const auto path=std::wstring(L"shell:::{20D04FE0-3AEA-1069-A2D8-08002B30309D}\\::")+explorer_entry_id;
    ShellExecuteW(owner,L"open",path.c_str(),nullptr,nullptr,SW_SHOWNORMAL);
}
}
