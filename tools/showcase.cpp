// Documentation-only capture. Never calls App::run, loads a profile, registers
// hooks/startup, parks icons, or moves user files. All windows and fixtures are
// owned by this process. The production drawer, compositor and settings UI are
// reused without replacing their drawing or animation code.
#include "app.hpp"
#include <commctrl.h>
#include <gdiplus.h>
#include <fstream>
#include <cmath>
#include <atomic>
#include <future>
#include <thread>

namespace {
using namespace Gdiplus;
constexpr UINT scene_command=WM_APP+100;
edge::App* app{};
std::filesystem::path output;
Bitmap* backdrop{};
bool recording{};
POINT previous_cursor{};

CLSID encoder(const wchar_t* mime) {
    UINT count{},bytes{}; GetImageEncodersSize(&count,&bytes);
    std::vector<unsigned char> data(bytes);
    auto* codecs=reinterpret_cast<ImageCodecInfo*>(data.data());
    GetImageEncoders(count,bytes,codecs);
    for(UINT i=0;i<count;++i) if(wcscmp(codecs[i].MimeType,mime)==0) return codecs[i].Clsid;
    throw std::runtime_error("Image encoder unavailable");
}
void save(Bitmap& image,const std::filesystem::path& path,const wchar_t* mime) {
    const auto clsid=encoder(mime);
    if(image.Save(path.c_str(),&clsid,nullptr)!=Ok) throw std::runtime_error("Image save failed");
}
std::unique_ptr<Bitmap> artwork(int width,int height,int variant,bool lettering=false) {
    auto bitmap=std::make_unique<Bitmap>(width,height,PixelFormat32bppARGB);
    Graphics g(bitmap.get()); g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.ScaleTransform(width/1280.0f,height/800.0f);
    LinearGradientBrush base(Rect(0,0,1280,800),Color(255,16,42,53),Color(255,43,106,111),32.0f);
    g.FillRectangle(&base,0,0,1280,800);
    for(int i=0;i<7;++i) {
        GraphicsPath path;
        const float y=260.0f+i*72.0f+variant*12.0f;
        path.AddBezier(-140.0f,y+440,160.0f,y-200,650.0f,y+340,1430.0f,y-100);
        path.AddLine(1430.0f,y-100,1430.0f,920.0f); path.AddLine(1430.0f,920.0f,-140.0f,920.0f); path.CloseFigure();
        SolidBrush wave(Color(255,static_cast<BYTE>(31+i*7),static_cast<BYTE>(81+i*10),static_cast<BYTE>(84+i*9)));
        g.FillPath(&wave,&path);
        Pen rim(Color(38,210,236,221),1.2f); g.DrawPath(&rim,&path);
    }
    SolidBrush sun(Color(255,231,209,160)); g.FillEllipse(&sun,965,104,144,144);
    if(lettering) {
        g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
        FontFamily family(L"Segoe UI"); Font brand(&family,42,FontStyleRegular,UnitPixel);
        SolidBrush white(Color(240,236,247,239)),muted(Color(205,189,215,209));
        g.DrawString(L"EdgeTuck",-1,&brand,PointF(100,245),&white);
        FontFamily chinese(L"Microsoft YaHei UI"); Font title(&chinese,22,FontStyleRegular,UnitPixel);
        g.DrawString(L"收起纷杂，留住风景。",-1,&title,PointF(101,309),&muted);
        Font caption_font(&family,12,FontStyleRegular,UnitPixel);
        g.DrawString(L"NATIVE DESKTOP DRAWERS",-1,&caption_font,PointF(103,225),&muted);
    }
    return bitmap;
}
LRESULT CALLBACK keep_above(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR) {
    if(msg==WM_MOUSEMOVE || msg==WM_MOUSELEAVE) {
        // Keep file-opening input blocked by automated_test, but let genuine
        // pointer motion exercise the production hover handlers for recording.
        const bool before=app->smoke_dispatch; app->smoke_dispatch=true;
        const auto result=DefSubclassProc(hwnd,msg,wp,lp);
        app->smoke_dispatch=before; return result;
    }
    if(msg==WM_WINDOWPOSCHANGING) {
        auto* pos=reinterpret_cast<WINDOWPOS*>(lp);
        if(!(pos->flags&SWP_NOZORDER)) pos->hwndInsertAfter=HWND_TOPMOST;
    }
    return DefSubclassProc(hwnd,msg,wp,lp);
}
void raise_drawers() {
    for(auto& d:app->drawers) SetWindowPos(d->panel,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
}
void scene(int kind) {
    recording=kind==4;
    for(auto& d:app->drawers) { d->pinned=false; d->set_open(false); d->finish_animation(d->motion_serial); }
    ShowWindow(app->control,SW_HIDE);
    app->settings.theme=kind==3?edge::Theme::Dark:edge::Theme::Light;
    app->settings.glass=true; app->refresh_theme();
    if(kind==1) {
        auto& drawer=app->drawers.front(); drawer->pinned=true; drawer->set_open(true);
    } else if(kind==2 || kind==3) {
        app->control_page=kind==2?edge::ControlPage::Drawers:edge::ControlPage::Appearance;
        app->notice.clear(); app->show_control();
        SetWindowPos(app->control,HWND_TOPMOST,app->work.left+162,app->work.top+45,956,718,SWP_SHOWWINDOW);
        app->invalidate();
    }
    raise_drawers();
    if(kind==2 || kind==3) for(auto& d:app->drawers) ShowWindow(d->panel,SW_HIDE);
    app->sync_live_capture();
}
LRESULT CALLBACK background_proc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp) {
    if(msg==WM_ERASEBKGND) return 1;
    if(msg==WM_PAINT) {
        PAINTSTRUCT ps{}; auto dc=BeginPaint(hwnd,&ps);
        RECT client{}; GetClientRect(hwnd,&client);
        auto memory=CreateCompatibleDC(dc);
        auto bitmap=CreateCompatibleBitmap(dc,client.right,client.bottom);
        auto previous=SelectObject(memory,bitmap);
        {
            // WGC can observe GDI drawing before EndPaint. Publish the entire
            // background at once so the lens never samples clear/partial art.
            Graphics g(memory); g.Clear(Color(255,49,111,116)); g.TranslateTransform(40,40);
            if(backdrop) g.DrawImage(backdrop,0,0);
            SolidBrush pulse(Color(255,static_cast<BYTE>(130+30*std::sin(GetTickCount64()/350.0)),190,175));
            g.FillEllipse(&pulse,103,741,5,5);
        }
        BitBlt(dc,0,0,client.right,client.bottom,memory,0,0,SRCCOPY);
        SelectObject(memory,previous); DeleteObject(bitmap); DeleteDC(memory);
        EndPaint(hwnd,&ps); return 0;
    }
    if(msg==WM_TIMER) { InvalidateRect(hwnd,nullptr,FALSE); return 0; }
    if(msg==scene_command) { scene(static_cast<int>(wp)); return 0; }
    if(msg==WM_CLOSE) { PostQuitMessage(0); return 0; }
    if(msg==WM_KEYDOWN && wp==VK_ESCAPE) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(hwnd,msg,wp,lp);
}
bool pump(int ms) {
    const auto until=GetTickCount64()+ms;
    do {
        MSG msg{};
        while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)) {
            if(msg.message==WM_QUIT) return false;
            TranslateMessage(&msg); DispatchMessageW(&msg);
        }
        if(GetTickCount64()<until) MsgWaitForMultipleObjectsEx(0,nullptr,2,QS_ALLINPUT,MWMO_INPUTAVAILABLE);
    } while(GetTickCount64()<until);
    return true;
}
void capture(const std::filesystem::path& path,bool cursor=false) {
    // Capture only the owned scene. Refuse to save if another process covers it.
    for(int y=8;y<800;y+=64) for(int x=8;x<1280;x+=64) {
        DWORD pid{}; GetWindowThreadProcessId(WindowFromPoint({app->work.left+x,app->work.top+y}),&pid);
        if(pid!=GetCurrentProcessId()) throw std::runtime_error("Showcase is covered; refusing screen capture");
    }
    HDC screen=GetDC(nullptr),memory=CreateCompatibleDC(screen);
    HBITMAP bitmap=CreateCompatibleBitmap(screen,1280,800);
    const auto old=SelectObject(memory,bitmap);
    BitBlt(memory,0,0,1280,800,screen,app->work.left,app->work.top,SRCCOPY|CAPTUREBLT);
    if(cursor) {
        CURSORINFO info{sizeof(info)};
        if(GetCursorInfo(&info) && (info.flags&CURSOR_SHOWING)) {
            ICONINFO icon{};
            if(GetIconInfo(info.hCursor,&icon)) {
                DrawIconEx(memory,info.ptScreenPos.x-app->work.left-static_cast<int>(icon.xHotspot),info.ptScreenPos.y-app->work.top-static_cast<int>(icon.yHotspot),info.hCursor,0,0,0,nullptr,DI_NORMAL);
                if(icon.hbmColor) DeleteObject(icon.hbmColor); if(icon.hbmMask) DeleteObject(icon.hbmMask);
            }
        }
    }
    SelectObject(memory,old);
    { Bitmap image(bitmap,nullptr); save(image,path,recording?L"image/bmp":L"image/png"); }
    DeleteObject(bitmap); DeleteDC(memory); ReleaseDC(nullptr,screen);
}
void fixtures() {
    const auto root=output/L"fixtures"; std::filesystem::create_directory(root);
    const wchar_t* names[]{L"工作",L"常用",L"灵感"};
    const wchar_t* pictures[]{L"山海.png",L"晨光.png",L"青岚.png",L"远山.png",L"海岸.png",L"夜色.png"};
    for(int i=0;i<3;++i) {
        auto& model=app->settings.drawers[i]; model.name=names[i];
        const auto folder=root/(std::wstring(L"ET_")+names[i]); std::filesystem::create_directory(folder);
        model.folder=folder.wstring();
        for(const auto* name:{L"文档",L"项目",L"参考资料"}) {
            const auto path=folder/name; std::filesystem::create_directory(path); model.items.push_back(path.wstring());
        }
        for(int p=0;p<6;++p) { auto image=artwork(240,160,p); const auto path=folder/pictures[p]; save(*image,path,L"image/png"); model.items.push_back(path.wstring()); }
        for(const auto* name:{L"阅读清单.txt",L"灵感笔记.txt",L"待办事项.txt"}) {
            const auto path=folder/name; std::ofstream(path)<<"EdgeTuck showcase fixture.\n"; model.items.push_back(path.wstring());
        }
    }
}
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int) {
    int argc{}; auto argv=CommandLineToArgvW(GetCommandLineW(),&argc);
    if(!argv || argc!=2) { if(argv) LocalFree(argv); return 2; }
    output=std::filesystem::absolute(argv[1]); LocalFree(argv);
    // A new directory is mandatory; do not mix captures with existing files.
    if(std::filesystem::exists(output)) return 3;
    std::filesystem::create_directories(output);
    GetCursorPos(&previous_cursor); const auto previous_foreground=GetForegroundWindow();
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if(FAILED(OleInitialize(nullptr))) return 4;
    ULONG_PTR token{}; GdiplusStartupInput input; GdiplusStartup(&token,&input,nullptr);
    INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_STANDARD_CLASSES|ICC_BAR_CLASSES}; InitCommonControlsEx(&controls);
    int result=0;
    try {
        edge::App owned(instance); app=&owned;
        owned.smoke=true; owned.automated_test=true; owned.save_allowed=false;
        owned.config_path=output/L"unused-settings.dat";
        owned.work={40,60,1320,860}; owned.scale=1; owned.grid_x=80; owned.grid_y=80;
        owned.settings.theme=edge::Theme::Light; owned.settings.live_background=true;
        owned.settings.drawers={{1,L"工作",edge::Edge::Right,2,5,5,{}},{2,L"常用",edge::Edge::Top,4,4,4,{}},{3,L"灵感",edge::Edge::Left,3,4,4,{}}};
        owned.settings.storage_directory=(output/L"fixtures").wstring();
        fixtures(); owned.graphics.initialize();
        if(!owned.graphics.composition) throw std::runtime_error("Composition is required for showcase capture");
        owned.graphics.composition->live_mode(true);
        owned.refresh_theme(); owned.create_windows();
        WNDCLASSW wc{}; wc.hInstance=instance; wc.lpfnWndProc=background_proc; wc.lpszClassName=L"EdgeTuck.Showcase"; wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);
        RegisterClassW(&wc);
        auto art=artwork(1280,800,0,true); backdrop=art.get();
        // Capture padding supplies pixels for edge refraction outside the
        // 1280x800 documentation crop, just as the real desktop would.
        owned.test_background=CreateWindowExW(WS_EX_TOPMOST|WS_EX_TOOLWINDOW,wc.lpszClassName,L"EdgeTuck isolated documentation scene",WS_POPUP,0,20,1360,880,nullptr,nullptr,instance,nullptr);
        ShowWindow(owned.test_background,SW_SHOWNOACTIVATE); UpdateWindow(owned.test_background);
        SetTimer(owned.test_background,11,16,nullptr);
        owned.graphics.composition->live_source_for_test(owned.test_background);
        owned.create_drawers();
        // The showcase records the right edge; other sample categories remain
        // in the real settings list without adding off-camera preview windows.
        while(owned.drawers.size()>1) owned.drawers.pop_back();
        for(auto& d:owned.drawers) { SetWindowSubclass(d->panel,keep_above,1,0); d->prepare_images(edge::ImagePriority::Visible); }
        SetCursorPos(70,900); scene(1); if(!pump(2200)) throw std::runtime_error("Showcase cancelled");
        if(!owned.drawers.front()->panel_canvas.surface->has_refraction()) throw std::runtime_error("Optical material unavailable");
        capture(output/L"drawer.png");
        scene(2); if(!pump(800)) throw std::runtime_error("Showcase cancelled"); capture(output/L"settings.png");
        scene(3); if(!pump(600)) throw std::runtime_error("Showcase cancelled"); capture(output/L"appearance.png");
        scene(4); if(!pump(500)) throw std::runtime_error("Showcase cancelled");
        std::filesystem::create_directory(output/L"frames");
        std::atomic<bool> cancel_recording{false};
        // Screen readback and BMP writes must not occupy the UI thread: live
        // glass and hover messages need to run while a frame is being saved.
        auto recorder=std::async(std::launch::async,[&] {
            std::ofstream timing(output/L"timing.csv"); timing<<"frame,milliseconds\n";
            const auto started=std::chrono::steady_clock::now();
            unsigned frame=0;
            for(; !cancel_recording; ++frame) {
                const auto now=std::chrono::steady_clock::now();
                const auto elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(now-started).count();
                if(elapsed>=5500) break;
                const double phase=elapsed/1000.0;
                int x=1000,y=375;
                if(phase<1) x=static_cast<int>(1000+277*std::min(1.0,phase/.8));
                else if(phase<3.4) x=1230;
                else if(phase<4.2) x=static_cast<int>(1230-480*(phase-3.4)/.8);
                else x=750;
                SetCursorPos(owned.work.left+x,owned.work.top+y);
                wchar_t name[32]{}; swprintf_s(name,L"%04u.bmp",frame);
                timing<<frame<<','<<elapsed<<'\n'; capture(output/L"frames"/name,true);
                std::this_thread::sleep_until(now+std::chrono::milliseconds(33));
            }
            return frame;
        });
        while(recorder.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready) {
            if(!pump(5)) { cancel_recording=true; break; }
        }
        const auto frame_count=recorder.get();
        if(cancel_recording) throw std::runtime_error("Showcase cancelled");
        recording=false;
        std::ofstream report(output/L"capture.json");
        report<<"{\"isolated\":true,\"read_user_settings\":false,\"saved_user_settings\":false,\"real_ui\":true,\"owned_background\":true,\"optical\":true,\"frames\":"<<frame_count<<",\"live_frames\":"<<owned.graphics.composition->live_frames()<<",\"width\":1280,\"height\":800}";
        backdrop=nullptr;
    } catch(const std::exception& error) { std::ofstream(output/L"error.txt")<<error.what(); result=1; }
      catch(...) { std::ofstream(output/L"error.txt")<<"Windows rendering error"; result=1; }
    app=nullptr; SetCursorPos(previous_cursor.x,previous_cursor.y);
    if(IsWindow(previous_foreground)) SetForegroundWindow(previous_foreground);
    GdiplusShutdown(token); OleUninitialize(); return result;
}
