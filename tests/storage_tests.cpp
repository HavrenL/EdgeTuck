#include "storage.hpp"
#include "storage_move.hpp"
#include "references.hpp"
#include "sorting.hpp"
#include <ole2.h>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <cstdlib>
using namespace edge;
static int checks{};
static void require(bool okay,const char* text) { ++checks; if(!okay) throw std::runtime_error(text); }
static void write(const std::filesystem::path& path,const std::string& bytes) { std::ofstream out(path,std::ios::binary); out<<bytes; require(out.good(),"write fixture"); }
static std::string read(const std::filesystem::path& path) { std::ifstream in(path,std::ios::binary); return {std::istreambuf_iterator<char>(in),{}}; }
static void describe(const MigrationResult& result) {
    const int length=WideCharToMultiByte(CP_UTF8,0,result.message.data(),static_cast<int>(result.message.size()),nullptr,0,nullptr,nullptr);
    std::string message(length,'\0'); WideCharToMultiByte(CP_UTF8,0,result.message.data(),static_cast<int>(result.message.size()),message.data(),length,nullptr,nullptr);
    std::cerr<<message<<"; committed="<<result.committed<<"; moved="<<result.moved_folders<<"; recovery="<<result.recovery_required<<'\n';
}
struct Fixture {
    std::filesystem::path root,source,target,config;
    Settings settings; std::atomic_bool cancel{};
    explicit Fixture(const std::filesystem::path& base,const wchar_t* name) {
        root=base/name; source=root/L"desktop"; target=root/L"storage"; config=root/L"state"/L"settings.dat";
        std::filesystem::create_directories(source); std::filesystem::create_directories(target);
        settings.storage_mode=StorageMode::Desktop;
        settings.drawers={{1,L"资料",Edge::Right,1,4,5,{}}}; std::wstring error;
        require(bind_new_folder(settings.drawers[0],source,error),"bound test folder");
        write(std::filesystem::path(settings.drawers[0].folder)/L"资料.txt","original");
        refresh_folder(settings.drawers[0],error); require(save_settings(config,settings,error),"save initial config");
    }
    std::filesystem::path old() const { return settings.drawers[0].folder; }
    MigrationResult run(const MigrationProgress& report={},MigrationOptions options={false}) { return migrate_storage(nullptr,settings,StorageMode::Directory,target,config,cancel,report,options); }
};
static void crash_child(const Fixture& f,bool committed) {
    wchar_t executable[32768]{}; GetModuleFileNameW(nullptr,executable,32768);
    std::wstring command=L"\""+std::wstring(executable)+L"\" "+(committed?L"--crash-committed":L"--crash-moved")+L" \""+f.root.wstring()+L"\"";
    STARTUPINFOW startup{sizeof(startup)}; PROCESS_INFORMATION process{};
    require(CreateProcessW(executable,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&process)!=FALSE,"start owned migration crash fixture");
    const auto wait=WaitForSingleObject(process.hProcess,10000); if(wait!=WAIT_OBJECT_0) TerminateProcess(process.hProcess,99);
    DWORD code{}; GetExitCodeProcess(process.hProcess,&code); CloseHandle(process.hThread); CloseHandle(process.hProcess);
    require(wait==WAIT_OBJECT_0 && code==77,"owned migration process interrupted at requested boundary");
}
int wmain(int argc,wchar_t** argv) {
    OleInitialize(nullptr); std::filesystem::path root;
    try {
        if(argc==3) {
            const std::filesystem::path fixture=argv[2];
            require(fixture.is_absolute() && fixture.parent_path().filename().wstring().starts_with(L"EdgeTuck-storage-test-") && std::filesystem::equivalent(fixture.parent_path().parent_path(),std::filesystem::temp_directory_path()),"crash child limited to owned temp fixture");
            Settings settings; std::wstring error; std::atomic_bool cancel{};
            require(load_settings(fixture/L"state"/L"settings.dat",settings,error) && error.empty(),"load crash fixture");
            const auto stage=std::wstring_view(argv[1])==L"--crash-committed"?MigrationStage::Committed:MigrationStage::Moved;
            migrate_storage(nullptr,settings,StorageMode::Directory,fixture/L"storage",fixture/L"state"/L"settings.dat",cancel,[&](MigrationStage current,const auto&){if(current==stage) std::_Exit(77);});
            return 3;
        }
        root=std::filesystem::temp_directory_path()/(L"EdgeTuck-storage-test-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
        require(std::filesystem::create_directory(root),"unique owned test root");
        {
            Fixture f(root,L"success"); const auto original=f.old();
            std::filesystem::create_directories(original/L"nested"/L"empty"); write(original/L"nested"/L"binary.dat",std::string("a\0b",3));
            write(original/L"zero",""); write(original/L"hidden.txt","hidden"); SetFileAttributesW((original/L"hidden.txt").c_str(),FILE_ATTRIBUTE_HIDDEN);
            write(original/L"资料.txt:edgetuck-test","stream contents");
            const auto external=f.root/L"legacy.txt"; write(external,"external");
            f.settings.drawers[0].items.insert(f.settings.drawers[0].items.begin(),external.wstring());
            f.settings.drawers[0].legacy_items={external.wstring()}; f.settings.drawers[0].restore_position=true; f.settings.drawers[0].restore_x=17;
            f.settings.drawers[0].sort=SortMode::Recent; f.settings.drawers[0].sort_descending=true;
            f.settings.drawers[0].recent_uses={{(original/L"资料.txt").wstring(),100},{external.wstring(),50}};
            std::wstring error; require(save_settings(f.config,f.settings,error),"save legacy fixture");
            const auto collision=f.target/original.filename(); std::filesystem::create_directory(collision); write(collision/L"keep.txt","untouched");
            auto result=f.run();
            if(!result.committed || !result.retained_sources.empty()) {
                const auto bytes=WideCharToMultiByte(CP_UTF8,0,result.message.c_str(),-1,nullptr,0,nullptr,nullptr);
                std::string message(bytes,'\0'); WideCharToMultiByte(CP_UTF8,0,result.message.c_str(),-1,message.data(),bytes,nullptr,nullptr);
                std::cerr<<"Migration: "<<message<<"; committed="<<result.committed<<"; copies="<<result.copies.size()<<"; retained="<<result.retained_sources.size()<<'\n';
            }
            require(result.committed && result.retained_sources.empty(),"copy verification commit and recycle succeed");
            const auto destination=std::filesystem::path(result.settings.drawers[0].folder);
            require(!std::filesystem::exists(original) && destination!=collision && read(collision/L"keep.txt")=="untouched","source removed only after copy and collisions untouched");
            require(read(destination/L"资料.txt")=="original" && read(destination/L"nested"/L"binary.dat")==std::string("a\0b",3) && std::filesystem::is_directory(destination/L"nested"/L"empty"),"nested empty and binary files preserved");
            require(read(destination/L"hidden.txt")=="hidden" && (GetFileAttributesW((destination/L"hidden.txt").c_str())&FILE_ATTRIBUTE_HIDDEN),"hidden file content and attribute preserved");
            require(read(destination/L"资料.txt:edgetuck-test")=="stream contents","named file streams preserved");
            require(read(external)=="external" && result.settings.drawers[0].legacy_items==std::vector<std::wstring>{external.wstring()},"external legacy references never migrated");
            require(result.settings.drawers[0].items[0]==external.wstring() && !result.settings.drawers[0].restore_position,"item order and clean new desktop positioning");
            Settings loaded; require(load_settings(f.config,loaded,error) && loaded.storage_mode==StorageMode::Directory && loaded.drawers[0].folder==destination && folder_available(loaded.drawers[0]),"new bindings survive reload");
            require(loaded.drawers[0].sort==SortMode::Recent && recent_use(loaded.drawers[0],(destination/L"资料.txt").wstring())==100 && recent_use(loaded.drawers[0],external.wstring())==50,"migration preserves sort preference and usage at rewritten paths");
            auto noop=migrate_storage(nullptr,loaded,StorageMode::Directory,f.target,f.config,f.cancel);
            require(noop.committed && noop.copies.empty() && folder_available(noop.settings.drawers[0]),"repeating same destination creates no duplicate");
            const auto back=f.root/L"return-desktop"; std::filesystem::create_directory(back);
            auto returned=migrate_storage(nullptr,loaded,StorageMode::Desktop,back,f.config,f.cancel);
            if(!returned.committed) describe(returned);
            require(returned.committed && returned.retained_sources.empty() && returned.settings.storage_mode==StorageMode::Desktop && std::filesystem::path(returned.settings.drawers[0].folder).parent_path()==back,"migration back to desktop mode");
            require(read(std::filesystem::path(returned.settings.drawers[0].folder)/L"资料.txt")=="original","roundtrip retains contents");
        }
        {
            Fixture f(root,L"directory-stream"); write(f.old().wstring()+L":edgetuck-test","directory metadata");
            auto result=f.run();
            require(!result.committed && std::filesystem::is_empty(f.target) && read(f.old().wstring()+L":edgetuck-test")=="directory metadata","directory streams unsupported by Shell copy rejected before copying");
        }
        {
            Fixture f(root,L"multiple");
            DrawerModel second{2,L"照片",Edge::Left,1,4,5,{}}; std::wstring error;
            require(bind_new_folder(second,f.source,error),"second drawer folder");
            write(std::filesystem::path(second.folder)/L"photo.bin","pixels");
            refresh_folder(second,error); f.settings.drawers.push_back(second);
            require(save_settings(f.config,f.settings,error),"save multiple drawers");
            auto result=f.run();
            require(result.committed && result.copies.size()==2 && result.retained_sources.empty(),"all drawers migrate together");
            for(const auto& d:result.settings.drawers) require(folder_available(d) && std::filesystem::path(d.folder).parent_path()==f.target,"each migrated binding belongs to destination");
        }
        {
            Fixture f(root,L"protected-target"); std::wstring error;
            wchar_t module[32768]{}; GetModuleFileNameW(nullptr,module,32768);
            const auto in_program=std::filesystem::path(module).parent_path()/(L"storage-test-"+std::to_wstring(GetCurrentProcessId()));
            const auto in_config=default_config_path().parent_path()/(L"storage-test-"+std::to_wstring(GetCurrentProcessId()));
            require(!prepare_storage_directory(f.settings,in_program,error) && !std::filesystem::exists(in_program),"program subdirectory rejected before creation");
            require(!prepare_storage_directory(f.settings,in_config,error) && !std::filesystem::exists(in_config),"configuration subdirectory rejected before creation");
        }
        {
            Fixture f(root,L"cancel"); const auto before=read(f.config); f.cancel=true; auto result=f.run();
            require(!result.committed && std::filesystem::exists(f.old()) && read(f.config)==before && std::filesystem::is_empty(f.target),"cancel before copy leaves state untouched");
        }
        {
            Fixture f(root,L"nested"); const auto bad=f.old()/L"inside"; auto result=migrate_storage(nullptr,f.settings,StorageMode::Directory,bad,f.config,f.cancel);
            require(!result.committed && !std::filesystem::exists(bad) && folder_available(f.settings.drawers[0]),"reject target inside source before creating it");
        }
        {
            Fixture f(root,L"changed"); const auto before=read(f.config);
            auto result=f.run([&](MigrationStage stage,const auto&) { if(stage==MigrationStage::Verify) write(f.old()/L"资料.txt","modified"); });
            require(!result.committed && read(f.config)==before && read(f.old()/L"资料.txt")=="modified","same-size source edits prevent switch and cleanup");
        }
        {
            Fixture f(root,L"late-edit");
            auto result=f.run([&](MigrationStage stage,const auto&) { if(stage==MigrationStage::Cleanup) write(f.old()/L"资料.txt","new data"); });
            require(result.committed && result.retained_sources.size()==1 && read(f.old()/L"资料.txt")=="new data","post-commit edit retains source instead of deleting newer data");
            require(read(std::filesystem::path(result.settings.drawers[0].folder)/L"资料.txt")=="original","verified destination remains available after retained source");
        }
        {
            Fixture f(root,L"config-save"); const auto before=read(f.config);
            auto result=f.run([&](MigrationStage stage,const auto&) { if(stage==MigrationStage::Commit) SetFileAttributesW(f.config.c_str(),FILE_ATTRIBUTE_READONLY); });
            SetFileAttributesW(f.config.c_str(),FILE_ATTRIBUTE_NORMAL);
            require(!result.committed && read(f.config)==before && folder_available(f.settings.drawers[0]),"failed durable configuration commit never deletes source");
        }
        {
            Fixture f(root,L"locked"); HANDLE lock=CreateFileW((f.old()/L"资料.txt").c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);
            require(lock!=INVALID_HANDLE_VALUE,"lock owned file"); auto result=f.run(); CloseHandle(lock);
            require(!result.committed && std::filesystem::is_empty(f.target) && folder_available(f.settings.drawers[0]),"file being written fails preflight without partial copy");
        }
        {
            Fixture f(root,L"replace"); const auto identity=f.settings.drawers[0].folder_identity;
            std::filesystem::rename(f.old(),f.source/L"moved-original"); std::filesystem::create_directory(f.old());
            auto result=f.run(); require(!result.committed && std::filesystem::is_directory(f.old()) && result.settings.drawers[0].folder_identity==identity,"replacement source directory never claimed or recycled");
        }
        {
            Fixture f(root,L"fast-many"); const auto identity=f.settings.drawers[0].folder_identity;
            const auto data=f.old()/L"node_modules"; std::filesystem::create_directory(data); bool written=true;
            for(int i=0;i<2048;++i) { std::ofstream file(data/(std::to_wstring(i)+L".txt")); file<<i; written&=file.good(); }
            require(written,"prepare 2048 owned small files");
            write(f.old().wstring()+L":edgetuck-test","folder stream");
            bool copied=false,verified=false; const auto began=GetTickCount64();
            const auto result=f.run([&](MigrationStage stage,const auto&){copied|=stage==MigrationStage::Copy; verified|=stage==MigrationStage::Verify;},{true});
            if(!result.committed) describe(result);
            require(result.committed && result.moved_folders==1 && result.copies.empty() && !copied && !verified,"same volume uses folder rename without per-file copy/hash stages");
            const std::filesystem::path destination=result.settings.drawers[0].folder;
            require(result.settings.drawers[0].folder_identity==identity && folder_available(result.settings.drawers[0]) && !std::filesystem::exists(f.old()),"same volume preserves folder identity");
            require(read(destination/L"node_modules"/L"2047.txt")=="2047" && read(destination.wstring()+L":edgetuck-test")=="folder stream","whole tree and directory streams retained");
            std::cout<<"Same-volume migration with 2048 small files: "<<GetTickCount64()-began<<" ms\n";
        }
        {
            Fixture f(root,L"fast-collision"); const auto existing=f.target/f.old().filename(); std::filesystem::create_directory(existing); write(existing/L"keep.txt","existing copy");
            const auto result=f.run({},{true});
            require(result.committed && std::filesystem::path(result.settings.drawers[0].folder)!=existing && read(existing/L"keep.txt")=="existing copy","existing destination never overwritten, merged or adopted");
        }
        {
            Fixture f(root,L"fast-cancel"); const auto original=read(f.config);
            auto result=f.run([&](MigrationStage stage,const auto&){if(stage==MigrationStage::Moved) f.cancel=true;},{true});
            require(!result.committed && !result.recovery_required && result.moved_folders==0 && folder_available(f.settings.drawers[0]) && read(f.config)==original && std::filesystem::is_empty(f.target),"cancel after rename restores original folder and configuration");
        }
        {
            Fixture f(root,L"fast-save-failure"); const auto original=read(f.config);
            const auto result=f.run([&](MigrationStage stage,const auto&){if(stage==MigrationStage::Commit) SetFileAttributesW(f.config.c_str(),FILE_ATTRIBUTE_READONLY);},{true});
            SetFileAttributesW(f.config.c_str(),FILE_ATTRIBUTE_NORMAL);
            require(!result.committed && !result.recovery_required && folder_available(f.settings.drawers[0]) && read(f.config)==original && std::filesystem::is_empty(f.target),"failed configuration commit rolls back completed rename");
        }
        {
            Fixture f(root,L"fast-rollback-conflict");
            const auto result=f.run([&](MigrationStage stage,const auto&){if(stage==MigrationStage::Moved){std::filesystem::create_directory(f.old()); write(f.old()/L"new.txt","new user data"); f.cancel=true;}},{true});
            require(!result.committed && result.recovery_required && read(f.old()/L"new.txt")=="new user data" && read(f.target/f.old().filename()/L"资料.txt")=="original","rollback conflict preserves both original and newly created data");
            std::wstring message; require(!recover_storage(f.config,message),"recovery refuses to overwrite conflicting folder");
            std::filesystem::remove(f.old()/L"new.txt"); std::filesystem::remove(f.old());
            require(recover_storage(f.config,message) && folder_available(f.settings.drawers[0]),"recovery succeeds after owned collision fixture removed");
        }
        {
            Fixture f(root,L"fast-multiple-cancel"); std::wstring error;
            DrawerModel second{2,L"第二个",Edge::Left,1,4,5,{}};
            require(bind_new_folder(second,f.source,error),"second fast-move fixture"); write(std::filesystem::path(second.folder)/L"keep.txt","second");
            f.settings.drawers.push_back(second); require(save_settings(f.config,f.settings,error),"save multiple fast-move drawers");
            int moved=0; const auto result=f.run([&](MigrationStage stage,const auto&){if(stage==MigrationStage::Moved && ++moved==2) f.cancel=true;},{true});
            require(moved==2 && !result.committed && !result.recovery_required && std::filesystem::is_empty(f.target),"cancel rolls back multiple renamed folders");
            for(const auto& d:f.settings.drawers) require(folder_available(d),"original identities restored after multi-folder rollback");
        }
        {
            Fixture f(root,L"fast-identity-change");
            const auto result=f.run([&](MigrationStage stage,const auto&){if(stage==MigrationStage::Move){std::filesystem::rename(f.old(),f.source/L"real-original");std::filesystem::create_directory(f.old());write(f.old()/L"keep.txt","replacement");}},{true});
            require(!result.committed && read(f.old()/L"keep.txt")=="replacement" && read(f.source/L"real-original"/L"资料.txt")=="original" && std::filesystem::is_empty(f.target),"source replaced after planning is never moved or overwritten");
        }
        for(const bool committed:{false,true}) {
            Fixture f(root,committed?L"crash-committed":L"crash-moved"); const auto original=read(f.config); crash_child(f,committed);
            require(!std::filesystem::exists(f.old()),"process crash leaves moved directory for recovery");
            std::wstring message; require(recover_storage(f.config,message),"interrupted migration recovers from durable plan");
            Settings loaded; require(load_settings(f.config,loaded,message) && folder_available(loaded.drawers[0]),"recovered binding matches surviving original directory");
            require(committed?std::filesystem::path(loaded.drawers[0].folder).parent_path()==f.target:read(f.config)==original && folder_available(f.settings.drawers[0]),"commit boundary determines keep-new versus restore-old recovery");
            require(recover_storage(f.config,message),"completed recovery is idempotent");
        }
        std::cout<<"Storage migration: PASS; "<<checks<<" checks; copy/hash/streams/recycle/collision/roundtrip/cancel/edit/save-failure/identity/fast-move/crash-recovery\n";
        // The path was exclusively created above, has no external input, and all
        // test files (including recycle candidates) are inside that owned root.
        require(std::filesystem::equivalent(root.parent_path(),std::filesystem::temp_directory_path()) && root.filename().wstring().starts_with(L"EdgeTuck-storage-test-"),"cleanup stays within owned fixture");
        for(const auto& item:std::filesystem::recursive_directory_iterator(root)) if(item.is_regular_file()) SetFileAttributesW(item.path().c_str(),FILE_ATTRIBUTE_NORMAL);
        std::filesystem::remove_all(root); OleUninitialize(); return 0;
    } catch(const std::exception& e) { std::cerr<<"FAIL: "<<e.what()<<"; owned fixtures retained\n"; OleUninitialize(); return 1; }
}
