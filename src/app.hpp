#pragma once
#include "model.hpp"
#include "paint.hpp"
#include "motion.hpp"
#include "grid.hpp"
#include "shell_files.hpp"
#include "references.hpp"
#include "archive.hpp"
#include "desktop_icons.hpp"
#include "runtime_options.hpp"
#include "sorting.hpp"
#include "explorer_entry.hpp"
#include <shellapi.h>
#include <memory>
#include <optional>
#include <set>

namespace edge {
constexpr UINT WM_EDGE_TRAY = WM_APP + 1;
constexpr UINT WM_EDGE_SYNC = WM_APP + 2;
constexpr UINT WM_EDGE_REMOVE = WM_APP + 3;
constexpr UINT WM_EDGE_SHOW = WM_APP + 4;
constexpr UINT WM_EDGE_SHUTDOWN = WM_APP + 5;
constexpr UINT WM_EDGE_ANIMATION_DONE = WM_APP + 6;
constexpr UINT WM_EDGE_LIVE_FRAME = WM_APP + 7;
constexpr UINT WM_EDGE_CAPTURE_SYNC = WM_APP + 8;
constexpr UINT WM_EDGE_FILES = WM_APP + 9;
constexpr UINT WM_EDGE_DEFERRED = WM_APP + 10;
constexpr UINT WM_EDGE_FOLDERS = WM_APP + 11;
constexpr UINT WM_EDGE_SORTED = WM_APP + 12;
constexpr UINT WM_EDGE_EXPLORER_ENTRY = WM_APP + 13;
constexpr UINT_PTR folder_timer = 71;
class App;
class DropTarget;

struct Button {
    int id;
    D2D1_RECT_F bounds;
    std::wstring label;
    bool selected{}, prominent{};
};
enum class ControlPage { Drawers, Appearance, General };

class Drawer {
public:
    App& app;
    int id;
    HWND panel{};
    Canvas panel_canvas;
    DropTarget* panel_drop{};
    bool tracking_panel{}, dropping{}, menu_open{}, pinned{}, preview{}, animating{};
    bool panel_glass{};
    RECT bounds{};
    Offset closed{};
    UINT_PTR motion_serial{};
    int geometry_changes{}, motion_submissions{};
    unsigned canvas_generation{};
    DWORD content_probe{};
    float progress{}, from{}, target{};
    MotionClock::time_point animation_start{};
    ULONGLONG preview_deadline{};
    int scroll{}, selected{-1};
    std::set<int> selection;
    int focused{}, anchor{}, pressed{-1};
    POINT press_point{};
    bool dragging_out{};
    uint64_t sort_ticket{};
    bool sort_on_close{true};
    std::optional<SortResult> pending_sort;
    ShellMenu* active_menu{};
    int resize_kind{};
    POINT drag_origin{};
    int origin_start{}, origin_end{}, origin_depth{};
    std::vector<Slot> origin_layout;

    Drawer(App& app, int id);
    ~Drawer();
    DrawerModel& model();
    void create();
    void update_geometry();
    void apply_theme();
    void set_open(bool open);
    void finish_animation(UINT_PTR serial);
    float current_progress() const;
    void update_input_region(bool expanded);
    POINT content_point(POINT client) const;
    bool collapsed() const { return !animating && progress == 0; }
    void watch_pointer();
    void paint();
    void context_menu(POINT position, bool is_handle);
    void add_paths(const std::vector<std::wstring>& paths);
    void open_item(int index, bool double_click=false);
    void sort_menu(POINT point);
    void choose_sort(SortMode mode,bool descending);
    void request_sort(bool after_close=true);
    void apply_sort();
    void opened_item(const std::wstring& path,bool double_click,bool succeeded);
    std::vector<std::wstring> selected_paths() const;
    void select_item(int item, bool control, bool shift);
    void drag_files();
    void remove_selected();
    void new_item(bool folder);
    void rename_selected();
    void key_down(UINT key);
    void update_drag(POINT point);
    int hit_item(float x, float y);
    int item_count() const;
    int columns() const;
    ContentGrid content_grid() const;
    float icon_size() const;
    bool prepare_images(ImagePriority priority);
    int resize_hit(POINT client) const;
    static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
};

class App {
public:
    HINSTANCE instance{};
    HWND control{}, broker{};
    HWND material_window{};
    Graphics graphics;
    Canvas control_canvas{&graphics};
    Settings settings;
    std::filesystem::path config_path;
    std::vector<std::unique_ptr<Drawer>> drawers;
    std::vector<Button> buttons;
    std::optional<DrawerModel> undo;
    RECT work{};
    float scale{1};
    int grid_x{80}, grid_y{80};
    bool dark{}, transparency{}, animations{}, save_allowed{true}, quitting{}, smoke{}, smoke_ok{true};
    int smoke_step{}, selected_button{-1}, drawer_scroll{};
    std::wstring notice;
    StartupStatus startup;
    HWINEVENTHOOK foreground_hook{}, desktop_hook{}, destroy_hook{},location_hook{};
    bool capture_sync_pending{};
    HWND desktop_host{};
    bool desktop_order_pending{};
    HWINEVENTHOOK reorder_hook{};
    HWND desktop_capture_source{};
    bool storage_migrating{};
    bool folders_dirty{}, folder_sync_pending{}, syncing_folders{};
    int folder_retries{};
    std::map<int,ParkResult> folder_positions;
    int interaction_depth{};
    bool metrics_pending{}, shutdown_pending{};
    std::vector<FileChange> pending_changes;
    FileWatch file_watch;
    SortQueue sort_queue;
    uint64_t next_sort_ticket{};
    IDataObject* drag_data{};
    int drag_source{};
    std::vector<std::wstring> drag_paths;
    UINT taskbar_created{};
    HICON icon{};
    int paint_count{}, animation_submissions{}, smoke_geometry_before{};

