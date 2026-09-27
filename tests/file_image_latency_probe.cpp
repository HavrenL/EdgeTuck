#include "file_images.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <chrono>
#include <algorithm>

// Manual read-only probe. Explicit folders only; never scans shortcut targets.
// A fresh FileImages cache is cold, but Windows' own Shell cache is not purged.
int wmain(int argc,wchar_t** argv) {
    if(argc<4) return 2;
    const bool serial=std::wstring(argv[1])==L"--serial";
    std::ofstream output{std::filesystem::path(argv[2])};
    if(!output) return 2;
    struct Item { std::wstring path; double ready{-1}; };
    std::vector<Item> items;
    for(int arg=3;arg<argc;++arg) {
        std::error_code error;
        for(const auto& file:std::filesystem::directory_iterator(argv[arg],error)) {
            if(!(GetFileAttributesW(file.path().c_str())&FILE_ATTRIBUTE_HIDDEN)) items.push_back({file.path().wstring()});
            if(items.size()>=120) break;
        }
    }
    edge::FileImages images;
    const auto start=std::chrono::steady_clock::now();
    const auto ms=[&] { return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count(); };
    size_t ready=0;
    if(!serial) for(const auto& item:items) images.request(item.path,40,nullptr);
    const auto enqueue_ms=ms();
    while(ready<items.size() && ms()<30000) {
        for(auto& item:items) {
            if(item.ready>=0) continue;
            if(images.request(item.path,40,nullptr)) { item.ready=ms(); ++ready; }
            else if(serial) break;
        }
        if(ready<items.size()) Sleep(1);
    }
    output<<"mode="<<(serial?"serial":"batch")<<" items="<<items.size()<<" ready="<<ready<<" enqueue_ms="<<enqueue_ms<<" total_ms="<<ms()<<'\n';
    for(const auto& item:items) {
        const auto path=std::filesystem::path(item.path).u8string();
        output<<item.ready<<'\t'<<reinterpret_cast<const char*>(path.c_str())<<'\n';
    }
    const auto warm=ms();
    size_t hits=0;
    for(const auto& item:items) hits+=images.request(item.path,40,nullptr)!=nullptr;
    output<<"warm_hits="<<hits<<" warm_lookup_ms="<<ms()-warm<<'\n';
    std::cout<<"Ready "<<ready<<'/'<<items.size()<<" in "<<ms()<<" ms\n";
    return ready==items.size()?0:1;
}
