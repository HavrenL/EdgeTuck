#include "file_images.hpp"
#include <shlobj.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <chrono>
#include <filesystem>

namespace edge {
using Microsoft::WRL::ComPtr;
namespace {
std::shared_ptr<const FileImage> extract(const std::wstring& path,int size) {
    ComPtr<IShellItemImageFactory> factory;
    if(FAILED(SHCreateItemFromParsingName(path.c_str(),nullptr,IID_PPV_ARGS(&factory)))) return {};
    HBITMAP bitmap{};
    struct Release { HBITMAP& bitmap; ~Release() { if(bitmap) DeleteObject(bitmap); } } release{bitmap};
    // Launchers have an icon, not a document preview. A failed thumbnail request
    // can still load Shell providers / cross-process brokers for every shortcut.
    const auto extension=std::filesystem::path(path).extension().wstring();
    const bool icon_only=_wcsicmp(extension.c_str(),L".lnk")==0 || _wcsicmp(extension.c_str(),L".url")==0 ||
        _wcsicmp(extension.c_str(),L".exe")==0 || _wcsicmp(extension.c_str(),L".ico")==0;
    const bool thumbnail=!icon_only && SUCCEEDED(factory->GetImage({size,size},SIIGBF_THUMBNAILONLY,&bitmap));
    if(!thumbnail) {
        if(bitmap) { DeleteObject(bitmap); bitmap=nullptr; }
        if(FAILED(factory->GetImage({size,size},SIIGBF_ICONONLY,&bitmap))) return {};
    }
    ComPtr<IWICImagingFactory> wic;
    if(FAILED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&wic)))) return {};
    ComPtr<IWICBitmap> source;
    // Shell's HBITMAP has straight BGRA. Marking it premultiplied skips the
    // conversion below, making translucent edges add excess light on glass.
    // Keep its source alpha, then let WIC premultiply exactly once for Direct2D.
    if(FAILED(wic->CreateBitmapFromHBITMAP(bitmap,nullptr,WICBitmapUseAlpha,&source))) return {};
    auto result=std::make_shared<FileImage>(); result->thumbnail=thumbnail;
    if(FAILED(source->GetSize(&result->width,&result->height)) || !result->width || !result->height || result->width>256 || result->height>256) return {};
    ComPtr<IWICFormatConverter> converter;
    if(FAILED(wic->CreateFormatConverter(&converter)) || FAILED(converter->Initialize(source.Get(),GUID_WICPixelFormat32bppPBGRA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom))) return {};
    result->pixels.resize(result->width*result->height*4);
    if(FAILED(converter->CopyPixels(nullptr,result->width*4,static_cast<UINT>(result->pixels.size()),result->pixels.data()))) return {};
    return result;
}
}
struct FileImages::State {
    using Key=std::pair<std::wstring,int>;
    struct Entry {
        std::shared_ptr<const FileImage> image;
        std::set<HWND> waiting;
        bool ready{};
        unsigned long long touched{},serial{};
    };
    std::map<Key,Entry> entries;
    struct Job { Key key; unsigned long long serial; ImagePriority priority; };
    std::deque<Job> jobs;
    std::set<HWND> waiting_for_slot;
#ifdef EDGETUCK_IMAGE_TESTS
    std::function<std::shared_ptr<const FileImage>(const std::wstring&,int)> loader;
#endif
    std::mutex mutex;
    std::condition_variable wake,finished;
    std::thread worker;
    bool stopping{},done{};
    unsigned long long clock{},serial{};
    size_t bytes{};
    auto background_begin() {
        return std::find_if(jobs.begin(),jobs.end(),[](const auto& job) { return job.priority==ImagePriority::Background; });
    }
    void enqueue(Job job) {
        if(job.priority==ImagePriority::Visible) jobs.insert(background_begin(),std::move(job));
        else jobs.push_back(std::move(job));
    }
    void trim() {
        while(entries.size()>256 || bytes>16*1024*1024) {
            auto oldest=entries.end();
            for(auto it=entries.begin();it!=entries.end();++it) if(it->second.ready && (oldest==entries.end() || it->second.touched<oldest->second.touched)) oldest=it;
            if(oldest==entries.end()) break;
            if(oldest->second.image) bytes-=oldest->second.image->pixels.size();
            entries.erase(oldest);
        }
    }
    void run() {
        const HRESULT initialized=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
        for(;;) {
            Job job;
            {
                std::unique_lock lock(mutex); wake.wait(lock,[&] { return stopping || !jobs.empty(); });
                if(stopping) break;
                job=std::move(jobs.front()); jobs.pop_front();
                const auto it=entries.find(job.key);
                if(it==entries.end() || it->second.serial!=job.serial) continue;
            }
            SetThreadPriority(GetCurrentThread(),job.priority==ImagePriority::Visible?THREAD_PRIORITY_NORMAL:THREAD_PRIORITY_BELOW_NORMAL);
            std::shared_ptr<const FileImage> image;
            try {
#ifdef EDGETUCK_IMAGE_TESTS
                if(loader) image=loader(job.key.first,job.key.second);
                else
#endif
                if(SUCCEEDED(initialized)) image=extract(job.key.first,job.key.second);
            } catch(...) {}
            std::set<HWND> waiting;
            {
                std::lock_guard lock(mutex);
                if(stopping) break;
                auto it=entries.find(job.key);
                if(it!=entries.end() && it->second.serial==job.serial) {
                    it->second.image=std::move(image); it->second.ready=true;
                    if(it->second.image) bytes+=it->second.image->pixels.size();
                    waiting=std::move(it->second.waiting);
                }
                waiting.merge(waiting_for_slot); waiting_for_slot.clear(); trim();
            }
            for(auto hwnd:waiting) {
                DWORD pid{}; GetWindowThreadProcessId(hwnd,&pid);
                if(pid==GetCurrentProcessId()) PostMessageW(hwnd,file_image_ready,0,0);
            }
        }
        if(SUCCEEDED(initialized)) CoUninitialize();
        { std::lock_guard lock(mutex); done=true; }
        finished.notify_all();
    }
};
FileImages::FileImages():state(std::make_shared<State>()) {}
#ifdef EDGETUCK_IMAGE_TESTS
FileImages::FileImages(std::function<std::shared_ptr<const FileImage>(const std::wstring&,int)> loader):FileImages() {
    state->loader=std::move(loader);
}
#endif
FileImages::~FileImages() {
    std::unique_lock lock(state->mutex); state->stopping=true; state->jobs.clear(); state->wake.notify_one();
    if(!state->worker.joinable()) return;
    // A slow third-party Shell provider must not hold up application exit.
    // The worker owns shared state only, never Canvas/App pointers or GPU objects.
    const bool done=state->finished.wait_for(lock,std::chrono::milliseconds(200),[&] { return state->done; });
    lock.unlock();
    if(done) state->worker.join(); else state->worker.detach();
}
std::shared_ptr<const FileImage> FileImages::request(const std::wstring& path,int pixels,HWND repaint,ImagePriority priority) {
    std::lock_guard lock(state->mutex);
    const State::Key key{path,std::clamp(pixels,16,256)};
    auto it=state->entries.find(key);
    if(it!=state->entries.end()) {
        it->second.touched=++state->clock;
        if(!it->second.ready && repaint) it->second.waiting.insert(repaint);
        if(!it->second.ready && priority==ImagePriority::Visible) {
            const auto job=std::find_if(state->jobs.begin(),state->jobs.end(),[&](const auto& job) { return job.key==key && job.serial==it->second.serial; });
            if(job!=state->jobs.end() && job->priority==ImagePriority::Background) {
                auto promoted=std::move(*job); state->jobs.erase(job);
                promoted.priority=ImagePriority::Visible; state->enqueue(std::move(promoted));
            }
        }
        return it->second.image;
    }
    if(state->stopping) return {};
    if(state->jobs.size()>=128) {
        // Visible work must not be rejected behind a full prewarm queue. Retry
        // displaced / rejected windows on the next completion, without polling.
        if(priority==ImagePriority::Visible && state->jobs.back().priority==ImagePriority::Background) {
            const auto displaced=state->entries.find(state->jobs.back().key);
            if(displaced!=state->entries.end()) {
                state->waiting_for_slot.merge(displaced->second.waiting);
                state->entries.erase(displaced);
            }
            state->jobs.pop_back();
        } else { if(repaint) state->waiting_for_slot.insert(repaint); return {}; }
    }
    State::Entry entry; entry.touched=++state->clock; entry.serial=++state->serial;
    if(repaint) entry.waiting.insert(repaint);
    state->enqueue({key,entry.serial,priority}); state->entries.emplace(key,std::move(entry)); state->trim();
    if(!state->worker.joinable()) { auto shared=state; state->worker=std::thread([shared] { shared->run(); }); }
    state->wake.notify_one(); return {};
}
void FileImages::forget(const std::wstring& path) {
    std::lock_guard lock(state->mutex);
    std::erase_if(state->jobs,[&](const auto& job) { return job.key.first==path; });
    for(auto it=state->entries.begin();it!=state->entries.end();) {
        if(it->first.first==path) { if(it->second.image) state->bytes-=it->second.image->pixels.size(); it=state->entries.erase(it); }
        else ++it;
    }
}
}
