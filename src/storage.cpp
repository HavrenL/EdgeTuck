#include "storage.hpp"
#include "storage_move.hpp"
#include "references.hpp"
#include "shell_files.hpp"
#include <bcrypt.h>
#include <array>
#include <map>
#include <stdexcept>
#include <algorithm>

namespace edge {
namespace {
struct Stop { std::wstring text; };
void require(bool okay,const std::wstring& text) { if(!okay) throw Stop{text}; }
void cancelled(const std::atomic_bool& value) { require(!value.load(),L"迁移已取消，原位置保持不变。"); }
struct Handle {
    HANDLE value{INVALID_HANDLE_VALUE};
    ~Handle() { if(value!=INVALID_HANDLE_VALUE) CloseHandle(value); }
};
std::wstring resolved(const std::filesystem::path& path) {
    Handle handle{CreateFileW(path.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,nullptr)};
    require(handle.value!=INVALID_HANDLE_VALUE,L"无法访问目录："+path.wstring());
    std::wstring value(32768,L'\0'); const auto length=GetFinalPathNameByHandleW(handle.value,value.data(),static_cast<DWORD>(value.size()),FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
    require(length>0 && length<value.size(),L"无法确认目录的真实位置。");
    value.resize(length); while(value.size()>7 && value.back()==L'\\') value.pop_back(); return value;
}
std::wstring prospective_location(const std::filesystem::path& path) {
    auto existing=path.lexically_normal(); std::vector<std::wstring> tail;
    while(GetFileAttributesW(existing.c_str())==INVALID_FILE_ATTRIBUTES) {
        require(existing.has_parent_path() && existing.parent_path()!=existing,L"目标磁盘当前不可用。");
        tail.push_back(existing.filename().wstring()); existing=existing.parent_path();
    }
    auto value=resolved(existing);
    for(auto it=tail.rbegin();it!=tail.rend();++it) { if(value.back()!=L'\\') value+=L'\\'; value+=*it; }
    return value;
}
struct Digest {
    BCRYPT_ALG_HANDLE algorithm{}; BCRYPT_HASH_HANDLE hash{};
    std::vector<UCHAR> storage;
    Digest() {
        require(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)>=0,L"无法初始化文件校验。");
        DWORD bytes{},length{};
        if(BCryptGetProperty(algorithm,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&length),sizeof(length),&bytes,0)<0) { BCryptCloseAlgorithmProvider(algorithm,0); algorithm=nullptr; throw Stop{L"无法初始化文件校验。"}; }
        storage.resize(length);
    }
    ~Digest() { if(hash) BCryptDestroyHash(hash); if(algorithm) BCryptCloseAlgorithmProvider(algorithm,0); }
    std::array<UCHAR,32> file(const std::filesystem::path& path,const std::atomic_bool& cancel) {
        // Deny writers while reading; files currently being written cannot pass.
        Handle input{CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN,nullptr)};
        require(input.value!=INVALID_HANDLE_VALUE,L"文件正在使用或无法读取："+path.wstring());
        require(BCryptCreateHash(algorithm,&hash,storage.data(),static_cast<ULONG>(storage.size()),nullptr,0,0)>=0,L"无法开始文件校验。");
        std::vector<UCHAR> buffer(1024*1024); DWORD bytes{};
        for(;;) {
            cancelled(cancel);
            require(ReadFile(input.value,buffer.data(),static_cast<DWORD>(buffer.size()),&bytes,nullptr)!=FALSE,L"读取文件失败："+path.wstring());
            if(!bytes) break;
            require(BCryptHashData(hash,buffer.data(),bytes,0)>=0,L"文件校验失败。");
        }
        std::array<UCHAR,32> result{};
        require(BCryptFinishHash(hash,result.data(),static_cast<ULONG>(result.size()),0)>=0,L"文件校验失败。");
        BCryptDestroyHash(hash); hash=nullptr; return result;
    }
};
struct Entry {
    bool directory{}; std::uintmax_t bytes{}; std::array<UCHAR,32> hash{};
    bool operator==(const Entry&) const = default;
};
using Manifest=std::map<std::wstring,Entry>;
void require_no_directory_streams(const std::filesystem::path& path) {
    WIN32_FIND_STREAM_DATA stream{}; HANDLE search=FindFirstStreamW(path.c_str(),FindStreamInfoStandard,&stream,0);
    if(search==INVALID_HANDLE_VALUE) { const auto error=GetLastError(); require(error==ERROR_HANDLE_EOF || error==ERROR_INVALID_PARAMETER || error==ERROR_NOT_SUPPORTED,L"无法核对文件夹的附加数据："+path.wstring()); return; }
    bool named=false;
    do { named|=wcscmp(stream.cStreamName,L"::$DATA")!=0; } while(FindNextStreamW(search,&stream));
    const auto error=GetLastError(); FindClose(search);
    require(error==ERROR_HANDLE_EOF,L"无法核对文件夹的附加数据："+path.wstring());
    // Explorer's folder copy can omit directory streams. Reject up front, so
    // cleanup can never discard metadata that the Shell did not preserve.
    require(!named,L"文件夹含特殊附加数据，暂不支持自动迁移，原文件保持不变："+path.wstring());
}
Manifest inspect(const std::filesystem::path& root,const std::atomic_bool& cancel) {
    require(!folder_identity(root).empty(),L"文件夹已改变或不可用："+root.wstring());
    require_no_directory_streams(root);
    Manifest result; Digest digest;
    for(const auto& item:std::filesystem::recursive_directory_iterator(root)) {
        cancelled(cancel);
        const auto path=item.path(); const DWORD attributes=GetFileAttributesW(path.c_str());
        require(attributes!=INVALID_FILE_ATTRIBUTES,L"无法读取项目："+path.wstring());
        require(!(attributes&FILE_ATTRIBUTE_REPARSE_POINT),L"此文件夹含链接或云端占位项目，请先在资源管理器中处理："+path.wstring());
        Entry entry; entry.directory=(attributes&FILE_ATTRIBUTE_DIRECTORY)!=0;
        if(!entry.directory) { entry.bytes=item.file_size(); entry.hash=digest.file(path,cancel); }
        else require_no_directory_streams(path);
        const auto relative=path.lexically_relative(root).wstring();
        result.emplace(relative,entry);
        // Named NTFS streams can contain user data too. The Shell copies them;
        // include them in verification rather than comparing only visible bytes.
        if(!entry.directory) {
            WIN32_FIND_STREAM_DATA stream{}; HANDLE search=FindFirstStreamW(path.c_str(),FindStreamInfoStandard,&stream,0);
            if(search!=INVALID_HANDLE_VALUE) {
                try {
                    do { if(wcscmp(stream.cStreamName,L"::$DATA")==0) continue;
                        Entry alternate; alternate.bytes=static_cast<std::uintmax_t>(stream.StreamSize.QuadPart);
                        alternate.hash=digest.file(path.wstring()+stream.cStreamName,cancel);
                        result.emplace(relative+stream.cStreamName,alternate);
                    } while(FindNextStreamW(search,&stream));
                    const auto error=GetLastError(); require(error==ERROR_HANDLE_EOF,L"无法核对文件的附加数据："+path.wstring());
                } catch(...) { FindClose(search); throw; }
                FindClose(search);
            } else { const auto error=GetLastError(); require(error==ERROR_HANDLE_EOF || error==ERROR_INVALID_PARAMETER || error==ERROR_NOT_SUPPORTED,L"无法核对文件的附加数据："+path.wstring()); }
        }
    }
    return result;
}
}
bool prepare_storage_directory(const Settings& settings,const std::filesystem::path& input,std::wstring& error) {
    error.clear();
    try {
        require(input.is_absolute() && input.wstring().find(L'\0')==std::wstring::npos,L"请选择完整的文件夹路径。");
        const auto target=input.lexically_normal();
        for(const auto& d:settings.drawers) if(!d.folder.empty()) require(!path_within(target.wstring(),std::filesystem::path(d.folder).lexically_normal().wstring()),L"收纳目录不能位于现有抽屉文件夹内部。");
        require(std::filesystem::is_directory(target.root_path()),L"目标磁盘当前不可用。");
        wchar_t program[32768]{}; GetModuleFileNameW(nullptr,program,32768);
        auto validate=[&](const std::wstring& actual) {
            for(const auto& d:settings.drawers) if(!d.folder.empty() && folder_available(d)) require(!path_within(actual,resolved(d.folder)),L"目标目录实际指向现有抽屉内部，无法迁移。");
            require(!path_within(actual,resolved(std::filesystem::path(program).parent_path())),L"请选择程序目录之外的文件夹，删除软件时才能独立保留文件。");
            require(!path_within(actual,prospective_location(default_config_path().parent_path())),L"收纳目录不能放在轻屉的配置目录中。");
        };
        validate(prospective_location(target)); // Resolve parent aliases before creating anything.
        std::filesystem::create_directories(target);
        require(!folder_identity(target).empty(),L"请选择普通文件夹作为收纳目录。");
        validate(resolved(target));
        return true;
    } catch(const Stop& e) { error=e.text; } catch(...) { error=L"无法使用收纳目录，请检查磁盘、路径和权限。"; }
    return false;
}
MigrationResult migrate_storage(HWND owner,const Settings& before,StorageMode mode,const std::filesystem::path& target,
    const std::filesystem::path& config,const std::atomic_bool& cancel,const MigrationProgress& report,MigrationOptions options) {
    MigrationResult result; result.settings=before;
    struct Work { size_t index{}; Manifest manifest; std::wstring destination,identity; bool fast{}; };
    std::vector<Work> work;
    bool journal_active=false;
    auto progress=[&](MigrationStage stage,const std::wstring& text) { if(report) report(stage,text); };
    try {
        cancelled(cancel); std::wstring error;
        require(mode==StorageMode::Desktop || mode==StorageMode::Directory,L"存放模式无效。");
        require(prepare_storage_directory(before,target,error),error);
        require(!path_within(resolved(target),resolved(config.parent_path())),L"收纳目录不能放在轻屉的配置目录中。");
        Digest config_digest; const auto original_config=config_digest.file(config,cancel);
        const auto target_actual=resolved(target);
        const auto target_identity=folder_identity(target);
        result.settings.storage_mode=mode;
        if(mode==StorageMode::Directory) result.settings.storage_directory=target.lexically_normal().wstring();
        for(size_t i=0;i<before.drawers.size();++i) {
            const auto& d=before.drawers[i];
            require(folder_available(d),L"请先恢复不可用的抽屉文件夹："+d.name);
            if(same_path(resolved(std::filesystem::path(d.folder).parent_path()),target_actual)) continue;
            for(const auto& other:before.drawers) if(other.id!=d.id) require(!path_within(resolved(other.folder),resolved(d.folder)),L"抽屉文件夹彼此嵌套，无法整批迁移。");
            const bool fast=options.same_volume_move && same_storage_volume(d.folder,target);
            progress(MigrationStage::Inspect,(fast?L"检查文件夹位置：":L"检查原文件：")+d.name);
            cancelled(cancel);
            work.push_back({i,fast?Manifest{}:inspect(d.folder,cancel),{},{},fast});
        }
        const auto journal_root=config.parent_path()/L"migrations";
        std::filesystem::create_directories(journal_root);
        result.journal=journal_root/(std::to_wstring(GetTickCount64())+L"-"+std::to_wstring(GetCurrentProcessId()));
        require(std::filesystem::create_directory(result.journal),L"无法建立迁移恢复记录。");
        require(save_settings(result.journal/L"before.dat",before,error),error);
        for(auto& item:work) {
            if(item.fast) continue;
            cancelled(cancel); const auto& d=before.drawers[item.index];
            require(folder_available(d),L"原文件夹在迁移过程中发生变化："+d.name);
            progress(MigrationStage::Copy,L"复制文件："+d.name);
            auto copied=transfer_files(owner,{d.folder},target,true);
            // Shell progress also reports descendants when copying a directory.
            // Only the requested root is a new drawer binding.
            std::vector<FileTransfer> roots;
            for(const auto& entry:copied.completed) if(same_path(entry.from,d.folder)) roots.push_back(entry);
            result.copies.insert(result.copies.end(),roots.begin(),roots.end());
            require(SUCCEEDED(copied.status) && !copied.aborted && roots.size()==1,L"文件复制未完成，原位置保持不变。已生成的副本保留在目标目录。");
            item.destination=roots.front().to; item.identity=folder_identity(item.destination);
            require(!item.identity.empty() && !same_path(resolved(d.folder),resolved(item.destination)),L"无法确认新文件夹的身份。");
            auto& next=result.settings.drawers[item.index]; next.folder=item.destination; next.folder_identity=item.identity;
            next.restore_position=false; next.restore_x=0; next.restore_y=0;
        }
        for(const auto& item:work) {
            if(item.fast) continue;
            const auto& d=before.drawers[item.index]; progress(MigrationStage::Verify,L"核对文件内容："+d.name);
            require(folder_available(d) && folder_identity(item.destination)==item.identity && inspect(d.folder,cancel)==item.manifest && inspect(item.destination,cancel)==item.manifest,
                L"文件在复制期间发生变化或副本不完整，未切换位置，也未删除原文件："+d.name);
        }
        // Resolve collisions only after any cross-volume copies have completed.
        // A leftover copy is ordinary user data, never proof of a drawer binding.
        std::vector<std::wstring> reserved;
        for(auto& item:work) if(item.fast) {
            const auto& d=before.drawers[item.index];
            const auto name=std::filesystem::path(d.folder).filename().wstring();
            for(int suffix=1;suffix<=10000;++suffix) {
                const auto candidate=(target/(name+(suffix==1?L"":L" ("+std::to_wstring(suffix)+L")"))).wstring();
                const auto attributes=GetFileAttributesW(candidate.c_str()); const auto code=GetLastError();
                if(attributes!=INVALID_FILE_ATTRIBUTES || std::any_of(reserved.begin(),reserved.end(),[&](const auto& p){return same_path(p,candidate);})) continue;
                require(code==ERROR_FILE_NOT_FOUND || code==ERROR_PATH_NOT_FOUND,L"无法确认目标文件夹是否可用。");
                item.destination=candidate; item.identity=d.folder_identity; reserved.push_back(candidate); break;
            }
            require(!item.destination.empty(),L"同名文件夹过多，请选择其他收纳目录。");
            auto& next=result.settings.drawers[item.index]; next.folder=item.destination; next.folder_identity=item.identity;
            next.restore_position=false; next.restore_x=0; next.restore_y=0;
        }
        for(auto& d:result.settings.drawers) for(auto* paths:{&d.items,&d.legacy_items}) for(auto& path:*paths) {
            for(const auto& item:work) { const auto& old=before.drawers[item.index].folder;
                if(path_within(path,old)) { path=item.destination+path.substr(old.size()); break; }
            }
        }
        for(auto& d:result.settings.drawers) for(auto& use:d.recent_uses) {
            for(const auto& item:work) { const auto& old=before.drawers[item.index].folder;
                if(path_within(use.path,old)) { use.path=item.destination+use.path.substr(old.size()); break; }
            }
        }
        const bool fast=std::any_of(work.begin(),work.end(),[](const auto& item){return item.fast;});
        if(fast) {
            // Persist the full intended paths BEFORE the first rename. Startup
            // can undo an uncommitted move without traversing the project files.
            require(config_digest.file(config,cancel)==original_config,L"配置已被其他操作修改，迁移没有切换位置。");
            require(CopyFileW(config.c_str(),(result.journal/L"original.dat").c_str(),TRUE)!=FALSE,L"无法保存原配置恢复记录。");
            require(config_digest.file(result.journal/L"original.dat",cancel)==original_config,L"原配置已发生变化，尚未移动文件夹。");
            require(flush_storage_record(result.journal/L"before.dat") && flush_storage_record(result.journal/L"original.dat"),L"恢复记录未能写入磁盘。");
            require(save_settings(result.journal/L"planned.dat",result.settings,error),error);
            journal_active=true;
            require(flush_storage_record(result.journal/L"planned.dat"),L"移动计划未能写入磁盘。");
            for(const auto& item:work) if(item.fast) {
                const auto& d=before.drawers[item.index]; cancelled(cancel);
                progress(MigrationStage::Move,L"同盘快速移动："+d.name); cancelled(cancel);
                require(folder_identity(target)==target_identity,L"目标目录在迁移过程中发生变化。");
                require(move_storage_folder(d.folder,item.destination,d.folder_identity,error),error);
                ++result.moved_folders;
                progress(MigrationStage::Moved,L"已移动文件夹："+d.name);
                require(folder_available(result.settings.drawers[item.index]),L"移动后的文件夹无法核对，请检查恢复记录。");
            }
        }
        cancelled(cancel); progress(MigrationStage::Commit,L"保存新的抽屉位置…"); cancelled(cancel);
        require(config_digest.file(config,cancel)==original_config,L"配置已被其他操作修改，迁移没有切换位置。");
        require(save_settings(result.journal/L"copied.dat",result.settings,error),error);
        require(save_settings(config,result.settings,error),error);
        result.committed=true;
        progress(MigrationStage::Committed,L"已保存新的抽屉位置。");
        if(journal_active) { if(finish_storage_record(result.journal)) journal_active=false; else result.recovery_required=true; }
        for(const auto& item:work) if(!item.fast) result.retained_sources.push_back(before.drawers[item.index]);
        for(const auto& item:work) {
            if(item.fast) continue;
            const auto& d=before.drawers[item.index]; progress(MigrationStage::Cleanup,L"将旧文件夹送到回收站："+d.name);
            // Re-check both sides immediately before recycling. A late edit must
            // leave the old folder accessible, never delete newer source data.
            try {
                if(cancel.load() || !folder_available(d) || folder_identity(item.destination)!=item.identity ||
                    inspect(d.folder,cancel)!=item.manifest || inspect(item.destination,cancel)!=item.manifest) continue;
                if(recycle_archive(owner,d)) std::erase_if(result.retained_sources,[&](const auto& old){return same_path(old.folder,d.folder);});
            } catch(...) { /* The binding is committed; retain the source. */ }
        }
        result.message=work.empty()?L"所有抽屉已在所选目录，已保存新建位置。":!result.retained_sources.empty()?L"已切换到新位置；部分旧文件夹未清理，已保留，请在资源管理器中检查。":result.moved_folders==work.size()?L"迁移完成，已通过同盘快速移动切换位置，没有重新复制文件。":L"迁移完成：同盘文件夹直接移动，跨盘副本校验后切换，原文件夹已回收。";
    } catch(const Stop& e) { result.message=e.text; }
    catch(...) { result.message=L"迁移未能完成，请检查文件占用、空间和权限；未清理的原文件与副本均保留。"; }
    if(result.committed && journal_active) result.recovery_required=!finish_storage_record(result.journal);
    if(!result.committed) {
        if(journal_active) {
            // Ignore cancellation while rolling back. No copy is removed here.
            std::wstring error;
            if(restore_storage_moves(before,result.settings,error) && finish_storage_record(result.journal)) result.moved_folders=0;
            else { result.recovery_required=true; result.message=error.empty()?L"文件夹已恢复，但恢复记录未能结束。":error; result.message+=L"\r\n恢复记录："+result.journal.wstring(); }
        }
        result.settings=before;
    }
    if(result.recovery_required && result.committed) result.message+=L"\r\n恢复记录尚未结束，下次启动会核对。";
    return result;
}
}
