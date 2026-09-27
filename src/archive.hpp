#pragma once
#include "model.hpp"
#include <windows.h>
namespace edge {
std::wstring folder_identity(const std::filesystem::path& path);
bool folder_available(const DrawerModel& drawer);
std::wstring archive_folder_name(const std::wstring& title);
// Identity check precedes name/path adoption, including external renames.
bool rebind_folder(DrawerModel& drawer,const std::filesystem::path& path);
bool bind_new_folder(DrawerModel& drawer, const std::filesystem::path& desktop, std::wstring& error);
bool refresh_folder(DrawerModel& drawer, std::wstring& error);
// Reconnect a renamed folder by its recorded filesystem identity, never its prefix.
bool reconnect_folder(DrawerModel& drawer, const std::filesystem::path& desktop);
struct FileTransfer { std::wstring from,to; };
struct TransferResult {
    HRESULT status{S_OK}; bool aborted{};
    std::vector<FileTransfer> completed;
};
TransferResult transfer_files(HWND owner, const std::vector<std::wstring>& paths,
    const std::filesystem::path& destination, bool copy=false,
    const std::vector<std::wstring>& protected_folders={});
// Only an explicitly bound, identity-matching directory may be recycled.
bool recycle_archive(HWND owner,const DrawerModel& drawer);
}
