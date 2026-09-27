#include "app.hpp"
#include <windowsx.h>
#include <algorithm>

namespace edge {
std::vector<std::wstring> Drawer::selected_paths() const {
    std::vector<std::wstring> paths; const auto* d=app.find(id); if(!d) return paths;
    const bool available=d->folder.empty() || folder_available(*d);
    for(int index:selection) if(index>=0 && index<static_cast<int>(d->items.size())) {
        const auto& path=d->items[index];
        if(available || !path_within(path,d->folder)) paths.push_back(path);
    }
    return paths;
}
void Drawer::select_item(int item, bool control, bool shift) {
    if(item<0 || item>=item_count()) { if(!control) selection.clear(); }
    else {
        if(shift) {
            if(!control) selection.clear();
            for(int i=std::max(0,std::min(anchor,item));i<=std::max(anchor,item);++i) selection.insert(i);
        } else if(control) { if(selection.contains(item)) selection.erase(item); else selection.insert(item); anchor=item; }
        else { selection.clear(); selection.insert(item); anchor=item; }
        focused=item;
    }
    InvalidateRect(panel,nullptr,FALSE);
}
class FileDragSource final : public IDropSource {
    LONG references{1};
public:
    bool desktop_release{};
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out) override {
        if(!out) return E_POINTER; *out=nullptr;
        if(iid!=IID_IUnknown && iid!=IID_IDropSource) return E_NOINTERFACE;
        *out=static_cast<IDropSource*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&references); }
    ULONG STDMETHODCALLTYPE Release() override { auto n=InterlockedDecrement(&references); if(!n) delete this; return n; }
    HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL escape,DWORD keys) override {
        if(escape || (keys&MK_RBUTTON)) return DRAGDROP_S_CANCEL;
        if(!(keys&MK_LBUTTON)) {
            POINT point{}; GetCursorPos(&point);
            // Handle desktop returns through the same collision-safe file operation.
            desktop_release=desktop_at(point);
            return desktop_release?DRAGDROP_S_CANCEL:DRAGDROP_S_DROP;
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override {
        POINT point{}; GetCursorPos(&point);
        if(desktop_at(point)) { SetCursor(LoadCursorW(nullptr,IDC_ARROW)); return S_OK; }
        return DRAGDROP_S_USEDEFAULTCURSORS;
    }
};
void Drawer::drag_files() {
    const auto paths=selected_paths(); if(paths.empty() || dragging_out) return;
    Interaction interaction(app);
    auto data=file_data(paths); if(!data) return;
    ComPtr<FileDragSource> source; source.Attach(new FileDragSource);
    dragging_out=true; pressed=-1; ReleaseCapture();
    app.drag_data=data.Get(); app.drag_source=id; app.drag_paths=paths;
    DWORD effect{}; const HRESULT hr=DoDragDrop(data.Get(),source.Get(),DROPEFFECT_COPY | DROPEFFECT_LINK | (app.smoke?0:DROPEFFECT_MOVE),&effect);
    app.drag_data=nullptr; app.drag_source=0; app.drag_paths.clear(); dragging_out=false;
    if(hr==DRAGDROP_S_CANCEL && source->desktop_release) app.return_to_desktop(id,paths);
    else if(!app.smoke) app.schedule_folders(true);
    watch_pointer();
}
void Drawer::remove_selected() {
    app.return_to_desktop(id,selected_paths());
}
void Drawer::new_item(bool folder) {
    if(app.smoke) { app.notice=L"隔离预览不向真实桌面新建文件。"; app.invalidate(); return; }
    Interaction interaction(app); menu_open=true;
    if(!app.ensure_folder(id)) { menu_open=false; return; }
    if(model().items.size()>=1000) { app.notice=L"抽屉已达到显示上限，请在资源管理器中操作。"; menu_open=false; return; }
    DWORD error{}; const auto path=create_desktop_item(model().folder,folder,error);
    if(path.empty()) { app.notice=L"无法在抽屉文件夹中新建，请检查写入权限。"; app.show_control(); }
    else {
        if(!place_references(app.settings,id,{path.wstring()})) {
            app.notice=L"文件已创建在抽屉文件夹中，请刷新查看。"; app.invalidate(); menu_open=false; return;
        }
        app.references_changed();
        const auto& items=model().items;
        for(size_t i=0;i<items.size();++i) if(same_path(items[i],path.wstring())) { select_item(static_cast<int>(i),false,false); break; }
        rename_selected();
    }
    menu_open=false; watch_pointer();
}
void Drawer::rename_selected() {
    const auto paths=selected_paths(); if(paths.size()!=1 || app.automated_test) return;
    Interaction interaction(app); const bool was_menu=menu_open; menu_open=true;
    const auto name=app.ask_name(panel,L"重命名文件",std::filesystem::path(paths[0]).filename().wstring(),255);
    if(name && *name!=std::filesystem::path(paths[0]).filename().wstring()) {
        const HRESULT hr=rename_file(panel,paths[0],*name);
        if(SUCCEEDED(hr)) {
            const auto renamed=(std::filesystem::path(paths[0]).parent_path()/ *name).wstring();
            if(GetFileAttributesW(renamed.c_str())!=INVALID_FILE_ATTRIBUTES && rename_references(app.settings,paths[0],renamed)) app.references_changed();
        } else if(hr!=HRESULT_FROM_WIN32(ERROR_CANCELLED)) { app.notice=L"未能重命名，请检查名称、权限或文件占用。"; app.show_control(); }
    }
    menu_open=was_menu;
}
void Drawer::key_down(UINT key) {
    const bool control=GetKeyState(VK_CONTROL)<0, shift=GetKeyState(VK_SHIFT)<0;
    if(key==VK_ESCAPE) { selection.clear(); pinned=false; set_open(false); return; }
    if(control && key=='A') { for(int i=0;i<item_count();++i) selection.insert(i); InvalidateRect(panel,nullptr,FALSE); return; }
    if(control && key=='C') { Interaction interaction(app); clipboard_files(panel,selected_paths()); return; }
    if(control && key=='V') {
        app.paste_files(id);
        return;
    }
    if(control && shift && key=='N') { new_item(true); return; }
    if(key==VK_DELETE) { remove_selected(); return; }
    if(key==VK_F2) { rename_selected(); return; }
    if(key==VK_F5) {
        app.refresh_archives(); app.schedule_folders();
        for(const auto& p:model().items) app.graphics.file_images.forget(p);
        panel_canvas.icons.clear(); InvalidateRect(panel,nullptr,FALSE); return;
    }
    if(key==VK_RETURN) { open_item(focused); return; }
    if(item_count()==0) return;
    int next=focused;
    if(key==VK_LEFT) --next; else if(key==VK_RIGHT) ++next;
    else if(key==VK_UP) next-=columns(); else if(key==VK_DOWN) next+=columns();
    else if(key==VK_HOME) next=0; else if(key==VK_END) next=item_count()-1;
    else if(key==VK_SPACE) { select_item(focused,control,shift); return; }
    else return;
    next=std::clamp(next,0,item_count()-1);
    if(control && !shift) focused=next; else select_item(next,control,shift);
    const int row=next/columns(), rows=std::max(1,content_grid().rows());
    if(row<scroll) scroll=row; else if(row>=scroll+rows) scroll=row-rows+1;
    InvalidateRect(panel,nullptr,FALSE);
}
}
