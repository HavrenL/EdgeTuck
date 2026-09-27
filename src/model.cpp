#include "model.hpp"
#include "sorting.hpp"
#include <algorithm>
#include <windows.h>
#include <shlobj.h>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <set>
#include <iostream>

namespace edge {
static std::string utf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (!size) throw std::runtime_error("Invalid Unicode");
    std::string out(size, '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), out.data(), size, nullptr, nullptr);
    return out;
}
static std::wstring wide(const std::string& value) {
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (!size) throw std::runtime_error("Invalid UTF-8");
    std::wstring out(size, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), out.data(), size);
    return out;
}
Settings defaults() {
    Settings s;
    s.drawers = {{1, L"日常", Edge::Right, 1, 4, 5, {}}, {2, L"工作", Edge::Left, 2, 4, 5, {}}, {3, L"灵感", Edge::Top, 5, 5, 4, {}}};
    return s;
}
std::wstring edge_name(Edge edge) {
    return edge == Edge::Left ? L"左侧" : edge == Edge::Right ? L"右侧" : L"顶部";
}
std::filesystem::path default_config_path() {
    PWSTR folder = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &folder))) throw std::runtime_error("LocalAppData unavailable");
    std::filesystem::path path = std::filesystem::path(folder) / L"EdgeTuck" / L"settings.dat";
    CoTaskMemFree(folder);
    return path;
}
std::filesystem::path default_storage_directory() {
    PWSTR folder{};
    if(FAILED(SHGetKnownFolderPath(FOLDERID_Profile,0,nullptr,&folder))) throw std::runtime_error("User profile unavailable");
    const auto path=std::filesystem::path(folder)/L"EdgeTuck";
    CoTaskMemFree(folder); return path;
}
bool load_settings(const std::filesystem::path& path, Settings& result, std::wstring& error) {
    error.clear();
    try {
        if (!std::filesystem::exists(path)) { result = defaults(); return true; }
        if (std::filesystem::file_size(path) > 4 * 1024 * 1024) throw std::runtime_error("File too large");
        std::ifstream in(path, std::ios::binary);
        std::string signature;
        std::getline(in, signature);
        if (signature != "EDGETUCK 1" && signature != "EDGETUCK 2" && signature != "EDGETUCK 3" && signature != "EDGETUCK 4" && signature != "EDGETUCK 5" && signature != "EDGETUCK 6" && signature != "EDGETUCK 7") throw std::runtime_error("Unsupported configuration version");
        const int version=signature.back()-'0';
        Settings s;
        s.storage_mode=StorageMode::Desktop; // Upgrades never relocate existing archives.
        int theme = 0, glass = 0, motion = 0, count = 0;
        if (!(in >> theme >> glass >> motion >> s.hover_ms >> s.close_ms >> count) || theme < 0 || theme > 2 || (glass != 0 && glass != 1) || (motion != 0 && motion != 1) || s.hover_ms < 0 || s.hover_ms > 2000 || s.close_ms < 0 || s.close_ms > 3000 || count < 0 || count > 32) throw std::runtime_error("Invalid settings");
        s.theme = static_cast<Theme>(theme); s.glass = glass != 0; s.motion = motion != 0;
        // 0.2 removes the old hover threshold, including existing 0.1 profiles.
        s.hover_ms = 0;
        s.close_ms = 0; // 0.2.1 removes the leave threshold as well.
        if (signature != "EDGETUCK 1") {
            int refraction{}, lighting{}, chromatic{};
            auto& m = s.material;
            if (!(in >> m.blur >> m.depth >> m.light >> m.dispersion >> refraction >> lighting >> chromatic)
                || !valid_material(m) || refraction < 0 || refraction > 1 || lighting < 0 || lighting > 1 || chromatic < 0 || chromatic > 1)
                throw std::runtime_error("Invalid glass material");
            m.refraction = refraction != 0; m.lighting = lighting != 0; m.chromatic = chromatic != 0;
        }
        if (version>=3) {
            int live{}; if (!(in >> live) || live<0 || live>1) throw std::runtime_error("Invalid background mode");
            s.live_background=live!=0;
        }
        std::set<int> ids;
        if(version>=5) {
            int responsive{};
            if(!(in>>responsive) || responsive<0 || responsive>1) throw std::runtime_error("Invalid process priority");
            s.responsive_priority=responsive!=0;
        }
        if(version>=6) {
            int mode{}; std::string directory;
            if(!(in>>mode>>std::quoted(directory)) || mode<0 || mode>1 || directory.size()>131072) throw std::runtime_error("Invalid storage mode");
            s.storage_mode=static_cast<StorageMode>(mode); s.storage_directory=wide(directory);
            if(!s.storage_directory.empty() && (s.storage_directory.find(L'\0')!=std::wstring::npos || !std::filesystem::path(s.storage_directory).is_absolute())) throw std::runtime_error("Invalid storage directory");
        }
        for (int i = 0; i < count; ++i) {
            DrawerModel d;
            std::string name;
            int side = 0, items = 0;
            if (!(in >> d.id >> side >> d.start >> d.span >> d.depth >> std::quoted(name) >> items) || d.id < 1 || d.id > 1000000 || !ids.insert(d.id).second || side < 0 || side > 2 || d.start < 0 || d.start > 10000 || d.span < 2 || d.span > 1000 || d.depth < 3 || d.depth > 20 || name.empty() || name.size() > 256 || items < 0 || items > 1000) throw std::runtime_error("Invalid drawer");
            d.edge = static_cast<Edge>(side); d.name = wide(name);
            for (int j = 0; j < items; ++j) {
                std::string item;
                if (!(in >> std::quoted(item)) || item.empty() || item.size() > 131072) throw std::runtime_error("Invalid reference");
                const auto pathText = wide(item);
                if (pathText.find(L'\0') != std::wstring::npos || !std::filesystem::path(pathText).is_absolute()) throw std::runtime_error("Reference must be absolute");
                d.items.push_back(pathText);
                if(version>=7) {
                    uint64_t time{};
                    if(!(in>>time) || time>0x7FFFFFFFFFFFFFFFULL) throw std::runtime_error("Invalid recent use time");
                    if(time) d.recent_uses.push_back({pathText,time});
                }
            }
            if(version>=4) {
                std::string folder,identity; int restore{},legacy{};
                if(!(in>>std::quoted(folder)>>std::quoted(identity)>>restore>>d.restore_x>>d.restore_y>>legacy)
                    || folder.size()>131072 || identity.size()>128 || restore<0 || restore>1 || legacy<0 || legacy>1000)
                    throw std::runtime_error("Invalid folder binding");
                d.folder=wide(folder); d.folder_identity=wide(identity); d.restore_position=restore!=0;
                if(!d.folder.empty() && (d.folder.find(L'\0')!=std::wstring::npos || !std::filesystem::path(d.folder).is_absolute() || d.folder_identity.empty()))
                    throw std::runtime_error("Invalid folder path");
                for(int j=0;j<legacy;++j) {
                    std::string p; if(!(in>>std::quoted(p)) || p.empty() || p.size()>131072) throw std::runtime_error("Invalid legacy reference");
                    auto w=wide(p); if(w.find(L'\0')!=std::wstring::npos || !std::filesystem::path(w).is_absolute()) throw std::runtime_error("Invalid legacy path");
                    d.legacy_items.push_back(std::move(w));
                }
            } else d.legacy_items=d.items;
            if(version>=7) {
                int sort{},descending{};
                if(!(in>>sort>>descending) || sort<0 || sort>5 || descending<0 || descending>1) throw std::runtime_error("Invalid sort mode");
                d.sort=static_cast<SortMode>(sort); d.sort_descending=descending!=0;
            }
            s.drawers.push_back(std::move(d));
        }
        in >> std::ws;
        if (!in.eof()) throw std::runtime_error("Unexpected trailing data");
        result = std::move(s);
        return true;
    } catch (...) {
        const auto backup = std::filesystem::path(path.wstring() + L".bak");
        std::error_code ec;
        if (path.extension() != L".bak" && std::filesystem::exists(backup, ec)) {
            std::wstring backup_error;
            if (load_settings(backup, result, backup_error)) { error = L"设置文件异常，已恢复上一份有效设置。"; return true; }
        }
        error = L"设置文件无法读取。已使用临时默认布局，原设置文件未被覆盖。"; return false;
    }
}
bool save_settings(const std::filesystem::path& path, const Settings& settings, std::wstring& error) {
    error.clear();
    const auto temporary = std::filesystem::path(path.wstring() + L".tmp");
    try {
        if (!valid_material(settings.material)) throw std::runtime_error("Invalid glass material");
        if((settings.storage_mode!=StorageMode::Desktop && settings.storage_mode!=StorageMode::Directory) ||
            (!settings.storage_directory.empty() && (settings.storage_directory.find(L'\0')!=std::wstring::npos || !std::filesystem::path(settings.storage_directory).is_absolute()))) throw std::runtime_error("Invalid storage directory");
        std::filesystem::create_directories(path.parent_path());
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out << "EDGETUCK 7\n" << static_cast<int>(settings.theme) << ' ' << settings.glass << ' ' << settings.motion << ' ' << settings.hover_ms << ' ' << settings.close_ms << ' ' << settings.drawers.size() << '\n';
        const auto& m = settings.material;
        out << std::setprecision(9) << m.blur << ' ' << m.depth << ' ' << m.light << ' ' << m.dispersion << ' ' << m.refraction << ' ' << m.lighting << ' ' << m.chromatic << '\n';
        out << settings.live_background << '\n';
        out << settings.responsive_priority << '\n';
        out << static_cast<int>(settings.storage_mode) << ' ' << std::quoted(utf8(settings.storage_directory)) << '\n';
        for (const auto& d : settings.drawers) {
            if(d.sort<SortMode::Manual || d.sort>SortMode::Recent) throw std::runtime_error("Invalid sort mode");
            out << d.id << ' ' << static_cast<int>(d.edge) << ' ' << d.start << ' ' << d.span << ' ' << d.depth << ' ' << std::quoted(utf8(d.name)) << ' ' << d.items.size() << '\n';
            const auto used=recent_times(d);
            for (size_t i=0;i<d.items.size();++i) out << std::quoted(utf8(d.items[i])) << ' ' << used[i] << '\n';
            out<<std::quoted(utf8(d.folder))<<' '<<std::quoted(utf8(d.folder_identity))<<' '<<d.restore_position<<' '<<d.restore_x<<' '<<d.restore_y<<' '<<d.legacy_items.size()<<'\n';
            for(const auto& item:d.legacy_items) out<<std::quoted(utf8(item))<<'\n';
            out<<static_cast<int>(d.sort)<<' '<<d.sort_descending<<'\n';
        }
        if (out.tellp() > 4 * 1024 * 1024) throw std::runtime_error("Configuration exceeds size limit");
        out.flush();
        if (!out) throw std::system_error(errno, std::generic_category(), "Write failed");
        out.close();
        if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
            Settings previous; std::wstring previous_error;
            if (load_settings(path, previous, previous_error) && previous_error.empty()) {
                const auto backup = std::filesystem::path(path.wstring() + L".bak");
                if (!CopyFileW(path.c_str(), backup.c_str(), FALSE)) throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "Backup failed");
            }
        }
        if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            const DWORD code = GetLastError();
            // Redirected / virtualized folders can report different devices for two
            // siblings. Keep a known-good backup before using a recoverable copy.
            if (code != ERROR_NOT_SAME_DEVICE || !CopyFileW(temporary.c_str(), path.c_str(), FALSE)) throw std::system_error(static_cast<int>(code == ERROR_NOT_SAME_DEVICE ? GetLastError() : code), std::system_category(), "Replace failed");
            DeleteFileW(temporary.c_str());
        }
        return true;
    } catch (const std::system_error& e) {
        std::cerr << "Configuration save failed: " << e.what() << '\n';
        error = L"设置未能保存（错误 " + std::to_wstring(e.code().value()) + L"），请检查配置目录的写入权限。";
        OutputDebugStringA(e.what()); DeleteFileW(temporary.c_str()); return false;
    } catch (const std::exception& e) { std::cerr << "Configuration save failed: " << e.what() << '\n'; error = L"设置未能保存，请检查配置目录的写入权限。"; DeleteFileW(temporary.c_str()); return false; }
}
}
