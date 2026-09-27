#include "composition.hpp"
#include "motion.hpp"
#include <d2d1helper.h>
#include <iostream>
#include <stdexcept>
#include <winrt/base.h>

static void require(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
static bool completes_without_capture(HWND window,UINT message,UINT_PTR serial) {
    const auto deadline=GetTickCount64()+900;
    while(GetTickCount64()<deadline) {
        MSG event{};
        while(PeekMessageW(&event,nullptr,0,0,PM_REMOVE)) {
            if(event.hwnd==window && event.message==message && event.wParam==serial) return true;
            TranslateMessage(&event); DispatchMessageW(&event);
        }
        MsgWaitForMultipleObjectsEx(0,nullptr,10,QS_ALLINPUT,MWMO_INPUTAVAILABLE);
    }
    return false;
}
int main() {
    try {
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        edge::CompositionEngine engine; engine.live_mode(true);
        // Hidden owned windows and synthetic frames: no wallpaper replacement,
        // user input, Explorer operation or visible flashing test background.
        for(int side=0;side<3;++side) for(float dpi:{96.0f,144.0f}) {
            const int w=static_cast<int>(400*dpi/96),h=static_cast<int>(300*dpi/96);
            const HWND hwnd=CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP|WS_EX_TOOLWINDOW,L"STATIC",L"EdgeTuck compositor test",WS_POPUP,0,0,w,h,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
            require(hwnd!=nullptr,"owned window creation");
            {
                auto surface=engine.create(hwnd); surface->appearance(true,false);
                POINT draw_offset{}; auto target=surface->begin(w,h,dpi,draw_offset);
                target->Clear(D2D1::ColorF(1,0,0,1)); surface->end(); target.Reset();
                const auto closed=edge::closed_offset(side,static_cast<float>(w),static_cast<float>(h),14*dpi/96);
                surface->position(closed.x,closed.y); surface->capture_backdrop(side);
                engine.frame_for_test({0,0,1000,800},0xFF3060A0);
                require(surface->has_refraction() && !surface->has_live_blur(),"startup collapsed edge uses optical material");
                require(surface->content_opacity_for_test()==0,"opaque foreground stays invisible while closed");
                auto crop=surface->update_region_for_test();
                require(side==2?crop.bottom-crop.top==static_cast<int>(14*dpi/96):crop.right-crop.left==static_cast<int>(14*dpi/96),"only exposed edge is shaded");
                const int x=(crop.left+crop.right)/2,y=(crop.top+crop.bottom)/2;
                const DWORD before=surface->probe_pixel(x,y,true);
                engine.frame_for_test({0,0,1000,800},0xFFA04020);
                require(surface->probe_pixel(x,y,true)!=before,"collapsed optical edge follows a new frame");
                surface->slide_to(0,0,1,WM_APP+1,1); Sleep(3); engine.frame_for_test({0,0,1000,800},0xFFA04020);
                require(surface->content_opacity_for_test()==1,"opening restores prepainted foreground");
                surface->position(0,0); surface->slide_to(closed.x,closed.y,1,WM_APP+1,2); Sleep(3);
                engine.frame_for_test({0,0,1000,800},0xFF408060);
                require(surface->content_opacity_for_test()==0 && surface->has_refraction(),"closing hides all content and retains liquid material");
                surface->material(edge::glass_preset(2));
                require(surface->has_refraction(),"closed material adjustment retains optical lens");
                target=surface->begin(w,h,dpi,draw_offset);
                target->Clear(D2D1::ColorF(1,1,1,1)); surface->end(); target.Reset();
                require(surface->content_opacity_for_test()==0,"asynchronous content repaint cannot expose closed labels or selection");
                surface->appearance(false,false); require(!surface->has_refraction(),"solid material releases optical subscription");
                // Live background remains selected in settings, but solid material
                // has no capture session or incoming desktop frames to drive motion.
                require(!engine.live_active(),"solid regression runs without a capture session");
                surface->slide_to(0,0,80,WM_APP+42,3);
                require(completes_without_capture(hwnd,WM_APP+42,3),"solid opening completes without any background frames");
                surface->position(0,0);
                surface->slide_to(closed.x,closed.y,80,WM_APP+42,4);
                require(completes_without_capture(hwnd,WM_APP+42,4),"solid closing completes without any background frames");
                surface->position(closed.x,closed.y);
                surface->slide_to(0,0,80,WM_APP+42,5);
                Sleep(20);
                surface->slide_to(closed.x,closed.y,80,WM_APP+42,6);
                require(completes_without_capture(hwnd,WM_APP+42,6),"solid reversal completes without any background frames");
                surface->position(closed.x,closed.y);
                surface->appearance(true,false); surface->capture_backdrop(side);
                engine.frame_for_test({0,0,1000,800},0xFF408060);
                require(surface->has_refraction() && surface->content_opacity_for_test()==0,"theme changes recover closed liquid glass without content");
                surface->position(0,0); engine.frame_for_test({0,0,1000,800},0xFF408060);
                crop=surface->update_region_for_test(); require(crop.right==w && crop.bottom==h,"reopening updates full material surface");
            }
            DestroyWindow(hwnd);
        }
        std::cout<<"PASS: collapsed live optical material, fresh frames, foreground hiding, solid open/close/reversal without capture, material changes and update regions on three edges at 100/150% DPI\n";
        return 0;
    } catch(const winrt::hresult_error& error) { std::cerr<<"FAIL HRESULT "<<std::hex<<static_cast<unsigned long>(error.code().value)<<'\n'; return 1; }
      catch(const std::exception& error) { std::cerr<<"FAIL: "<<error.what()<<'\n'; return 1; }
}
