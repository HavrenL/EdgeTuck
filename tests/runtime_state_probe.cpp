#include "app.hpp"
#include <winternl.h>
#include <iostream>
#include <cstddef>
#include <array>
#include <sstream>

// Read-only diagnostic for this exact MSVC Release build. Do not write remote
// memory or call any application action; fail closed on layout/range mismatch.
static HANDLE process{};
static void read_bytes(uintptr_t address,void* out,SIZE_T length) {
    SIZE_T read{};
    if(!address || !ReadProcessMemory(process,reinterpret_cast<void*>(address),out,length,&read) || read!=length)
        throw std::runtime_error("Remote memory could not be read");
}
template<class T> static T read(uintptr_t address) { T value{}; read_bytes(address,&value,sizeof(value)); return value; }
static std::wstring text(uintptr_t address) {
    // MSVC x64 release basic_string: 16-byte storage, size, capacity.
    const size_t length=read<size_t>(address+16),capacity=read<size_t>(address+24);
    if(length>32768 || capacity<length || capacity>1024*1024) throw std::runtime_error("String layout mismatch");
    std::wstring result(length,L'\0');
    if(length) read_bytes(capacity<8?address:read<uintptr_t>(address),result.data(),length*2);
    return result;
}
int main(int argc,char** argv) {
    try {
        std::wostringstream output;
        HWND control=FindWindowW(L"EdgeTuck.Control",nullptr); DWORD pid{};
        if(!control || !GetWindowThreadProcessId(control,&pid)) throw std::runtime_error("No settings window");
        process=OpenProcess(PROCESS_QUERY_INFORMATION|PROCESS_VM_READ,FALSE,pid);
        if(!process) throw std::runtime_error("Process read access failed");
        const auto app=static_cast<uintptr_t>(GetWindowLongPtrW(control,GWLP_USERDATA));
        const bool legacy=argc==2 && std::string(argv[1])=="--legacy-075";
        const size_t model_size=legacy?(offsetof(edge::DrawerModel,sort)+7)/8*8:sizeof(edge::DrawerModel);
        const size_t undo_delta=sizeof(std::optional<edge::DrawerModel>)-(model_size+1+7)/8*8;
        const auto field=[&](size_t offset) {return app+offset-(legacy && offset>offsetof(edge::App,undo)?undo_delta:0);};
        const auto config=text(field(offsetof(edge::App,config_path)));
        const auto notice=text(field(offsetof(edge::App,notice)));
        const auto models=field(offsetof(edge::App,settings))+offsetof(edge::Settings,drawers);
        const auto first=read<uintptr_t>(models),last=read<uintptr_t>(models+8);
        // Explicit compatibility with the pre-sort model, whose earlier fields
        // and the App/Drawer offsets read below are unchanged.
        if(last<first || (last-first)%model_size || (last-first)/model_size>32) throw std::runtime_error("Vector layout mismatch");
        output<<L"Pid: "<<pid<<L"\nConfig: "<<config<<L"\nNotice: "<<notice
            <<L"\nDrawers: "<<(last-first)/model_size<<L"\nSaveAllowed: "<<read<bool>(field(offsetof(edge::App,save_allowed)))
            <<L"\nStartupState: "<<static_cast<int>(read<edge::StartupStatus>(field(offsetof(edge::App,startup))).state)
            <<L"\nSmoke: "<<read<bool>(field(offsetof(edge::App,smoke)))
            <<L"\nFolderSyncPending: "<<read<bool>(field(offsetof(edge::App,folder_sync_pending)))
            <<L"\nSyncingFolders: "<<read<bool>(field(offsetof(edge::App,syncing_folders)))
            <<L"\nStorageMigrating: "<<read<bool>(field(offsetof(edge::App,storage_migrating)))
            <<L"\nTheme: "<<read<int>(field(offsetof(edge::App,settings))+offsetof(edge::Settings,theme))
            <<L"\nPriority: "<<read<bool>(field(offsetof(edge::App,settings))+offsetof(edge::Settings,responsive_priority))
            <<L"\nStorageMode: "<<static_cast<int>(read<edge::StorageMode>(field(offsetof(edge::App,settings))+offsetof(edge::Settings,storage_mode)))
            <<L"\nStorageDirectory: "<<text(field(offsetof(edge::App,settings))+offsetof(edge::Settings,storage_directory))
            <<L"\nBlur: "<<read<float>(field(offsetof(edge::App,settings))+offsetof(edge::Settings,material))<<L'\n';
        for(auto address=first;address<last;address+=model_size)
            output<<L"Drawer "<<read<int>(address+offsetof(edge::DrawerModel,id))<<L": "<<text(address+offsetof(edge::DrawerModel,name))
                <<L" | "<<text(address+offsetof(edge::DrawerModel,folder))<<L" | "<<text(address+offsetof(edge::DrawerModel,folder_identity))<<L'\n';
        const auto windows=field(offsetof(edge::App,drawers));
        const auto window_first=read<uintptr_t>(windows),window_last=read<uintptr_t>(windows+8);
        if(window_last<window_first || (window_last-window_first)%sizeof(uintptr_t) || (window_last-window_first)/sizeof(uintptr_t)>32)
            throw std::runtime_error("Drawer window vector layout mismatch");
        const auto scale=read<float>(field(offsetof(edge::App,scale)));
        const auto grid_x=read<int>(field(offsetof(edge::App,grid_x)));
        for(auto slot=window_first;slot<window_last;slot+=sizeof(uintptr_t)) {
            const auto drawer=read<uintptr_t>(slot);
            const auto id=read<int>(drawer+offsetof(edge::Drawer,id));
            const auto bounds=read<RECT>(drawer+offsetof(edge::Drawer,bounds));
            const auto progress=read<float>(drawer+offsetof(edge::Drawer,progress));
            const auto scroll=read<int>(drawer+offsetof(edge::Drawer,scroll));
            const edge::ContentGrid grid{bounds.right-bounds.left,bounds.bottom-bounds.top,grid_x,
                static_cast<int>(std::lround(76*scale)),static_cast<int>(std::lround(38*scale))};
            size_t items=0;
            for(auto address=first;address<last;address+=model_size) if(read<int>(address+offsetof(edge::DrawerModel,id))==id) {
                const auto paths=address+offsetof(edge::DrawerModel,items);
                items=(read<uintptr_t>(paths+8)-read<uintptr_t>(paths))/sizeof(std::wstring);
                if(!legacy) output<<L"Sort "<<id<<L": mode="<<read<int>(address+offsetof(edge::DrawerModel,sort))<<L" descending="<<read<bool>(address+offsetof(edge::DrawerModel,sort_descending))<<L'\n';
            }
            // MSVC release map stores its tree head followed by its size.
            const auto cached=read<size_t>(drawer+offsetof(edge::Drawer,panel_canvas)+offsetof(edge::Canvas,icons)+sizeof(uintptr_t));
            const int expected=std::max(0,std::min(static_cast<int>(items),(scroll+grid.rows())*grid.columns())-scroll*grid.columns());
            output<<L"Images "<<id<<L": cached="<<cached<<L" page="<<expected<<L" progress="<<progress<<L" scroll="<<scroll<<L'\n';
        }
        using Query=NTSTATUS (NTAPI*)(HANDLE,PROCESSINFOCLASS,PVOID,ULONG,PULONG);
        auto query=reinterpret_cast<Query>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"NtQueryInformationProcess"));
        PROCESS_BASIC_INFORMATION basic{};
        if(query && query(process,ProcessBasicInformation,&basic,sizeof(basic),nullptr)>=0) {
            const auto peb=read<PEB>(reinterpret_cast<uintptr_t>(basic.PebBaseAddress));
            const auto params=read<RTL_USER_PROCESS_PARAMETERS>(reinterpret_cast<uintptr_t>(peb.ProcessParameters));
            auto remote_text=[](UNICODE_STRING value) { if(value.Length>32768) return std::wstring{}; std::wstring result(value.Length/2,L'\0'); if(value.Length) read_bytes(reinterpret_cast<uintptr_t>(value.Buffer),result.data(),value.Length); return result; };
            output<<L"Image: "<<remote_text(params.ImagePathName)<<L'\n';
            const auto environment=read<uintptr_t>(reinterpret_cast<uintptr_t>(peb.ProcessParameters)+0x80);
            MEMORY_BASIC_INFORMATION region{};
            if(VirtualQueryEx(process,reinterpret_cast<void*>(environment),&region,sizeof(region))) {
                const size_t bytes=std::min<size_t>(region.RegionSize-(environment-reinterpret_cast<uintptr_t>(region.BaseAddress)),128*1024);
                std::vector<wchar_t> values(bytes/2+1,L'\0'); read_bytes(environment,values.data(),bytes);
                for(size_t pos=0;pos<bytes/2 && values[pos];) {
                    std::wstring entry(values.data()+pos); pos+=entry.size()+1;
                    if(entry.starts_with(L"LOCALAPPDATA=") || entry.starts_with(L"USERPROFILE=") || entry.starts_with(L"APPDATA=")) output<<entry<<L'\n';
                }
            }
        }
        const auto wide=output.str();
        const int length=WideCharToMultiByte(CP_UTF8,0,wide.data(),static_cast<int>(wide.size()),nullptr,0,nullptr,nullptr);
        std::string utf8(length,'\0'); WideCharToMultiByte(CP_UTF8,0,wide.data(),static_cast<int>(wide.size()),utf8.data(),length,nullptr,nullptr);
        std::cout<<utf8;
        CloseHandle(process); return 0;
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; if(process) CloseHandle(process); return 1; }
}
