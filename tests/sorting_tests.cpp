#include "sorting.hpp"
#include "references.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>

static int checks{};
static void require(bool value,const char* message) { ++checks; if(!value) throw std::runtime_error(message); }
int wmain(int argc,wchar_t** argv) {
    using namespace edge;
    try {
        if(argc==4 && std::wstring(argv[1])==L"--verify-upgrade") {
            Settings before,after; std::wstring error;
            require(load_settings(argv[2],before,error) && load_settings(argv[3],after,error),"both profile snapshots load");
            const auto root=std::filesystem::temp_directory_path()/(L"EdgeTuck-Profile-Compare-"+std::to_wstring(GetCurrentProcessId()));
            require(std::filesystem::create_directory(root),"new comparison fixture");
            const auto a=root/L"before.dat",b=root/L"after.dat";
            require(save_settings(a,before,error) && save_settings(b,after,error),"normalize snapshots using current serializer");
            std::string first,second;
            { std::ifstream f(a,std::ios::binary),g(b,std::ios::binary); first.assign(std::istreambuf_iterator<char>(f),{}); second.assign(std::istreambuf_iterator<char>(g),{}); }
            require(first==second,"profile bindings, items, order, appearance and preferences remain identical");
            std::filesystem::remove(a); std::filesystem::remove(b); std::filesystem::remove(root);
            std::cout<<"PASS: profile semantics unchanged after format upgrade; "<<before.drawers.size()<<" drawers\n"; return 0;
        }
        const std::wstring a=L"C:\\sort\\a2.txt",b=L"C:\\sort\\a10.txt",c=L"C:\\sort\\z.bin",folder=L"C:\\sort\\Folder",missing=L"C:\\sort\\missing.txt";
        std::vector<SortEntry> entries{{b,true,false,10,100,3},{a,true,false,2,300,0},{c,true,false,4,200,9},{folder,true,true,0,250,0},{missing,false,false,0,0,0}};
        require(sorted_paths(SortMode::Name,false,entries)==std::vector<std::wstring>{folder,a,b,c,missing},"natural names, folders first, unavailable last");
        require(sorted_paths(SortMode::Name,true,entries)==std::vector<std::wstring>{folder,c,b,a,missing},"descending names");
        require(sorted_paths(SortMode::Size,false,entries)==std::vector<std::wstring>{folder,a,c,b,missing},"ascending file sizes without recursive folder sizes");
        require(sorted_paths(SortMode::Size,true,entries)==std::vector<std::wstring>{folder,b,c,a,missing},"descending file sizes");
        require(sorted_paths(SortMode::Modified,true,entries)==std::vector<std::wstring>{folder,a,c,b,missing},"newest modification first");
        require(sorted_paths(SortMode::Type,false,entries)==std::vector<std::wstring>{folder,c,a,b,missing},"extension groups and natural name tie break");
        require(sorted_paths(SortMode::Recent,false,entries)==std::vector<std::wstring>{c,b,a,folder,missing},"recent always newest first, unused retain order, no folders override");
        require(sorted_paths(SortMode::Manual,false,entries)==std::vector<std::wstring>{b,a,c,folder,missing},"manual order unchanged");
        DrawerModel drawer; drawer.id=1; drawer.name=L"test"; drawer.items={a,b};
        require(!record_recent_use(drawer,c,5),"unlisted paths are not recorded");
        require(record_recent_use(drawer,b,5) && record_recent_use(drawer,a,3),"valid usage recorded");
        require(recent_use(drawer,a)>recent_use(drawer,b),"clock rollback does not reorder actual launch chronology");
        require(record_recent_use(drawer,L"c:\\SORT\\A2.TXT",7) && drawer.recent_uses.size()==2,"case-insensitive usage is one record");
        require(drawer.items==std::vector<std::wstring>{a,b},"recording usage never changes visible order");
        Settings settings; settings.drawers={drawer};
        require(rename_references(settings,a,L"C:\\sort\\renamed.txt"),"reference rename");
        require(recent_use(settings.drawers[0],L"C:\\sort\\renamed.txt")>0,"rename retains usage");
        remove_references(settings,1,{b}); prune_recent_uses(settings.drawers[0]);
        require(settings.drawers[0].recent_uses.size()==1,"deleted references do not retain stale history");
        const auto root=std::filesystem::temp_directory_path()/(L"EdgeTuck-Sorting-"+std::to_wstring(GetCurrentProcessId()));
        require(std::filesystem::create_directory(root),"test directory must be newly owned");
        const auto small=root/L"small.txt",large=root/L"large.txt",config=root/L"settings.dat",old=root/L"old.dat";
        { std::ofstream file(small); file<<"ab"; } { std::ofstream file(large); file<<"1234567890"; }
        drawer.items={small.wstring(),large.wstring()}; drawer.sort=SortMode::Size; drawer.sort_descending=true; drawer.recent_uses.clear();
        SortQueue queue; queue.request(drawer,42,nullptr,0);
        std::vector<SortResult> ready;
        for(int attempts=0;attempts<2000 && ready.empty();++attempts) { ready=queue.take(); if(ready.empty()) Sleep(1); }
        require(ready.size()==1 && ready[0].ticket==42 && ready[0].before==drawer.items && ready[0].after==std::vector<std::wstring>{large.wstring(),small.wstring()},"background metadata result retains request identity and size order");
        queue.stop();
        drawer.sort=SortMode::Recent; record_recent_use(drawer,large.wstring(),100);
        settings.drawers={drawer}; std::wstring error;
        require(save_settings(config,settings,error),"save v7 sorting settings");
        Settings loaded; require(load_settings(config,loaded,error),"load v7 sorting settings");
        require(loaded.drawers[0].sort==SortMode::Recent && loaded.drawers[0].sort_descending && recent_use(loaded.drawers[0],large.wstring())==100,"sort and usage survive restart");
        { std::ofstream file(old,std::ios::binary); file<<"EDGETUCK 6\n0 1 1 0 0 1\n1.2 18 .75 .55 1 1 1\n1\n0\n1 \"\"\n1 1 0 4 5 \"legacy\" 2\n\"C:\\\\sort\\\\a10.txt\"\n\"C:\\\\sort\\\\a2.txt\"\n\"\" \"\" 0 0 0 0\n"; }
        require(load_settings(old,loaded,error) && error.empty(),"v6 profile remains readable");
        require(loaded.drawers[0].sort==SortMode::Manual && loaded.drawers[0].recent_uses.empty() && loaded.drawers[0].items==std::vector<std::wstring>{b,a},"upgrade preserves old order without inventing usage");
        { std::ofstream file(old,std::ios::binary); file<<"EDGETUCK 7\n0 1 1 0 0 1\n1.2 18 .75 .55 1 1 1\n1\n0\n1 \"\"\n1 1 0 4 5 \"bad\" 0\n\"\" \"\" 0 0 0 0\n99 0\n"; }
        require(!load_settings(old,loaded,error),"invalid sort mode rejected");
        for(const auto& path:{small,large,config,old}) std::filesystem::remove(path);
        require(std::filesystem::remove(root),"only owned empty fixture removed");
        std::cout<<"PASS: "<<checks<<" sorting checks\n"; return 0;
    } catch(const std::exception& error) { std::cerr<<"FAIL: "<<error.what()<<'\n'; return 1; }
}
