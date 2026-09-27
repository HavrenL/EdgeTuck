#pragma once
#include "storage.hpp"

namespace edge {
bool same_storage_volume(const std::filesystem::path& from,const std::filesystem::path& to);
// Rename only the held, identity-checked directory. Never copy or replace a target.
bool move_storage_folder(const std::filesystem::path& from,const std::filesystem::path& to,
    const std::wstring& identity,std::wstring& error);
bool flush_storage_record(const std::filesystem::path& path);
bool finish_storage_record(const std::filesystem::path& journal);
bool restore_storage_moves(const Settings& before,const Settings& planned,std::wstring& error);
}
