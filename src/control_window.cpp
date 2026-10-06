#include "app.hpp"
#include <windowsx.h>
#include <algorithm>
#include <cmath>

namespace edge {
namespace {
// The settings window has a neutral palette; drawer materials keep their own.
Palette control_palette(bool dark) {
    using D2D1::ColorF;
    if(dark) return {ColorF(0x202020),ColorF(0x2B2B2B),ColorF(0x353535),ColorF(0xF4F4F4),ColorF(0xB7B7B7),ColorF(0x414141),ColorF(0x86D8B6),ColorF(0x303E37)};
    return {ColorF(0xF3F3F3),ColorF(0xFFFFFF),ColorF(0xF9F9F9),ColorF(0x202020),ColorF(0x686868),ColorF(0xE5E5E5),ColorF(0x287556),ColorF(0xE8F1EC)};
}
enum class ButtonStyle { Standard, Primary, Quiet };
struct ControlPainter {
    App& app;
    Canvas& c;
    Palette p;
    int hit(int id,D2D1_RECT_F bounds,const std::wstring& label,bool selected=false) {
        const int index=static_cast<int>(app.buttons.size());
        app.buttons.push_back({id,bounds,label,selected}); return index;
    }
    void focus(int index,D2D1_RECT_F b,float radius=4) {
        if(app.selected_button==index) c.border(box(b.left+2,b.top+2,b.right-b.left-4,b.bottom-b.top-4),p.text,radius,1.5f);
    }
    void button(int id,D2D1_RECT_F b,const std::wstring& label,ButtonStyle style=ButtonStyle::Standard,bool selected=false) {
        const int index=hit(id,b,label,selected);
        const bool hover=app.hovered_button==index,pressed=app.pressed_button==index;
        const bool primary=style==ButtonStyle::Primary;
        if(style!=ButtonStyle::Quiet || hover || pressed || selected) {
            auto fill=primary?p.accent:selected?p.accent_soft:pressed?p.line:hover?p.raised:p.card;
            if(primary && (hover || pressed)) fill.a=pressed?.76f:.9f;
            c.rect(b,fill,4);
        }
        if(style==ButtonStyle::Standard) c.border(b,selected?p.accent:p.line,4);
        const auto ink=primary?D2D1::ColorF(app.dark?0x10251C:0xFFFFFF):p.text;
        c.text(label,b,13,ink,false,DWRITE_TEXT_ALIGNMENT_CENTER,true);
        focus(index,b);
    }
    void navigation(int id,float y,const std::wstring& label,int glyph,bool selected) {
        const auto b=box(12,y,156,40); const int index=hit(id,b,label,selected);
        if(selected || app.hovered_button==index) c.rect(b,selected?p.line:p.raised,4);
        if(selected) c.rect(box(12,y+12,3,16),p.accent,1.5f);
        const auto color=selected?p.accent:p.muted;
        const float x=29,cy=y+12;
        if(glyph==0) c.logo(box(x,cy,16,17),color);
        else if(glyph==1) {
            c.border(box(x,cy,17,17),color,3,1.3f);
            c.rect(box(x+3,cy+3,5,11),color,1);
        } else {
            for(int i=0;i<3;++i) {
                const float yy=cy+3+i*6.0f,xx=x+(i==1?11:5);
                c.line(x,yy,x+17,yy,color,1.2f); c.rect(box(xx-1,yy-2,2,4),color,1);
            }
        }
        c.text(label,box(57,y,100,40),14,p.text,false,DWRITE_TEXT_ALIGNMENT_LEADING,true);
        focus(index,b);
    }
    void card(float x,float y,float w,float h) {
        c.rect(box(x,y,w,h),p.card,8); c.border(box(x+.5f,y+.5f,w-1,h-1),p.line,8);
    }
    void row(float x,float y,float w,const std::wstring& title,const std::wstring& detail,float reserve=170) {
        card(x,y,w,72);
        c.text(title,box(x+20,y+12,w-reserve-28,24),14,p.text,false,DWRITE_TEXT_ALIGNMENT_LEADING,true);
        c.text(detail,box(x+20,y+37,w-reserve-28,22),12,p.muted,false,DWRITE_TEXT_ALIGNMENT_LEADING,true);
    }
    void toggle(int id,float right,float y,bool on) {
        const auto b=box(right-86,y,86,40); const int index=hit(id,b,on?L"开":L"关",on);
        c.text(on?L"开":L"关",box(right-86,y,28,40),12,p.muted);
        const auto track=box(right-42,y+10,40,20);
        c.rect(track,on?p.accent:app.hovered_button==index?p.raised:p.background,10);
        if(!on) c.border(track,p.muted,10);
        c.rect(box(right-42+(on?23.0f:5.0f),y+14,12,12),on?D2D1::ColorF(app.dark?0x173126:0xFFFFFF):p.muted,6);
        focus(index,b);
    }
};
}

void App::paint_control() {
    PAINTSTRUCT ps{}; BeginPaint(control,&ps);
    auto& c=control_canvas;
    if(c.begin(control)) {
        ++paint_count; buttons.clear();
        const auto p=control_palette(dark); ControlPainter ui{*this,c,p};
        c.clear(p.background);
        const float x=212,w=c.width-x-28;
        c.line(180,20,180,c.height-20,p.line);
        c.rect(box(24,28,32,32),p.accent_soft,8); c.logo(box(33,36,14,16),p.accent);
        c.text(L"轻屉",box(67,24,90,25),19,p.text,true);
        c.text(L"EdgeTuck",box(68,49,96,18),11,p.muted);
        ui.navigation(200,102,L"我的抽屉",0,control_page==ControlPage::Drawers);
        ui.navigation(201,148,L"外观",1,control_page==ControlPage::Appearance);
        ui.navigation(202,194,L"常规",2,control_page==ControlPage::General);
        ui.button(101,box(20,c.height-112,140,34),L"回到桌面");
        c.text(L"关闭窗口后仍在托盘运行",box(20,c.height-72,146,24),10,p.muted);
        c.text(L"版本 0.8.12",box(24,c.height-36,132,20),11,p.muted);

        const wchar_t* titles[]{L"我的抽屉",L"外观",L"常规"};
        c.text(titles[static_cast<int>(control_page)],box(x,24,w,42),28,p.text,true);
        if(control_page==ControlPage::Drawers) {
            size_t total=0; for(const auto& d:settings.drawers) total+=d.items.size();
            c.text(std::to_wstring(settings.drawers.size())+L" 个抽屉  ·  "+std::to_wstring(total)+L" 个项目",box(x,70,w-150,24),13,p.muted);
            ui.button(100,box(x+w-116,35,116,34),L"＋ 新建抽屉",ButtonStyle::Primary);
            if(undo) ui.button(103,box(x+w-210,35,82,34),L"撤销解散",ButtonStyle::Quiet);
            const float top=118,bottom=c.height-104;
            control_rows=std::max(1,static_cast<int>((bottom-top)/64));
            drawer_list_bounds=box(x,top,w,control_rows*64.0f);
            const int count=static_cast<int>(settings.drawers.size());
            drawer_scroll=std::clamp(drawer_scroll,0,std::max(0,count-control_rows));
            for(int row=0;row<control_rows && row+drawer_scroll<count;++row) {
                const auto& d=settings.drawers[row+drawer_scroll]; const float y=top+row*64.0f;
                ui.card(x,y,w,58);
                c.rect(box(x+14,y+12,34,34),p.background,6);
                c.logo(box(x+24,y+20,14,17),p.accent);
                c.text(d.name,box(x+62,y+6,w-204,25),14,p.text,true,DWRITE_TEXT_ALIGNMENT_LEADING,true);
                c.text(edge_name(d.edge)+L" · "+std::to_wstring(d.items.size())+L" 项"+(d.legacy_items.empty()?L"":L" · 含旧引用"),box(x+62,y+30,w-204,21),12,p.muted,false,DWRITE_TEXT_ALIGNMENT_LEADING,true);
                ui.button(1000+d.id*10,box(x+w-122,y+13,66,32),L"预览",ButtonStyle::Quiet);
                ui.button(1003+d.id*10,box(x+w-48,y+13,34,32),L"···",ButtonStyle::Quiet);
            }
            if(count==0) {
                const float cy=top+70;
                c.rect(box(x+w/2-28,cy,56,56),p.accent_soft,12); c.logo(box(x+w/2-11,cy+15,22,26),p.accent);
                c.text(L"从一个抽屉开始",box(x,cy+72,w,30),18,p.text,true,DWRITE_TEXT_ALIGNMENT_CENTER);
                c.text(L"新建抽屉后，把文件拖到屏幕边缘即可收纳。",box(x,cy+109,w,28),13,p.muted,false,DWRITE_TEXT_ALIGNMENT_CENTER);
            }
            if(count>control_rows) {
                const float y=drawer_list_bounds.bottom+6;
                c.text(L"显示 "+std::to_wstring(drawer_scroll+1)+L"–"+std::to_wstring(std::min(count,drawer_scroll+control_rows))+L"，共 "+std::to_wstring(count)+L" 个",box(x,y,210,28),12,p.muted);
                if(drawer_scroll>0) ui.button(210,box(x+w-76,y,32,28),L"↑",ButtonStyle::Quiet);
                if(drawer_scroll+control_rows<count) ui.button(211,box(x+w-36,y,32,28),L"↓",ButtonStyle::Quiet);
                const float track=control_rows*64.0f-6,thumb=std::max(24.0f,track*control_rows/count);
                c.rect(box(x+w+9,top+(track-thumb)*drawer_scroll/(count-control_rows),3,thumb),p.muted,1.5f);
            }
        } else if(control_page==ControlPage::Appearance) {
            c.text(L"设置应用主题、抽屉材质和开合动画。",box(x,70,w,24),13,p.muted);
            c.text(L"应用主题",box(x,112,w,24),13,p.text,true);
            const float cardw=(w-24)/3;
            const wchar_t* names[]{L"跟随系统",L"浅色",L"深色"};
            for(int i=0;i<3;++i) {
                const float left=x+i*(cardw+12); const auto b=box(left,146,cardw,112);
                const int index=ui.hit(10+i,b,names[i],static_cast<int>(settings.theme)==i);
                const bool selected=static_cast<int>(settings.theme)==i;
                c.rect(b,hovered_button==index?p.raised:p.card,8);
                c.border(b,selected?p.accent:p.line,8,selected?1.5f:1.0f);
                const auto preview=box(left+12,158,cardw-24,57);
                c.rect(preview,D2D1::ColorF(i==2?0x202020:0xEEEEEE),4);
                const float pw=preview.right-preview.left;
                c.rect(box(preview.left+8,preview.top+8,pw*.23f,41),D2D1::ColorF(i==2?0x333333:0xFFFFFF),3);
                const float inner=preview.left+pw*.23f+15;
                c.rect(box(inner,preview.top+10,pw*.59f,9),D2D1::ColorF(i==2?0x3B3B3B:0xFFFFFF),2);
                c.rect(box(inner,preview.top+25,pw*.59f,20),D2D1::ColorF(i==0?0x363636:i==2?0x303030:0xFFFFFF),3);
                c.text(names[i],box(left+12,225,cardw-24,24),13,p.text,false,DWRITE_TEXT_ALIGNMENT_CENTER,true);
                ui.focus(index,b,6);
            }
            c.text(L"抽屉效果",box(x,282,w,24),13,p.text,true);
            ui.row(x,316,w,L"材质",L"选择桌面抽屉的背景效果",228);
            ui.button(20,box(x+w-206,336,98,32),L"液态玻璃",ButtonStyle::Standard,settings.glass);
            ui.button(21,box(x+w-100,336,80,32),L"纯色",ButtonStyle::Standard,!settings.glass);
            ui.row(x,394,w,L"玻璃细节",L"调整模糊、折射、光线和实时背景");
            ui.button(40,box(x+w-110,414,90,32),L"自定义…");
            ui.row(x,472,w,L"开合动画",L"鼠标进入和离开时平滑展开、收起");
            ui.toggle(30,x+w-22,488,settings.motion);
        } else {
            c.text(L"管理启动行为和文件存放位置。",box(x,70,w,24),13,p.muted);
            ui.row(x,118,w,L"开机启动",startup.state==StartupState::OtherPath?L"启动项指向旧版本，请修复路径":startup.state==StartupState::Error?L"无法读取启动项，可以重试":L"登录 Windows 后在托盘运行");
            if(startup.state==StartupState::OtherPath || startup.state==StartupState::Error)
                ui.button(50,box(x+w-110,138,90,32),startup.state==StartupState::OtherPath?L"修复路径":L"重试");
            else ui.toggle(50,x+w-22,134,startup.state==StartupState::On);
            ui.row(x,196,w,L"提高运行优先级",L"开启后使用「高于正常」优先级");
            ui.toggle(51,x+w-22,212,settings.responsive_priority);
            c.text(L"文件",box(x,292,w,24),13,p.text,true);
            ui.row(x,326,w,L"存放位置与迁移",archive_root().wstring());
            ui.button(52,box(x+w-110,346,90,32),L"管理…");
            c.text(L"关于",box(x,422,w,24),13,p.text,true);
            ui.row(x,456,w,L"轻屉 · EdgeTuck",L"版本 0.8.12  ·  原生桌面收纳工具");
            ui.button(104,box(x+w-110,476,90,32),L"退出轻屉");
        }
        c.line(x,c.height-56,x+w,c.height-56,p.line);
        const bool arranged=std::any_of(folder_positions.begin(),folder_positions.end(),[](const auto& entry){return entry.second==ParkResult::AutoArrange;});
        const bool unavailable=std::any_of(folder_positions.begin(),folder_positions.end(),[](const auto& entry){return entry.second==ParkResult::Unavailable;});
        const auto status=notice.empty()?(arranged?L"自动排列已开启，分类文件夹仍显示在桌面。":unavailable?L"部分文件夹暂未收起，可在抽屉菜单中打开查看。":control_page==ControlPage::Drawers?L"拖入即收纳 · 文件始终保存在普通文件夹中":L"设置即时生效。关闭窗口后，轻屉仍在托盘运行。"):notice;
        c.text(status,box(x,c.height-48,w,40),12,p.muted);
        c.end();
    }
    EndPaint(control,&ps);
}

int App::control_hit(float x,float y) const {
    for(size_t i=0;i<buttons.size();++i) if(contains(buttons[i].bounds,x,y)) return static_cast<int>(i);
    return -1;
}
void App::control_page_to(ControlPage page) {
    control_page=page; selected_button=hovered_button=pressed_button=-1; control_wheel=0;
    // Discard old hit regions before another queued input can see them.
    buttons.clear(); invalidate(); UpdateWindow(control);
}
bool App::control_input(UINT message,WPARAM wparam,LPARAM lparam) {
    const float dpi=GetDpiForWindow(control)/96.0f;
    const auto pointer=[&] { return control_hit(GET_X_LPARAM(lparam)/dpi,GET_Y_LPARAM(lparam)/dpi); };
    const auto repaint=[&] { invalidate(); };
    const auto scroll=[&](int amount) {
        drawer_scroll=std::clamp(drawer_scroll+amount,0,std::max(0,static_cast<int>(settings.drawers.size())-control_rows));
        hovered_button=selected_button=pressed_button=-1; repaint(); UpdateWindow(control);
    };
    switch(message) {
    case WM_SIZE: hovered_button=selected_button=pressed_button=-1; buttons.clear(); repaint(); return true;
    case WM_GETMINMAXINFO: {
        RECT r{0,0,static_cast<LONG>(820*dpi),static_cast<LONG>(620*dpi)};
        AdjustWindowRectExForDpi(&r,static_cast<DWORD>(GetWindowLongPtrW(control,GWL_STYLE)),FALSE,0,GetDpiForWindow(control));
        auto* info=reinterpret_cast<MINMAXINFO*>(lparam); info->ptMinTrackSize={r.right-r.left,r.bottom-r.top}; return true;
    }
    case WM_MOUSEMOVE: {
        if(!control_tracking) { TRACKMOUSEEVENT track{sizeof(track),TME_LEAVE,control,0}; TrackMouseEvent(&track); control_tracking=true; }
        const int next=pointer(); if(next!=hovered_button) { hovered_button=next; repaint(); } return true;
    }
    case WM_MOUSELEAVE: control_tracking=false; hovered_button=-1; repaint(); return true;
    case WM_LBUTTONDOWN: SetFocus(control); selected_button=-1; pressed_button=pointer(); if(pressed_button>=0) SetCapture(control); repaint(); return true;
    case WM_LBUTTONUP: {
        const int index=pointer(); const int pressed=pressed_button; pressed_button=-1;
        if(GetCapture()==control) ReleaseCapture();
        repaint();
        if(index>=0 && index==pressed) { const int id=buttons[index].id; action(id); }
        return true;
    }
    case WM_CAPTURECHANGED: pressed_button=-1; repaint(); return true;
    case WM_MOUSEWHEEL: {
        if(control_page!=ControlPage::Drawers) return true;
        POINT point{GET_X_LPARAM(lparam),GET_Y_LPARAM(lparam)}; ScreenToClient(control,&point);
        if(!contains(drawer_list_bounds,point.x/dpi,point.y/dpi)) return true;
        control_wheel+=GET_WHEEL_DELTA_WPARAM(wparam);
        const int steps=control_wheel/WHEEL_DELTA; control_wheel%=WHEEL_DELTA;
        if(steps) scroll(-steps); return true;
    }
    case WM_KEYDOWN:
        if(wparam==VK_ESCAPE) action(101);
        else if(wparam==VK_TAB && !buttons.empty()) {
            const int count=static_cast<int>(buttons.size());
            selected_button=selected_button<0?((GetKeyState(VK_SHIFT)<0)?count-1:0):(selected_button+((GetKeyState(VK_SHIFT)<0)?count-1:1))%count;
            repaint();
        } else if((wparam==VK_RETURN || wparam==VK_SPACE) && selected_button>=0 && selected_button<static_cast<int>(buttons.size())) {
            const int id=buttons[selected_button].id; action(id);
        } else if(control_page==ControlPage::Drawers && (wparam==VK_NEXT || wparam==VK_PRIOR || wparam==VK_HOME || wparam==VK_END)) {
            scroll(wparam==VK_NEXT?control_rows:wparam==VK_PRIOR?-control_rows:wparam==VK_HOME?-static_cast<int>(settings.drawers.size()):static_cast<int>(settings.drawers.size()));
        }
        return true;
    }
    return false;
}

void App::control_drawer_menu(int id) {
    const auto* d=find(id); if(!d) return;
    POINT point{}; GetCursorPos(&point);
    const float dpi=GetDpiForWindow(control)/96.0f;
    for(const auto& b:buttons) if(b.id==1003+id*10) {
        point={static_cast<LONG>(b.bounds.right*dpi),static_cast<LONG>(b.bounds.bottom*dpi)}; ClientToScreen(control,&point); break;
    }
    HMENU popup=CreatePopupMenu(),sides=CreatePopupMenu();
    AppendMenuW(popup,MF_STRING|(d->folder.empty()?MF_GRAYED:0),1,L"打开文件夹");
    AppendMenuW(popup,MF_STRING,2,L"重命名");
    AppendMenuW(popup,MF_STRING|(d->folder.empty()?MF_GRAYED:0),4,L"设置英文目录名…");
    for(int i=0;i<3;++i) AppendMenuW(sides,MF_STRING|(static_cast<int>(d->edge)==i?MF_CHECKED:0),10+i,edge_name(static_cast<Edge>(i)).c_str());
    AppendMenuW(popup,MF_POPUP,reinterpret_cast<UINT_PTR>(sides),L"移到边缘");
    AppendMenuW(popup,MF_SEPARATOR,0,nullptr); AppendMenuW(popup,MF_STRING,3,L"解散抽屉");
    int chosen{};
    { Interaction interaction(*this); chosen=TrackPopupMenu(popup,TPM_RETURNCMD|TPM_RIGHTALIGN|TPM_RIGHTBUTTON,point.x,point.y,0,control,nullptr); }
    DestroyMenu(popup);
    // Re-resolve after the nested menu loop; notifications may have refreshed models.
    d=find(id); if(!d) return;
    if(chosen==1 && !d->folder.empty()) ShellExecuteW(control,L"open",d->folder.c_str(),nullptr,nullptr,SW_SHOWNORMAL);
    if(chosen==2) rename_drawer(id);
    if(chosen==3) remove_drawer(id);
    if(chosen==4) rename_drawer_folder(id);
    if(chosen>=10 && chosen<=12) move_drawer_edge(id,static_cast<Edge>(chosen-10));
    hovered_button=selected_button=pressed_button=-1; invalidate();
}
}
