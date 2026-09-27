#include "app.hpp"
#include <commctrl.h>
#include <windowsx.h>
#include <array>
#include <cmath>

namespace edge {
namespace {
constexpr wchar_t material_class[] = L"EdgeTuck.Material";
struct MaterialWindow {
    App* app{};
    HFONT font{};
    HWND window{};
    std::array<HWND,4> sliders{}, values{};
    std::array<HWND,3> checks{}, presets{};
    HWND summary{}, realtime{};
    int preview_id{};
    bool was_pinned{};
    float scale{1};
    HWND child(LPCWSTR cls,LPCWSTR text,DWORD style,int id,int x,int y,int w,int h) {
        HWND result=CreateWindowExW(0,cls,text,WS_CHILD|WS_VISIBLE|style,
            static_cast<int>(x*scale),static_cast<int>(y*scale),static_cast<int>(w*scale),static_cast<int>(h*scale),
            window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),app->instance,nullptr);
        SendMessageW(result,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE); return result;
    }
    void sync() {
        SendMessageW(realtime,BM_SETCHECK,app->settings.live_background?BST_CHECKED:BST_UNCHECKED,0);
        const auto& m=app->settings.material;
        const float value[]{m.blur,m.depth,m.light,m.dispersion}, multiplier[]{100,1,100,100};
        for(int i=0;i<4;++i) {
            SendMessageW(sliders[i],TBM_SETPOS,TRUE,static_cast<LPARAM>(std::lround(value[i]*multiplier[i])));
            wchar_t text[32]{}; swprintf_s(text,L"%.2f",value[i]); SetWindowTextW(values[i],text);
        }
        const bool toggles[]{m.refraction,m.lighting,m.chromatic};
        std::wstring name=L"自定义";
        for(int i=0;i<3;++i) {
            SendMessageW(checks[i],BM_SETCHECK,toggles[i]?BST_CHECKED:BST_UNCHECKED,0);
            SendMessageW(presets[i],BM_SETCHECK,m==glass_preset(i)?BST_CHECKED:BST_UNCHECKED,0);
            if(m==glass_preset(i)) name=i==0?L"清透":i==1?L"柔和":L"晶亮";
        }
        SetWindowTextW(summary,(L"当前："+name+L"。修改即时应用，关闭后自动保存。").c_str());
    }
    void apply() {
        for(auto& d:app->drawers) {
            d->panel_canvas.material=app->settings.material;
            InvalidateRect(d->panel,nullptr,FALSE);
            UpdateWindow(d->panel); // Canvas owns device-loss fallback handling.
        }
        sync(); app->invalidate();
    }
    void preview() {
        KillTimer(window,9);
        for(auto& d:app->drawers) if(d->id==preview_id) { d->pinned=was_pinned; d->preview=false; d->set_open(false); }
        preview_id=0;
        if(app->drawers.empty()) return;
        auto& d=app->drawers.front();
        preview_id=d->id; was_pinned=d->pinned;
        if(!d->collapsed()) { d->set_open(false); SetTimer(window,9,220,nullptr); }
        else open_preview();
    }
    void open_preview() {
        for(auto& d:app->drawers) if(d->id==preview_id) {
            UpdateWindow(d->panel);
            d->pinned=true; d->preview=true; d->set_open(true); app->desktop_order();
        }
    }
    void create(HWND hwnd) {
        window=hwnd; scale=GetDpiForWindow(hwnd)/96.0f;
        font=CreateFontW(-static_cast<int>(14*scale),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Microsoft YaHei UI");
        child(L"STATIC",L"玻璃的通透、弯曲和光线，由你决定。",0,0,24,20,450,26);
        const LPCWSTR names[]{L"清透",L"柔和",L"晶亮"};
        for(int i=0;i<3;++i) presets[i]=child(L"BUTTON",names[i],BS_AUTORADIOBUTTON|BS_PUSHLIKE|WS_TABSTOP,110+i,24+i*152,62,140,34);
        const LPCWSTR labels[]{L"背景模糊",L"边缘弯曲",L"边缘亮度",L"细微色散"};
        const int maximum[]{600,36,150,200};
        for(int i=0;i<4;++i) {
            const int y=116+i*69;
            child(L"STATIC",labels[i],0,0,26,y,230,22);
            values[i]=child(L"STATIC",L"",SS_RIGHT,0,393,y,75,22);
            sliders[i]=child(TRACKBAR_CLASSW,L"",TBS_HORZ|TBS_NOTICKS|WS_TABSTOP,200+i,23,y+24,450,28);
            SendMessageW(sliders[i],TBM_SETRANGE,TRUE,MAKELPARAM(0,maximum[i]));
            SendMessageW(sliders[i],TBM_SETPAGESIZE,0,i==1?3:10);
        }
        const LPCWSTR toggle_names[]{L"曲面折射",L"边缘反光与散射",L"细微色散"};
        for(int i=0;i<3;++i) checks[i]=child(L"BUTTON",toggle_names[i],BS_AUTOCHECKBOX|WS_TABSTOP,300+i,25+i*155,400,i==1?155:140,28);
        summary=child(L"STATIC",L"",0,0,25,443,450,24);
        realtime=child(L"BUTTON",L"实时背景（动态壁纸）",BS_AUTOCHECKBOX|WS_TABSTOP,303,25,477,440,28);
        child(L"STATIC",L"实时跟随桌面变化，遮住部分也会继续更新。",0,0,25,508,445,24);
        child(L"BUTTON",L"重新预览抽屉",BS_PUSHBUTTON|WS_TABSTOP,401,25,552,150,34);
        child(L"BUTTON",L"完成",BS_DEFPUSHBUTTON|WS_TABSTOP,IDOK,367,552,100,34);
        sync();
    }
};
LRESULT CALLBACK material_proc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp) {
    auto* state=reinterpret_cast<MaterialWindow*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
    if(msg==WM_NCCREATE) {
        state=static_cast<MaterialWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(state));
    }
    if(!state) return DefWindowProcW(hwnd,msg,wp,lp);
    switch(msg) {
    case WM_CREATE: state->create(hwnd); return 0;
    case WM_TIMER: if(wp==9) { KillTimer(hwnd,9); state->open_preview(); } return 0;
    case WM_HSCROLL: {
        auto& m=state->app->settings.material;
        float* fields[]{&m.blur,&m.depth,&m.light,&m.dispersion};
        for(int i=0;i<4;++i) if(reinterpret_cast<HWND>(lp)==state->sliders[i])
            *fields[i]=static_cast<float>(SendMessageW(state->sliders[i],TBM_GETPOS,0,0))/(i==1?1.0f:100.0f);
        state->apply(); if(LOWORD(wp)==TB_ENDTRACK) state->app->save(); return 0;
    }
    case WM_COMMAND: {
        const int id=LOWORD(wp);
        if(id==IDOK || id==IDCANCEL) { SendMessageW(hwnd,WM_CLOSE,0,0); return 0; }
        if(id>=110 && id<=112) { state->app->settings.material=glass_preset(id-110); state->apply(); state->app->save(); }
        if(id>=300 && id<=302) {
            auto& m=state->app->settings.material; bool* flags[]{&m.refraction,&m.lighting,&m.chromatic};
            *flags[id-300]=SendMessageW(state->checks[id-300],BM_GETCHECK,0,0)==BST_CHECKED;
            state->apply(); state->app->save();
        }
        if(id==401) state->preview();
        if(id==303) { state->app->settings.live_background=SendMessageW(state->realtime,BM_GETCHECK,0,0)==BST_CHECKED; state->app->apply_live_mode(); state->sync(); }
        return 0;
    }
    case WM_CLOSE: state->app->save(); DestroyWindow(hwnd); return 0;
    case WM_DESTROY:
        for(auto& d:state->app->drawers) if(d->id==state->preview_id) { d->pinned=state->was_pinned; d->preview=false; if(!d->pinned) d->set_open(false); }
        state->app->material_window=nullptr;
        if(state->font) DeleteObject(state->font);
        return 0;
    case WM_NCDESTROY:
        SetWindowLongPtrW(hwnd,GWLP_USERDATA,0); delete state; break;
    }
    return DefWindowProcW(hwnd,msg,wp,lp);
}
}
void App::show_material() {
    if(material_window) { ShowWindow(material_window,SW_RESTORE); SetForegroundWindow(material_window); return; }
    settings.glass=true; refresh_theme();
    WNDCLASSEXW wc{sizeof(wc)}; wc.hInstance=instance; wc.lpfnWndProc=material_proc; wc.lpszClassName=material_class;
    wc.hCursor=LoadCursorW(nullptr,IDC_ARROW); wc.hbrBackground=GetSysColorBrush(COLOR_BTNFACE); wc.hIcon=icon;
    RegisterClassExW(&wc);
    auto* state=new MaterialWindow(); state->app=this;
    const DWORD style=WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU;
    RECT rect{0,0,static_cast<LONG>(495*scale),static_cast<LONG>(611*scale)};
    AdjustWindowRectExForDpi(&rect,style,FALSE,0,GetDpiForSystem());
    const int w=rect.right-rect.left,h=rect.bottom-rect.top;
    material_window=CreateWindowExW(0,material_class,L"自定义玻璃 · 轻屉",style,
        work.left+(work.right-work.left-w)/2,work.top+std::max(0L,(work.bottom-work.top-h)/2),w,h,control,nullptr,instance,state);
    if(!material_window) { notice=L"材质设置暂时无法打开。"; invalidate(); return; }
    backdrop(material_window,false); ShowWindow(material_window,SW_SHOW); SetForegroundWindow(material_window);
    state->preview();
}
}
