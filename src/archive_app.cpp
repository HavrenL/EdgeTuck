#include "app.hpp"
#include "diagnostics.hpp"
#include <algorithm>

namespace edge {
bool App::ensure_folder(int id) {
    auto* d=find(id); if(!d || storage_migrating || (smoke && !archive_test) || !save_allowed) return false;
    std::wstring error;
    if(!bind_new_folder(*d,archive_root(),error)) { notice=error; show_control(); invalidate(); return false; }
    if(!save()) return false; // Binding must be recoverable before any file movement.
    refresh_file_watch(); schedule_folders(true); return true;
}
void App::initialize_folders() {
    if((smoke && !archive_test) || !save_allowed) return;
    for(auto& d:settings.drawers) {
        std::wstring error;
        if(d.folder.empty()) bind_new_folder(d,archive_root(),error);
        else reconnect_folder(d,std::filesystem::path(d.folder).parent_path());
        if(!error.empty()) notice=error;
    }
    refresh_archives();
    if(save()) schedule_folders();
}
void App::schedule_folders(bool refresh) {
    if((smoke && !archive_test) || quitting || !save_allowed || syncing_folders) return;
    folders_dirty|=refresh;
    if(!folder_sync_pending) {
        folder_sync_pending=true; folder_retries=3;
        SetTimer(broker,folder_timer,120,nullptr);
    }
}
void App::refresh_archives() {
    if(smoke && !archive_test) return;
    bool changed=false;
    for(auto& d:settings.drawers) {
        std::wstring error; changed|=refresh_folder(d,error);
        if(!error.empty()) notice=error;
    }
    if(changed) references_changed();
    else invalidate();
}
void App::sync_folders() {
    KillTimer(broker,folder_timer);
    if(interaction_depth) { folder_sync_pending=false; return; }
    folder_sync_pending=false;
    if((smoke && !archive_test) || quitting || !save_allowed) return;
    syncing_folders=true;
    bool retry=false,changed=false;
    if(folders_dirty) {
        folders_dirty=false;
        for(auto& d:settings.drawers) if(!d.folder.empty()) changed|=reconnect_folder(d,std::filesystem::path(d.folder).parent_path());
        refresh_archives(); refresh_file_watch();
    }
    for(auto& d:settings.drawers) {
        if(d.folder.empty() || smoke) continue;
        const bool had_position=d.restore_position;
        const auto result=park_folder_icon(d);
        if(!folder_positions.contains(d.id) || folder_positions[d.id]!=result) {
            diagnostic(result==ParkResult::Parked?"archive.icon.parked":result==ParkResult::AutoArrange?"archive.icon.auto_arrange":"archive.icon.unavailable");
        }
        folder_positions[d.id]=result;
        changed|=had_position!=d.restore_position;
        retry|=result==ParkResult::Unavailable && folder_available(d);
    }
    if(changed) save();
    syncing_folders=false; invalidate();
    // Bounded retries only for Explorer's asynchronous creation/restart.
    if(retry && --folder_retries>0) { folder_sync_pending=true; SetTimer(broker,folder_timer,400,nullptr); }
}
static void apply_completed(Settings& settings,const TransferResult& result,bool copy,int destination=0) {
    if(copy) return;
    std::vector<RecentUse> transferred;
    for(const auto& moved:result.completed) {
        uint64_t used{}; for(const auto& d:settings.drawers) used=std::max(used,recent_use(d,moved.from));
        for(auto& d:settings.drawers) std::erase_if(d.legacy_items,[&](const auto& p){return path_within(p,moved.from);});
        rename_references(settings,moved.from,moved.to);
        if(used) transferred.push_back({moved.to,used});
    }
    // Append after all renames; the next rename prunes records not yet in the
    // destination's refreshed item list.
    for(auto& d:settings.drawers) if(d.id==destination) d.recent_uses.insert(d.recent_uses.end(),transferred.begin(),transferred.end());
}
bool App::import_paths(int destination,const std::vector<std::wstring>& paths,int source,size_t before,bool copy) {
    if(paths.empty() || storage_migrating) return false;
    if(smoke && !archive_test) {
        const bool okay=place_references(settings,destination,paths,source,before); if(okay) references_changed(); return okay;
    }
    Interaction interaction(*this);
    if(!ensure_folder(destination)) return false;
    auto* d=find(destination); if(!d) return false;
    if(source==destination && !copy) {
        // Reordering a legacy reference does not implicitly archive its file.
        const bool okay=place_references(settings,destination,paths,source,before);
        if(okay) { find(destination)->sort=SortMode::Manual; references_changed(); }
        return okay;
    }
    if(source) {
        const auto* from=find(source); if(!from) return false;
        if(!from->folder.empty() && !folder_available(*from)) { notice=L"来源抽屉文件夹不可用，未移动文件。"; invalidate(); return false; }
        for(const auto& path:paths) if(std::none_of(from->items.begin(),from->items.end(),[&](const auto& p){return same_path(p,path);})) return false;
    }
    std::wstring error; refresh_folder(*d,error);
    size_t added=0;
    for(const auto& path:paths) if(copy || std::none_of(d->items.begin(),d->items.end(),[&](const auto& p){return same_path(p,path);})) ++added;
    if(d->items.size()+added>1000) { notice=L"目标抽屉最多显示 1000 项，本次操作未执行。"; invalidate(); return false; }
    std::vector<std::wstring> protected_paths;
    for(const auto& x:settings.drawers) if(!x.folder.empty()) protected_paths.push_back(x.folder);
    const auto result=transfer_files(control,paths,d->folder,copy,protected_paths);
    diagnostic(copy?"archive.copy":"archive.move",result.status);
    apply_completed(settings,result,copy,destination);
    refresh_archives();
    std::vector<std::wstring> landed;
    for(const auto& item:result.completed) landed.push_back(item.to);
    if(!landed.empty()) place_references(settings,destination,landed,0,before);
    references_changed(); schedule_folders();
    if(FAILED(result.status) || result.aborted || result.completed.size()<paths.size()) {
        notice=L"文件操作未全部完成：已完成 "+std::to_wstring(result.completed.size())+L" 项，其余文件保留原处。";
        if(result.status==E_INVALIDARG) notice=L"不能把抽屉文件夹、它的上级目录或无效路径放进抽屉。";
        show_control(); invalidate();
    }
    return !result.completed.empty();
}
void App::return_to_desktop(int source,const std::vector<std::wstring>& paths) {
    if(paths.empty() || storage_migrating) return;
    if(smoke && !archive_test) { if(remove_references(settings,source,paths)) references_changed(); return; }
    Interaction interaction(*this); auto* d=find(source); if(!d) return;
    std::vector<std::wstring> move,legacy;
    for(const auto& p:paths) {
        if(std::any_of(d->legacy_items.begin(),d->legacy_items.end(),[&](const auto& x){return same_path(x,p);})) legacy.push_back(p);
        else if(!d->folder.empty() && same_path(std::filesystem::path(p).parent_path().wstring(),d->folder)) move.push_back(p);
    }
    if(!move.empty()) {
        if(!folder_available(*d)) { notice=L"抽屉文件夹不可用，未移动文件。"; invalidate(); return; }
        const auto result=transfer_files(control,move,desktop_root());
        diagnostic("archive.return_to_desktop",result.status);
        apply_completed(settings,result,false);
        if(FAILED(result.status) || result.aborted || result.completed.size()<move.size()) notice=L"部分文件未能移回桌面，未完成的文件仍留在抽屉文件夹中。";
    }
    std::erase_if(d->legacy_items,[&](const auto& p){return std::any_of(legacy.begin(),legacy.end(),[&](const auto& x){return same_path(x,p);});});
    remove_references(settings,source,legacy);
    refresh_archives(); references_changed(); schedule_folders();
}
void App::paste_files(int destination) {
    Interaction interaction(*this); ComPtr<IDataObject> data;
    if(FAILED(OleGetClipboard(&data))) return;
    // Copy/paste retains normal Explorer semantics; cut/paste actually moves.
    const bool move=preferred_drop_effect(data.Get())==DROPEFFECT_MOVE;
    if(import_paths(destination,data_paths(data.Get()),0,std::numeric_limits<size_t>::max(),!move) && move) {
        completed_file_move(data.Get());
        // Optimized move already handled the files. Remove stale cut payload.
        if(OleIsCurrentClipboard(data.Get())==S_OK) OleSetClipboard(nullptr);
    }
}
}
