#pragma once
#include "archive.hpp"
#include <atomic>
#include <functional>
namespace edge {
enum class MigrationStage { Inspect, Copy, Verify, Commit, Cleanup, Move, Moved, Rollback, Committed };
using MigrationProgress=std::function<void(MigrationStage,const std::wstring&)>;
struct MigrationResult {
    Settings settings;
    bool committed{};
    bool recovery_required{};
    size_t moved_folders{};
    std::wstring message;
    std::filesystem::path journal;
    std::vector<FileTransfer> copies;
    std::vector<DrawerModel> retained_sources;
};
bool prepare_storage_directory(const Settings& settings,const std::filesystem::path& target,std::wstring& error);
bool recover_storage(const std::filesystem::path& config,std::wstring& message);
struct MigrationOptions { bool same_volume_move{true}; };
// Caller supplies an STA and an unchanged configuration snapshot. Same-volume
// renames are journaled and reversible until commit; copies are verified before
// their sources can be recycled. Test files must be owned by the caller.
MigrationResult migrate_storage(HWND owner,const Settings& before,StorageMode mode,
    const std::filesystem::path& target,const std::filesystem::path& config,
    const std::atomic_bool& cancel,const MigrationProgress& progress={},MigrationOptions options={});
}
