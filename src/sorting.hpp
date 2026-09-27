#pragma once
#include "model.hpp"
#include <windows.h>
#include <memory>

namespace edge {
uint64_t recent_use(const DrawerModel& drawer,const std::wstring& path);
std::vector<uint64_t> recent_times(const DrawerModel& drawer);
bool record_recent_use(DrawerModel& drawer,const std::wstring& path,uint64_t time);
void prune_recent_uses(DrawerModel& drawer);
struct SortEntry {
    std::wstring path;
    bool known{},directory{};
    uint64_t size{},modified{},used{};
};
std::vector<std::wstring> sorted_paths(SortMode mode,bool descending,std::vector<SortEntry> entries);
struct SortResult {
    int drawer{};
    uint64_t ticket{};
    SortMode mode{};
    bool descending{};
    std::vector<std::wstring> before,after;
};
SortResult recent_sort(const DrawerModel& drawer,uint64_t ticket);
class SortQueue {
    struct Impl;
    std::unique_ptr<Impl> impl;
public:
    SortQueue();
    ~SortQueue();
    void request(const DrawerModel& drawer,uint64_t ticket,HWND notify,UINT message);
    std::vector<SortResult> take();
    void stop();
};
}
