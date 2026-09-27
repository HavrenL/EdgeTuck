#include "app.hpp"
#include "drop_target.hpp"
#include <fstream>
#include <iostream>
#include <algorithm>
namespace edge {
static void require(bool value,const char* what) { if(!value) throw std::runtime_error(what); }
static void write_fixture(const std::filesystem::path& path,const char* bytes) { std::ofstream out(path,std::ios::binary); out<<bytes; }
void App::archive_test_tick() {
    if(archive_test_running) return;
    archive_test_running=true;
    try {
        auto& first=*drawers.front(); auto& last=*drawers.back();
        static int retries{};
        auto awaiting=[&](bool ready) {
            if(ready) { retries=0; return false; }
            if(++retries>15) throw std::runtime_error("file notification timeout");
            archive_test_running=false; return true;
        };
        const auto input=archive_fixture/L"外部文件.txt";
        if(archive_step==0) {
            require(folder_available(first.model()) && folder_available(last.model()),"test folders bound");
            require(settings.storage_mode==StorageMode::Directory && std::filesystem::path(first.model().folder).parent_path()==archive_fixture/L"independent","fresh drawers use independent storage");
            write_fixture(input,"first contents");
            first.model().legacy_items={input.wstring()}; first.model().items={input.wstring()};
            refresh_archives(); require(std::filesystem::exists(input),"existing reference remains untouched");
            auto data=file_data({input.wstring()}); DWORD effect=DROPEFFECT_MOVE;
            const int depth_before=interaction_depth;
            // A modifier can temporarily forbid a drop. Releasing it must
            // restore the target without leaving/re-entering the drawer.
            first.panel_drop->DragEnter(data.Get(),MK_CONTROL,{},&effect);
            require(effect==DROPEFFECT_NONE && !first.dropping,"unsupported copy has no receiving highlight");
            first.panel_drop->DragOver(0,{},&effect);
            require(effect==DROPEFFECT_MOVE && first.dropping,"modifier release restores move feedback");
            first.panel_drop->DragLeave(); first.panel_drop->DragLeave();
            require(!first.dropping && interaction_depth==depth_before && std::filesystem::exists(input),"cancel clears feedback and preserves file");
            effect=DROPEFFECT_COPY|DROPEFFECT_MOVE;
            first.panel_drop->DragEnter(data.Get(),0,{},&effect);
            first.panel_drop->DragOver(MK_CONTROL,{},&effect);
            require(effect==DROPEFFECT_COPY && first.dropping,"copy modifier retains receiving highlight");
            first.panel_drop->DragLeave();
            effect=DROPEFFECT_MOVE;
            first.panel_drop->DragEnter(data.Get(),0,{},&effect); require(effect==DROPEFFECT_MOVE,"move feedback");
            first.panel_drop->Drop(data.Get(),0,{},&effect);
            require(effect==DROPEFFECT_NONE && !std::filesystem::exists(input),"optimized native OLE drop moves file without source deletion request");
            require(!first.dropping && interaction_depth==depth_before,"completed drop releases image and receiving state");
            require(first.model().items.size()==1 && first.model().legacy_items.empty(),"legacy reference explicitly archived");
            require(rename_drawer_to(first.id,L"归档改名") && first.model().name==L"归档改名" &&
                std::filesystem::path(first.model().folder).filename()==L"ET_归档改名" && folder_available(first.model()),"app rename updates bound folder and references in isolated profile");
            const auto extra=std::filesystem::path(first.model().folder)/L"second.txt";
            write_fixture(extra,"second contents"); refresh_archives();
            const auto from=first.model().items[0];
            record_recent_use(first.model(),from,100); record_recent_use(first.model(),extra.wstring(),200);
            const auto transfer_paths=first.model().items;
            require(import_paths(last.id,transfer_paths,first.id),"cross drawer file transfer");
            require(first.model().items.empty() && last.model().items.size()==2,"both folder views updated");
            require(recent_use(last.model(),(std::filesystem::path(last.model().folder)/std::filesystem::path(from).filename()).wstring())==100 &&
                recent_use(last.model(),(std::filesystem::path(last.model().folder)/L"second.txt").wstring())==200,"multiple moved files retain independent usage records");
            return_to_desktop(last.id,last.model().items);
            require(last.model().items.empty() && std::filesystem::exists(input),"actual desktop return");
            require(import_paths(first.id,{input.wstring()},0,0,true),"copy import");
            require(std::filesystem::exists(input) && first.model().items.size()==1,"copy retains desktop file");
            write_fixture(std::filesystem::path(last.model().folder)/L"external.txt","external event");
        } else if(archive_step==1) {
            if(awaiting(last.model().items.size()==1)) return;
            require(last.model().items.size()==1,"empty folder receives external creation notification");
            const auto from=last.model().items[0];
            require(SUCCEEDED(rename_file(control,from,L"renamed.txt")),"external rename");
        } else if(archive_step==2) {
            if(awaiting(last.model().items.size()==1 && std::filesystem::path(last.model().items[0]).filename()==L"renamed.txt")) return;
            require(last.model().items.size()==1 && std::filesystem::path(last.model().items[0]).filename()==L"renamed.txt","rename synchronized");
            const auto file=last.model().items[0]; require(DeleteFileW(file.c_str())!=FALSE,"delete owned file");
            SHChangeNotify(SHCNE_DELETE,SHCNF_PATHW,file.c_str(),nullptr);
        } else if(archive_step==3) {
            if(awaiting(last.model().items.empty())) return;
            require(last.model().items.empty(),"external deletion synchronized");
            const auto old=last.model().folder;
            require(SUCCEEDED(rename_file(control,old,L"Renamed drawer folder")),"rename backing folder");
        } else if(archive_step==4) {
            if(awaiting(std::filesystem::path(last.model().folder).filename()==L"Renamed drawer folder")) return;
            require(std::filesystem::path(last.model().folder).filename()==L"Renamed drawer folder" && folder_available(last.model()) && last.model().name==L"Renamed drawer folder","backing folder identity, path and drawer title updated");
            const int id=first.id; const auto folder=first.model().folder; const auto file=first.model().items[0];
            remove_drawer(id);
            require(std::filesystem::exists(file) && std::filesystem::is_directory(folder),"remove drawer preserves actual contents");
            restore_drawer(); require(find(id) && find(id)->folder==folder,"undo reuses folder");
            const auto config=archive_fixture/L"test-settings.dat"; std::wstring error; Settings restored;
            require(save_settings(config,settings,error) && load_settings(config,restored,error),"real app state persists in isolated profile");
            require(restored.drawers.back().folder==folder,"reload retains folder binding");
            file_watch.clear();
            std::cout<<"Native archive: PASS; actual OLE move, copy, desktop return, external create/rename/delete notifications, backing-folder rename, detach/undo, isolated persistence\n";
            KillTimer(broker,70); KillTimer(broker,folder_timer); quitting=true; PostQuitMessage(0);
        }
        ++archive_step;
    } catch(const std::exception& e) {
        smoke_ok=false; std::cerr<<"Native archive FAIL stage "<<archive_step<<": "<<e.what()<<'\n';
        KillTimer(broker,70); quitting=true; PostQuitMessage(2);
    }
    archive_test_running=false;
}
}
