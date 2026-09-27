#pragma once
#include "model.hpp"
#include <limits>

namespace edge {
bool same_path(const std::wstring& a, const std::wstring& b);
bool path_within(const std::wstring& path, const std::wstring& parent);
// Atomic metadata transaction. A source id of zero adds references; no filesystem writes.
bool place_references(Settings& settings, int destination, const std::vector<std::wstring>& paths,
    int source = 0, size_t before = std::numeric_limits<size_t>::max());
bool remove_references(Settings& settings, int drawer, const std::vector<std::wstring>& paths);
bool rename_references(Settings& settings, const std::wstring& from, const std::wstring& to);
}
