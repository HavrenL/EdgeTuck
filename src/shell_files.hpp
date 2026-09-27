#pragma once
#include <windows.h>
#include <shlobj.h>
#include <wrl/client.h>
#include <filesystem>
#include <vector>
#include <string>
#include <map>

namespace edge {
using Microsoft::WRL::ComPtr;
ComPtr<IShellItemArray> shell_items(const std::vector<std::wstring>& paths,HRESULT* status=nullptr);
ComPtr<IDataObject> file_data(const std::vector<std::wstring>& paths);
std::vector<std::wstring> data_paths(IDataObject* data);
bool clipboard_files(HWND owner, const std::vector<std::wstring>& paths);
DWORD preferred_drop_effect(IDataObject* data);
void completed_file_move(IDataObject* data);
void reveal_file(const std::wstring& path);
bool desktop_at(POINT point);
std::filesystem::path desktop_directory();
std::filesystem::path create_desktop_item(const std::filesystem::path& directory, bool folder, DWORD& error);
HRESULT rename_file(HWND owner, const std::wstring& path, const std::wstring& name);
enum class MenuLoadStage:LONG { Starting,Initializing,Resolving,Binding,Querying,Ready };
struct ShellMenuLoad {
    HRESULT status{S_OK};
    DWORD resolve_ms{},bind_ms{},query_ms{};
    void (*progress)(void*,MenuLoadStage){};
    void* context{};
    bool asynchronous{true};
};
class ShellMenu {
    ComPtr<IContextMenu> menu;
    ComPtr<IContextMenu2> menu2;
    ComPtr<IContextMenu3> menu3;
public:
    bool fill(HWND owner, HMENU popup, const std::vector<std::wstring>& paths,DWORD keys=MAXDWORD,ShellMenuLoad* load=nullptr);
    bool message(UINT msg, WPARAM wp, LPARAM lp, LRESULT& result);
    HRESULT invoke(HWND owner, UINT command, POINT point,DWORD keys=MAXDWORD);
    std::wstring verb(UINT command) const;
};
struct FileChange { LONG event{}; std::wstring from, to; };
class FileWatch {
    std::map<std::wstring,ULONG> registrations;
public:
    ~FileWatch();
    void clear();
    void update(HWND window, UINT message, const std::vector<std::wstring>& paths);
    static FileChange read(WPARAM wp, LPARAM lp);
};
}
