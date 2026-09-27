#include "references.hpp"
#include "shell_files.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace edge;
static int checks{};
static void require(bool okay,const char* reason) { ++checks; if(!okay) throw std::runtime_error(reason); }
static std::vector<FileChange> events;
static LRESULT CALLBACK proc(HWND hwnd,UINT message,WPARAM wp,LPARAM lp) {
    if(message==WM_APP+1) { events.push_back(FileWatch::read(wp,lp)); return 0; }
    return DefWindowProcW(hwnd,message,wp,lp);
}
static bool wait_rename(const std::wstring& from,const std::wstring& to) {
    const auto until=GetTickCount64()+5000;
    do {
        MSG m{}; while(PeekMessageW(&m,nullptr,0,0,PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
        for(const auto& e:events) if((e.event&(SHCNE_RENAMEITEM|SHCNE_RENAMEFOLDER)) && same_path(e.from,from) && same_path(e.to,to)) return true;
        MsgWaitForMultipleObjects(0,nullptr,FALSE,50,QS_ALLINPUT);
    } while(GetTickCount64()<until);
    return false;
}
int main() {
    OleInitialize(nullptr); HWND window{}; std::filesystem::path root;
    try {
        Settings settings; settings.drawers={{1,L"one",Edge::Left,0,3,3,{L"C:\\fixture\\a.txt",L"C:\\fixture\\b.txt",L"C:\\fixture\\c.txt"}},
            {2,L"two",Edge::Right,0,3,3,{L"C:\\fixture\\d.txt"}}};
        require(place_references(settings,2,{L"C:\\fixture\\a.txt",L"C:\\fixture\\c.txt"},1,0),"cross drawer metadata transfer");
        require(settings.drawers[0].items==std::vector<std::wstring>{L"C:\\fixture\\b.txt"},"only transferred references removed from source");
        require(settings.drawers[1].items==std::vector<std::wstring>{L"C:\\fixture\\a.txt",L"C:\\fixture\\c.txt",L"C:\\fixture\\d.txt"},"insertion order preserved");
        require(place_references(settings,2,{L"C:\\fixture\\d.txt"},2,0),"same drawer reorder");
        require(settings.drawers[1].items[0]==L"C:\\fixture\\d.txt","reorder uses target slot");
        require(!place_references(settings,2,{L"relative.txt"}),"relative paths rejected");
        require(!place_references(settings,2,{L"C:\\fixture\\nonmember.txt"},1),"source ownership required");
        require(place_references(settings,2,{L"C:\\fixture\\A.txt"}),"case insensitive reference");
        require(settings.drawers[1].items.size()==3,"case duplicate not added");
        require(rename_references(settings,L"C:\\fixture",L"C:\\renamed"),"folder rename updates descendants");
        require(!path_within(L"C:\\fixture2\\file",L"C:\\fixture"),"sibling prefix is not descendant");
        settings.drawers[1].items.clear();
        for(int i=0;i<1000;++i) settings.drawers[1].items.push_back(L"C:\\full\\"+std::to_wstring(i));
        const auto source=settings.drawers[0].items;
        require(!place_references(settings,2,source,1),"full destination rejects transfer");
        require(settings.drawers[0].items==source && settings.drawers[1].items.size()==1000,"failure rolls back both sides");
        require(remove_references(settings,1,source) && settings.drawers[0].items.empty(),"remove references is metadata only");

        root=std::filesystem::temp_directory_path()/(L"EdgeTuck-file-actions-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
        require(std::filesystem::create_directory(root),"create test-owned directory");
        DWORD error{};
        const auto folder=create_desktop_item(root,true,error);
        const auto a=create_desktop_item(root,false,error);
        { std::ofstream out(a,std::ios::binary); out<<"preserve these bytes"; }
        const DWORD attributes=GetFileAttributesW(a.c_str());
        const auto b=create_desktop_item(root,false,error);
        require(!a.empty() && !b.empty() && a!=b && std::filesystem::file_size(a)==20,"new item never overwrites existing file");
        require(GetFileAttributesW(a.c_str())==attributes,"reference operations do not change attributes");
        require(!create_desktop_item(root/L"absent",false,error).has_filename() && error!=0,"missing parent fails safely");
        auto data=file_data({a.wstring(),b.wstring()});
        require(data && data_paths(data.Get())==std::vector<std::wstring>{a.wstring(),b.wstring()},"native data object roundtrip");
        const auto nested=create_desktop_item(folder,false,error);
        require(data_paths(file_data({a.wstring(),nested.wstring()}).Get()).size()==2,"mixed parent native drag payload");
        HMENU popup=CreatePopupMenu(); ShellMenu menu;
        require(menu.fill(nullptr,popup,{a.wstring()}),"obtain real file Shell context menu");
        bool copy=false,rename=false,properties=false;
        for(UINT cmd=1;cmd<=0x200;++cmd) { const auto verb=menu.verb(cmd); copy|=verb==L"copy"; rename|=verb==L"rename"; properties|=verb==L"properties"; }
        require(copy && rename && properties,"system copy rename properties verbs retained"); DestroyMenu(popup);
        WNDCLASSW wc{}; wc.hInstance=GetModuleHandleW(nullptr); wc.lpszClassName=L"EdgeTuck.FileWatchTest"; wc.lpfnWndProc=proc; RegisterClassW(&wc);
        window=CreateWindowW(wc.lpszClassName,L"",0,0,0,0,0,HWND_MESSAGE,nullptr,wc.hInstance,nullptr);
        require(window!=nullptr,"notification window");
        FileWatch watch; watch.update(window,WM_APP+1,{a.wstring(),nested.wstring()});
        const auto renamed=root/L"renamed.txt";
        require(SUCCEEDED(rename_file(window,a.wstring(),renamed.filename().wstring())),"native file rename");
        require(wait_rename(a.wstring(),renamed.wstring()),"event-driven rename notification");
        require(std::filesystem::file_size(renamed)==20 && GetFileAttributesW(renamed.c_str())==attributes,"rename preserves data and attributes");
        require(FAILED(rename_file(window,renamed.wstring(),L"../escape.txt")),"invalid rename cannot escape parent");
        const auto moved_folder=root/L"renamed-folder";
        require(SUCCEEDED(rename_file(window,folder.wstring(),moved_folder.filename().wstring())),"folder rename");
        require(wait_rename(folder.wstring(),moved_folder.wstring()),"parent rename notification received");
        watch.clear(); DestroyWindow(window); window=nullptr;
        std::filesystem::remove_all(root); root.clear();
        std::cout<<"File actions: PASS; "<<checks<<" checks; atomic transfers, native menu/data object, collision-safe creation, Shell rename notifications, original bytes/attributes\n";
        OleUninitialize(); return 0;
    } catch(const std::exception& e) {
        if(window) DestroyWindow(window);
        // Leave failing fixtures for inspection. Never clean a user-owned path.
        std::cerr<<"FAIL: "<<e.what()<<"; fixtures="<<root.string()<<'\n'; OleUninitialize(); return 1;
    }
}
