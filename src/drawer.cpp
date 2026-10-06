#include "app.hpp"
#include "drop_target.hpp"
#include "menu_host.hpp"
#include "diagnostics.hpp"
#include <windowsx.h>
#include <dwmapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <algorithm>
#include <cmath>
#include <iostream>

namespace edge {
static constexpr UINT_PTR pointer_timer = 2, completion_watchdog = 4;
static constexpr int motion_duration_ms = 180;
static constexpr wchar_t drawer_class[] = L"EdgeTuck.Drawer";
static UINT_PTR next_motion_serial = 0;

Drawer::Drawer(App& app, int id) : app(app), id(id), panel_canvas(&app.graphics) { panel_canvas.composite = app.graphics.composition != nullptr; }
Drawer::~Drawer() {
    panel_canvas.reset();
    if (panel) { RevokeDragDrop(panel); DestroyWindow(panel); }
    if (panel_drop) panel_drop->Release();
}
DrawerModel& Drawer::model() { return *app.find(id); }
void Drawer::create() {
    WNDCLASSEXW wc{sizeof(wc)}; wc.style = CS_DBLCLKS; wc.hInstance = app.instance; wc.lpszClassName = drawer_class;
    wc.lpfnWndProc = window_proc; wc.hCursor = LoadCursorW(nullptr, IDC_ARROW); wc.hIcon = app.icon;
    RegisterClassExW(&wc);
    const DWORD ex = WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | (panel_canvas.composite ? WS_EX_NOREDIRECTIONBITMAP : 0);
    panel = CreateWindowExW(ex, drawer_class, model().name.c_str(), WS_POPUP, 0, 0, 100, 100, nullptr, nullptr, app.instance, this);
    if (!panel) throw std::runtime_error("Cannot create drawer");
    panel_drop = new DropTarget(*this); check(RegisterDragDrop(panel, panel_drop));
    const BOOL disabled = TRUE;
    DwmSetWindowAttribute(panel, DWMWA_TRANSITIONS_FORCEDISABLED, &disabled, sizeof(disabled));
    DwmSetWindowAttribute(panel, DWMWA_EXCLUDED_FROM_PEEK, &disabled, sizeof(disabled));
    apply_theme();
    panel_canvas.prepare(panel);
    update_geometry();
    // Warm the drawing surface before the first hover; the moving edge is the
    // same surface, not a second hint window or a delayed swap.
    paint();
    if (panel_canvas.surface) panel_canvas.surface->position(closed.x, closed.y);
    if (!panel_canvas.composite) finish_animation(motion_serial);
    update_input_region(false); ShowWindow(panel, SW_SHOWNOACTIVATE);
    request_sort();
}
void Drawer::apply_theme() {
    // One compositor owns every transparent corner. No caption, DWM frame,
    // extended glass margins, or separately rounded window surface underneath.
    const DWM_SYSTEMBACKDROP_TYPE type = DWMSBT_NONE;
    const DWM_WINDOW_CORNER_PREFERENCE corners = DWMWCP_DONOTROUND;
    const DWMNCRENDERINGPOLICY policy = DWMNCRP_DISABLED;
    const MARGINS margins{};
    DwmSetWindowAttribute(panel, DWMWA_SYSTEMBACKDROP_TYPE, &type, sizeof(type));
    DwmSetWindowAttribute(panel, DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof(corners));
    DwmSetWindowAttribute(panel, DWMWA_NCRENDERING_POLICY, &policy, sizeof(policy));
    DwmExtendFrameIntoClientArea(panel, &margins);
    const bool material_changed=panel_glass!=(app.transparency && panel_canvas.composite);
    panel_glass = app.transparency && panel_canvas.composite;
    panel_canvas.glass = panel_glass; panel_canvas.dark = app.dark;
    panel_canvas.material = app.settings.material;
    if(panel_canvas.surface) {
        // Material selection also selects the animation driver. Apply it before
        // the next paint/hover, and hand an in-flight slide to the new driver at
        // its current position with only the remaining duration.
        const bool resume=material_changed && animating;
        const int remaining=resume?std::max(1,static_cast<int>(std::ceil(motion_duration_ms-motion_elapsed(animation_start)))):0;
        if(resume) {
            const auto current=offset_at(closed,current_progress());
            panel_canvas.surface->position(current.x,current.y);
        }
        panel_canvas.surface->appearance(panel_glass,app.dark);
        if(resume) {
            const auto destination=offset_at(closed,target);
            panel_canvas.surface->slide_to(destination.x,destination.y,remaining,WM_EDGE_ANIMATION_DONE,motion_serial);
            ++motion_submissions; ++app.animation_submissions;
        }
    }
    InvalidateRect(panel, nullptr, FALSE);
}
void Drawer::update_geometry() {
    const auto& d = model();
    const int axis = app.origin(d.edge) + d.start * app.pitch(d.edge);
    const int span = d.span * app.pitch(d.edge);
    const int depth = d.depth * (d.edge == Edge::Top ? app.grid_y : app.grid_x);
    RECT next{};
    if (d.edge == Edge::Left) next = {app.work.left, axis, app.work.left + depth, axis + span};
    else if (d.edge == Edge::Right) next = {trailing_grid_start(app.work.left,app.work.right,app.grid_x,d.depth), axis, app.work.right, axis + span};
    else next = {axis, app.work.top, axis + span, app.work.top + depth};
    // Pointer motion within the same cell must not cancel animation, drop the
    // optical scene, resize surfaces, or redraw unrelated drawers.
    if (EqualRect(&next, &bounds)) return;
    if (animating) finish_animation(motion_serial);
    bounds = next;
    closed = closed_offset(static_cast<int>(d.edge), static_cast<float>(bounds.right-bounds.left), static_cast<float>(bounds.bottom-bounds.top), 14 * app.scale);
    ++geometry_changes;
    const auto native_offset = panel_canvas.composite ? Offset{} : offset_at(closed, progress);
    SetWindowPos(panel, nullptr, bounds.left+static_cast<int>(native_offset.x), bounds.top+static_cast<int>(native_offset.y), bounds.right-bounds.left, bounds.bottom-bounds.top, SWP_NOACTIVATE | SWP_NOZORDER);
    if (panel_canvas.surface) { const auto offset = offset_at(closed, progress); panel_canvas.surface->position(offset.x, offset.y); }
    update_input_region(!collapsed());
    prepare_images(target>0?ImagePriority::Visible:ImagePriority::Background);
    // Re-crop the retained scene at the new screen coordinates before presenting
    // this snapped size. Never sample a drawer that is already on screen.
    InvalidateRect(panel, nullptr, FALSE); UpdateWindow(panel);
}
void Drawer::update_input_region(bool expanded) {
    const int width = bounds.right-bounds.left, height = bounds.bottom-bounds.top;
    RECT input{0, 0, width, height};
    if (!expanded && panel_canvas.composite) {
        const int peek = static_cast<int>(14 * app.scale);
        if (model().edge == Edge::Right) input.left = width-peek;
        else if (model().edge == Edge::Left) input.right = peek;
        else input.bottom = peek;
    }
    // Only transition endpoints change the hit region. The compositor animates
    // the entire rounded body at the display cadence between those endpoints.
    const int corner_diameter=static_cast<int>(std::lround(2*drawer_corner_dip*app.scale));
    const int inset=static_cast<int>(std::lround(drawer_visual_inset_dip*app.scale));
    HRGN region = panel_canvas.composite ? CreateRectRgn(input.left, input.top, input.right, input.bottom) : CreateRoundRectRgn(inset, inset, width-inset+1, height-inset+1, corner_diameter, corner_diameter);
    if (SetWindowRgn(panel, region, FALSE) == 0) DeleteObject(region);
}
float Drawer::current_progress() const {
    return animating ? motion_progress(from, target, motion_elapsed(animation_start), static_cast<float>(motion_duration_ms)) : progress;
}
POINT Drawer::content_point(POINT client) const {
    if (!panel_canvas.composite) return client;
    const auto offset = offset_at(closed, current_progress());
    client.x -= static_cast<LONG>(std::lround(offset.x)); client.y -= static_cast<LONG>(std::lround(offset.y));
    return client;
}
void Drawer::set_open(bool open) {
    const float next = open ? 1.0f : 0.0f;
    if (next == target && (animating || progress == next)) return;
    const bool images_needed=open && prepare_images(ImagePriority::Visible);
    // Keep the clean scene alive until the closing body has completely left.
    // A neighbor entered during those 180 ms must not capture that body.
    if (!open && panel_canvas.surface) panel_canvas.surface->suspend_refraction();
    if (open && panel_canvas.surface) {
        const float capture_ms = panel_canvas.surface->capture_backdrop(static_cast<int>(model().edge));
        if (app.smoke) std::cout << "Optical background sample: " << capture_ms << " ms\n";
    }
    if (open) {
        for (auto& other : app.drawers) if (other.get()!=this && !other->pinned) {
            other->set_open(false);
            RECT overlap{};
            // At a shared corner, clear the old body immediately instead of
            // placing two expanding/closing panels on top of one another.
            if (other->animating && IntersectRect(&overlap,&bounds,&other->bounds)) other->finish_animation(other->motion_serial);
        }
        // A settings-button preview needs time to reach it from the button.
        // Any real pointer entry cancels this preview-only grace period.
        preview_deadline = preview ? GetTickCount64()+1500 : 0;
    }
    from = current_progress(); target = next; animation_start = MotionClock::now();
    if(images_needed) {
        // Existing foreground bitmaps move with the surface. Redraw only when
        // this page still needs images; completion messages handle later ones.
        InvalidateRect(panel,nullptr,FALSE);
    }
    motion_serial = ++next_motion_serial;
    update_input_region(true); ShowWindow(panel, SW_SHOWNOACTIVATE);
    const auto destination = offset_at(closed, next);
    animating = app.animations && panel_canvas.surface != nullptr;
    if (animating) {
        ++motion_submissions; ++app.animation_submissions;
        panel_canvas.surface->slide_to(destination.x, destination.y, motion_duration_ms, WM_EDGE_ANIMATION_DONE, motion_serial);
        // A one-shot recovery deadline, never a frame-driving timer.
        SetTimer(panel, completion_watchdog, motion_duration_ms+300, nullptr);
    } else finish_animation(motion_serial);
    if (open || animating) SetTimer(panel, pointer_timer, 40, nullptr);
    app.desktop_order();
}
void Drawer::finish_animation(UINT_PTR serial) {
    if (serial != motion_serial) return;
    KillTimer(panel, completion_watchdog);
    progress = target; animating = false;
    const auto offset = offset_at(closed, progress);
    if (panel_canvas.surface) {
        if (progress == 0 && !app.settings.live_background) panel_canvas.surface->invalidate_refraction();
        panel_canvas.surface->position(offset.x, offset.y);
        if(app.settings.live_background) panel_canvas.surface->capture_backdrop(static_cast<int>(model().edge));
    }
    else if(!panel_canvas.composite) {
        ++geometry_changes;
        SetWindowPos(panel, nullptr, bounds.left+static_cast<int>(offset.x), bounds.top+static_cast<int>(offset.y), 0, 0, SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER);
        InvalidateRect(panel,nullptr,FALSE);
    }
    update_input_region(progress > 0);
    if (progress == 0) { KillTimer(panel, pointer_timer); preview = false; app.desktop_order(); }
    apply_sort();
}
void Drawer::watch_pointer() {
    if (pinned || dropping || menu_open || resize_kind || dragging_out || pressed>=0) return;
    POINT point{}; GetCursorPos(&point);
    const HWND root = GetAncestor(WindowFromPoint(point), GA_ROOT);
    POINT local = point; ScreenToClient(panel, &local); local = content_point(local);
    if (root == panel && local.x >= 0 && local.y >= 0 && local.x < bounds.right-bounds.left && local.y < bounds.bottom-bounds.top) { preview_deadline = 0; return; }
    if (preview_deadline && GetTickCount64() < preview_deadline) return;
    set_open(false);
}
int Drawer::item_count() const {
    const auto* d = app.find(id); return d ? static_cast<int>(d->items.size()) : 0;
}
ContentGrid Drawer::content_grid() const {
    RECT client{}; GetClientRect(panel, &client);
    return {client.right, client.bottom, app.grid_x, static_cast<int>(std::lround(76*app.scale)),
        static_cast<int>(std::lround(38*app.scale))};
}
float Drawer::icon_size() const {
    const auto grid=content_grid();
    return std::max(12.0f,std::min({40.0f,grid.cell_width/app.scale-12,grid.cell_height/app.scale-30}));
}
bool Drawer::prepare_images(ImagePriority priority) {
    if(!panel) return false;
    const auto grid=content_grid();
    const int cols=grid.columns(),rows=grid.rows(),count=item_count();
    const int first_row=std::clamp(scroll,0,std::max(0,(count+cols-1)/cols-rows));
    const int size=image_pixels(icon_size(),static_cast<float>(GetDpiForWindow(panel)));
    // Explicitly queue only the page this drawer will show. Collapsed/covered
    // HWNDs need not receive WM_PAINT for startup prewarming to take place.
    bool needed=false;
    for(int i=first_row*cols;i<std::min(count,(first_row+rows)*cols);++i) {
        const auto& path=model().items[i];
        if(panel_canvas.icons.contains({path,size})) continue;
        needed=true; app.graphics.file_images.request(path,size,panel,priority);
    }
    return needed;
}
int Drawer::columns() const { return content_grid().columns(); }
int Drawer::hit_item(float x, float y) {
    return content_grid().hit(static_cast<int>(std::floor(x * app.scale)),
        static_cast<int>(std::floor(y * app.scale)), scroll, item_count());
}
void Drawer::paint() {
    PAINTSTRUCT ps{}; BeginPaint(panel, &ps);
    auto& c = panel_canvas;
    if (c.begin(panel)) {
        panel_glass = app.transparency && c.surface != nullptr;
        ++app.paint_count;
        auto p = palette(app.dark); const auto& d = model();
        if (panel_glass) {
            p.text = D2D1::ColorF(0xFFFFFF);
            p.muted = D2D1::ColorF(0xE1EAF0);
            p.line = D2D1::ColorF(0xFFFFFF, .13f);
        }
        c.clear(panel_glass ? D2D1::ColorF(0, 0.0f) : p.card);
        // With no compositor, leave only the solid material on a closed edge.
        if(!c.surface && collapsed()) { c.end(); EndPaint(panel,&ps); return; }
        // The optical shader owns the entire material, including the one rim.
        // Foreground content must not add a second translucent sheet or outline.
        if (dropping) c.drop_highlight();
            c.text(d.name, box(18, 7, c.width - 106, 24), 15, p.text, true, DWRITE_TEXT_ALIGNMENT_LEADING, true);
            // Compact sort glyph beside the existing overflow menu.
            const float sx=c.width-69,sy=12;
            for(int stroke=0;stroke<3;++stroke) c.line(sx,sy+stroke*5,sx+13-stroke*4,sy+stroke*5,p.muted,1.35f);
            c.line(sx+19,sy-1,sx+19,sy+12,p.muted,1.35f);
            c.line(sx+16,sy+9,sx+19,sy+12,p.muted,1.35f); c.line(sx+22,sy+9,sx+19,sy+12,p.muted,1.35f);
            for(int dot=0;dot<3;++dot) c.rect(box(c.width-36+dot*6.0f,18,3,3),p.muted,1.5f);
            const auto grid = content_grid();
            const int cols = grid.columns(), visible_rows = grid.rows();
            const int total_rows = (item_count() + cols - 1) / cols;
            scroll = std::clamp(scroll, 0, std::max(0, total_rows - visible_rows));
            const float cell = grid.cell_width / app.scale, row = grid.cell_height / app.scale;
            const float top = grid.top() / app.scale, bottom = grid.bottom() / app.scale;
            const float icon_size = this->icon_size();
            c.target->PushAxisAlignedClip(box(0, top, c.width, bottom - top), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            for (int i = scroll * cols; i < item_count() && i < (scroll + visible_rows) * cols; ++i) {
                const float x = static_cast<float>(i % cols) * cell, y = top + static_cast<float>(i / cols - scroll) * row;
                const bool hovered=i==selected, chosen=selection.contains(i), focus_here=GetFocus()==panel && i==focused;
                if (hovered || chosen || focus_here) {
                    const auto highlight=box(x+4,y+2,cell-8,row-4);
                    if(panel_glass) c.glass_highlight(highlight,chosen,hovered,focus_here);
                    else {
                        if(hovered || chosen) { c.rect(highlight,p.raised,item_highlight_corner_dip); c.border(highlight,chosen?p.accent:p.line,item_highlight_corner_dip); }
                        if(focus_here && !chosen) c.border(box(x+6,y+4,cell-12,row-8),p.line,item_highlight_corner_dip-2);
                    }
                }
                {
                    c.file_icon(d.items[i], box(x + (cell - icon_size) / 2, y + 6, icon_size, icon_size), p.accent,
                        target>0 || !collapsed()?ImagePriority::Visible:ImagePriority::Background);
                    if(std::any_of(d.legacy_items.begin(),d.legacy_items.end(),[&](const auto& path){return same_path(path,d.items[i]);}))
                        c.text(L"引用",box(x+cell-29,y+4,26,15),9,p.muted,false,DWRITE_TEXT_ALIGNMENT_CENTER);
                }
                const std::wstring name = std::filesystem::path(d.items[i]).filename().wstring();
                c.text(name, box(x + 4, y + icon_size + 9, cell - 8, row - icon_size - 11), 11, p.text, false, DWRITE_TEXT_ALIGNMENT_CENTER);
            }
            if (d.items.empty() && visible_rows >= 1 && scroll == 0) {
                const float hint = top + std::max(0.0f, (bottom - top - 51) / 2);
                c.text(L"拖入文件，收进抽屉", box(20, hint, c.width - 40, 26), 14, p.muted, false, DWRITE_TEXT_ALIGNMENT_CENTER);
                c.text(L"双击图标打开 · 滚轮浏览", box(20, hint + 29, c.width - 40, 22), 11, p.muted, false, DWRITE_TEXT_ALIGNMENT_CENTER);
            }
            c.target->PopAxisAlignedClip();
            if (visible_rows > 0 && total_rows > visible_rows) {
                const float track = bottom - top - 12, thumb = std::max(20.0f, track * visible_rows / total_rows);
                c.rect(box(c.width - 8, top + 6 + (track - thumb) * scroll / std::max(1, total_rows - visible_rows), 3, thumb), p.muted, 1.5f);
            }
            for (int i = 0; i < 3; ++i) c.line(c.width - 15 + i * 3.0f, c.height - 8, c.width - 8, c.height - 15 + i * 3.0f, p.line);
        if(app.stress_test && c.surface) content_probe=c.probe_pixel(static_cast<int>((c.width-34.5f)*app.scale),static_cast<int>(19.5f*app.scale));
        c.end();
        if(c.surface && canvas_generation!=c.generation) {
            // New surfaces have no slide offset or pixels from the old scene.
            // Content was fully painted above; now restore state as one unit.
            canvas_generation=c.generation;
            finish_animation(motion_serial);
        }
        if(c.surface && (target>0 || app.settings.live_background)) c.surface->capture_backdrop(static_cast<int>(model().edge));
    }
    EndPaint(panel, &ps);
}
void Drawer::add_paths(const std::vector<std::wstring>& paths) {
    if(paths.empty()) return;
    app.import_paths(id,paths);
}
void Drawer::open_item(int index,bool double_click) {
    if (index < 0 || index >= item_count()) return;
    const auto path = model().items[index];
    Interaction interaction(app);
    if(!model().folder.empty() && path_within(path,model().folder) && !folder_available(model())) {
        app.notice=L"抽屉文件夹已失联，请在资源管理器中恢复原文件夹后再打开。"; app.show_control(); app.invalidate(); return;
    }
    SHELLEXECUTEINFOW info{sizeof(info)}; info.fMask = SEE_MASK_FLAG_NO_UI; info.hwnd = panel; info.lpFile = path.c_str(); info.nShow = SW_SHOWNORMAL;
    const bool succeeded=!app.automated_test && ShellExecuteExW(&info)!=FALSE;
    if (!succeeded && !app.automated_test) { app.notice = L"文件无法打开，可能已被移动或删除。可在抽屉中右键移除失效引用。"; app.show_control(); app.invalidate(); }
    opened_item(path,double_click,succeeded);
}
void Drawer::context_menu(POINT point, bool is_handle) {
    if(app.interaction_depth) return;
    Interaction interaction(app);
    menu_open = true;
    int item = -1;
    if (!is_handle) { POINT client = point; ScreenToClient(panel, &client); client=content_point(client); item = hit_item(client.x / app.scale, client.y / app.scale); }
    if(item>=0 && !selection.contains(item)) select_item(item,false,false);
    HMENU menu = CreatePopupMenu(); int command=0; bool show_local=true;
    if (item >= 0) {
        FileMenuRequest request; request.owner=panel; request.point=point; request.paths=selected_paths();
        for(const auto& d:app.settings.drawers) if(d.id!=id) request.transfers.emplace_back(0x8000+d.id,d.name);
        for(auto& drawer:app.drawers) EnableWindow(drawer->panel,FALSE);
        const auto result=isolated_file_menu(request);
        for(auto& drawer:app.drawers) EnableWindow(drawer->panel,TRUE);
        const char* stages[]{"starting","initializing","resolving","binding","querying","ready"};
        std::string event=result.cooldown?"menu.selection.cooldown":result.timed_out?"menu.host.timeout":FAILED(result.status)?"menu.host.failed":"menu.host.completed";
        event+=" stage="+std::string(stages[static_cast<int>(result.stage)])+" preparation_ms="+std::to_string(result.preparation_ms)
            +" launch_ms="+std::to_string(result.launch_ms)+" initialize_ms="+std::to_string(result.initialize_ms)
            +" resolve_ms="+std::to_string(result.resolve_ms)+" bind_ms="+std::to_string(result.bind_ms)+" query_ms="+std::to_string(result.query_ms)
            +" stage_ms="+std::to_string(result.stage_ms)+" reused="+std::to_string(result.reused)+" host_pid="+std::to_string(result.host_pid)+" items="+std::to_string(request.paths.size());
        diagnostic(event,result.status,panel);
        if(SUCCEEDED(result.status)) { command=static_cast<int>(result.command); show_local=false; }
        else if(result.status==E_ABORT) show_local=false;
        else {
            append_file_menu_actions(menu,request.transfers,true);
            app.notice=result.cooldown?L"此项目的系统菜单暂不可用，稍后会自动重试，也可手动重试。":
                L"此项目的系统菜单未能加载，已使用基本菜单。可重试或在资源管理器中操作。"; app.invalidate();
        }
    } else {
        HMENU create=CreatePopupMenu(); AppendMenuW(create,MF_STRING,0x7030,L"文件夹\tCtrl+Shift+N"); AppendMenuW(create,MF_STRING,0x7031,L"文本文档");
        AppendMenuW(menu,MF_POPUP,reinterpret_cast<UINT_PTR>(create),L"新建到此抽屉");
        AppendMenuW(menu,MF_STRING,0x7032,L"粘贴文件\tCtrl+V");
        if(!model().folder.empty()) AppendMenuW(menu,MF_STRING,0x7035,L"打开抽屉文件夹");
        if(!model().legacy_items.empty()) AppendMenuW(menu,MF_STRING,0x7036,L"将旧引用归档到此抽屉");
        AppendMenuW(menu,MF_STRING,0x7033,L"排序方式…");
        AppendMenuW(menu,MF_STRING,0x7034,L"刷新\tF5");
        AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
        AppendMenuW(menu, MF_STRING, 0x7010, L"打开轻屉设置"); AppendMenuW(menu, MF_STRING, 0x7011, L"重命名抽屉");
        if(!model().folder.empty()) AppendMenuW(menu,MF_STRING,0x7014,L"设置英文目录名…");
        AppendMenuW(menu, MF_STRING | (pinned ? MF_CHECKED : 0), 0x7012, L"保持展开");
        HMENU sides = CreatePopupMenu();
        AppendMenuW(sides, MF_STRING, 0x7020, L"左侧"); AppendMenuW(sides, MF_STRING, 0x7021, L"右侧"); AppendMenuW(sides, MF_STRING, 0x7022, L"顶部");
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(sides), L"移到边缘");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr); AppendMenuW(menu, MF_STRING, 0x7013, L"解散抽屉");
    }
    const HWND owner = panel;
    if(show_local) { SetForegroundWindow(owner); command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, 0, owner, nullptr); }
    DestroyMenu(menu); PostMessageW(owner, WM_NULL, 0, 0); active_menu=nullptr;
    if (command == 0x7001) open_item(item);
    if (command == 0x7002 && item >= 0) reveal_file(model().items[item]);
    if (command == 0x7003) remove_selected();
    if (command == 0x7004) clipboard_files(panel,selected_paths());
    if (command == 0x7005) rename_selected();
    if (command == 0x7007) { retry_file_menu(); PostMessageW(panel,WM_CONTEXTMENU,reinterpret_cast<WPARAM>(panel),MAKELPARAM(point.x,point.y)); }
    if (command == 0x7010) app.show_control();
    if (command == 0x7011) PostMessageW(app.broker, WM_APP + 21, id, 0);
    if (command == 0x7014) PostMessageW(app.broker, WM_APP + 22, id, 0);
    if (command == 0x7012) { pinned = !pinned; if (pinned) set_open(true); }
    if (command == 0x7013) PostMessageW(app.broker, WM_EDGE_REMOVE, id, 0);
    if (command >= 0x7020 && command <= 0x7022) {
        // Defer window reconstruction until this window procedure has returned.
        PostMessageW(app.broker, WM_APP + 20, id, command - 0x7020);
    }
    if(command==0x7030 || command==0x7031) new_item(command==0x7030);
    if(command==0x7032) app.paste_files(id);
    if(command==0x7033) { menu_open=false; sort_menu(point); }
    if(command==0x7034) key_down(VK_F5);
    if(command==0x7035 && !model().folder.empty()) ShellExecuteW(panel,L"open",model().folder.c_str(),nullptr,nullptr,SW_SHOWNORMAL);
    if(command==0x7036) { const auto legacy=model().legacy_items; app.import_paths(id,legacy); }
    if(command>=0x8000) app.import_paths(command-0x8000,selected_paths(),id);
    menu_open=false;
    app.desktop_order();
}
int Drawer::resize_hit(POINT client) const {
    RECT r{}; GetClientRect(panel, &r); const int hit = static_cast<int>(7 * app.scale); const auto* d = app.find(id);
    if (!d) return 0;
    if (d->edge == Edge::Top) {
        if (client.x < hit) return 1;
        if (client.x >= r.right - hit) return 2;
        if (client.y >= r.bottom - hit) return 3;
    } else {
        if (client.y < hit) return 1;
        if (client.y >= r.bottom - hit) return 2;
        if ((d->edge == Edge::Left && client.x >= r.right - hit) || (d->edge == Edge::Right && client.x < hit)) return 3;
    }
    if (client.y < content_grid().top() && client.x < r.right - 80 * app.scale) return 4;
    return 0;
}
void Drawer::update_drag(POINT point) {
    const Edge side = model().edge;
    const int difference = side == Edge::Top ? point.x - drag_origin.x : point.y - drag_origin.y;
    const int cells = snap_cells(difference, app.pitch(side));
    auto layout = origin_layout;
    if (resize_kind == 1) resize_start(layout, id, origin_start + cells, app.capacity(side), app.minimum_span(side));
    if (resize_kind == 2) resize_end(layout, id, origin_end + cells, app.capacity(side), app.minimum_span(side));
    if (resize_kind == 4) move_slot(layout, id, origin_start + cells, app.capacity(side));
    if (resize_kind == 3) {
        const int delta = side == Edge::Top ? point.y - drag_origin.y : side == Edge::Left ? point.x - drag_origin.x : drag_origin.x - point.x;
        const int grid = side == Edge::Top ? app.grid_y : app.grid_x;
        model().depth = std::clamp(origin_depth + snap_cells(delta, grid), 3, app.maximum_depth(side));
    }
    app.apply_slots(side, layout);
}
LRESULT Drawer::window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* drawer = reinterpret_cast<Drawer*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) { drawer = static_cast<Drawer*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams); SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(drawer)); }
    if (!drawer) return DefWindowProcW(hwnd, message, wparam, lparam);
    LRESULT shell_result{};
    if(drawer->active_menu && drawer->active_menu->message(message,wparam,lparam,shell_result)) return shell_result;
    // Automated native tests dispatch their own mouse messages. A real pointer
    // resting at the screen edge must not open unrelated test drawers.
    if(drawer->app.automated_test && !drawer->app.smoke_dispatch &&
        ((message>=WM_MOUSEFIRST && message<=WM_MOUSELAST) || message==WM_MOUSELEAVE)) return 0;
    switch (message) {
    // Read-only probes for isolated UI tests. Normal profiles do not expose them.
    case WM_APP+90: if(drawer->app.smoke) return static_cast<LRESULT>(drawer->model().items.size()); break;
    case WM_APP+91: if(drawer->app.smoke) return static_cast<LRESULT>(drawer->selection.size()); break;
    case WM_NCCALCSIZE: return 0;
    case WM_NCHITTEST: return HTCLIENT;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_SETFOCUS: case WM_KILLFOCUS: InvalidateRect(hwnd,nullptr,FALSE); return 0;
    case WM_KEYDOWN: if(drawer->target>0) drawer->key_down(static_cast<UINT>(wparam)); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: drawer->paint(); return 0;
    case WM_SIZE: InvalidateRect(hwnd, nullptr, FALSE); return 0;
    case file_image_ready: InvalidateRect(hwnd,nullptr,FALSE); return 0;
    case WM_CLOSE: drawer->set_open(false); return 0;
    case WM_EDGE_ANIMATION_DONE: drawer->finish_animation(wparam); return 0;
    case WM_MOUSEMOVE: {
        if(drawer->pressed>=0 && (wparam&MK_LBUTTON)) {
            const POINT point{GET_X_LPARAM(lparam),GET_Y_LPARAM(lparam)};
            if(std::abs(point.x-drawer->press_point.x)>=GetSystemMetrics(SM_CXDRAG) || std::abs(point.y-drawer->press_point.y)>=GetSystemMetrics(SM_CYDRAG)) { drawer->drag_files(); return 0; }
        }
        if (drawer->resize_kind) { POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)}; ClientToScreen(hwnd, &point); drawer->update_drag(point); return 0; }
        if (!drawer->tracking_panel) { TRACKMOUSEEVENT event{sizeof(event), TME_LEAVE, hwnd, 0}; TrackMouseEvent(&event); drawer->tracking_panel = true; }
        const POINT local = drawer->content_point({GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
        // The HWND covers the expanded bounds during motion, including pixels
        // the moving body hasn't reached. Those pixels must not reopen it.
        if (local.x < 0 || local.y < 0 || local.x >= drawer->bounds.right-drawer->bounds.left || local.y >= drawer->bounds.bottom-drawer->bounds.top) {
            drawer->watch_pointer(); return 0;
        }
        if (drawer->target == 0) drawer->set_open(true);
        drawer->preview_deadline = 0;
        const int selected = drawer->hit_item(local.x/drawer->app.scale, local.y/drawer->app.scale);
        if (selected != drawer->selected) { drawer->selected = selected; InvalidateRect(hwnd, nullptr, FALSE); }
        return 0;
    }
    case WM_MOUSELEAVE:
        drawer->tracking_panel = false;
        if (drawer->selected != -1) { drawer->selected = -1; InvalidateRect(hwnd, nullptr, FALSE); }
        drawer->watch_pointer();
        return 0;
    case WM_TIMER:
        if(wparam==render_retry_timer) { KillTimer(hwnd,render_retry_timer); InvalidateRect(hwnd,nullptr,FALSE); }
        if (wparam == pointer_timer) drawer->watch_pointer();
        if (wparam == completion_watchdog) drawer->finish_animation(drawer->motion_serial);
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lparam) == HTCLIENT) {
            POINT point{}; GetCursorPos(&point); ScreenToClient(hwnd, &point); point = drawer->content_point(point);
            const int hit = drawer->current_progress() < .15f ? 4 : drawer->resize_hit(point);
            LPCWSTR cursor = IDC_ARROW;
            if (hit == 1 || hit == 2) cursor = drawer->model().edge == Edge::Top ? IDC_SIZEWE : IDC_SIZENS;
            if (hit == 3) cursor = drawer->model().edge == Edge::Top ? IDC_SIZENS : IDC_SIZEWE;
            if (hit == 4) cursor = IDC_SIZEALL;
            SetCursor(LoadCursorW(nullptr, cursor)); return TRUE;
        }
        break;
    case WM_LBUTTONDOWN: {
        const bool peek = drawer->current_progress() < .15f;
        const POINT client = drawer->content_point({GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
        drawer->resize_kind = peek ? 4 : drawer->resize_hit(client);
        if (peek) {
            RECT rect{}; GetClientRect(hwnd, &rect); const bool top = drawer->model().edge == Edge::Top;
            if ((top ? client.x : client.y) < 9 * drawer->app.scale) drawer->resize_kind = 1;
            else if ((top ? client.x >= rect.right-9*drawer->app.scale : client.y >= rect.bottom-9*drawer->app.scale)) drawer->resize_kind = 2;
            drawer->set_open(true);
        }
        if (drawer->resize_kind) {
            SetCapture(hwnd); drawer->drag_origin = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)}; ClientToScreen(hwnd, &drawer->drag_origin); const auto& d = drawer->model();
            drawer->origin_start = d.start; drawer->origin_end = d.start+d.span; drawer->origin_depth = d.depth; drawer->origin_layout = drawer->app.slots(d.edge);
        } else {
            SetForegroundWindow(hwnd); SetFocus(hwnd); drawer->app.desktop_order();
            const int item=drawer->hit_item(client.x/drawer->app.scale,client.y/drawer->app.scale);
            const bool control=(wparam&MK_CONTROL)!=0, shift=(wparam&MK_SHIFT)!=0;
            if(control || shift || !drawer->selection.contains(item)) drawer->select_item(item,control,shift);
            else drawer->focused=item;
            drawer->pressed=item; drawer->press_point={GET_X_LPARAM(lparam),GET_Y_LPARAM(lparam)};
            if(item>=0) SetCapture(hwnd);
        }
        return 0;
    }
    case WM_LBUTTONUP:
        drawer->pressed=-1;
        if(GetCapture()==hwnd && !drawer->resize_kind) ReleaseCapture();
        if (drawer->resize_kind) { drawer->resize_kind = 0; ReleaseCapture(); drawer->app.save(); }
        else {
            const POINT local = drawer->content_point({GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
            RECT rect{}; GetClientRect(hwnd, &rect);
            if (local.y >= 0 && local.y < drawer->content_grid().top()) {
                POINT point{static_cast<LONG>(rect.right-78*drawer->app.scale),drawer->content_grid().top()}; ClientToScreen(hwnd,&point);
                if(local.x>=rect.right-80*drawer->app.scale && local.x<rect.right-42*drawer->app.scale) drawer->sort_menu(point);
                else if(local.x>=rect.right-42*drawer->app.scale) drawer->context_menu(point,true);
            }
        }
        drawer->apply_sort();
        return 0;
    case WM_CAPTURECHANGED: drawer->pressed=-1; if (drawer->resize_kind) { drawer->resize_kind = 0; drawer->app.save(); } return 0;
    case WM_LBUTTONDBLCLK: {
        const POINT local = drawer->content_point({GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
        drawer->open_item(drawer->hit_item(local.x/drawer->app.scale, local.y/drawer->app.scale),true); return 0;
    }
    case WM_MOUSEWHEEL: drawer->scroll += GET_WHEEL_DELTA_WPARAM(wparam) < 0 ? 1 : -1; InvalidateRect(hwnd, nullptr, FALSE); return 0;
    case WM_CONTEXTMENU: {
        POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)}; if (point.x == -1 && point.y == -1) GetCursorPos(&point);
        drawer->context_menu(point, drawer->current_progress() < .9f); return 0;
    }
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}
}
