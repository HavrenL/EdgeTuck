#include "runtime_options.hpp"
#include <shellapi.h>
#include <iostream>
#include <stdexcept>
using namespace edge;
static int checks{};
static void require(bool value,const char* description) { ++checks; if(!value) throw std::runtime_error(description); }
int main() {
    HKEY key{},read_only{}; bool owned=false;
    const auto path=L"Software\\EdgeTuck\\Tests\\Runtime-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64());
    const auto prior=GetPriorityClass(GetCurrentProcess());
    try {
        const std::wstring executable=L"C:\\Program Files\\轻屉 软件\\EdgeTuck.exe";
        const auto command=startup_command(executable);
        int count{}; auto args=CommandLineToArgvW(command.c_str(),&count);
        const bool valid=args && count==2 && executable==args[0] && std::wstring(args[1])==L"--tray";
        if(args) LocalFree(args);
        require(valid,"quoted Unicode path launches exactly the executable and tray argument");
        require(startup_command(L"relative.exe").empty(),"relative startup path rejected");
        require(startup_command(L"C:\\bad\"name.exe").empty(),"embedded quote rejected");
        require(startup_command(L"C:\\"+std::wstring(270,L'a')+L"\\EdgeTuck.exe").empty(),"overlong Run command rejected");
        DWORD disposition{};
        require(RegCreateKeyExW(HKEY_CURRENT_USER,path.c_str(),0,nullptr,0,KEY_QUERY_VALUE|KEY_SET_VALUE,nullptr,&key,&disposition)==ERROR_SUCCESS && disposition==REG_CREATED_NEW_KEY,"isolated registry key created"); owned=true;
        const wchar_t other[]=L"keep this entry";
        require(RegSetValueExW(key,L"Unrelated",0,REG_SZ,reinterpret_cast<const BYTE*>(other),sizeof(other))==ERROR_SUCCESS,"sibling value fixture");
        std::wstring read;
        require(read_run_entry(key,read)==ERROR_SUCCESS && read.empty(),"unregistered state");
        require(write_run_entry(key,command,true)==ERROR_SUCCESS,"enable startup entry in test-only key");
        require(read_run_entry(key,read)==ERROR_SUCCESS && read==command,"registration verified by readback");
        require(RegOpenKeyExW(HKEY_CURRENT_USER,path.c_str(),0,KEY_QUERY_VALUE,&read_only)==ERROR_SUCCESS,"read-only handle");
        require(write_run_entry(read_only,command,true)==ERROR_ACCESS_DENIED,"write error reported");
        RegCloseKey(read_only); read_only=nullptr;
        require(write_run_entry(key,command,false)==ERROR_SUCCESS && read_run_entry(key,read)==ERROR_SUCCESS && read.empty(),"disable removes only owned value");
        wchar_t kept[32]{}; DWORD bytes=sizeof(kept);
        require(RegGetValueW(key,nullptr,L"Unrelated",RRF_RT_REG_SZ,nullptr,kept,&bytes)==ERROR_SUCCESS && std::wstring(kept)==other,"other values remain intact");
        require(write_run_entry(key,L"",false)==ERROR_SUCCESS,"disable is idempotent");
        DWORD error{};
        require(set_responsive_priority(true,error) && GetPriorityClass(GetCurrentProcess())==ABOVE_NORMAL_PRIORITY_CLASS,"above-normal process priority applies");
        require(set_responsive_priority(false,error) && GetPriorityClass(GetCurrentProcess())==NORMAL_PRIORITY_CLASS,"normal priority restored");
        SetPriorityClass(GetCurrentProcess(),prior);
        RegCloseKey(key); key=nullptr;
        // This unique key is created above and is not under a Windows startup location.
        RegDeleteKeyW(HKEY_CURRENT_USER,path.c_str()); owned=false;
        std::cout<<"Runtime options: PASS; "<<checks<<" checks; quoted command, isolated registry on/off, permissions, priority switching\n";
        return 0;
    } catch(const std::exception& e) {
        if(read_only) RegCloseKey(read_only); if(key) RegCloseKey(key);
        if(owned) RegDeleteKeyW(HKEY_CURRENT_USER,path.c_str());
        SetPriorityClass(GetCurrentProcess(),prior);
        std::cerr<<"FAIL: "<<e.what()<<'\n'; return 1;
    }
}
