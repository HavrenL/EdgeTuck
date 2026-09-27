#include "references.hpp"
#include "sorting.hpp"
#include <windows.h>
#include <algorithm>

namespace edge {
bool same_path(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}
bool path_within(const std::wstring& path, const std::wstring& parent) {
    return same_path(path, parent) || (path.size()>parent.size() &&
        (path[parent.size()]==L'\\' || path[parent.size()]==L'/') &&
        CompareStringOrdinal(path.c_str(),static_cast<int>(parent.size()),parent.c_str(),static_cast<int>(parent.size()),TRUE)==CSTR_EQUAL);
}
static bool includes(const std::vector<std::wstring>& paths, const std::wstring& path) {
    return std::any_of(paths.begin(),paths.end(),[&](const auto& p){return same_path(p,path);});
}
bool place_references(Settings& settings, int destination, const std::vector<std::wstring>& paths, int source, size_t before) {
    auto next=settings.drawers;
    auto dest=std::find_if(next.begin(),next.end(),[&](const auto& d){return d.id==destination;});
    auto src=std::find_if(next.begin(),next.end(),[&](const auto& d){return d.id==source;});
    if(dest==next.end() || (source && src==next.end()) || paths.empty()) return false;
    std::vector<std::wstring> incoming;
    for(const auto& raw:paths) {
        if(raw.empty() || raw.size()>32767 || raw.find(L'\0')!=std::wstring::npos || !std::filesystem::path(raw).is_absolute()) return false;
        auto path=std::filesystem::path(raw).lexically_normal().wstring();
        if(source && !includes(src->items,path)) return false;
        if(!includes(incoming,path)) incoming.push_back(std::move(path));
    }
    before=std::min(before,dest->items.size());
    const size_t removed_before=static_cast<size_t>(std::count_if(dest->items.begin(),dest->items.begin()+before,[&](const auto& p){return includes(incoming,p);}));
    std::erase_if(dest->items,[&](const auto& p){return includes(incoming,p);});
    before-=removed_before;
    dest->items.insert(dest->items.begin()+before,incoming.begin(),incoming.end());
    if(source && source!=destination) for(const auto& path:incoming) if(const auto time=recent_use(*src,path)) dest->recent_uses.push_back({path,time});
    if(source && source!=destination) std::erase_if(src->items,[&](const auto& p){return includes(incoming,p);});
    for(auto& d:next) prune_recent_uses(d);
    size_t units=0;
    for(const auto& d:next) { if(d.items.size()>1000) return false; for(const auto& p:d.items) units+=p.size(); }
    if(units>500000) return false;
    settings.drawers=std::move(next); return true;
}
bool remove_references(Settings& settings, int drawer, const std::vector<std::wstring>& paths) {
    bool changed=false;
    for(auto& d:settings.drawers) if(!drawer || d.id==drawer)
        changed |= std::erase_if(d.items,[&](const auto& p){return includes(paths,p);})>0;
    return changed;
}
bool rename_references(Settings& settings, const std::wstring& from, const std::wstring& to) {
    if(from.empty() || to.empty() || !std::filesystem::path(to).is_absolute()) return false;
    bool changed=false;
    for(auto& d:settings.drawers) {
        for(auto& use:d.recent_uses) if(path_within(use.path,from)) use.path=to+use.path.substr(from.size());
        for(auto& p:d.items) if(path_within(p,from)) { p=to+p.substr(from.size()); changed=true; }
        std::vector<std::wstring> unique;
        for(auto& p:d.items) if(!includes(unique,p)) unique.push_back(std::move(p));
        d.items=std::move(unique);
        prune_recent_uses(d);
    }
    return changed;
}
}
