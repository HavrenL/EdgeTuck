#include "menu_host.hpp"
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <fstream>
using namespace edge;
static int ticks{},checks{};
static void require(bool value,const char* reason){++checks; if(!value) throw std::runtime_error(reason);}
static LRESULT CALLBACK proc(HWND window,UINT message,WPARAM wp,LPARAM lp) { if(message==WM_TIMER) { ++ticks; return 0; } return DefWindowProcW(window,message,wp,lp); }
int wmain(int argc,wchar_t** argv) {
    if(argc==2) return file_menu_host(argv[1]);
    std::filesystem::path root; HWND window{};
    try {
        root=std::filesystem::temp_directory_path()/(L"EdgeTuck-menu-test-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
        require(std::filesystem::create_directory(root),"owned fixture");
        WNDCLASSW cls{}; cls.hInstance=GetModuleHandleW(nullptr); cls.lpszClassName=L"EdgeTuck.MenuHostTest"; cls.lpfnWndProc=proc; RegisterClassW(&cls);
        window=CreateWindowW(cls.lpszClassName,L"",WS_POPUP,0,0,1,1,nullptr,nullptr,cls.hInstance,nullptr); require(window!=nullptr,"parent window"); SetTimer(window,1,20,nullptr);
        if(argc==3 && std::wstring_view(argv[1])==L"--probe-location") {
            FileMenuRequest probe; probe.owner=window; probe.paths={argv[2]}; probe.test_behavior=10;
            probe.test_trace=(root/L"trace.txt").wstring();
            const auto result=isolated_file_menu(probe);
            std::cout<<"Location command: invoked="<<result.invoked<<" host_pid="<<result.host_pid<<" status="<<std::hex<<result.status<<std::dec<<std::endl;
            const auto until=GetTickCount64()+5000;
            while(GetTickCount64()<until) {
                MSG message{}; while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) {TranslateMessage(&message); DispatchMessageW(&message);}
                MsgWaitForMultipleObjectsEx(0,nullptr,50,QS_ALLINPUT,MWMO_INPUTAVAILABLE);
            }
            std::ifstream trace(probe.test_trace); std::cout<<trace.rdbuf(); trace.close();
            DestroyWindow(window); std::filesystem::remove(probe.test_trace); std::filesystem::remove(root); return result.invoked?0:1;
        }
        if(argc==3 && (std::wstring_view(argv[1])==L"--probe-read-only" || std::wstring_view(argv[1])==L"--probe-sync" || std::wstring_view(argv[1])==L"--probe-reuse" || std::wstring_view(argv[1])==L"--probe-idle")) {
            FileMenuRequest probe; probe.owner=window; probe.paths={argv[2]}; probe.test_behavior=std::wstring_view(argv[1])==L"--probe-sync"?7:5;
            const int samples=std::wstring_view(argv[1])==L"--probe-reuse"?4:1;
            bool failed=false;
            for(int sample=0;sample<samples;++sample) {
            const auto begin=GetTickCount64(); const auto result=isolated_file_menu(probe);
            std::cout<<"Real menu query: elapsed_ms="<<GetTickCount64()-begin<<" timeout="<<result.timed_out<<" invoked="<<result.invoked<<" status="<<std::hex<<result.status<<std::dec<<" parent_ticks="<<ticks
                <<" sample="<<sample<<" reused="<<result.reused<<" host_pid="<<result.host_pid
                <<" stage="<<static_cast<int>(result.stage)<<" preparation_ms="<<result.preparation_ms<<" launch_ms="<<result.launch_ms<<" initialize_ms="<<result.initialize_ms
                <<" resolve_ms="<<result.resolve_ms<<" bind_ms="<<result.bind_ms<<" query_ms="<<result.query_ms<<" stage_ms="<<result.stage_ms<<std::endl;
            failed|=FAILED(result.status);
            if(sample+1<samples) Sleep(200);
            }
            if(std::wstring_view(argv[1])==L"--probe-idle") Sleep(6000);
            DestroyWindow(window); std::filesystem::remove(root); return failed?1:0;
        }
        FileMenuRequest request; request.owner=window; request.paths={(root/L"用户文件 with spaces.txt").wstring()}; request.transfers={{0x8005,L"另一个抽屉"}};
        request.test_behavior=13;
        auto result=isolated_file_menu(request);
        require(SUCCEEDED(result.status) && result.command==0x7002,"helper startup must not hide a Shell window using the default show state");
        request.test_behavior=3;
        result=isolated_file_menu(request); require(result.status==S_OK && result.command==0x7002 && !result.invoked,"command IPC round trip with Unicode paths");
        const auto first_pid=result.host_pid;
        Sleep(50); auto other=request; other.paths={(root/L"other.txt").wstring()}; other.transfers={{0x8006,L"新的抽屉"}};
        result=isolated_file_menu(other); require(result.reused && result.host_pid==first_pid && result.command==0x7002,"idle worker receives a fresh request for a different selection");
        Sleep(50); request.test_behavior=8; result=isolated_file_menu(request);
        require(SUCCEEDED(result.status),"idle retirement fixture succeeds");
        Sleep(350); request.test_behavior=3; result=isolated_file_menu(request);
        require(SUCCEEDED(result.status) && !result.reused,"expired idle worker is replaced automatically");
        Sleep(50); request.test_behavior=9; result=isolated_file_menu(request);
        require(SUCCEEDED(result.status),"completed result returns even if extension cleanup hangs");
        request.test_behavior=3; const auto cleanup_begin=GetTickCount64(); result=isolated_file_menu(request);
        require(SUCCEEDED(result.status) && !result.reused && GetTickCount64()-cleanup_begin<2000,"stuck cleanup worker replaced without waiting for it");
        Sleep(50); request.test_behavior=9; result=isolated_file_menu(request);
        HANDLE cleaning=OpenProcess(SYNCHRONIZE,FALSE,result.host_pid); require(cleaning!=nullptr,"cleanup worker handle");
        const bool retired=WaitForSingleObject(cleaning,4500)==WAIT_OBJECT_0; CloseHandle(cleaning);
        require(retired,"unchosen extension stuck releasing menu is automatically retired");
        request.test_behavior=1; const auto before=ticks; const auto began=GetTickCount64();
        result=isolated_file_menu(request,350);
        require(result.timed_out && FAILED(result.status) && GetTickCount64()-began<2500,"hung extension preparation has bounded timeout");
        require(ticks>before+2,"parent messages continue while extension is hung");
        require(result.stage==MenuLoadStage::Initializing && result.stage_ms>=250 && result.preparation_ms>=350,"timeout records actual preparation stage and elapsed time");
        const auto cached=GetTickCount64(); result=isolated_file_menu(request,350);
        require(FAILED(result.status) && result.cooldown && !result.timed_out && GetTickCount64()-cached<150,"same failed selection briefly avoids repeatedly hanging");
        auto unrelated=request; unrelated.paths={(root/L"different.txt").wstring()}; unrelated.test_behavior=3;
        result=isolated_file_menu(unrelated);
        require(SUCCEEDED(result.status) && result.command==0x7002 && !result.cooldown,"one failed selection does not block unrelated menus");
        result=isolated_file_menu(request,350); require(result.cooldown,"unrelated success does not discard failed selection cooldown");
        advance_menu_retry_clock(30001); request.test_behavior=3; result=isolated_file_menu(request);
        require(SUCCEEDED(result.status) && result.command==0x7002 && !result.cooldown,"selection automatically retries after cooldown without restart");
        retry_file_menu(); request.test_behavior=2; result=isolated_file_menu(request,2000);
        require(FAILED(result.status) && !result.invoked,"crashed helper does not crash parent or invoke command");
        result=isolated_file_menu(request); require(result.cooldown,"crashed helper also has bounded cooldown");
        retry_file_menu(); request.test_behavior=4;
        result=isolated_file_menu(request,2000); require(result.invoked && SUCCEEDED(result.status),"running command released from preparation timeout");
        const auto invoking_pid=result.host_pid;
        result=isolated_file_menu(other);
        require(!result.reused && result.host_pid!=invoking_pid && SUCCEEDED(result.status),"next menu cannot reuse or terminate a running file operation");
        const auto deadline=GetTickCount64()+2500;
        while(!std::filesystem::exists(request.paths[0]) && GetTickCount64()<deadline) Sleep(20);
        require(std::filesystem::exists(request.paths[0]),"helper survives parent return to finish chosen operation");
        request.paths={(root/L"asynchronous.txt").wstring()}; request.test_behavior=6;
        const auto async_begin=GetTickCount64(); result=isolated_file_menu(request,2000);
        require(result.invoked && SUCCEEDED(result.status),"asynchronous Shell command detached from parent");
        while(!std::filesystem::exists(request.paths[0]) && GetTickCount64()-async_begin<2500) Sleep(20);
        require(std::filesystem::exists(request.paths[0]) && GetTickCount64()-async_begin>=350,"Shell process reference keeps helper alive until released");
        request.paths={(root/L"thread-reference.txt").wstring()}; request.test_behavior=11;
        const auto thread_begin=GetTickCount64(); result=isolated_file_menu(request,2000);
        require(result.invoked && SUCCEEDED(result.status),"asynchronous Shell command can acquire a calling-thread reference");
        while(!std::filesystem::exists(request.paths[0]) && GetTickCount64()-thread_begin<2500) Sleep(20);
        require(std::filesystem::exists(request.paths[0]) && GetTickCount64()-thread_begin>=350,"Shell thread reference retains the helper until background work completes");
        request.paths={(root/L"missing-parent"/L"missing.txt").wstring()}; request.test_behavior=5;
        result=isolated_file_menu(request);
        require(FAILED(result.status) && result.status!=E_FAIL && !result.timed_out && result.stage==MenuLoadStage::Resolving,"missing path preserves the original Shell HRESULT and stage");
        request.test_behavior=3; result=isolated_file_menu(request);
        require(SUCCEEDED(result.status) && !result.cooldown,"ordinary Shell failure does not latch the selection unavailable");
        retry_file_menu(); request.paths={L"relative.txt"}; result=isolated_file_menu(request); require(FAILED(result.status),"relative request rejected");
        require(file_menu_host(L"--shell-menu=0")==1 && file_menu_host(L"--shell-menu=invalid")==1,"invalid helper handles rejected");
        DestroyWindow(window); window=nullptr;
        require(std::filesystem::equivalent(root.parent_path(),std::filesystem::temp_directory_path()) && root.filename().wstring().starts_with(L"EdgeTuck-menu-test-"),"cleanup restricted to owned fixture");
        std::filesystem::remove_all(root);
        std::cout<<"Menu isolation: PASS; "<<checks<<" checks; timeout, responsive parent, crash, retry, IPC, operation lifetime\n"; return 0;
    } catch(const std::exception& e) { if(window) DestroyWindow(window); std::cerr<<"FAIL: "<<e.what()<<"; owned fixtures retained\n"; return 1; }
}
