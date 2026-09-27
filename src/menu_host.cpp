#include "menu_host.hpp"
#include "shell_files.hpp"
#include <algorithm>
#include <span>
#include <stdexcept>
#include <cstring>
#include <limits>
#include <memory>
#include <shlwapi.h>
#ifdef EDGETUCK_MENU_TESTS
#include <thread>
#include <fstream>
#endif

namespace edge {
namespace {
constexpr DWORD signature=0x45544d31,maximum_bytes=4*1024*1024;
enum Phase:LONG { Preparing,Showing,Invoking,Finished,Aborted,Idle };
struct alignas(8) Shared {
    DWORD magic{},bytes{},parent{},keys{},behavior{};
    UINT command{}; HRESULT status{S_OK};
    volatile LONG phase{Preparing};
    alignas(8) volatile LONG64 heartbeat{};
    ULONGLONG began{};
    volatile LONG stage{};
    alignas(8) volatile LONG64 stage_started[6]{},prepared{};
    UINT64 event{},request_event{},parent_handle{},window{},owner{}; POINT point{};
#ifdef EDGETUCK_MENU_TESTS
    wchar_t trace[1024]{};
#endif
};
#ifdef EDGETUCK_MENU_TESTS
void trace(Shared& shared,const char* stage,HRESULT status=S_OK) {
    if(!shared.trace[0]) return;
    std::ofstream output(std::filesystem::path(shared.trace),std::ios::app);
    output<<GetTickCount64()-shared.began<<" ms "<<stage<<" hr="<<std::hex<<status<<std::dec<<'\n';
    EnumWindows([](HWND window,LPARAM value)->BOOL {
        DWORD pid{}; GetWindowThreadProcessId(window,&pid); char name[128]{}; GetClassNameA(window,name,128);
        if(pid==GetCurrentProcessId() || strcmp(name,"CabinetWClass")==0) {
            auto& stream=*reinterpret_cast<std::ofstream*>(value);
            stream<<" window="<<reinterpret_cast<UINT_PTR>(window)<<" pid="<<pid<<" class="<<name<<" visible="<<IsWindowVisible(window)<<" owner="<<reinterpret_cast<UINT_PTR>(GetWindow(window,GW_OWNER))<<'\n';
        }
        return TRUE;
    },reinterpret_cast<LPARAM>(&output));
}
#endif
struct Handle {
    HANDLE value{};
    ~Handle() { if(value && value!=INVALID_HANDLE_VALUE) CloseHandle(value); }
};
struct View { void* value{}; ~View(){if(value) UnmapViewOfFile(value);} };
// Properties and some Shell verbs outlive InvokeCommand. Let those components
// hold this helper alive until their own work/dialogs have finished.
class ShellLifetime final:public IUnknown {
    volatile LONG references{1};
    Handle released{CreateEventW(nullptr,TRUE,FALSE,nullptr)};
    ComPtr<IUnknown> previous_thread;
public:
    ShellLifetime() {
        if(!released.value) throw std::runtime_error("Shell lifetime event failed");
        // Shell background tasks use SHGetThreadRef as well as the process
        // reference. A process reference alone does not keep those tasks alive.
        SHGetThreadRef(&previous_thread);
        if(FAILED(SHSetThreadRef(this))) throw std::runtime_error("Shell thread reference failed");
        SHSetInstanceExplorer(this);
    }
    ~ShellLifetime() { SHSetInstanceExplorer(nullptr); SHSetThreadRef(previous_thread.Get()); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** value) override {
        if(!value) return E_POINTER; *value=nullptr;
        if(iid!=IID_IUnknown) return E_NOINTERFACE;
        *value=static_cast<IUnknown*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&references); }
    ULONG STDMETHODCALLTYPE Release() override { const auto count=InterlockedDecrement(&references); if(!count) SetEvent(released.value); return count; }
    void finish() {
        Release();
        while(WaitForSingleObject(released.value,0)!=WAIT_OBJECT_0) {
            MsgWaitForMultipleObjectsEx(1,&released.value,INFINITE,QS_ALLINPUT,MWMO_INPUTAVAILABLE);
            MSG message{};
            while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
        }
    }
};
struct Attributes {
    std::vector<std::byte> data;
    LPPROC_THREAD_ATTRIBUTE_LIST list{};
    ~Attributes(){if(list) DeleteProcThreadAttributeList(list);}
};
struct MenuFailure {
    std::vector<std::wstring> paths;
    DWORD keys{};
    ULONGLONG expires{};
    FileMenuResult result;
};
std::vector<MenuFailure> failures;
#ifdef EDGETUCK_MENU_TESTS
ULONGLONG retry_clock_offset{};
#endif
ULONGLONG retry_clock() {
    return GetTickCount64()
#ifdef EDGETUCK_MENU_TESTS
        +retry_clock_offset
#endif
        ;
}
void remember_failure(const FileMenuRequest& request,DWORD keys,const FileMenuResult& result) {
    if(failures.size()>=16) failures.erase(failures.begin());
    failures.push_back({request.paths,keys,retry_clock()+30000,result});
}
void need(bool okay){if(!okay) throw std::runtime_error("Invalid menu request");}
template<class T> void put(std::vector<std::byte>& data,T value) {
    const auto size=data.size(); data.resize(size+sizeof(value)); memcpy(data.data()+size,&value,sizeof(value));
}
void put_text(std::vector<std::byte>& data,const std::wstring& text) {
    need(text.size()<=32767 && text.find(L'\0')==std::wstring::npos);
    put(data,static_cast<DWORD>(text.size())); const auto size=data.size();
    need(size+text.size()*sizeof(wchar_t)<=maximum_bytes);
    data.resize(size+text.size()*sizeof(wchar_t)); memcpy(data.data()+size,text.data(),text.size()*sizeof(wchar_t));
}
struct Reader {
    std::span<const std::byte> data;
    template<class T> T get() { need(data.size()>=sizeof(T)); T value{}; memcpy(&value,data.data(),sizeof(T)); data=data.subspan(sizeof(T)); return value; }
    std::wstring text() {
        const auto count=get<DWORD>(); need(count<=32767 && count*sizeof(wchar_t)<=data.size());
        std::wstring value(count,L'\0'); memcpy(value.data(),data.data(),count*sizeof(wchar_t)); data=data.subspan(count*sizeof(wchar_t));
        need(value.find(L'\0')==std::wstring::npos); return value;
    }
};
void pulse(Shared& shared) {
    InterlockedExchange64(&shared.heartbeat,static_cast<LONG64>(GetTickCount64()));
    SetEvent(reinterpret_cast<HANDLE>(shared.event));
}
void progress(void* context,MenuLoadStage stage) {
    auto& shared=*static_cast<Shared*>(context);
    const auto index=static_cast<LONG>(stage);
    InterlockedExchange64(&shared.stage_started[index],static_cast<LONG64>(GetTickCount64()));
    InterlockedExchange(&shared.stage,index); pulse(shared);
}
void timing(Shared& shared,FileMenuResult& result) {
    const auto stage=InterlockedCompareExchange(&shared.stage,0,0);
    const auto prepared=InterlockedCompareExchange64(&shared.prepared,0,0);
    const auto now=prepared?static_cast<ULONGLONG>(prepared):GetTickCount64();
    ULONGLONG started[6]{};
    for(int i=0;i<6;++i) started[i]=static_cast<ULONGLONG>(InterlockedCompareExchange64(&shared.stage_started[i],0,0));
    const auto elapsed=[&](int index) -> DWORD {
        if(!started[index]) return 0;
        const auto end=index<5 && started[index+1]?started[index+1]:now;
        return static_cast<DWORD>(end-started[index]);
    };
    result.stage=static_cast<MenuLoadStage>(stage);
    result.preparation_ms=static_cast<DWORD>(now-shared.began);
    result.launch_ms=elapsed(0); result.initialize_ms=elapsed(1);
    result.resolve_ms=elapsed(2); result.bind_ms=elapsed(3); result.query_ms=elapsed(4);
    result.stage_ms=stage<5?elapsed(stage):0;
}
struct HostWindow { Shared* shared{}; ShellMenu* menu{}; };
LRESULT CALLBACK host_proc(HWND window,UINT message,WPARAM wp,LPARAM lp) {
    auto* host=reinterpret_cast<HostWindow*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    if(message==WM_NCCREATE) { host=static_cast<HostWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams); SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(host)); }
    if(host) {
        if(message==WM_TIMER) { pulse(*host->shared); return 0; }
        LRESULT result{}; if(host->menu && host->menu->message(message,wp,lp,result)) return result;
    }
    return DefWindowProcW(window,message,wp,lp);
}
// CAS is the boundary between an unchosen menu and a running file command.
// The parent must never terminate a helper after Invoking is published.
bool abort_menu(Shared& shared,HANDLE process) {
    const auto phase=InterlockedCompareExchange(&shared.phase,Preparing,Preparing);
    if(phase==Invoking) return false;
    if(InterlockedCompareExchange(&shared.phase,Aborted,phase)!=phase) return false;
    TerminateProcess(process,ERROR_TIMEOUT); return true;
}
struct WarmHost {
    Handle mapping,changed,requested,parent,process;
    View view;
    ~WarmHost() {
        if(process.value && view.value) {
            auto& shared=*static_cast<Shared*>(view.value);
            while(WaitForSingleObject(process.value,0)==WAIT_TIMEOUT &&
                InterlockedCompareExchange(&shared.phase,Preparing,Preparing)!=Invoking) {
                if(abort_menu(shared,process.value)) break;
            }
        }
    }
};
// A single, short-lived worker. No app timer polls it while idle; the child
// waits on a request/parent-exit event and retires itself after 30 seconds.
std::unique_ptr<WarmHost> warm_host;
}
void retry_file_menu(){failures.clear();}
#ifdef EDGETUCK_MENU_TESTS
void advance_menu_retry_clock(DWORD milliseconds){retry_clock_offset+=milliseconds;}
#endif
void append_file_menu_actions(HMENU menu,const std::vector<std::pair<UINT,std::wstring>>& transfers,bool basic) {
    if(basic) {
        AppendMenuW(menu,MF_STRING,0x7001,L"打开");
        AppendMenuW(menu,MF_STRING,0x7004,L"复制文件\tCtrl+C");
        AppendMenuW(menu,MF_STRING,0x7005,L"重命名\tF2");
    }
    AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
    AppendMenuW(menu,MF_STRING,0x7002,L"在文件资源管理器中显示");
    AppendMenuW(menu,MF_STRING,0x7003,L"移回桌面（旧引用仅移除）\tDelete");
    HMENU transfer=CreatePopupMenu();
    for(const auto& [id,name]:transfers) AppendMenuW(transfer,MF_STRING,id,name.c_str());
    if(GetMenuItemCount(transfer)>0) AppendMenuW(menu,MF_POPUP,reinterpret_cast<UINT_PTR>(transfer),L"转到其他抽屉"); else DestroyMenu(transfer);
    if(basic) { AppendMenuW(menu,MF_SEPARATOR,0,nullptr); AppendMenuW(menu,MF_STRING|MF_GRAYED,0,L"系统菜单暂不可用"); AppendMenuW(menu,MF_STRING,0x7007,L"重试系统菜单"); }
}
FileMenuResult isolated_file_menu(const FileMenuRequest& request,DWORD prepare_timeout) {
    FileMenuResult result;
    try {
        need(IsWindow(request.owner) && !request.paths.empty() && request.paths.size()<=1000 && request.transfers.size()<=32);
        std::vector<std::byte> data(sizeof(Shared));
        put(data,static_cast<DWORD>(request.paths.size()));
        for(const auto& path:request.paths) { need(std::filesystem::path(path).is_absolute()); put_text(data,path); }
        put(data,static_cast<DWORD>(request.transfers.size()));
        for(const auto& [id,name]:request.transfers) { need(id>=0x8000 && id<=0x8000+1000000); put(data,id); put_text(data,name); }
        need(data.size()<=maximum_bytes);
        const DWORD keys=(GetKeyState(VK_SHIFT)<0?MK_SHIFT:0)|(GetKeyState(VK_CONTROL)<0?MK_CONTROL:0);
        std::erase_if(failures,[](const MenuFailure& failure){return failure.expires<=retry_clock();});
        for(const auto& failure:failures) if(failure.keys==keys && failure.paths==request.paths) {
            result=failure.result; result.timed_out=false; result.cooldown=true; return result;
        }
        auto host=std::move(warm_host);
        if(host && (WaitForSingleObject(host->process.value,0)!=WAIT_TIMEOUT ||
            InterlockedCompareExchange(&static_cast<Shared*>(host->view.value)->phase,Preparing,Idle)!=Idle)) host.reset();
        const bool reused=host!=nullptr; result.reused=reused;
        if(!host) {
            host=std::make_unique<WarmHost>();
            SECURITY_ATTRIBUTES security{sizeof(security),nullptr,TRUE};
            host->mapping.value=CreateFileMappingW(INVALID_HANDLE_VALUE,&security,PAGE_READWRITE,0,maximum_bytes,nullptr);
            host->changed.value=CreateEventW(&security,FALSE,FALSE,nullptr);
            host->requested.value=CreateEventW(&security,FALSE,FALSE,nullptr);
            host->parent.value=OpenProcess(SYNCHRONIZE,TRUE,GetCurrentProcessId());
            need(host->mapping.value && host->changed.value && host->requested.value && host->parent.value);
            host->view.value=MapViewOfFile(host->mapping.value,FILE_MAP_ALL_ACCESS,0,0,maximum_bytes); need(host->view.value!=nullptr);
        }
        ResetEvent(host->changed.value);
        memcpy(host->view.value,data.data(),data.size()); auto& shared=*static_cast<Shared*>(host->view.value);
        shared.magic=signature; shared.bytes=static_cast<DWORD>(data.size()); shared.parent=GetCurrentProcessId();
        shared.event=reinterpret_cast<UINT64>(host->changed.value); shared.request_event=reinterpret_cast<UINT64>(host->requested.value);
        shared.parent_handle=reinterpret_cast<UINT64>(host->parent.value); shared.owner=reinterpret_cast<UINT64>(request.owner); shared.point=request.point;
        shared.keys=keys; shared.began=GetTickCount64();
        progress(&shared,MenuLoadStage::Starting);
#ifdef EDGETUCK_MENU_TESTS
        shared.behavior=request.test_behavior;
        need(request.test_trace.size()<std::size(shared.trace));
        std::copy(request.test_trace.begin(),request.test_trace.end(),shared.trace);
#endif
        if(reused) { need(SetEvent(host->requested.value)!=FALSE); }
        else {
        Attributes attributes; SIZE_T size{}; InitializeProcThreadAttributeList(nullptr,1,0,&size); attributes.data.resize(size);
        auto* list=reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data.data()); need(InitializeProcThreadAttributeList(list,1,0,&size)!=FALSE); attributes.list=list;
        HANDLE inherited[]{host->mapping.value,host->changed.value,host->requested.value,host->parent.value}; need(UpdateProcThreadAttribute(list,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,inherited,sizeof(inherited),nullptr,nullptr)!=FALSE);
        // Shell location verbs can propagate the caller's startup show state to
        // Explorer. Keep the helper console-free and its owner window initially
        // hidden, without telling subsequently opened folders to hide as well.
        STARTUPINFOEXW startup{}; startup.StartupInfo.cb=sizeof(startup); startup.lpAttributeList=list;
        wchar_t program[32768]{}; need(GetModuleFileNameW(nullptr,program,32768)!=0);
        std::wstring command=L"\""+std::wstring(program)+L"\" --shell-menu="+std::to_wstring(reinterpret_cast<UINT_PTR>(host->mapping.value));
        PROCESS_INFORMATION info{};
        need(CreateProcessW(program,command.data(),nullptr,nullptr,TRUE,EXTENDED_STARTUPINFO_PRESENT|CREATE_SUSPENDED|CREATE_NO_WINDOW,nullptr,nullptr,&startup.StartupInfo,&info)!=FALSE);
        host->process.value=info.hProcess; Handle thread{info.hThread}; AllowSetForegroundWindow(info.dwProcessId);
        if(ResumeThread(thread.value)==static_cast<DWORD>(-1)) { TerminateProcess(host->process.value,1); throw std::runtime_error("Menu launch failed"); }
        }
        AllowSetForegroundWindow(GetProcessId(host->process.value));
        result.host_pid=GetProcessId(host->process.value);
        const HANDLE process=host->process.value;
        HANDLE waits[]{process,host->changed.value};
        for(;;) {
            const auto phase=InterlockedCompareExchange(&shared.phase,Preparing,Preparing);
            if(phase==Invoking) { result.invoked=true; timing(shared,result); return result; }
            if(phase==Finished || phase==Idle) {
                result.command=shared.command; result.status=shared.status;
                timing(shared,result);
                // Rebuild the menu for every request, retaining only the
                // process/DLL initialization. Never cache an Invoking helper.
                warm_host=std::move(host);
                return result;
            }
            const auto now=GetTickCount64();
            const bool closed=!IsWindow(request.owner);
            if((phase==Preparing && now-shared.began>prepare_timeout) || (phase==Showing && now-static_cast<ULONGLONG>(InterlockedCompareExchange64(&shared.heartbeat,0,0))>5000) || closed) {
                if(abort_menu(shared,process)) {
                    result.timed_out=!closed; result.status=closed?E_ABORT:HRESULT_FROM_WIN32(ERROR_TIMEOUT);
                    timing(shared,result); if(!closed) remember_failure(request,keys,result); return result;
                }
                continue;
            }
            const auto waiting=MsgWaitForMultipleObjectsEx(2,waits,100,QS_ALLINPUT,MWMO_INPUTAVAILABLE);
            if(waiting==WAIT_OBJECT_0) {
                const auto ended=InterlockedCompareExchange(&shared.phase,Preparing,Preparing);
                if(ended==Finished || ended==Idle || ended==Invoking) continue;
                result.status=E_FAIL; timing(shared,result); remember_failure(request,keys,result); return result;
            }
            MSG message{};
            while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) {
                if(message.message==WM_QUIT) { abort_menu(shared,process); PostQuitMessage(static_cast<int>(message.wParam)); result.status=E_ABORT; timing(shared,result); return result; }
                TranslateMessage(&message); DispatchMessageW(&message);
            }
        }
    } catch(...) { result.status=E_FAIL; return result; }
}
namespace {
struct MenuWindow {
    HostWindow host;
    HWND window{}; HMENU popup{};
    ShellMenu shell;
    static void CALLBACK cleanup_timeout(void* context,BOOLEAN) {
        auto& shared=*static_cast<Shared*>(context);
        if(InterlockedCompareExchange(&shared.phase,Aborted,Finished)==Finished) TerminateProcess(GetCurrentProcess(),ERROR_TIMEOUT);
    }
    ~MenuWindow() {
        // Release hooks are extension code too. Only arm this one-shot guard
        // while cleaning up an unchosen menu; it never covers invoked verbs.
        HANDLE watchdog{};
        if(InterlockedCompareExchange(&host.shared->phase,Preparing,Preparing)==Finished)
            CreateTimerQueueTimer(&watchdog,nullptr,cleanup_timeout,host.shared,3000,0,WT_EXECUTEONLYONCE);
#ifdef EDGETUCK_MENU_TESTS
        if(host.shared->behavior==9) Sleep(30000);
#endif
        host.menu=nullptr;
        if(popup) DestroyMenu(popup);
        if(window) { KillTimer(window,1); DestroyWindow(window); }
        shell=ShellMenu{};
        if(watchdog) DeleteTimerQueueTimer(nullptr,watchdog,INVALID_HANDLE_VALUE);
    }
};
void serve_menu(Shared& shared,bool& initialized) {
        need(shared.magic==signature && shared.bytes>=sizeof(Shared) && shared.bytes<=maximum_bytes);
        DWORD parent{}; GetWindowThreadProcessId(reinterpret_cast<HWND>(shared.owner),&parent); need(parent==shared.parent);
        progress(&shared,MenuLoadStage::Initializing);
        Reader input{{reinterpret_cast<const std::byte*>(&shared)+sizeof(Shared),shared.bytes-sizeof(Shared)}};
        FileMenuRequest request; const auto count=input.get<DWORD>(); need(count>0 && count<=1000);
        for(DWORD i=0;i<count;++i) { auto path=input.text(); need(!path.empty() && std::filesystem::path(path).is_absolute()); request.paths.push_back(std::move(path)); }
        const auto transfers=input.get<DWORD>(); need(transfers<=32);
        for(DWORD i=0;i<transfers;++i) { const auto id=input.get<UINT>(); need(id>=0x8000 && id<=0x8000+1000000); request.transfers.emplace_back(id,input.text()); }
        need(input.data.empty());
        MenuWindow resources; resources.host.shared=&shared;
#ifdef EDGETUCK_MENU_TESTS
        if(shared.behavior==13) {
            // An invoked Shell component may use the process's default show
            // state. Its window must be visible even though the host is quiet.
            HWND probe=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,L"STATIC",L"EdgeTuck show-state fixture",WS_POPUP,-32000,-32000,1,1,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
            need(probe!=nullptr); ShowWindow(probe,SW_SHOWDEFAULT);
            shared.command=IsWindowVisible(probe)?0x7002:0; DestroyWindow(probe);
            InterlockedExchange(&shared.phase,Finished); pulse(shared); return;
        }
        if(shared.behavior==1) { Sleep(30000); return; }
        if(shared.behavior==2) { TerminateProcess(GetCurrentProcess(),7); return; }
        if(shared.behavior==3 || shared.behavior==8 || shared.behavior==9) {
            shared.command=0x7002; InterlockedExchange(&shared.phase,Finished); pulse(shared);
            return;
        }
        if(shared.behavior==4) {
            InterlockedExchange(&shared.phase,Invoking); pulse(shared); Sleep(450);
            HANDLE file=CreateFileW(request.paths[0].c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
            if(file!=INVALID_HANDLE_VALUE) CloseHandle(file); return;
        }
        if(shared.behavior==6 || shared.behavior==11) {
            ShellLifetime lifetime; IUnknown* reference{};
            need(SUCCEEDED(shared.behavior==11?SHGetThreadRef(&reference):SHGetInstanceExplorer(&reference)) && reference);
            InterlockedExchange(&shared.phase,Invoking); pulse(shared);
            std::thread worker([reference]{Sleep(350); reference->Release();});
            lifetime.finish(); worker.join();
            HANDLE file=CreateFileW(request.paths[0].c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
            if(file!=INVALID_HANDLE_VALUE) CloseHandle(file); return;
        }
#else
        need(shared.behavior==0);
#endif
        if(!initialized) {
            shared.status=OleInitialize(nullptr);
            if(FAILED(shared.status)) { InterlockedExchange(&shared.phase,Finished); pulse(shared); return; }
            initialized=true;
        }
        auto& host=resources.host; auto& shell=resources.shell;
        WNDCLASSW cls{}; cls.hInstance=GetModuleHandleW(nullptr); cls.lpszClassName=L"EdgeTuck.FileMenuHost"; cls.lpfnWndProc=host_proc; cls.hCursor=LoadCursorW(nullptr,IDC_ARROW); RegisterClassW(&cls);
        // No cross-process owner or attached input queue: a dead extension must
        // not make Windows consider the drawer's UI thread hung as well.
        HWND window=resources.window=CreateWindowExW(WS_EX_TOOLWINDOW,cls.lpszClassName,L"轻屉文件菜单",WS_POPUP,shared.point.x,shared.point.y,1,1,nullptr,nullptr,cls.hInstance,&host);
        need(window!=nullptr); shared.window=reinterpret_cast<UINT64>(window); ShowWindow(window,SW_HIDE); SetTimer(window,1,200,nullptr);
        HMENU popup=resources.popup=CreatePopupMenu(); host.menu=&shell;
        need(popup!=nullptr);
        ShellMenuLoad load; load.progress=progress; load.context=&shared;
#ifdef EDGETUCK_MENU_TESTS
        if(shared.behavior==7) load.asynchronous=false;
#endif
        if(!shell.fill(window,popup,request.paths,shared.keys,&load)) {
            shared.status=load.status; InterlockedExchange64(&shared.prepared,static_cast<LONG64>(GetTickCount64()));
            InterlockedExchange(&shared.phase,Finished); pulse(shared); return;
        }
        progress(&shared,MenuLoadStage::Ready);
        InterlockedExchange64(&shared.prepared,InterlockedCompareExchange64(&shared.stage_started[5],0,0));
#ifdef EDGETUCK_MENU_TESTS
        if(shared.behavior==5 || shared.behavior==7) {
            // Read-only integration probe: query a real folder menu without
            // displaying it and without ever invoking one of its commands.
            shared.status=S_OK; shared.command=0x7002; InterlockedExchange(&shared.phase,Finished); pulse(shared); return;
        }
#endif
        append_file_menu_actions(popup,request.transfers,false);
        if(InterlockedCompareExchange(&shared.phase,Showing,Preparing)!=Preparing) return;
        pulse(shared); ShowWindow(window,SW_SHOWNOACTIVATE); SetForegroundWindow(window);
        UINT command{};
#ifdef EDGETUCK_MENU_TESTS
        if(shared.behavior==10) {
            // Manual diagnostic: only the read-only location verb may be chosen.
            for(int item=0;item<GetMenuItemCount(popup);++item) {
                const auto id=GetMenuItemID(popup,item);
                if(shell.verb(id)==L"opencontaining") { command=id; break; }
            }
            need(command!=0);
        } else
#endif
        command=TrackPopupMenu(popup,TPM_RETURNCMD|TPM_RIGHTBUTTON,shared.point.x,shared.point.y,0,window,nullptr);
        DestroyMenu(popup); resources.popup=nullptr;
        if(command>0 && command<=0x6FFF) {
            const auto verb=shell.verb(command);
            if(verb==L"rename") shared.command=0x7005;
            else {
                if(InterlockedCompareExchange(&shared.phase,Invoking,Showing)!=Showing) return;
                pulse(shared); KillTimer(window,1);
                ShellLifetime lifetime;
#ifdef EDGETUCK_MENU_TESTS
                trace(shared,"before invoke");
#endif
                const auto status=shell.invoke(window,command,shared.point,shared.keys);
#ifdef EDGETUCK_MENU_TESTS
                trace(shared,"after invoke",status);
#endif
                if(FAILED(status)) MessageBoxW(window,L"Windows 未能完成所选文件操作。",L"轻屉",MB_OK|MB_ICONINFORMATION);
                // Materialize delayed clipboard data before this host exits.
                if(verb==L"copy" || verb==L"cut") OleFlushClipboard();
                host.menu=nullptr; shell=ShellMenu{}; lifetime.finish();
#ifdef EDGETUCK_MENU_TESTS
                trace(shared,"lifetime finished");
#endif
                return;
            }
        } else shared.command=command;
        shared.status=S_OK; InterlockedExchange(&shared.phase,Finished); pulse(shared);
}
}
int file_menu_host(std::wstring_view argument) {
    constexpr std::wstring_view prefix=L"--shell-menu=";
    if(!argument.starts_with(prefix)) return 1;
    const std::wstring number(argument.substr(prefix.size())); wchar_t* end{};
    const auto raw=wcstoull(number.c_str(),&end,10); if(!raw || end!=number.c_str()+number.size()) return 1;
    Handle mapping{reinterpret_cast<HANDLE>(raw)}; DWORD flags{};
    if(!GetHandleInformation(mapping.value,&flags) || !(flags&HANDLE_FLAG_INHERIT)) return 1;
    View view{MapViewOfFile(mapping.value,FILE_MAP_ALL_ACCESS,0,0,0)}; if(!view.value) return 1;
    MEMORY_BASIC_INFORMATION region{}; if(!VirtualQuery(view.value,&region,sizeof(region)) || region.RegionSize<maximum_bytes) return 1;
    auto& shared=*static_cast<Shared*>(view.value);
    if(shared.magic!=signature) return 1;
    Handle event{reinterpret_cast<HANDLE>(shared.event)},requested{reinterpret_cast<HANDLE>(shared.request_event)},parent{reinterpret_cast<HANDLE>(shared.parent_handle)};
    if(!GetHandleInformation(event.value,&flags) || !GetHandleInformation(requested.value,&flags) || !GetHandleInformation(parent.value,&flags)) return 1;
    bool initialized=false;
    for(;;) {
        try { serve_menu(shared,initialized); }
        catch(...) {
            // Exceptions after command dispatch must never publish a reusable
            // process or make the parent terminate an operation still in flight.
            if(InterlockedCompareExchange(&shared.phase,Preparing,Preparing)==Invoking) return 1;
            shared.status=E_FAIL; InterlockedExchange(&shared.phase,Finished); pulse(shared);
        }
        if(InterlockedCompareExchange(&shared.phase,Idle,Finished)!=Finished) return 0;
        pulse(shared);
        DWORD idle_ms=30000;
#ifdef EDGETUCK_MENU_TESTS
        if(shared.behavior==8) idle_ms=200;
#endif
        const auto until=GetTickCount64()+idle_ms;
        HANDLE waits[]{parent.value,requested.value};
        for(;;) {
            const auto now=GetTickCount64();
            if(now>=until && InterlockedCompareExchange(&shared.phase,Aborted,Idle)==Idle) {
                // No command has run in this process. Avoid waiting on arbitrary
                // extension DLL shutdown code when retiring an idle worker.
                TerminateProcess(GetCurrentProcess(),0); return 0;
            }
            const DWORD remaining=now<until?static_cast<DWORD>(until-now):INFINITE;
            const auto waiting=MsgWaitForMultipleObjectsEx(2,waits,remaining,QS_ALLINPUT,MWMO_INPUTAVAILABLE);
            if(waiting==WAIT_OBJECT_0) { TerminateProcess(GetCurrentProcess(),0); return 0; }
            if(waiting==WAIT_OBJECT_0+1) break;
            if(waiting==WAIT_FAILED) return 1;
            MSG message{}; while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
        }
        if(InterlockedCompareExchange(&shared.phase,Preparing,Preparing)!=Preparing) return 1;
    }
}
}
