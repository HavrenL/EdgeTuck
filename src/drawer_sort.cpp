#include "app.hpp"
#include <algorithm>

namespace edge {
void Drawer::request_sort(bool after_close) {
    sort_ticket=++app.next_sort_ticket; sort_on_close=after_close; pending_sort.reset();
    if(model().sort==SortMode::Manual) return;
    if(model().sort==SortMode::Recent) {
        pending_sort=recent_sort(model(),sort_ticket); apply_sort();
    } else app.sort_queue.request(model(),sort_ticket,app.broker,WM_EDGE_SORTED);
}
void Drawer::apply_sort() {
    if(!pending_sort || app.interaction_depth || app.storage_migrating || animating || menu_open || dropping || dragging_out || pressed>=0) return;
    if(sort_on_close && !collapsed()) return;
    const auto& result=*pending_sort;
    if(result.ticket!=sort_ticket || result.mode!=model().sort || result.descending!=model().sort_descending) { pending_sort.reset(); return; }
    if(result.before!=model().items) { request_sort(sort_on_close); return; }
    const bool changed=result.after!=model().items;
    const bool reset_scroll=scroll!=0 && (changed || !sort_on_close || model().sort==SortMode::Recent);
    if(changed) {
        model().items=std::move(pending_sort->after);
        selection.clear(); selected=-1; focused=anchor=0; scroll=0;
    }
    if(reset_scroll) scroll=0;
    pending_sort.reset();
    if(changed) app.save();
    if(changed || reset_scroll) {
        prepare_images(collapsed()?ImagePriority::Background:ImagePriority::Visible);
        InvalidateRect(panel,nullptr,FALSE);
    }
}
void Drawer::opened_item(const std::wstring& path,bool double_click,bool succeeded) {
    if(!double_click || !succeeded) return;
    FILETIME now{}; GetSystemTimeAsFileTime(&now);
    const uint64_t time=(static_cast<uint64_t>(now.dwHighDateTime)<<32)|now.dwLowDateTime;
    if(!record_recent_use(model(),path,time)) return;
    app.save(); // Persist usage without repainting or changing visible indices.
    if(model().sort==SortMode::Recent) request_sort(true);
}
void Drawer::choose_sort(SortMode mode,bool descending) {
    if(mode<SortMode::Manual || mode>SortMode::Recent || app.storage_migrating) return;
    model().sort=mode; model().sort_descending=mode==SortMode::Recent?true:descending;
    app.save(); request_sort(mode==SortMode::Recent);
}
void Drawer::sort_menu(POINT point) {
    if(menu_open || app.storage_migrating) return;
    Interaction interaction(app); menu_open=true;
    HMENU menu=CreatePopupMenu();
    const wchar_t* labels[]{L"自定义顺序",L"名称",L"大小",L"类型",L"修改时间",L"最近使用"};
    for(int i=0;i<6;++i) AppendMenuW(menu,MF_STRING,100+i,labels[i]);
    CheckMenuRadioItem(menu,100,105,100+static_cast<UINT>(model().sort),MF_BYCOMMAND);
    AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
    const UINT disabled=model().sort==SortMode::Recent || model().sort==SortMode::Manual?MF_GRAYED:0;
    AppendMenuW(menu,MF_STRING|disabled,110,L"升序"); AppendMenuW(menu,MF_STRING|disabled,111,L"降序");
    CheckMenuRadioItem(menu,110,111,model().sort_descending?111:110,MF_BYCOMMAND);
    AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
    AppendMenuW(menu,MF_STRING|MF_GRAYED,0,L"最近使用：双击打开，收回后更新");
    SetForegroundWindow(panel);
    const int command=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_RIGHTBUTTON,point.x,point.y,0,panel,nullptr);
    DestroyMenu(menu); PostMessageW(panel,WM_NULL,0,0);
    if(command>=100 && command<=105) {
        const auto mode=static_cast<SortMode>(command-100);
        choose_sort(mode,mode==SortMode::Size || mode==SortMode::Modified || mode==SortMode::Recent);
    } else if(command==110 || command==111) choose_sort(model().sort,command==111);
    menu_open=false; app.desktop_order(); watch_pointer();
}
}
