#pragma once
#include "archive.hpp"
namespace edge {
struct RenameResult {
    Settings settings;
    bool saved{},recovery_required{};
    std::wstring message;
};
// Only renames an identity-bound folder within its existing parent. A failed
// config write rolls the directory back without replacing any other object.
RenameResult rename_archive(const Settings& before,int id,const std::wstring& name,const std::filesystem::path& config);
}
