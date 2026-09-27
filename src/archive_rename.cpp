#include "archive_rename.hpp"
#include "storage_move.hpp"
#include "references.hpp"
#include <algorithm>

namespace edge {
RenameResult rename_archive(const Settings& before,int id,const std::wstring& name,const std::filesystem::path& config) {
    RenameResult result; result.settings=before;
    auto current=std::find_if(before.drawers.begin(),before.drawers.end(),[&](const auto& d){return d.id==id;});
    if(current==before.drawers.end() || name.empty() || name.size()>64 || name.find(L'\0')!=std::wstring::npos ||
        std::any_of(name.begin(),name.end(),[](wchar_t c){return c<32;})) {result.message=L"抽屉名称无效。"; return result;}
    if(!folder_available(*current)) {result.message=L"抽屉文件夹不可用，未修改名称。请先恢复原文件夹。"; return result;}
    const std::filesystem::path old(current->folder),target=old.parent_path()/archive_folder_name(name);
    const bool move=old.wstring()!=target.wstring();
    if(!old.is_absolute() || old!=old.lexically_normal() || target.parent_path()!=old.parent_path()) {result.message=L"无法确认原文件夹位置。"; return result;}
    if(move) {
        for(const auto& other:before.drawers) if(other.id!=id && !other.folder.empty() &&
            (path_within(other.folder,current->folder) || path_within(current->folder,other.folder))) {result.message=L"抽屉目录彼此嵌套，请先在资源管理器中处理。"; return result;}
        const auto attrs=GetFileAttributesW(target.c_str()); const auto error=GetLastError();
        if((attrs!=INVALID_FILE_ATTRIBUTES && !same_path(old.wstring(),target.wstring())) ||
            (attrs==INVALID_FILE_ATTRIBUTES && error!=ERROR_FILE_NOT_FOUND && error!=ERROR_PATH_NOT_FOUND)) {
            result.message=L"目标名称已存在或无法确认，未覆盖或合并："+target.filename().wstring(); return result;
        }
    }
    auto next=before;
    auto& drawer=next.drawers[static_cast<size_t>(current-before.drawers.begin())]; drawer.name=name;
    if(move) {
        drawer.folder=target.wstring();
        for(auto& d:next.drawers) for(auto& p:d.legacy_items) if(path_within(p,current->folder)) p=drawer.folder+p.substr(current->folder.size());
        rename_references(next,current->folder,drawer.folder);
        if(!move_storage_folder(old,target,current->folder_identity,result.message)) return result;
    }
    if(save_settings(config,next,result.message)) {
        result.saved=true; result.settings=std::move(next);
        result.message=move?L"抽屉和文件夹已同步改名。":L"抽屉名称已保存，文件夹名称已对应。"; return result;
    }
    const auto save_error=result.message;
    if(move) {
        std::wstring rollback_error;
        if(!move_storage_folder(target,old,current->folder_identity,rollback_error)) {
            // Keep the surviving identity-bound path usable in memory. On restart
            // reconnect_folder finds this same directory in the unchanged parent.
            result.settings=std::move(next); result.recovery_required=true;
            result.message=L"名称未能保存，文件夹未能恢复原名称，文件仍保留："+target.wstring()+L"\r\n"+rollback_error;
            return result;
        }
    }
    result.message=save_error+L" 原名称已保留。"; return result;
}
}
