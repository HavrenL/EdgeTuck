#pragma once
#include <windows.h>
#include <string>
#include <string_view>
#include <vector>
#include <utility>
#include "shell_files.hpp"

namespace edge {
struct FileMenuRequest {
    HWND owner{}; POINT point{};
    std::vector<std::wstring> paths;
    std::vector<std::pair<UINT,std::wstring>> transfers;
#ifdef EDGETUCK_MENU_TESTS
    DWORD test_behavior{};
    std::wstring test_trace;
#endif
};
struct FileMenuResult {
    UINT command{}; HRESULT status{S_OK}; bool timed_out{},invoked{},cooldown{},reused{};
    MenuLoadStage stage{MenuLoadStage::Starting};
    DWORD preparation_ms{},launch_ms{},initialize_ms{},resolve_ms{},bind_ms{},query_ms{},stage_ms{},host_pid{};
};
FileMenuResult isolated_file_menu(const FileMenuRequest& request,DWORD prepare_timeout=3000);
void retry_file_menu();
#ifdef EDGETUCK_MENU_TESTS
void advance_menu_retry_clock(DWORD milliseconds);
#endif
// Runs before the normal app, singleton mutex, profile, graphics and tray setup.
int file_menu_host(std::wstring_view argument);
void append_file_menu_actions(HMENU menu,const std::vector<std::pair<UINT,std::wstring>>& transfers,bool basic);
}
