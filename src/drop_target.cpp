#include "drop_target.hpp"
#include "app.hpp"

namespace edge {
DropTarget::DropTarget(Drawer& drawer) : drawer(drawer) {
    // Explorer already supplies the thumbnail/stack in its data object. Let
    // Shell retain that image across the boundary instead of drawing a copy.
    CoCreateInstance(CLSID_DragDropHelper,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&drag_image));
}
DropTarget::~DropTarget() { if(image_entered) drag_image->DragLeave(); }
HRESULT DropTarget::QueryInterface(REFIID iid,void** out) {
    if(!out) return E_POINTER; *out=nullptr;
    if(iid==IID_IUnknown || iid==IID_IDropTarget) { *out=static_cast<IDropTarget*>(this); AddRef(); return S_OK; }
    return E_NOINTERFACE;
}
ULONG DropTarget::AddRef() { return InterlockedIncrement(&references); }
ULONG DropTarget::Release() { const auto count=InterlockedDecrement(&references); if(!count) delete this; return count; }
static DWORD choose_effect(DWORD allowed,DWORD keys,bool preview) {
    if(preview) return allowed&DROPEFFECT_LINK;
    if(keys&MK_CONTROL) return allowed&DROPEFFECT_COPY;
    if(keys&MK_SHIFT) return allowed&DROPEFFECT_MOVE;
    return (allowed&DROPEFFECT_MOVE)?DROPEFFECT_MOVE:(allowed&DROPEFFECT_COPY);
}
HRESULT DropTarget::DragEnter(IDataObject* data,DWORD keys,POINTL point,DWORD* effect) {
    if(!effect) return E_POINTER;
    DragLeave();
    allowed=*effect;
    FORMATETC format{CF_HDROP,nullptr,DVASPECT_CONTENT,-1,TYMED_HGLOBAL};
    accepted=!drawer.app.storage_migrating && data && SUCCEEDED(data->QueryGetData(&format)) && choose_effect(allowed,0,(drawer.app.smoke && !drawer.app.archive_test))!=0;
    internal=data && data==drawer.app.drag_data && drawer.app.drag_source!=0;
    if(accepted) { ++drawer.app.interaction_depth; holding_interaction=true; }
    update_feedback(keys); *effect=chosen;
    if(drag_image && data) {
        POINT screen{point.x,point.y};
        image_entered=SUCCEEDED(drag_image->DragEnter(drawer.panel,data,&screen,chosen));
    }
    return S_OK;
}
void DropTarget::update_feedback(DWORD keys) {
    chosen=accepted && !drawer.app.storage_migrating?choose_effect(allowed,keys,(drawer.app.smoke && !drawer.app.archive_test)):DROPEFFECT_NONE;
    const bool highlight=chosen!=DROPEFFECT_NONE;
    if(drawer.dropping!=highlight) {
        drawer.dropping=highlight;
        if(highlight) drawer.set_open(true);
        InvalidateRect(drawer.panel,nullptr,FALSE);
    }
}
HRESULT DropTarget::DragOver(DWORD keys,POINTL point,DWORD* effect) {
    if(!effect) return E_POINTER;
    update_feedback(keys); *effect=chosen;
    if(image_entered) { POINT screen{point.x,point.y}; drag_image->DragOver(&screen,chosen); }
    return S_OK;
}
void DropTarget::clear_feedback() {
    const bool repaint=drawer.dropping;
    accepted=false; internal=false; drawer.dropping=false; chosen=allowed=0;
    if(repaint) InvalidateRect(drawer.panel,nullptr,FALSE);
    if(holding_interaction) { holding_interaction=false; if(--drawer.app.interaction_depth==0) PostMessageW(drawer.app.broker,WM_EDGE_DEFERRED,0,0); }
}
HRESULT DropTarget::DragLeave() {
    if(image_entered) { drag_image->DragLeave(); image_entered=false; }
    clear_feedback();
    return S_OK;
}
HRESULT DropTarget::Drop(IDataObject* data,DWORD keys,POINTL point,DWORD* effect) {
    if(!effect) return E_POINTER;
    const DWORD action=accepted && !drawer.app.storage_migrating?choose_effect(allowed,keys,(drawer.app.smoke && !drawer.app.archive_test)):DROPEFFECT_NONE;
    *effect=DROPEFFECT_NONE;
    // The file operation may open a collision dialog. Do not leave the drag
    // image hovering over that dialog while the operation runs.
    if(image_entered) drag_image->Show(FALSE);
    if(action && data) {
        const bool own=internal && data==drawer.app.drag_data;
        POINT local{point.x,point.y}; ScreenToClient(drawer.panel,&local); local=drawer.content_point(local);
        const int item=drawer.hit_item(local.x/drawer.app.scale,local.y/drawer.app.scale);
        const auto before=item>=0?static_cast<size_t>(item):drawer.model().items.size();
        const auto paths=own?drawer.app.drag_paths:data_paths(data);
        if(drawer.app.import_paths(drawer.id,paths,own?drawer.app.drag_source:0,before,action==DROPEFFECT_COPY)) {
            if((drawer.app.smoke && !drawer.app.archive_test)) *effect=DROPEFFECT_LINK;
            else if(action==DROPEFFECT_COPY) *effect=DROPEFFECT_COPY;
            else { completed_file_move(data); *effect=DROPEFFECT_NONE; }
        }
    }
    if(image_entered) {
        POINT screen{point.x,point.y}; drag_image->Drop(data,&screen,*effect); image_entered=false;
    }
    clear_feedback(); return S_OK;
}
}
