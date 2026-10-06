#pragma once
#include "layout.hpp"
#include "material.hpp"
#include <filesystem>
#include <string>
#include <vector>
#include <cstdint>

namespace edge {
enum class Edge { Left, Right, Top };
enum class Theme { System, Light, Dark };
enum class StorageMode { Desktop, Directory };
enum class SortMode { Manual, Name, Size, Type, Modified, Recent };
struct RecentUse { std::wstring path; uint64_t time{}; };
struct DrawerModel {
    int id{};
    std::wstring name;
    Edge edge{Edge::Right};
    int start{};
    int span{4};
    int depth{5};
    std::vector<std::wstring> items;
    std::wstring folder; // Physical directory; never inferred from an ET_ name.
    std::wstring folder_identity;
    std::vector<std::wstring> legacy_items; // Pre-0.6 references, moved only on explicit archive.
    bool restore_position{};
    int restore_x{}, restore_y{};
    SortMode sort{SortMode::Manual};
    bool sort_descending{};
    std::vector<RecentUse> recent_uses;
    bool english_folder{}; // Physical name is independent of the displayed title.
};
struct Settings {
    Theme theme{Theme::System};
    bool glass{true};
    bool motion{true};
    bool live_background{true};
    bool responsive_priority{false};
    int hover_ms{0};
    int close_ms{0};
    GlassMaterial material;
    std::vector<DrawerModel> drawers;
    StorageMode storage_mode{StorageMode::Directory};
    std::wstring storage_directory; // Empty uses the ordinary user-profile folder.
};
Settings defaults();
std::filesystem::path default_config_path();
std::filesystem::path default_storage_directory();
bool load_settings(const std::filesystem::path& path, Settings& settings, std::wstring& error);
bool save_settings(const std::filesystem::path& path, const Settings& settings, std::wstring& error);
std::wstring edge_name(Edge edge);
}
