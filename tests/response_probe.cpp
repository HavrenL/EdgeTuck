#include "app.hpp"
#include <iostream>
#include <fstream>
#include <chrono>
#include <cstddef>
#include <dwmapi.h>

// Bounded manual probe: reads runtime state, opens/closes one exposed drawer
// with the real cursor, then restores the pointer. Never changes configuration.
static HANDLE process{};
template<class T> T read(uintptr_t address) {
    T value{}; SIZE_T length{};
    if(!ReadProcessMemory(process,reinterpret_cast<void*>(address),&value,sizeof(value),&length) || length!=sizeof(value))
        throw std::runtime_error("Cannot read running state");
    return value;
}
int wmain(int argc,wchar_t** argv) {
    if(argc!=2) return 2;
    if(std::wstring(argv[1])==L"--clock") {
        DEVMODEW display{}; display.dmSize=sizeof(display);
        if(EnumDisplaySettingsW(nullptr,ENUM_CURRENT_SETTINGS,&display)) std::cout<<"display_hz="<<display.dmDisplayFrequency<<'\n';
        auto tick=GetTickCount64(); const auto start=std::chrono::steady_clock::now();
        unsigned changes=0;
        while(std::chrono::steady_clock::now()-start<std::chrono::milliseconds(120)) {
            const auto next=GetTickCount64();
            if(next!=tick) { std::cout<<"tick_step_ms="<<next-tick<<'\n'; tick=next; ++changes; }
            YieldProcessor();
        }
        std::cout<<"tick_changes="<<changes<<" in 120 ms\n";
        const auto precise_start=edge::MotionClock::now(); const auto coarse_start=GetTickCount64();
        float old_position=-1,new_position=-1; int old_repeats=0,new_repeats=0;
        for(int frame=0;frame<24;++frame) {
            if(FAILED(DwmFlush())) break;
            const float precise=edge::motion_elapsed(precise_start),coarse=static_cast<float>(GetTickCount64()-coarse_start);
            if(precise>=180) break;
            const float old_next=edge::motion_progress(0,1,coarse,180),new_next=edge::motion_progress(0,1,precise,180);
            old_repeats+=old_next==old_position; new_repeats+=new_next==new_position;
            old_position=old_next; new_position=new_next;
            std::cout<<"cadence_sample="<<frame<<" precise_ms="<<precise<<" coarse_ms="<<coarse<<" old_position="<<old_next<<" new_position="<<new_next<<'\n';
        }
        std::cout<<"repeated_positions_old="<<old_repeats<<" new="<<new_repeats<<" (clock sample, not displayed-frame measurement)\n";
        return 0;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    POINT original{}; GetCursorPos(&original);
    struct Restore { POINT p; ~Restore() { SetCursorPos(p.x,p.y); if(process) CloseHandle(process); } } restore{original};
    try {
        HWND control=FindWindowW(L"EdgeTuck.Control",nullptr); DWORD pid{};
        if(!control || !GetWindowThreadProcessId(control,&pid)) throw std::runtime_error("No running EdgeTuck");
        process=OpenProcess(PROCESS_VM_READ|PROCESS_QUERY_INFORMATION,FALSE,pid);
        const auto app=static_cast<uintptr_t>(GetWindowLongPtrW(control,GWLP_USERDATA));
        const auto windows=app+offsetof(edge::App,drawers);
        const auto first=read<uintptr_t>(windows),last=read<uintptr_t>(windows+8);
        if(last<first || (last-first)/sizeof(uintptr_t)>32) throw std::runtime_error("Drawer layout mismatch");
        uintptr_t drawer{}; POINT enter{},leave{};
        for(auto slot=first;slot<last && !drawer;slot+=sizeof(uintptr_t)) {
            const auto candidate=read<uintptr_t>(slot);
            if(read<float>(candidate+offsetof(edge::Drawer,target))!=0 || read<bool>(candidate+offsetof(edge::Drawer,animating))) continue;
            const auto hwnd=read<HWND>(candidate+offsetof(edge::Drawer,panel));
            RECT bounds{},hit{}; GetWindowRect(hwnd,&bounds);
            HRGN region=CreateRectRgn(0,0,0,0); const int kind=GetWindowRgn(hwnd,region); GetRgnBox(region,&hit); DeleteObject(region);
            if(kind==ERROR) continue;
            for(int i=1;i<6;++i) {
                POINT point{bounds.left+hit.left+(hit.right-hit.left)*i/6,bounds.top+hit.top+(hit.bottom-hit.top)*i/6};
                if(GetAncestor(WindowFromPoint(point),GA_ROOT)!=hwnd) continue;
                POINT outside=point;
                if(hit.right-hit.left<40) outside.x=hit.left>0?bounds.left-30:bounds.right+30;
                else outside.y=bounds.bottom+30;
                if(GetAncestor(WindowFromPoint(outside),GA_ROOT)==hwnd) continue;
                drawer=candidate; enter=point; leave=outside; break;
            }
        }
        if(!drawer) throw std::runtime_error("No uncovered closed drawer handle; nothing changed");
        std::ofstream log{std::filesystem::path(argv[1])};
        log<<"pid="<<pid<<" drawer="<<read<int>(drawer+offsetof(edge::Drawer,id))<<"\n";
        using Clock=std::chrono::steady_clock;
        for(int cycle=0;cycle<5;++cycle) {
            SetCursorPos(leave.x,leave.y); Sleep(350);
            for(bool opening:{true,false}) {
                const auto start=Clock::now(); const auto before=read<int>(app+offsetof(edge::App,paint_count));
                const auto point=opening?enter:leave; SetCursorPos(point.x,point.y);
                double target_ms=-1,finish_ms=-1;
                while(std::chrono::duration<double,std::milli>(Clock::now()-start).count()<750) {
                    const auto target=read<float>(drawer+offsetof(edge::Drawer,target));
                    const auto progress=read<float>(drawer+offsetof(edge::Drawer,progress));
                    const double elapsed=std::chrono::duration<double,std::milli>(Clock::now()-start).count();
                    if(target==(opening?1.0f:0.0f) && target_ms<0) target_ms=elapsed;
                    if(target_ms>=0 && progress==(opening?1.0f:0.0f) && !read<bool>(drawer+offsetof(edge::Drawer,animating))) { finish_ms=elapsed; break; }
                    Sleep(1);
                }
                const auto paints=read<int>(app+offsetof(edge::App,paint_count))-before;
                log<<"cycle="<<cycle<<" open="<<opening<<" target_ms="<<target_ms<<" finish_ms="<<finish_ms<<" paints="<<paints<<"\n";
                Sleep(100);
            }
        }
        std::cout<<"Response sample saved\n";
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
    return 0;
}
