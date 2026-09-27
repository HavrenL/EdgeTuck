#include "archive.hpp"
#include "references.hpp"
#include "shell_files.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace edge;
static int checks{};
static void require(bool okay,const char* message) { ++checks; if(!okay) throw std::runtime_error(message); }
static void write(const std::filesystem::path& path,const char* value) { std::ofstream file(path,std::ios::binary); file<<value; }
static std::string read(const std::filesystem::path& path) { std::ifstream file(path,std::ios::binary); return {std::istreambuf_iterator<char>(file),{}}; }
int main() {
    OleInitialize(nullptr); std::filesystem::path root;
    try {
        root=std::filesystem::temp_directory_path()/(L"EdgeTuck-archive-core-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
        require(std::filesystem::create_directory(root),"owned fixture created");
        DrawerModel a{1,L"工作",Edge::Right,0,4,5,{}},b{2,L"工作",Edge::Left,0,4,5,{}};
        std::wstring error; const auto original=root/L"报告.txt"; write(original,"original");
        a.items={original.wstring()};
        require(bind_new_folder(a,root,error) && bind_new_folder(b,root,error),"create real folders");
        require(a.folder!=b.folder && std::filesystem::path(a.folder).filename()==L"ET_工作","unique prefixed names without claiming existing directory");
        require(b.name==L"工作 (2)" && std::filesystem::path(b.folder).filename()==archive_folder_name(b.name),"new collision suffix appears in both drawer and folder names");
        require(a.legacy_items==a.items && read(original)=="original","migration retains references without moving files");
        require(folder_available(a),"binding has filesystem identity");
        const DWORD attributes=GetFileAttributesW(original.c_str());
        auto moved=transfer_files(nullptr,{original.wstring()},a.folder);
        require(SUCCEEDED(moved.status) && !moved.aborted && moved.completed.size()==1,"native archive succeeds");
        const auto archived=std::filesystem::path(moved.completed[0].to);
        require(!std::filesystem::exists(original) && archived.parent_path()==a.folder && read(archived)=="original","actual file moved into drawer with bytes preserved");
        require(GetFileAttributesW(archived.c_str())==attributes,"archive does not set hidden or system attributes");
        write(original,"collision"); moved=transfer_files(nullptr,{original.wstring()},a.folder);
        require(SUCCEEDED(moved.status) && moved.completed.size()==1 && moved.completed[0].to!=archived,"collision keeps both files");
        require(read(archived)=="original" && read(moved.completed[0].to)=="collision","neither same-name file overwritten");
        a.legacy_items.clear(); refresh_folder(a,error);
        require(a.items.size()==2,"folder enumerates actual files");
        auto copied=transfer_files(nullptr,{archived.wstring()},b.folder,true);
        require(copied.completed.size()==1 && std::filesystem::exists(archived) && read(copied.completed[0].to)=="original","copy retains source");
        auto across=transfer_files(nullptr,{archived.wstring()},b.folder);
        require(across.completed.size()==1 && !std::filesystem::exists(archived) && across.completed[0].to!=copied.completed[0].to,"cross drawer move handles destination collision");
        auto returned=transfer_files(nullptr,{across.completed[0].to},root);
        require(returned.completed.size()==1 && std::filesystem::path(returned.completed[0].to).parent_path()==root && read(returned.completed[0].to)=="original","return to desktop root");
        auto invalid=transfer_files(nullptr,{a.folder},b.folder,false,{a.folder,b.folder});
        require(FAILED(invalid.status) && folder_available(a),"managed directory cannot be nested in another drawer");
        invalid=transfer_files(nullptr,{root.wstring()},a.folder);
        require(FAILED(invalid.status),"cannot move parent into its child");
        const auto absent=root/L"absent.txt",valid=root/L"valid.txt"; write(valid,"valid");
        invalid=transfer_files(nullptr,{valid.wstring(),absent.wstring()},a.folder);
        require(FAILED(invalid.status) && std::filesystem::exists(valid),"preflight failure leaves entire batch untouched");
        DWORD code{}; const auto nested=create_desktop_item(a.folder,true,code); write(nested/L"inside.txt","nested");
        const auto same=std::filesystem::path(b.folder)/nested.filename(); std::filesystem::create_directory(same); write(same/L"inside.txt","existing");
        auto folder_move=transfer_files(nullptr,{nested.wstring()},b.folder);
        require(folder_move.completed.size()==1 && folder_move.completed[0].to!=same && read(same/L"inside.txt")=="existing","same-name folders are not merged");
        require(read(std::filesystem::path(folder_move.completed[0].to)/L"inside.txt")=="nested","moved directory keeps nested contents");
        const auto old=a.folder,renamed=(root/L"用户改名").wstring();
        require(MoveFileW(old.c_str(),renamed.c_str())!=FALSE,"external rename fixture");
        require(reconnect_folder(a,root) && a.folder==renamed && folder_available(a),"reconnect by identity after rename while app closed");
        const auto identity=a.folder_identity;
        MoveFileW(a.folder.c_str(),(root/L"relocated").c_str()); CreateDirectoryW(a.folder.c_str(),nullptr);
        require(!folder_available(a) && !bind_new_folder(a,root,error) && a.folder_identity==identity,"replacement directory not silently claimed");
        require(reconnect_folder(a,root),"original directory rediscovered independent of prefix");
        Settings settings; settings.drawers={a,b}; a.restore_position=true;
        settings.drawers[0].restore_position=true; settings.drawers[0].restore_x=17; settings.drawers[0].restore_y=912;
        settings.drawers[0].legacy_items={valid.wstring()};
        const auto config=root/L"config.dat"; require(save_settings(config,settings,error),"version 4 save");
        Settings loaded; require(load_settings(config,loaded,error),"version 4 reload");
        require(loaded.drawers[0].folder==a.folder && loaded.drawers[0].folder_identity==a.folder_identity && loaded.drawers[0].restore_x==17 && loaded.drawers[0].restore_y==912 && loaded.drawers[0].legacy_items==std::vector<std::wstring>{valid.wstring()},"mapping, recovery coordinates and legacy references survive restart");
        std::cout<<"Archive core: PASS; "<<checks<<" checks; movement, copy, collisions, folder identity, safe migration, persistence\n";
        // root was exclusively created above; no user directory is accepted as input.
        std::filesystem::remove_all(root); OleUninitialize(); return 0;
    } catch(const std::exception& e) { std::cerr<<"FAIL: "<<e.what()<<"; owned fixtures retained\n"; OleUninitialize(); return 1; }
}
