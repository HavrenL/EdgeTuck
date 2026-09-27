#include "storage_move.hpp"
#include "references.hpp"
#include <fstream>
#include <cstring>
#include <set>
#include <shlobj.h>

namespace edge {
namespace {
struct Handle { HANDLE value{INVALID_HANDLE_VALUE}; ~Handle(){if(value!=INVALID_HANDLE_VALUE) CloseHandle(value);} };
std::wstring identity(HANDLE file) {
    BY_HANDLE_FILE_INFORMATION info{};
    if(!GetFileInformationByHandle(file,&info) || !(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) || (info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)) return {};
    return std::to_wstring(info.dwVolumeSerialNumber)+L":"+std::to_wstring(info.nFileIndexHigh)+L":"+std::to_wstring(info.nFileIndexLow);
}
std::wstring volume(HANDLE file) {
    wchar_t path[32768]{}; const auto size=GetFinalPathNameByHandleW(file,path,32768,VOLUME_NAME_GUID|FILE_NAME_NORMALIZED);
    if(!size || size>=32768) return {};
    const std::wstring text(path,size); const auto end=text.find(L"}\\");
    return text.starts_with(L"\\\\?\\Volume{") && end!=std::wstring::npos?text.substr(0,end+2):L"";
}
std::string bytes(const std::filesystem::path& path) {
    const auto attrs=GetFileAttributesW(path.c_str());
    if(attrs==INVALID_FILE_ATTRIBUTES || (attrs&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY))) throw std::runtime_error("Invalid journal file");
    std::ifstream in(path,std::ios::binary|std::ios::ate); const auto size=in.tellg();
    if(!in || size<0 || size>4*1024*1024) throw std::runtime_error("Invalid journal size");
    std::string data(static_cast<size_t>(size),'\0'); in.seekg(0); in.read(data.data(),size);
    if(!in) throw std::runtime_error("Cannot read journal"); return data;
}
bool plans_valid(const Settings& before,const Settings& planned) {
    if(before.drawers.size()!=planned.drawers.size() || before.drawers.size()>32) return false;
    std::set<int> ids;
    for(size_t i=0;i<before.drawers.size();++i) {
        const auto& a=before.drawers[i]; const auto& b=planned.drawers[i];
        if(a.id!=b.id || !ids.insert(a.id).second || a.folder.empty() || b.folder.empty() || a.folder_identity.empty() || b.folder_identity.empty()) return false;
        if(a.folder_identity==b.folder_identity && !same_path(a.folder,b.folder) && (path_within(a.folder,b.folder) || path_within(b.folder,a.folder))) return false;
    }
    return true;
}
}
bool same_storage_volume(const std::filesystem::path& from,const std::filesystem::path& to) {
    Handle a{CreateFileW(from.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,nullptr)};
    Handle b{CreateFileW(to.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,nullptr)};
    const auto av=volume(a.value),bv=volume(b.value); return !av.empty() && same_path(av,bv);
}
bool move_storage_folder(const std::filesystem::path& from,const std::filesystem::path& to,const std::wstring& expected,std::wstring& error) {
    auto fail=[&](const std::wstring& reason){error=reason+L"："+from.wstring()+L" → "+to.wstring(); return false;};
    if(expected.empty() || !from.is_absolute() || !to.is_absolute() || from!=from.lexically_normal() || to!=to.lexically_normal() || from==to ||
        (path_within(to.wstring(),from.wstring()) && !same_path(to.wstring(),from.wstring()))) return fail(L"无法确认移动路径");
    // Keep the source directory and destination parent from being renamed while
    // using them. Resolve the absolute target through the held parent handle.
    Handle source{CreateFileW(from.c_str(),DELETE|FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr)};
    Handle parent{CreateFileW(to.parent_path().c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr)};
    if(source.value==INVALID_HANDLE_VALUE || parent.value==INVALID_HANDLE_VALUE) return fail(L"文件夹正在使用或无法访问（错误 "+std::to_wstring(GetLastError())+L"）");
    if(identity(source.value)!=expected || identity(parent.value).empty()) return fail(L"文件夹身份已变化");
    const auto a=volume(source.value),b=volume(parent.value);
    if(a.empty() || !same_path(a,b)) return fail(L"不是同一磁盘分区，不能快速移动");
    const auto name=to.filename().wstring();
    if(name.empty() || name==L"." || name==L".." || name.find(L'\0')!=std::wstring::npos) return fail(L"文件夹名称无效");
    wchar_t parent_path[32768]{}; const auto parent_size=GetFinalPathNameByHandleW(parent.value,parent_path,32768,FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
    if(!parent_size || parent_size>=32768) return fail(L"无法确认目标目录");
    auto destination=std::wstring(parent_path,parent_size); if(destination.back()!=L'\\') destination+=L'\\'; destination+=name;
    const auto length=destination.size()*sizeof(wchar_t);
    std::vector<std::byte> buffer(sizeof(FILE_RENAME_INFO)+length);
    auto* info=reinterpret_cast<FILE_RENAME_INFO*>(buffer.data()); info->ReplaceIfExists=FALSE; info->RootDirectory=nullptr;
    info->FileNameLength=static_cast<DWORD>(length); memcpy(info->FileName,destination.data(),length);
    if(!SetFileInformationByHandle(source.value,FileRenameInfo,info,static_cast<DWORD>(buffer.size()))) return fail(L"无法移动文件夹（错误 "+std::to_wstring(GetLastError())+L"）");
    SHChangeNotify(SHCNE_RENAMEFOLDER,SHCNF_PATHW,from.c_str(),to.c_str()); return true;
}
bool flush_storage_record(const std::filesystem::path& path) {
    Handle file{CreateFileW(path.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr)};
    return file.value!=INVALID_HANDLE_VALUE && FlushFileBuffers(file.value)!=FALSE;
}
bool finish_storage_record(const std::filesystem::path& journal) {
    return MoveFileExW((journal/L"planned.dat").c_str(),(journal/L"resolved.dat").c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=FALSE;
}
bool restore_storage_moves(const Settings& before,const Settings& planned,std::wstring& error) {
    if(!plans_valid(before,planned)) { error=L"迁移记录无法核对，文件保持当前位置。"; return false; }
    bool okay=true;
    for(size_t n=before.drawers.size();n>0;--n) {
        const auto& a=before.drawers[n-1]; const auto& b=planned.drawers[n-1];
        // Cross-volume copies have a new identity; they are never deleted or
        // accepted as the original during rollback/recovery.
        if(a.folder_identity!=b.folder_identity || same_path(a.folder,b.folder)) continue;
        if(folder_available(a)) continue;
        std::wstring detail;
        if(!folder_available(b) || !move_storage_folder(b.folder,a.folder,a.folder_identity,detail)) {
            okay=false; error=L"部分文件夹未能恢复原位置，文件仍保留；请检查占用或同名文件夹。\r\n"+(detail.empty()?b.folder:detail);
        }
    }
    return okay;
}
bool recover_storage(const std::filesystem::path& config,std::wstring& message) {
    try {
        const auto root=config.parent_path()/L"migrations";
        if(!std::filesystem::exists(root)) return true;
        if(folder_identity(root).empty()) throw std::runtime_error("Invalid journal directory");
        for(const auto& entry:std::filesystem::directory_iterator(root)) {
            const auto record=entry.path(),plan=record/L"planned.dat";
            if(folder_identity(record).empty() || !std::filesystem::exists(plan)) continue;
            Settings before,planned; std::wstring error;
            // Read exact records, never the load_settings .bak recovery fallback.
            const auto plan_bytes=bytes(plan),old_bytes=bytes(record/L"original.dat"),current=bytes(config);
            if(!load_settings(record/L"before.dat",before,error) || !error.empty() || !load_settings(plan,planned,error) || !error.empty() || !plans_valid(before,planned)) throw std::runtime_error("Invalid move plan");
            if(current==plan_bytes) {
                for(const auto& d:planned.drawers) if(!folder_available(d)) throw std::runtime_error("Committed drawer unavailable");
                if(!finish_storage_record(record)) throw std::runtime_error("Cannot finish journal");
                message=L"已确认上次迁移保存的新位置。";
            } else if(current==old_bytes) {
                if(!restore_storage_moves(before,planned,error)) { message=error+L"\r\n恢复记录："+record.wstring(); return false; }
                if(!finish_storage_record(record)) throw std::runtime_error("Cannot finish rollback journal");
                message=L"上次迁移未完成，已恢复原文件夹位置。";
            } else { message=L"迁移恢复记录与当前配置不一致，暂不更改文件；请先检查："+record.wstring(); return false; }
        }
        return true;
    } catch(...) { message=L"迁移恢复未完成，文件保持当前位置，请检查配置目录中的 migrations 恢复记录。"; return false; }
}
}
