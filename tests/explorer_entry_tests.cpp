#include "explorer_entry.hpp"
#include <iostream>
#include <stdexcept>
#include <fstream>
using namespace edge;
static int checks{};
static void require(bool value,const char* message){++checks; if(!value) throw std::runtime_error(message);}
int main() {
    HKEY root{},foreign{}; bool owned{};
    const auto key=L"Software\\EdgeTuck\\Tests\\Explorer-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64());
    const auto fixture=std::filesystem::temp_directory_path()/(L"EdgeTuck-Explorer-"+std::to_wstring(GetCurrentProcessId()));
    try {
        DWORD disposition{};
        require(RegCreateKeyExW(HKEY_CURRENT_USER,key.c_str(),0,nullptr,0,KEY_ALL_ACCESS,nullptr,&root,&disposition)==ERROR_SUCCESS && disposition==REG_CREATED_NEW_KEY,"new isolated registry root"); owned=true;
        require(std::filesystem::create_directory(fixture),"new owned file fixture");
        const auto file=fixture/L"keep.txt"; {std::ofstream out(file); out<<"preserve";}
        const std::wstring exe=L"C:\\Program Files\\轻屉\\EdgeTuck.exe";
        auto status=read_explorer_entry(root,0);
        require(!status.error && !status.enabled,"default off");
        require(write_explorer_entry(root,0,true,L"relative",exe)==ERROR_INVALID_PARAMETER,"relative root rejected");
        require(write_explorer_entry(root,0,true,fixture,L"C:\\bad\"file.exe")==ERROR_INVALID_PARAMETER,"malformed icon path rejected");
        require(write_explorer_entry(root,0,true,fixture,exe)==ERROR_SUCCESS,"register in test-only registry subtree");
        status=read_explorer_entry(root,0);
        require(status.enabled && !status.error && status.target==fixture.wstring() && status.icon==L"\""+exe+L"\",-1","folder target and Unicode icon round trip");
        const auto cls=std::wstring(L"Software\\Classes\\CLSID\\")+explorer_entry_id;
        const wchar_t sibling[]=L"Software\\Classes\\CLSID\\{EdgeTuck-Test-Unrelated}";
        require(RegCreateKeyExW(root,sibling,0,nullptr,0,KEY_ALL_ACCESS,nullptr,&foreign,nullptr)==ERROR_SUCCESS,"unrelated sibling created"); RegCloseKey(foreign); foreign=nullptr;
        require(write_explorer_entry(root,0,true,L"D:\\新 收纳位置",exe)==ERROR_SUCCESS,"retarget after storage change");
        require(read_explorer_entry(root,0).target==L"D:\\新 收纳位置","updated target readback");
        require(write_explorer_entry(root,0,false,{}, {})==ERROR_SUCCESS,"remove owned entry");
        require(!read_explorer_entry(root,0).enabled,"removed state");
        require(std::filesystem::file_size(file)==8,"unregister never alters target files");
        require(RegOpenKeyExW(root,sibling,0,KEY_READ,&foreign)==ERROR_SUCCESS,"other Shell registrations untouched"); RegCloseKey(foreign); foreign=nullptr;
        require(write_explorer_entry(root,0,false,{}, {})==ERROR_SUCCESS,"remove is idempotent");
        require(RegCreateKeyExW(root,cls.c_str(),0,nullptr,0,KEY_ALL_ACCESS,nullptr,&foreign,nullptr)==ERROR_SUCCESS,"foreign registration fixture");
        require(write_explorer_entry(root,0,true,fixture,exe)==ERROR_ALREADY_EXISTS,"do not replace an unowned class");
        require(write_explorer_entry(root,0,false,{}, {})==ERROR_ALREADY_EXISTS,"do not remove an unowned class");
        RegCloseKey(foreign); foreign=nullptr;
        std::filesystem::remove(file); require(std::filesystem::remove(fixture),"only owned empty directory cleaned");
        RegCloseKey(root); root=nullptr; RegDeleteTreeW(HKEY_CURRENT_USER,key.c_str()); owned=false;
        std::cout<<"Explorer entry: PASS; "<<checks<<" isolated registry and file preservation checks\n"; return 0;
    } catch(const std::exception& e) {
        if(foreign) RegCloseKey(foreign); if(root) RegCloseKey(root);
        if(owned) RegDeleteTreeW(HKEY_CURRENT_USER,key.c_str());
        std::cerr<<"FAIL: "<<e.what()<<'\n'; return 1;
    }
}
