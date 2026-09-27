#pragma once
#include <windows.h>
#include <filesystem>
#include <string>

namespace edge {
inline constexpr wchar_t explorer_entry_id[]=L"{D9F414DE-B7AE-4B8C-AB8D-BF9CB8BD306F}";
struct ExplorerEntryStatus {
    bool enabled{};
    LSTATUS error{};
    std::wstring target,icon;
};
// An injected root keeps registry tests outside the real Shell namespace.
ExplorerEntryStatus read_explorer_entry(HKEY root,REGSAM view);
LSTATUS write_explorer_entry(HKEY root,REGSAM view,bool enable,const std::filesystem::path& target,const std::filesystem::path& executable);
ExplorerEntryStatus explorer_entry_status();
LSTATUS set_explorer_entry(bool enable,const std::filesystem::path& target,const std::filesystem::path& executable);
void open_explorer_entry(HWND owner);
}
