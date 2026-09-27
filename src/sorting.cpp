#include "sorting.hpp"
#include "references.hpp"
#include <shlwapi.h>
#include <algorithm>
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>

namespace edge {
uint64_t recent_use(const DrawerModel& drawer,const std::wstring& path) {
    uint64_t value{};
    for(const auto& use:drawer.recent_uses) if(same_path(use.path,path)) value=std::max(value,use.time);
    return value;
}
bool record_recent_use(DrawerModel& drawer,const std::wstring& path,uint64_t time) {
    if(!time || std::none_of(drawer.items.begin(),drawer.items.end(),[&](const auto& p){return same_path(p,path);})) return false;
    // Preserve order even if the system clock moves backwards or two launches
    // receive the same clock value. This is app usage, never NTFS access time.
    for(const auto& use:drawer.recent_uses) if(use.time>=time && use.time<UINT64_MAX) time=use.time+1;
    for(auto& use:drawer.recent_uses) if(same_path(use.path,path)) { use.time=time; return true; }
    drawer.recent_uses.push_back({path,time}); return true;
}
std::vector<uint64_t> recent_times(const DrawerModel& drawer) {
    const auto less=[](const std::wstring& a,const std::wstring& b) { return CompareStringOrdinal(a.c_str(),-1,b.c_str(),-1,TRUE)==CSTR_LESS_THAN; };
    std::map<std::wstring,uint64_t,decltype(less)> index(less);
    for(const auto& use:drawer.recent_uses) { auto& time=index[use.path]; time=std::max(time,use.time); }
    std::vector<uint64_t> times; times.reserve(drawer.items.size());
    for(const auto& path:drawer.items) { const auto found=index.find(path); times.push_back(found==index.end()?0:found->second); }
    return times;
}
void prune_recent_uses(DrawerModel& drawer) {
    std::vector<RecentUse> next;
    const auto times=recent_times(drawer);
    for(size_t i=0;i<drawer.items.size();++i) if(times[i]) next.push_back({drawer.items[i],times[i]});
    drawer.recent_uses=std::move(next);
}
std::vector<std::wstring> sorted_paths(SortMode mode,bool descending,std::vector<SortEntry> entries) {
    if(mode!=SortMode::Manual) std::stable_sort(entries.begin(),entries.end(),[&](const auto& a,const auto& b) {
        if(mode==SortMode::Recent) return a.used>b.used; // Always newest first; untracked items retain their order.
        if(a.known!=b.known) return a.known; // Unavailable metadata never masquerades as a zero-byte file.
        if(a.directory!=b.directory) return a.directory;
        const auto name_a=std::filesystem::path(a.path).filename().wstring(),name_b=std::filesystem::path(b.path).filename().wstring();
        int comparison{};
        if(mode==SortMode::Size && !a.directory) comparison=(a.size>b.size)-(a.size<b.size);
        if(mode==SortMode::Modified) comparison=(a.modified>b.modified)-(a.modified<b.modified);
        if(mode==SortMode::Type && !a.directory) comparison=CompareStringOrdinal(std::filesystem::path(a.path).extension().c_str(),-1,std::filesystem::path(b.path).extension().c_str(),-1,TRUE)-CSTR_EQUAL;
        if(comparison) return descending?comparison>0:comparison<0;
        const int name=StrCmpLogicalW(name_a.c_str(),name_b.c_str());
        return mode==SortMode::Name && descending?name>0:name<0;
    });
    std::vector<std::wstring> paths; paths.reserve(entries.size());
    for(auto& entry:entries) paths.push_back(std::move(entry.path));
    return paths;
}
SortResult recent_sort(const DrawerModel& drawer,uint64_t ticket) {
    std::vector<SortEntry> entries;
    const auto times=recent_times(drawer);
    for(size_t i=0;i<drawer.items.size();++i) entries.push_back({drawer.items[i],false,false,0,0,times[i]});
    return {drawer.id,ticket,drawer.sort,drawer.sort_descending,drawer.items,sorted_paths(drawer.sort,drawer.sort_descending,std::move(entries))};
}
struct SortQueue::Impl {
    struct Request { DrawerModel drawer; uint64_t ticket; HWND notify; UINT message; };
    std::mutex mutex;
    std::condition_variable wake;
    std::map<int,Request> pending;
    std::map<int,uint64_t> latest;
    std::vector<SortResult> completed;
    bool stopping{};
    std::thread worker;
    void run() {
        for(;;) {
            Request request;
            {
                std::unique_lock lock(mutex); wake.wait(lock,[&]{return stopping || !pending.empty();});
                if(stopping) return;
                auto it=pending.begin(); request=std::move(it->second); pending.erase(it);
            }
            std::vector<SortEntry> entries;
            bool cancelled=false;
            for(const auto& path:request.drawer.items) {
                { std::lock_guard lock(mutex); if(stopping) return; if(latest[request.drawer.id]!=request.ticket) { cancelled=true; break; } }
                WIN32_FILE_ATTRIBUTE_DATA data{};
                const bool known=GetFileAttributesExW(path.c_str(),GetFileExInfoStandard,&data)!=FALSE;
                entries.push_back({path,known,known && (data.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)!=0,
                    (static_cast<uint64_t>(data.nFileSizeHigh)<<32)|data.nFileSizeLow,
                    (static_cast<uint64_t>(data.ftLastWriteTime.dwHighDateTime)<<32)|data.ftLastWriteTime.dwLowDateTime,0});
            }
            if(cancelled) continue;
            SortResult result{request.drawer.id,request.ticket,request.drawer.sort,request.drawer.sort_descending,request.drawer.items,
                sorted_paths(request.drawer.sort,request.drawer.sort_descending,std::move(entries))};
            {
                std::lock_guard lock(mutex); if(stopping) return;
                if(latest[request.drawer.id]!=request.ticket) continue;
                std::erase_if(completed,[&](const auto& item){return item.drawer==result.drawer;});
                completed.push_back(std::move(result));
            }
            if(request.notify) PostMessageW(request.notify,request.message,0,0);
        }
    }
};
SortQueue::SortQueue():impl(std::make_unique<Impl>()) {}
SortQueue::~SortQueue() { stop(); }
void SortQueue::stop() {
    { std::lock_guard lock(impl->mutex); impl->stopping=true; impl->pending.clear(); }
    impl->wake.notify_one(); if(impl->worker.joinable()) impl->worker.join();
}
void SortQueue::request(const DrawerModel& drawer,uint64_t ticket,HWND notify,UINT message) {
    std::lock_guard lock(impl->mutex); if(impl->stopping) return;
    impl->latest[drawer.id]=ticket;
    impl->pending.insert_or_assign(drawer.id,Impl::Request{drawer,ticket,notify,message});
    if(!impl->worker.joinable()) impl->worker=std::thread([this]{impl->run();});
    impl->wake.notify_one();
}
std::vector<SortResult> SortQueue::take() { std::lock_guard lock(impl->mutex); auto results=std::move(impl->completed); impl->completed.clear(); return results; }
}