    explicit App(HINSTANCE instance);
    ~App();
    bool live_test{},stress_test{};
    bool archive_test{}, archive_test_running{};
    std::filesystem::path archive_fixture;
    int archive_step{};
    void archive_test_tick();
    std::filesystem::path archive_root() const {
        if(archive_test) return settings.storage_mode==StorageMode::Desktop?archive_fixture:archive_fixture/L"independent";
        return settings.storage_mode==StorageMode::Desktop?desktop_directory():settings.storage_directory.empty()?default_storage_directory():std::filesystem::path(settings.storage_directory);
    }
    std::filesystem::path desktop_root() const { return archive_test?archive_fixture:desktop_directory(); }
    bool automated_test{}, smoke_dispatch{};
    HWND test_background{},test_occluder{};
    ControlPage control_page{ControlPage::Drawers};
    int hovered_button{-1}, pressed_button{-1}, control_wheel{};
    bool control_tracking{};
    D2D1_RECT_F drawer_list_bounds{};
    int control_rows{1};
    int run(bool smoke_test, bool start_hidden = false, bool force_fallback = false, bool material_preview = false, bool live_preview = false, bool test_live = false);
    void apply_live_mode();
    void sync_live_capture();
    void live_test_tick();
    void refresh_metrics();
    void refresh_theme();
    bool backdrop(HWND hwnd, bool glass);
    void create_windows();
    void create_drawers();
    void tray(bool remove = false);
    void show_control();
    void show_material();
    void show_storage();
    LSTATUS set_explorer_visible(bool enable);
    void sync_explorer();
    bool save();
    bool ensure_folder(int id);
    void initialize_folders();
    void schedule_folders(bool refresh=false);
    void sync_folders();
    void refresh_archives();
    bool import_paths(int destination, const std::vector<std::wstring>& paths, int source=0,
        size_t before=std::numeric_limits<size_t>::max(), bool copy=false);
    void return_to_desktop(int source, const std::vector<std::wstring>& paths);
    void paste_files(int destination);
    void references_changed();
    void refresh_file_watch();
    void file_changed(const FileChange& change);
    void finish_interaction();
    std::optional<std::wstring> ask_name(HWND owner, const std::wstring& title, const std::wstring& value, int limit=64);
    void invalidate();
    void desktop_order();
    void preview_drawer(int id);
    void add_drawer(Edge edge);
    void remove_drawer(int id);
    void restore_drawer();
    void rename_drawer(int id);
    bool rename_drawer_to(int id,const std::wstring& name);
    void sync_folder_names(HWND owner);
    void move_drawer_edge(int id, Edge edge);
    DrawerModel* find(int id);
    std::vector<Slot> slots(Edge edge) const;
    void apply_slots(Edge edge, const std::vector<Slot>& slots);
    int capacity(Edge edge) const;
    int minimum_span(Edge edge) const;
    int maximum_depth(Edge edge) const;
    int pitch(Edge edge) const;
    int origin(Edge edge) const;
    void paint_control();
    bool control_input(UINT message, WPARAM wparam, LPARAM lparam);
    int control_hit(float x, float y) const;
    void control_page_to(ControlPage page);
    void control_drawer_menu(int id);
    void action(int id);
    void menu(HWND owner, POINT point);
    void smoke_tick();
    static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
    static void CALLBACK event_proc(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD);
};
// Popup/drag loops dispatch arbitrary messages. Defer reconstruction until the
// operation and its current window procedure have both returned.
struct Interaction {
    App& app;
    explicit Interaction(App& app) : app(app) { ++app.interaction_depth; }
    ~Interaction() { if(--app.interaction_depth==0) PostMessageW(app.broker,WM_EDGE_DEFERRED,0,0); }
};
}
