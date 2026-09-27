#include "file_images.hpp"
#include <condition_variable>
#include <mutex>
#include <chrono>
#include <iostream>
#include <atomic>
#include <algorithm>

using namespace edge;
using namespace std::chrono_literals;
static void require(bool okay,const char* message) { if(!okay) throw std::runtime_error(message); }

// A blocked Shell provider makes ordering / cancellation checks deterministic;
// no dependency on disk speed, Windows' thumbnail cache, or installed providers.
struct Provider {
    std::mutex mutex;
    std::condition_variable changed;
    bool released{},entered{};
    std::vector<std::wstring> calls;
    std::shared_ptr<const FileImage> operator()(const std::wstring& path,int size) {
        std::unique_lock lock(mutex); calls.push_back(path);
        if(path==L"blocked") {
            entered=true; changed.notify_all();
            changed.wait(lock,[&] { return released; });
        }
        auto image=std::make_shared<FileImage>(); image->width=image->height=static_cast<unsigned>(size);
        image->pixels.resize(size*size*4,255); changed.notify_all(); return image;
    }
    void wait_blocked() { std::unique_lock lock(mutex); require(changed.wait_for(lock,2s,[&] { return entered; }),"provider did not start"); }
    void release() { std::lock_guard lock(mutex); released=true; changed.notify_all(); }
    auto snapshot() { std::lock_guard lock(mutex); return calls; }
};
struct ReleaseOnExit { Provider& provider; ~ReleaseOnExit() { provider.release(); } };
static auto wait_image(FileImages& images,const std::wstring& path,ImagePriority priority=ImagePriority::Background) {
    std::shared_ptr<const FileImage> image;
    const auto deadline=std::chrono::steady_clock::now()+2s;
    while(!(image=images.request(path,40,nullptr,priority)) && std::chrono::steady_clock::now()<deadline) Sleep(1);
    require(image!=nullptr,"queued image did not finish"); return image;
}
int main() {
    try {
        {
            Provider provider;
            FileImages images([&](const auto& path,int size) { return provider(path,size); });
            ReleaseOnExit release{provider};
            images.request(L"blocked",40,nullptr,ImagePriority::Background); provider.wait_blocked();
            images.request(L"unopened",40,nullptr,ImagePriority::Background);
            images.request(L"opened",40,nullptr,ImagePriority::Background);
            images.request(L"opened",40,nullptr,ImagePriority::Visible);
            images.request(L"next-visible",40,nullptr);
            provider.release();
            const auto warm=wait_image(images,L"unopened");
            const auto calls=provider.snapshot();
            require(calls==std::vector<std::wstring>{L"blocked",L"opened",L"next-visible",L"unopened"},"opened drawer must overtake background queue without duplicate extraction");
            require(images.request(L"unopened",40,nullptr)==warm,"prewarmed image must be immediately available when opened");
            Sleep(30);
            require(provider.snapshot()==calls,"idle cache must not re-extract or poll providers");
        }
        {
            Provider provider;
            FileImages images([&](const auto& path,int size) { return provider(path,size); });
            ReleaseOnExit release{provider};
            images.request(L"blocked",40,nullptr,ImagePriority::Background); provider.wait_blocked();
            images.request(L"renamed-away",40,nullptr,ImagePriority::Background);
            images.forget(L"renamed-away");
            images.forget(L"blocked");
            images.request(L"blocked",40,nullptr);
            images.request(L"fence",40,nullptr,ImagePriority::Background);
            provider.release(); wait_image(images,L"fence");
            const auto calls=provider.snapshot();
            require(std::count(calls.begin(),calls.end(),L"renamed-away")==0,"cancelled file must not call Shell");
            require(std::count(calls.begin(),calls.end(),L"blocked")==2,"changed in-flight file must be extracted again");
            require(images.request(L"blocked",40,nullptr)!=nullptr,"fresh extraction survives old result cancellation");
        }
        {
            Provider provider;
            FileImages images([&](const auto& path,int size) { return provider(path,size); });
            ReleaseOnExit release{provider};
            images.request(L"blocked",40,nullptr,ImagePriority::Background); provider.wait_blocked();
            for(int i=0;i<128;++i) images.request(L"background-"+std::to_wstring(i),40,nullptr,ImagePriority::Background);
            // Rejected requests subscribe to a completion message instead of
            // staying blank forever when the 128-slot queue is saturated.
            HWND notify=CreateWindowExW(0,L"STATIC",L"",0,0,0,0,0,HWND_MESSAGE,nullptr,GetModuleHandleW(nullptr),nullptr);
            require(notify!=nullptr,"notification test window");
            images.request(L"retry",40,notify,ImagePriority::Background);
            images.request(L"visible-overflow",40,nullptr);
            provider.release(); wait_image(images,L"visible-overflow");
            const auto calls=provider.snapshot();
            require(calls.size()>=2 && calls[1]==L"visible-overflow","full background queue must admit visible work first");
            MSG message{}; bool notified=false; const auto deadline=std::chrono::steady_clock::now()+2s;
            while(!notified && std::chrono::steady_clock::now()<deadline) {
                notified=PeekMessageW(&message,notify,file_image_ready,file_image_ready,PM_REMOVE)!=FALSE;
                if(!notified) Sleep(1);
            }
            DestroyWindow(notify);
            require(notified,"queue-overflow request must be notified to retry");
            wait_image(images,L"retry");
        }
        std::cout<<"Image queue PASS: visible priority, prewarm reuse, no idle extraction, cancellation, bounded overflow retry\n";
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
    return 0;
}
