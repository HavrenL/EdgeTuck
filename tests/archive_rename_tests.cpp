#include "archive_rename.hpp"
#include "sorting.hpp"
#include <ole2.h>
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace edge;
static int checks{};
static void require(bool value,const char* message) {++checks; if(!value) throw std::runtime_error(message);}
static void write(const std::filesystem::path& path,const char* text) {std::ofstream out(path,std::ios::binary); out<<text;}
static std::string bytes(const std::filesystem::path& path) {std::ifstream in(path,std::ios::binary); return {std::istreambuf_iterator<char>(in),{}};}
struct Fixture {
    std::filesystem::path root,config; Settings settings;
    Fixture(const std::filesystem::path& parent,const wchar_t* name):root(parent/name),config(root/L"settings.dat") {
        require(std::filesystem::create_directory(root),"owned case directory");
        DrawerModel drawer{1,L"旧分类",Edge::Right,0,4,5,{}}; std::wstring error;
        require(bind_new_folder(drawer,root/L"data",error),"owned bound folder");
        const auto file=std::filesystem::path(drawer.folder)/L"keep.txt"; write(file,"original data");
        drawer.items={file.wstring()}; drawer.recent_uses={{file.wstring(),100}};
        drawer.legacy_items={(root/L"external.txt").wstring()}; write(root/L"external.txt","external");
        settings.drawers={drawer}; require(save_settings(config,settings,error),"initial durable config");
    }
};
int main() {
    OleInitialize(nullptr);
    try {
        const auto root=std::filesystem::temp_directory_path()/(L"EdgeTuck-Rename-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
        require(std::filesystem::create_directory(root),"exclusive test root");
        require(archive_folder_name(L"个人/杂项")==L"ET_个人_杂项","Windows-invalid separators mapped consistently");
        require(archive_folder_name(L"分类. ")==L"ET_分类","trailing dot and space removed");
        {
            Fixture f(root,L"english"); const auto original=f.settings.drawers[0];
            auto result=rename_archive_folder(f.settings,1,L"Development",f.config);
            const auto& drawer=result.settings.drawers[0];
            require(result.saved && drawer.english_folder && drawer.name==original.name && std::filesystem::path(drawer.folder).filename()==L"ETDevelopment","explicit English conversion preserves display title");
            require(drawer.folder_identity==original.folder_identity && bytes(std::filesystem::path(drawer.folder)/L"keep.txt")=="original data" && recent_use(drawer,drawer.items[0])==100,"conversion retains identity, contents and usage paths");
            Settings loaded; std::wstring error;
            require(load_settings(f.config,loaded,error) && loaded.drawers[0].english_folder && loaded.drawers[0].folder==drawer.folder,"independent naming persists across restart");
            const auto handle=CreateFileW(drawer.folder.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,nullptr);
            require(handle!=INVALID_HANDLE_VALUE,"hold English directory against renaming");
            auto titled=rename_archive(loaded,1,L"开发工具",f.config); CloseHandle(handle);
            require(titled.saved && titled.settings.drawers[0].folder==drawer.folder && titled.settings.drawers[0].name==L"开发工具","title edit succeeds while directory is locked against physical renaming");
            auto offline=titled.settings.drawers[0]; const auto target=std::filesystem::path(offline.folder).parent_path()/L"ETProjects";
            require(MoveFileW(offline.folder.c_str(),target.c_str())!=FALSE && reconnect_folder(offline,target.parent_path()) && offline.name==L"开发工具" && offline.english_folder,"external directory rename reconnects by identity without replacing independent title");
        }
        {
            Fixture f(root,L"english-collision"); const auto target=std::filesystem::path(f.settings.drawers[0].folder).parent_path()/L"ETDevelopment";
            std::filesystem::create_directory(target); write(target/L"keep.txt","other directory"); const auto config=bytes(f.config);
            auto result=rename_archive_folder(f.settings,1,L"Development",f.config);
            require(!result.saved && !result.settings.drawers[0].english_folder && folder_available(f.settings.drawers[0]) && bytes(f.config)==config && bytes(target/L"keep.txt")=="other directory","English conversion collision preserves both folders and mode");
        }
        {
            Fixture f(root,L"english-save-failure"); const auto original=bytes(f.config);
            SetFileAttributesW(f.config.c_str(),FILE_ATTRIBUTE_READONLY);
            auto result=rename_archive_folder(f.settings,1,L"Development",f.config);
            SetFileAttributesW(f.config.c_str(),FILE_ATTRIBUTE_NORMAL);
            require(!result.saved && !result.recovery_required && !result.settings.drawers[0].english_folder && folder_available(f.settings.drawers[0]) && bytes(f.config)==original,"failed English conversion save rolls physical path back and retains legacy mode");
        }
        {
            Fixture f(root,L"invalid-English"); const auto config=bytes(f.config);
            for(const auto* alias:{L"",L"开发",L"Dev_Tools",L"Dev-Tools",L"Dev Tools",L"../Work",L"1Work"}) {
                auto result=rename_archive_folder(f.settings,1,alias,f.config);
                require(!result.saved && folder_available(f.settings.drawers[0]) && bytes(f.config)==config,"invalid English conversion cannot change folder or config");
            }
        }
        {
            Fixture f(root,L"success"); const auto original=f.settings.drawers[0];
            auto result=rename_archive(f.settings,1,L"常用",f.config);
            const auto& d=result.settings.drawers[0];
            require(result.saved && !result.recovery_required && d.name==L"常用" && std::filesystem::path(d.folder).filename()==L"ET_常用","display and physical names committed together");
            require(d.folder_identity==original.folder_identity && folder_available(d) && !std::filesystem::exists(original.folder),"same directory identity, original path gone");
            require(bytes(std::filesystem::path(d.folder)/L"keep.txt")=="original data" && recent_use(d,d.items[0])==100,"file contents, references and usage preserved");
            require(d.legacy_items==original.legacy_items && bytes(f.root/L"external.txt")=="external","external references untouched");
            Settings loaded; std::wstring error; require(load_settings(f.config,loaded,error) && loaded.drawers[0].folder==d.folder && loaded.drawers[0].name==d.name,"names and binding survive reload");
            auto again=rename_archive(result.settings,1,L"常用",f.config); require(again.saved && folder_available(again.settings.drawers[0]),"same name harmless");
        }
        {
            Fixture f(root,L"old-alias"); f.settings.drawers[0].name=L"常用"; std::wstring error; require(save_settings(f.config,f.settings,error),"old alias config");
            auto result=rename_archive(f.settings,1,L"常用",f.config);
            require(result.saved && std::filesystem::path(result.settings.drawers[0].folder).filename()==L"ET_常用","same display name repairs old folder mismatch");
        }
        {
            Fixture f(root,L"collision"); const auto target=std::filesystem::path(f.settings.drawers[0].folder).parent_path()/L"ET_常用";
            std::filesystem::create_directory(target); write(target/L"keep.txt","other directory"); const auto config=bytes(f.config);
            auto result=rename_archive(f.settings,1,L"常用",f.config);
            require(!result.saved && !result.recovery_required && folder_available(f.settings.drawers[0]) && bytes(f.config)==config,"collision retains name and config");
            require(bytes(target/L"keep.txt")=="other directory","no merge or overwrite");
        }
        {
            Fixture f(root,L"save-failure"); const auto original=bytes(f.config);
            SetFileAttributesW(f.config.c_str(),FILE_ATTRIBUTE_READONLY);
            auto result=rename_archive(f.settings,1,L"常用",f.config);
            SetFileAttributesW(f.config.c_str(),FILE_ATTRIBUTE_NORMAL);
            require(!result.saved && !result.recovery_required && folder_available(f.settings.drawers[0]) && bytes(f.config)==original,"failed save rolls folder back and retains original config");
        }
        {
            Fixture f(root,L"locked"); const auto& d=f.settings.drawers[0];
            // Attribute-only handles do not impose a delete-sharing conflict on
            // this system; a directory read handle models a real blocking user.
            const auto handle=CreateFileW(d.folder.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,nullptr);
            require(handle!=INVALID_HANDLE_VALUE,"hold rename-denying handle");
            auto result=rename_archive(f.settings,1,L"常用",f.config); CloseHandle(handle);
            require(!result.saved && folder_available(d) && result.settings.drawers[0].name==d.name,"locked folder never leaves display-only rename");
        }
        {
            Fixture f(root,L"identity-mismatch"); const auto& d=f.settings.drawers[0];
            const auto moved=std::filesystem::path(d.folder).parent_path()/L"original";
            require(MoveFileW(d.folder.c_str(),moved.c_str())!=FALSE,"relocate identity fixture");
            std::filesystem::create_directory(d.folder); write(std::filesystem::path(d.folder)/L"keep.txt","replacement");
            const auto config=bytes(f.config); auto result=rename_archive(f.settings,1,L"常用",f.config);
            require(!result.saved && bytes(f.config)==config && bytes(moved/L"keep.txt")=="original data" &&
                bytes(std::filesystem::path(d.folder)/L"keep.txt")=="replacement","a replacement folder is never renamed or claimed");
        }
        {
            Fixture f(root,L"external"); auto& d=f.settings.drawers[0]; const auto target=std::filesystem::path(d.folder).parent_path()/L"ET_资源管理器改名";
            require(MoveFileW(d.folder.c_str(),target.c_str())!=FALSE,"external rename fixture");
            Settings recovered; std::wstring error;
            require(load_settings(f.config,recovered,error) && reconnect_folder(recovered.drawers[0],target.parent_path()) &&
                folder_available(recovered.drawers[0]) && recovered.drawers[0].name==L"资源管理器改名","restart between directory rename and config save reconnects the original identity");
            require(reconnect_folder(d,target.parent_path()) && d.name==L"资源管理器改名" && recent_use(d,d.items[0])==100,"offline rename adopts title by identity and rewrites usage");
            const auto next=target.parent_path()/L"自定名称";
            require(MoveFileW(d.folder.c_str(),next.c_str())!=FALSE && rebind_folder(d,next) && d.name==L"自定名称","online rename without ET prefix updates title");
        }
        {
            Fixture f(root,L"pretty-title"); auto result=rename_archive(f.settings,1,L"个人/杂项",f.config); require(result.saved,"escaped-title rename");
            const auto& d=result.settings.drawers[0]; require(d.name==L"个人/杂项" && std::filesystem::path(d.folder).filename()==L"ET_个人_杂项","title preserves punctuation while folder uses valid name");
        }
        {
            Fixture f(root,L"case-only"); auto a=rename_archive(f.settings,1,L"demo",f.config); require(a.saved,"initial Latin name");
            auto b=rename_archive(a.settings,1,L"Demo",f.config);
            require(b.saved && b.settings.drawers[0].name==L"Demo" && std::filesystem::path(b.settings.drawers[0].folder).filename()==L"ET_Demo","case-only rename commits title and path spelling");
            bool exact=false; for(const auto& entry:std::filesystem::directory_iterator(f.root/L"data")) exact|=entry.path().filename()==L"ET_Demo";
            require(exact,"filesystem reports new case");
        }
        require(std::filesystem::equivalent(root.parent_path(),std::filesystem::temp_directory_path()) && root.filename().wstring().starts_with(L"EdgeTuck-Rename-"),"cleanup constrained to owned fixture");
        std::filesystem::remove_all(root);
        std::cout<<"Archive rename: PASS; "<<checks<<" checks\n"; OleUninitialize(); return 0;
    } catch(const std::exception& e) {std::cerr<<"FAIL: "<<e.what()<<"; owned fixtures retained\n"; OleUninitialize(); return 1;}
}
