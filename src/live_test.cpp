#include "app.hpp"
#include "diagnostics.hpp"
#include <iostream>
#include <cmath>

namespace edge {
namespace {
LRESULT CALLBACK background_proc(HWND hwnd,UINT message,WPARAM wp,LPARAM lp) {
    if(message==WM_PAINT) {
        PAINTSTRUCT ps{}; const HDC dc=BeginPaint(hwnd,&ps); RECT r{}; GetClientRect(hwnd,&r);
        HBRUSH brush=CreateSolidBrush(static_cast<COLORREF>(GetWindowLongPtrW(hwnd,GWLP_USERDATA)));
        FillRect(dc,&r,brush); DeleteObject(brush);
        const auto color=GetWindowLongPtrW(hwnd,GWLP_USERDATA);
        if(color&0x1000000) for(int y=0;y<r.bottom;y+=40) for(int x=0;x<r.right;x+=40) {
            RECT tile{x,y,x+40,y+40}; const auto tile_brush=CreateSolidBrush(RGB(color&255,40+y/40*5,30+x/40*4));
            FillRect(dc,&tile,tile_brush); DeleteObject(tile_brush);
        }
        EndPaint(hwnd,&ps); return 0;
    }
    return DefWindowProcW(hwnd,message,wp,lp);
}
bool color_matches(DWORD value,int r,int g,int b) {
    return std::abs(static_cast<int>((value>>16)&255)-r)<8 && std::abs(static_cast<int>((value>>8)&255)-g)<8 && std::abs(static_cast<int>(value&255)-b)<8;
}
}
void App::live_test_tick() {
    ++smoke_step;
    auto* engine=graphics.composition.get();
    const auto verify=[&](bool value,const char* name) { if(!value) { smoke_ok=false; std::cout<<"LIVE FAIL stage "<<smoke_step<<": "<<name<<std::endl; } };
    try {
        if(smoke_step==1) {
            verify(engine!=nullptr,"composition required"); if(!engine) throw std::runtime_error("Composition unavailable");
            show_control();
            WNDCLASSW wc{}; wc.lpfnWndProc=background_proc; wc.hInstance=instance; wc.lpszClassName=L"EdgeTuck.LiveTestBackground"; RegisterClassW(&wc);
            test_background=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,wc.lpszClassName,L"Owned realtime test background",WS_POPUP,
                work.left,work.top,work.right-work.left,work.bottom-work.top,nullptr,nullptr,instance,nullptr);
            SetWindowLongPtrW(test_background,GWLP_USERDATA,RGB(32,100,168)); ShowWindow(test_background,SW_SHOWNOACTIVATE); UpdateWindow(test_background);
            engine->live_source_for_test(test_background);
            auto& first=drawers.front(); first->pinned=true; first->preview=true; first->set_open(true); desktop_order();
            for(auto& d:drawers) { DWORD affinity{}; verify(GetWindowDisplayAffinity(d->panel,&affinity) && affinity==WDA_NONE,"desktop source needs no drawer capture exclusion"); }
        }
        if(smoke_step==2 || smoke_step==4 || smoke_step==5 || smoke_step==6 || smoke_step==8) {
            auto& d=smoke_step<=4?drawers.front():drawers.back();
            const auto* surface=d->panel_canvas.surface.get();
            verify(surface && surface->has_refraction(),"live optical material available");
            if(surface && surface->has_refraction()) {
                const int x=(d->bounds.right-d->bounds.left)/2,y=(d->bounds.bottom-d->bounds.top)/2;
                const DWORD source=surface->probe_pixel(x,y,false),material=surface->probe_pixel(x,y,true);
                const int r=smoke_step==2?32:smoke_step==8?35:190;
                const int g=smoke_step==2?100:smoke_step==8?175:64;
                const int b=smoke_step==2?168:smoke_step==8?70:90;
                verify(color_matches(source,r,g,b),"current background color without drawer feedback");
                verify(color_matches(material,r,g,b),"live background reaches optical shader");
                std::cout<<"Live stage "<<smoke_step<<" source="<<std::hex<<source<<" optical="<<material<<std::dec<<" frames="<<engine->live_frames()<<std::endl;
            }
        }
        if(smoke_step==3) { SetWindowLongPtrW(test_background,GWLP_USERDATA,RGB(190,64,90)); InvalidateRect(test_background,nullptr,FALSE); UpdateWindow(test_background); }
        if(smoke_step==4) {
            auto& old=drawers.front(); auto& next=drawers.back();
            next->model().start=capacity(Edge::Top)-next->model().span; next->update_geometry();
            old->pinned=false; old->set_open(false);
            next->pinned=true; next->preview=true; next->set_open(true);
            verify(old->collapsed(),"corner handoff has only one expanded body");
        }
        if(smoke_step==5) { auto& d=drawers.back(); --d->model().start; ++d->model().depth; d->update_geometry(); }
        if(smoke_step==6) {
            for(auto& d:drawers) d->preview=false;
            test_occluder=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,L"EdgeTuck.LiveTestBackground",L"Owned full desktop cover",WS_POPUP,
                work.left,work.top,work.right-work.left,work.bottom-work.top,nullptr,nullptr,instance,nullptr);
            SetWindowLongPtrW(test_occluder,GWLP_USERDATA,RGB(245,0,245));
            ShowWindow(test_occluder,SW_SHOWNOACTIVATE); UpdateWindow(test_occluder);
            SetWindowPos(test_occluder,HWND_TOP,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
            sync_live_capture(); verify(!engine->live_active(),"covered drawer pauses capture");
        }
        if(smoke_step==7) {
            ShowWindow(test_occluder,SW_HIDE);
            SetWindowLongPtrW(test_background,GWLP_USERDATA,RGB(35,175,70)); InvalidateRect(test_background,nullptr,FALSE); UpdateWindow(test_background);
            drawers.back()->preview=true; desktop_order(); verify(engine->live_active(),"uncovered drawer resumes capture");
        }
        if(smoke_step==8) { for(auto& d:drawers) { d->pinned=false; d->set_open(false); d->finish_animation(d->motion_serial); } }
        if(smoke_step==9) {
            verify(engine->live_active() && engine->has_retained_scene(),"visible collapsed glass keeps the live scene");
            for(const auto& d:drawers) verify(d->panel_canvas.surface && d->panel_canvas.surface->has_refraction(),"collapsed edge keeps liquid material");
            verify(engine->live_frames()>=3,"multiple real GPU background frames received");
            verify(engine->scene_capture_count()==0,"no GDI snapshots in realtime mode");
            settings.live_background=false; apply_live_mode();
            for(auto& d:drawers) { DWORD affinity{}; verify(GetWindowDisplayAffinity(d->panel,&affinity) && affinity==WDA_NONE,"compatibility mode restores capture visibility"); }
            auto& d=drawers.front(); d->pinned=true; d->preview=true; d->set_open(true); d->finish_animation(d->motion_serial);
            verify(d->panel_canvas.surface->has_refraction() && engine->scene_capture_count()==1,"compatibility mode still supplies optical material");
            settings.live_background=true; apply_live_mode();
            for(auto& drawer:drawers) { DWORD affinity{}; verify(GetWindowDisplayAffinity(drawer->panel,&affinity) && affinity==WDA_NONE,"realtime toggle keeps drawers visible to capture"); }
            d->preview=true; desktop_order();
            SetWindowLongPtrW(test_background,GWLP_USERDATA,RGB(32,100,168)); InvalidateRect(test_background,nullptr,FALSE); UpdateWindow(test_background);
        }
        if(smoke_step==10) {
            auto& d=drawers.front(); const auto* surface=d->panel_canvas.surface.get();
            verify(engine->live_active() && surface && surface->has_refraction(),"realtime resumes after mode switch");
            if(surface && surface->has_refraction()) verify(color_matches(surface->probe_pixel((d->bounds.right-d->bounds.left)/2,(d->bounds.bottom-d->bounds.top)/2,true),32,100,168),"mode switch uses fresh background");
            d->set_open(false);
            verify(d->panel_canvas.surface->has_refraction(),"closing keeps the optical material until the animation ends");
            d->set_open(true);
            verify(d->panel_canvas.surface->has_refraction(),"reversing does not flash system blur");
            std::cout<<"Native realtime: "<<(smoke_ok?"PASS":"FAIL")<<"; frames="<<engine->live_frames()<<"; error="<<std::hex<<engine->live_error()<<std::dec<<std::endl;
            if(stress_test) SetTimer(broker,70,16,nullptr);
            else SetTimer(broker,70,60,nullptr);
        }
        if(!stress_test && smoke_step>=11 && smoke_step<=28) {
            const int side=(smoke_step-11)/6,phase=(smoke_step-11)%6;
            auto& d=drawers.front();
            const auto pattern=[&](int red) {
                SetWindowLongPtrW(test_background,GWLP_USERDATA,0x1000000|red);
                InvalidateRect(test_background,nullptr,FALSE); UpdateWindow(test_background);
            };
            const auto partial_cover=[&] {
                SetWindowPos(test_background,HWND_TOP,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
                SetWindowPos(d->panel,HWND_TOP,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
                SetWindowPos(test_occluder,HWND_TOP,d->bounds.left,d->bounds.top,d->bounds.right-d->bounds.left-18,d->bounds.bottom-d->bounds.top,SWP_NOACTIVATE|SWP_SHOWWINDOW);
                sync_live_capture(); verify(engine->live_active(),"a narrow uncovered strip keeps capture active");
            };
            const auto pixels=[&](int red) {
                auto* surface=d->panel_canvas.surface.get();
                verify(surface && surface->has_refraction(),"partial cover keeps liquid glass");
                if(!surface || !surface->has_refraction()) return;
                const POINT origin=surface->sample_origin_for_test();
                const int x=std::clamp<int>(static_cast<int>(origin.x-work.left)+60,0,work.right-work.left-1);
                const int y=std::clamp<int>(static_cast<int>(origin.y-work.top)+60,0,work.bottom-work.top-1);
                const auto pixel=surface->probe_pixel(60,60,false);
                verify(color_matches(pixel,red,40+y/40*5,30+x/40*4),"sliding sample uses current desktop coordinates and live pixels, never the magenta cover");
                const int vx=side==0?d->bounds.right-d->bounds.left-60:60;
                const int vy=side==2?d->bounds.bottom-d->bounds.top-60:60;
                const int sx=std::clamp<int>(origin.x-work.left+vx,0,work.right-work.left-1),sy=std::clamp<int>(origin.y-work.top+vy,0,work.bottom-work.top-1);
                verify(color_matches(surface->probe_pixel(vx,vy,true),red,40+sy/40*5,30+sx/40*4),"visible moving glass samples the same desktop position through the optical shader");
                std::cout<<"Layering side="<<side<<" phase="<<phase<<" origin="<<origin.x<<','<<origin.y<<" pixel="<<std::hex<<pixel<<std::dec<<std::endl;
            };
            if(phase==0) {
                pattern(50); d->model().edge=static_cast<Edge>(side); d->model().start=2; d->model().span=4; d->model().depth=5;
                d->update_geometry(); d->pinned=true; d->preview=true; d->set_open(true); d->finish_animation(d->motion_serial); partial_cover();
                SetTimer(broker,70,250,nullptr); // Await the asynchronous capture's first frame.
            }
            if(phase==1) {
                SetTimer(broker,70,60,nullptr);
                pixels(50); pattern(80); d->set_open(false); partial_cover();
            }
            if(phase==2) {
                pixels(80);
                const auto origin=d->panel_canvas.surface->sample_origin_for_test();
                verify(origin.x!=d->bounds.left || origin.y!=d->bounds.top,"closing changes the sample origin before the endpoint");
                pattern(110); d->set_open(true); partial_cover();
            }
            if(phase==3) { pixels(110); }
            if(phase==4) {
                d->finish_animation(d->motion_serial); engine->live_frame(); pixels(110);
                SetWindowPos(test_occluder,HWND_TOP,work.left,work.top,work.right-work.left,work.bottom-work.top,SWP_NOACTIVATE);
                sync_live_capture(); verify(!engine->live_active(),"full desktop cover pauses capture");
                SetWindowPos(test_occluder,HWND_TOP,d->bounds.left,d->bounds.top,d->bounds.right-d->bounds.left-18,d->bounds.bottom-d->bounds.top,SWP_NOACTIVATE);
                // Our WinEvent hook skips this process in production; deliver
                // the same event explicitly for this owned test window.
                event_proc(nullptr,EVENT_OBJECT_LOCATIONCHANGE,test_occluder,OBJID_WINDOW,CHILDID_SELF,GetCurrentThreadId(),GetTickCount());
                pattern(140);
                SetTimer(broker,70,250,nullptr); // A paused session is reopened asynchronously.
            }
            if(phase==5) { verify(engine->live_active(),"restoring the same covering window resumes capture via its location event"); pixels(140); ShowWindow(test_occluder,SW_HIDE); SetTimer(broker,70,60,nullptr); }
            if(smoke_step==28) std::cout<<"Native layering: "<<(smoke_ok?"PASS":"FAIL")<<"; left/right/top partial cover, live close/reverse samples, full cover recovery"<<std::endl;
        }
        if(stress_test && smoke_step>=11 && smoke_step<=130) {
            auto& d=drawers.front();
            if(smoke_step==11) {
                // Model a frame notification dispatched during a foreground
                // update (COM/Shell calls can re-enter the window thread).
                if(d->panel_canvas.begin(d->panel)) {
                    d->panel_canvas.clear(D2D1::ColorF(0,0.0f));
                    Sleep(20); engine->live_frame(); d->panel_canvas.end();
                    verify(diagnostic_errors()==0,"reentrant background update defers until content draw finishes");
                }
            }
            if(smoke_step%20==11) { show_material(); }
            if(smoke_step%20==15 && material_window) SendMessageW(material_window,WM_CLOSE,0,0);
            d->pinned=true; d->preview=true; d->set_open(true); d->finish_animation(d->motion_serial);
            for(int i=0;i<6;++i) {
                d->model().edge=static_cast<Edge>((smoke_step/40)%3);
                d->model().start=(smoke_step+i)%5;
                d->model().span=3+(smoke_step+i)%3;
                d->model().depth=3+(smoke_step+i)%5;
                d->update_geometry();
                verify(d->panel_canvas.surface!=nullptr,"foreground surface survives rapid geometry");
                verify((d->content_probe>>24)>240,"foreground menu glyph remains visible");
                if(d->panel_canvas.surface) verify(d->panel_canvas.surface->has_refraction(),"optical material survives rapid geometry");
            }
            if(smoke_step==130) {
                SetTimer(broker,70,200,nullptr);
            }
        }
        if(stress_test && smoke_step>=131) {
            auto& d=drawers.front();
            if(smoke_step==131 || smoke_step==133) {
                if(smoke_step==133) { d->pinned=false; d->set_open(false); d->finish_animation(d->motion_serial); }
                const auto before=d->panel_canvas.generation;
                d->panel_canvas.reset(); d->paint();
                verify(d->panel_canvas.surface && d->panel_canvas.generation>before,"surface rebuild recreates content");
                RECT actual{}; GetWindowRect(d->panel,&actual);
                verify(EqualRect(&actual,&d->bounds),"surface rebuild does not move HWND offscreen");
                verify((d->content_probe>>24)>240,"surface rebuild restores the foreground menu glyph");
            }
            if(smoke_step==132 || smoke_step==135) {
                verify(d->panel_canvas.surface && d->panel_canvas.surface->has_refraction(),"live lens resumes after full surface rebuild");
            }
            if(smoke_step==134) { d->pinned=true; d->preview=true; d->set_open(true); desktop_order(); }
            if(smoke_step==135) {
                verify(diagnostic_errors()==0,"no graphics errors under rapid movement");
                std::cout<<"Native stress: "<<(smoke_ok?"PASS":"FAIL")<<"; 720 geometry changes, reentrant frame, 6 material dialogs, open/closed surface rebuild, foreground pixels; graphics_errors="<<diagnostic_errors()<<std::endl;
            }
        }
    } catch(...) { smoke_ok=false; std::cout<<"Native realtime exception at stage "<<smoke_step<<std::endl; smoke_step=stress_test?135:28; }
    if(smoke_step>=(stress_test?135:28)) {
        quitting=true;
        engine->live_active(false,broker,WM_EDGE_LIVE_FRAME);
        if(test_occluder) { DestroyWindow(test_occluder); test_occluder=nullptr; }
        if(test_background) { DestroyWindow(test_background); test_background=nullptr; }
        KillTimer(broker,70); quitting=true; PostQuitMessage(smoke_ok?0:2);
    }
}
}
