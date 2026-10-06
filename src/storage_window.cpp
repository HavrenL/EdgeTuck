#include "app.hpp"
#include "storage.hpp"
#include <thread>
#include <atomic>

namespace edge {
namespace {
constexpr wchar_t storage_class[]=L"EdgeTuck.Storage";
constexpr UINT progress_message=WM_APP+120,finished_message=WM_APP+121;
struct StorageWindow {
    App& app; HWND window{},path{},summary{},desktop{},directory{},status{},explorer{};
    HFONT font{}; float scale{1}; bool done{},busy{};
    StorageMode mode; std::wstring location;
    std::vector<HWND> editing;
    std::atomic_bool cancel{};
    std::thread worker;
    MigrationResult result;
    explicit StorageWindow(App& owner):app(owner),mode(owner.settings.storage_mode),
        location(owner.settings.storage_directory.empty()?default_storage_directory().wstring():owner.settings.storage_directory) {}
    ~StorageWindow() { cancel=true; if(worker.joinable()) worker.join(); if(font) DeleteObject(font); }
    HWND child(LPCWSTR cls,LPCWSTR text,DWORD style,int id,int x,int y,int w,int h) {
        auto control=CreateWindowExW(wcscmp(cls,L"EDIT")==0?WS_EX_CLIENTEDGE:0,cls,text,WS_CHILD|WS_VISIBLE|style,
            static_cast<int>(x*scale),static_cast<int>(y*scale),static_cast<int>(w*scale),static_cast<int>(h*scale),window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),app.instance,nullptr);
        SendMessageW(control,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE); return control;
    }
    std::filesystem::path target() const { return mode==StorageMode::Desktop?app.desktop_root():std::filesystem::path(location); }
    void sync() {
        SendMessageW(desktop,BM_SETCHECK,mode==StorageMode::Desktop?BST_CHECKED:BST_UNCHECKED,0);
        SendMessageW(directory,BM_SETCHECK,mode==StorageMode::Directory?BST_CHECKED:BST_UNCHECKED,0);
        SetWindowTextW(path,target().c_str());
        int inside=0; for(const auto& d:app.settings.drawers) if(same_path(std::filesystem::path(d.folder).parent_path().wstring(),app.desktop_root().wstring())) ++inside;
        const auto text=L"现有 "+std::to_wstring(app.settings.drawers.size())+L" 个抽屉，其中 "+std::to_wstring(inside)+L" 个保存在桌面。\r\n"+
            (ascii_path(target())?L"保存位置只影响新抽屉；下面的迁移按钮才会处理已有文件。":L"注意：上级路径含非英文字符，开发工具可能仍不兼容，请选择英文路径。");
        SetWindowTextW(summary,text.c_str());
        for(int id:{203,204}) EnableWindow(GetDlgItem(window,id),!busy && mode==StorageMode::Directory);
        const auto entry=app.smoke?ExplorerEntryStatus{}:explorer_entry_status();
        SendMessageW(explorer,BM_SETCHECK,entry.enabled?BST_CHECKED:BST_UNCHECKED,0);
        EnableWindow(GetDlgItem(window,209),!busy && entry.enabled && !entry.error);
    }
    void create(HWND hwnd) {
        window=hwnd; scale=GetDpiForWindow(hwnd)/96.0f;
        font=CreateFontW(-static_cast<int>(14*scale),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Microsoft YaHei UI");
        child(L"STATIC",L"文件属于你。新抽屉必填英文目录名，标题可用中文。",0,0,24,20,568,24);
        desktop=child(L"BUTTON",L"桌面模式：分类文件夹保存在桌面",BS_AUTORADIOBUTTON|WS_TABSTOP|WS_GROUP,201,24,62,560,28);
        directory=child(L"BUTTON",L"独立目录：选择自己的收纳文件夹（推荐）",BS_AUTORADIOBUTTON|WS_TABSTOP,202,24,99,560,28);
        path=child(L"EDIT",L"",ES_READONLY|ES_AUTOHSCROLL|WS_TABSTOP,210,24,142,568,31);
        editing={desktop,directory};
        editing.push_back(child(L"BUTTON",L"选择文件夹…",BS_PUSHBUTTON|WS_TABSTOP,203,24,187,133,32));
        editing.push_back(child(L"BUTTON",L"使用推荐位置",BS_PUSHBUTTON|WS_TABSTOP,204,167,187,133,32));
        editing.push_back(child(L"BUTTON",L"打开此位置",BS_PUSHBUTTON|WS_TABSTOP,205,310,187,128,32));
        summary=child(L"STATIC",L"",0,0,24,241,568,50);
        editing.push_back(child(L"BUTTON",L"保存新抽屉的位置",BS_PUSHBUTTON|WS_TABSTOP,206,24,300,180,34));
        editing.push_back(child(L"BUTTON",L"同步现有文件夹名称…",BS_PUSHBUTTON|WS_TABSTOP,211,224,300,210,34));
        child(L"STATIC",L"同盘直接移动整个文件夹；跨盘复制并校验后切换，旧文件夹送到回收站。\r\n同名目录会另取名称，不覆盖或合并；文件不依赖轻屉才能访问。",0,0,24,355,568,54);
        editing.push_back(child(L"BUTTON",L"一键迁移现有抽屉",BS_PUSHBUTTON|WS_TABSTOP,207,24,426,210,36));
        child(L"BUTTON",L"关闭",BS_DEFPUSHBUTTON|WS_TABSTOP,IDCANCEL,488,426,104,36);
        explorer=child(L"BUTTON",L"在“此电脑”中显示轻屉文件夹",BS_AUTOCHECKBOX|WS_TABSTOP,208,24,480,405,28);
        editing.push_back(explorer);
        editing.push_back(child(L"BUTTON",L"打开入口",BS_PUSHBUTTON|WS_TABSTOP,209,464,480,128,30));
        child(L"STATIC",L"收纳会移动文件，程序或开发环境的外部路径配置不会自动修改。",0,0,24,516,568,24);
        status=child(L"STATIC",L"",0,0,24,552,568,64);
        sync();
    }
    void choose() {
        ComPtr<IFileOpenDialog> picker;
        if(FAILED(CoCreateInstance(CLSID_FileOpenDialog,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&picker)))) return;
        DWORD flags{}; picker->GetOptions(&flags);
        picker->SetOptions(flags|FOS_PICKFOLDERS|FOS_FORCEFILESYSTEM|FOS_PATHMUSTEXIST|FOS_NOCHANGEDIR);
        picker->SetTitle(L"选择独立于轻屉程序的收纳文件夹");
        ComPtr<IShellItem> initial; if(SUCCEEDED(SHCreateItemFromParsingName(location.c_str(),nullptr,IID_PPV_ARGS(&initial)))) picker->SetFolder(initial.Get());
        if(FAILED(picker->Show(window))) return;
        ComPtr<IShellItem> chosen; PWSTR value{};
        if(SUCCEEDED(picker->GetResult(&chosen)) && SUCCEEDED(chosen->GetDisplayName(SIGDN_FILESYSPATH,&value))) {
            location=value; CoTaskMemFree(value); mode=StorageMode::Directory; sync();
        }
    }
    void apply() {
        if(app.smoke) { SetWindowTextW(status,L"隔离预览不会更改真实存储位置。"); return; }
        std::wstring error;
        if(!prepare_storage_directory(app.settings,target(),error)) { SetWindowTextW(status,error.c_str()); return; }
        auto candidate=app.settings; candidate.storage_mode=mode; candidate.storage_directory=location;
        if(!app.save_allowed || !save_settings(app.config_path,candidate,error)) { SetWindowTextW(status,error.empty()?L"当前配置不可写。":error.c_str()); return; }
        app.settings=std::move(candidate); app.sync_explorer(); app.invalidate();
        SetWindowTextW(status,L"已保存。以后新建的抽屉使用这个位置，已有抽屉保持原位置。"); sync();
    }
    void migrate() {
        if(app.smoke || !app.save_allowed) { SetWindowTextW(status,L"当前为隔离预览或配置不可写，无法迁移。"); return; }
        if(app.settings.drawers.empty()) { apply(); return; }
        busy=true; cancel=false; app.storage_migrating=true;
        for(auto hwnd:editing) EnableWindow(hwnd,FALSE);
        for(auto& d:app.drawers) { d->set_open(false); EnableWindow(d->panel,FALSE); }
        SetWindowTextW(GetDlgItem(window,IDCANCEL),L"取消迁移");
        const auto before=app.settings; const auto destination=target(); const auto selected_mode=mode;
        try {
            worker=std::thread([this,before,destination,selected_mode] {
                const auto initialized=OleInitialize(nullptr);
                if(SUCCEEDED(initialized)) {
                    result=migrate_storage(window,before,selected_mode,destination,app.config_path,cancel,
                        [this](MigrationStage,const std::wstring& text) {
                            auto message=std::make_unique<std::wstring>(text);
                            if(PostMessageW(window,progress_message,0,reinterpret_cast<LPARAM>(message.get()))) message.release();
                        });
                    OleUninitialize();
                } else result.message=L"无法启动文件操作，请稍后重试。";
                PostMessageW(window,finished_message,0,0);
            });
        } catch(...) { busy=false; app.storage_migrating=false; for(auto hwnd:editing) EnableWindow(hwnd,TRUE); for(auto& d:app.drawers) EnableWindow(d->panel,TRUE); SetWindowTextW(GetDlgItem(window,IDCANCEL),L"关闭"); SetWindowTextW(status,L"无法启动迁移任务。"); sync(); }
    }
    void complete() {
        if(worker.joinable()) worker.join(); busy=false; app.storage_migrating=false;
        if(result.recovery_required) app.save_allowed=false;
        if(result.committed) {
            app.settings=std::move(result.settings); app.undo.reset(); app.folder_positions.clear();
            for(auto& old:result.retained_sources) restore_folder_icon(old);
            app.refresh_archives(); app.references_changed(); app.schedule_folders(true);
            app.sync_explorer();
            mode=app.settings.storage_mode;
            location=app.settings.storage_directory.empty()?default_storage_directory().wstring():app.settings.storage_directory;
        }
        for(auto& d:app.drawers) EnableWindow(d->panel,TRUE);
        for(auto hwnd:editing) EnableWindow(hwnd,TRUE);
        SetWindowTextW(GetDlgItem(window,IDCANCEL),L"关闭");
        app.notice=result.message; app.invalidate(); SetWindowTextW(status,result.message.c_str()); sync();
    }
};
LRESULT CALLBACK storage_proc(HWND hwnd,UINT message,WPARAM wp,LPARAM lp) {
    auto* state=reinterpret_cast<StorageWindow*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
    if(message==WM_NCCREATE) { state=static_cast<StorageWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams); SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(state)); }
    if(!state) return DefWindowProcW(hwnd,message,wp,lp);
    switch(message) {
    case WM_CREATE: state->create(hwnd); return 0;
    case progress_message: { std::unique_ptr<std::wstring> text(reinterpret_cast<std::wstring*>(lp)); SetWindowTextW(state->status,text->c_str()); return 0; }
    case finished_message: state->complete(); return 0;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
        SetTextColor(reinterpret_cast<HDC>(wp),GetSysColor(COLOR_WINDOWTEXT));
        SetBkColor(reinterpret_cast<HDC>(wp),GetSysColor(COLOR_WINDOW));
        return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
    case WM_QUERYENDSESSION: return state->busy?FALSE:TRUE;
    case WM_COMMAND:
        if(LOWORD(wp)==IDCANCEL || LOWORD(wp)==IDOK) { SendMessageW(hwnd,WM_CLOSE,0,0); return 0; }
        if(state->busy) return 0;
        switch(LOWORD(wp)) {
        case 201: state->mode=StorageMode::Desktop; state->sync(); break;
        case 202: state->mode=StorageMode::Directory; state->sync(); break;
        case 203: state->choose(); break;
        case 204: state->location=default_storage_directory().wstring(); state->mode=StorageMode::Directory; state->sync(); break;
        case 205:
            if(GetFileAttributesW(state->target().c_str())==INVALID_FILE_ATTRIBUTES) SetWindowTextW(state->status,L"此位置尚未创建，保存位置或迁移时会自动创建。");
            else ShellExecuteW(hwnd,L"open",state->target().c_str(),nullptr,nullptr,SW_SHOWNORMAL);
            break;
        case 206: state->apply(); break;
        case 207: state->migrate(); break;
        case 208: {
            const bool enable=SendMessageW(state->explorer,BM_GETCHECK,0,0)==BST_CHECKED;
            const auto error=state->app.set_explorer_visible(enable);
            const auto text=error?L"入口未能更新（错误 "+std::to_wstring(error)+L"）。请先保存有效的存放位置。":
                std::wstring(enable?L"已添加到此电脑，选择文件时也可从此进入。":L"已移除此电脑入口，分类文件夹和内容保持原样。");
            SetWindowTextW(state->status,text.c_str()); state->sync(); break;
        }
        case 209: open_explorer_entry(hwnd); break;
        case 211: state->app.sync_folder_names(hwnd); SetWindowTextW(state->status,state->app.notice.c_str()); state->sync(); break;
        } return 0;
    case WM_CLOSE:
        if(state->busy) { state->cancel=true; SetWindowTextW(state->status,L"正在取消。尚未切换位置的同盘移动会恢复原位，已复制的副本会保留。请等待操作结束。"); return 0; }
        state->done=true; DestroyWindow(hwnd); return 0;
    }
    return DefWindowProcW(hwnd,message,wp,lp);
}
}
void App::show_storage() {
    if(interaction_depth) return;
    Interaction interaction(*this); StorageWindow state(*this);
    WNDCLASSW cls{}; cls.hInstance=instance; cls.lpfnWndProc=storage_proc; cls.lpszClassName=storage_class;
    cls.hCursor=LoadCursorW(nullptr,IDC_ARROW); cls.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_WINDOW+1); cls.hIcon=LoadIconW(instance,MAKEINTRESOURCEW(1));
    RegisterClassW(&cls);
    RECT r{0,0,static_cast<LONG>(616*scale),static_cast<LONG>(626*scale)};
    constexpr DWORD style=WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU;
    AdjustWindowRectExForDpi(&r,style,FALSE,0,GetDpiForSystem());
    const auto hwnd=CreateWindowExW(0,storage_class,L"轻屉 · 存放位置与迁移",style,
        work.left+((work.right-work.left)-(r.right-r.left))/2,work.top+((work.bottom-work.top)-(r.bottom-r.top))/2,
        r.right-r.left,r.bottom-r.top,control,nullptr,instance,&state);
    if(!hwnd) return;
    EnableWindow(control,FALSE); ShowWindow(hwnd,SW_SHOW); SetForegroundWindow(hwnd);
    MSG message{};
    while(!state.done && GetMessageW(&message,nullptr,0,0)>0) {
        if(!IsDialogMessageW(hwnd,&message)) { TranslateMessage(&message); DispatchMessageW(&message); }
    }
    if(!state.done) { state.cancel=true; if(state.worker.joinable()) state.worker.join(); if(state.busy) state.complete(); DestroyWindow(hwnd); PostQuitMessage(static_cast<int>(message.wParam)); }
    EnableWindow(control,TRUE); SetForegroundWindow(control);
}
}
