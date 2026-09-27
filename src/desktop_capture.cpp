#include "desktop_capture.hpp"
#include "diagnostics.hpp"
#include <dwmapi.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <atomic>
#include <thread>
#include <algorithm>

namespace edge {
using Microsoft::WRL::ComPtr;
using winrt::check_hresult;
namespace capture=winrt::Windows::Graphics::Capture;
namespace direct=winrt::Windows::Graphics::DirectX;
struct DesktopCapture::Impl {
    ComPtr<ID3D11Device> device;
    direct::Direct3D11::IDirect3DDevice capture_device{nullptr};
    HWND notify{},test_source{}; UINT message{};
    HANDLE command{CreateEventW(nullptr,FALSE,FALSE,nullptr)}, consumed{CreateEventW(nullptr,FALSE,FALSE,nullptr)}, stop{CreateEventW(nullptr,TRUE,FALSE,nullptr)};
    std::atomic<bool> enabled{false};
    bool consuming{};
    std::atomic<HRESULT> last_error{S_OK};
    struct Source {
        HWND hwnd{}; RECT bounds{};
        capture::GraphicsCaptureItem item{nullptr};
        capture::Direct3D11CaptureFramePool pool{nullptr};
        capture::GraphicsCaptureSession session{nullptr};
        ComPtr<ID3D11Texture2D> texture;
        winrt::Windows::Graphics::SizeInt32 size{};
        ~Source() { try { if(session) session.Close(); if(pool) pool.Close(); } catch(...) {} }
    };
    std::vector<std::unique_ptr<Source>> sources;
    RECT bounds{};
    std::thread thread;
    void close_sources() {
        // Close can pump COM messages. Detach the sessions before releasing
        // them so a nested pause cannot destroy the same vector twice.
        std::vector<std::unique_ptr<Source>> closing; closing.swap(sources);
    }
    Impl(ID3D11Device* value,HWND window,UINT msg):device(value),notify(window),message(msg) {
        try {
            if(!command || !consumed || !stop) throw std::runtime_error("Cannot create capture signals");
            thread=std::thread([this] { run(); });
        } catch(...) {
            if(command) CloseHandle(command); if(consumed) CloseHandle(consumed); if(stop) CloseHandle(stop); throw;
        }
    }
    ~Impl() {
        SetEvent(stop); if(thread.joinable()) thread.join();
        close_sources(); capture_device=nullptr;
        CloseHandle(command); CloseHandle(consumed); CloseHandle(stop);
    }
    HRESULT open() {
        try {
            MONITORINFO monitor{sizeof(monitor)};
            if(!GetMonitorInfoW(MonitorFromPoint({0,0},MONITOR_DEFAULTTOPRIMARY),&monitor)) return E_FAIL;
            bounds=monitor.rcMonitor;
            std::vector<HWND> windows;
            if(test_source) windows.push_back(test_source);
            else {
                // Explorer's desktop and dynamic-wallpaper hosts only.
                EnumWindows([](HWND hwnd,LPARAM value)->BOOL {
                    auto& list=*reinterpret_cast<std::vector<HWND>*>(value);
                    wchar_t name[80]{}; GetClassNameW(hwnd,name,80);
                    if(!IsWindowVisible(hwnd) || (wcscmp(name,L"Progman") && wcscmp(name,L"WorkerW"))) return TRUE;
                    DWORD pid{},shell_pid{}; GetWindowThreadProcessId(hwnd,&pid); GetWindowThreadProcessId(GetShellWindow(),&shell_pid);
                    RECT rect{}; GetWindowRect(hwnd,&rect);
                    if(pid==shell_pid && rect.right-rect.left>300 && rect.bottom-rect.top>300) list.push_back(hwnd);
                    return TRUE;
                },reinterpret_cast<LPARAM>(&windows));
                std::reverse(windows.begin(),windows.end());
            }
            if(windows.empty()) return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
            winrt::Windows::Foundation::IInspectable inspect{nullptr};
            ComPtr<IDXGIDevice> dxgi; check_hresult(device.As(&dxgi));
            check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi.Get(),reinterpret_cast<IInspectable**>(winrt::put_abi(inspect))));
            capture_device=inspect.as<direct::Direct3D11::IDirect3DDevice>();
            auto factory=winrt::get_activation_factory<capture::GraphicsCaptureItem,IGraphicsCaptureItemInterop>();
            std::vector<std::unique_ptr<Source>> opened;
            for(auto hwnd:windows) {
                auto source=std::make_unique<Source>(); source->hwnd=hwnd;
                GetWindowRect(hwnd,&source->bounds); RECT overlap{};
                if(!IntersectRect(&overlap,&bounds,&source->bounds)) continue;
                check_hresult(factory->CreateForWindow(hwnd,winrt::guid_of<capture::GraphicsCaptureItem>(),winrt::put_abi(source->item)));
                source->size=source->item.Size();
                source->pool=capture::Direct3D11CaptureFramePool::Create(capture_device,direct::DirectXPixelFormat::B8G8R8A8UIntNormalized,2,source->size);
                source->session=source->pool.CreateCaptureSession(source->item);
                source->session.IsCursorCaptureEnabled(false);
                source->session.IsBorderRequired(false);
                if(auto secondary=source->session.try_as<capture::IGraphicsCaptureSession6>()) secondary.IncludeSecondaryWindows(false);
                source->session.StartCapture(); opened.push_back(std::move(source));
            }
            if(opened.empty()) return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
            sources=std::move(opened); return S_OK;
        } catch(...) { return winrt::to_hresult(); }
    }
    void run() {
        const HANDLE wake[]{stop,command},ready[]{stop,command,consumed};
        while(WaitForSingleObject(stop,0)!=WAIT_OBJECT_0) {
            if(!enabled) { WaitForMultipleObjects(2,wake,FALSE,INFINITE); continue; }
            ResetEvent(command);
            if(FAILED(last_error)) WaitForMultipleObjects(2,wake,FALSE,1000);
            if(FAILED(DwmFlush())) WaitForMultipleObjects(2,wake,FALSE,16);
            ResetEvent(consumed);
            if(enabled && PostMessageW(notify,message,0,0)) WaitForMultipleObjects(3,ready,FALSE,INFINITE);
        }
    }
    void error(HRESULT hr) {
        if(FAILED(hr) && last_error!=hr) diagnostic("desktop.capture.failed",hr);
        last_error=hr;
    }
    bool consume(const std::function<void(const DesktopFrame&)>& draw,bool refresh_retained) {
        struct Wake {
            Impl& owner;
            Wake(Impl& value):owner(value) { owner.consuming=true; }
            ~Wake() { owner.consuming=false; if(!owner.enabled) owner.close_sources(); SetEvent(owner.consumed); }
        } wake{*this};
        if(!enabled) return false;
        if(sources.empty()) { const HRESULT hr=open(); error(hr); if(FAILED(hr)) return false; }
        try {
            bool changed=false,ready=true;
            ComPtr<ID3D11DeviceContext> gpu; device->GetImmediateContext(&gpu);
            for(auto& source:sources) {
                if(!IsWindow(source->hwnd)) { close_sources(); error(HRESULT_FROM_WIN32(ERROR_INVALID_WINDOW_HANDLE)); return false; }
                // Drain at most the two buffered frames and retain the newest.
                auto frame=source->pool.TryGetNextFrame();
                if(frame) { auto next=source->pool.TryGetNextFrame(); if(next) { frame.Close(); frame=next; } }
                if(frame) {
                    const auto size=frame.ContentSize();
                    auto access=frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
                    ComPtr<ID3D11Texture2D> texture; check_hresult(access->GetInterface(IID_PPV_ARGS(&texture)));
                    D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc);
                    if(size.Width!=source->size.Width || size.Height!=source->size.Height) {
                        frame.Close(); source->texture.Reset(); source->size=size;
                        source->pool.Recreate(capture_device,direct::DirectXPixelFormat::B8G8R8A8UIntNormalized,2,size);
                        ready=false; continue;
                    }
                    if(!source->texture) {
                        desc.BindFlags=D3D11_BIND_SHADER_RESOURCE; desc.CPUAccessFlags=0; desc.MiscFlags=0; desc.Usage=D3D11_USAGE_DEFAULT;
                        check_hresult(device->CreateTexture2D(&desc,nullptr,&source->texture));
                    }
                    gpu->CopyResource(source->texture.Get(),texture.Get()); gpu->Flush(); frame.Close();
                    GetWindowRect(source->hwnd,&source->bounds); changed=true;
                }
                ready=ready && source->texture!=nullptr;
            }
            if((!changed && !refresh_retained) || !ready) return false;
            DesktopFrame frame; frame.bounds=bounds;
            for(const auto& source:sources) frame.layers.push_back({source->bounds,source->texture});
            draw(frame); error(S_OK); return true;
        } catch(...) { error(winrt::to_hresult()); close_sources(); return false; }
    }
};
DesktopCapture::DesktopCapture(ID3D11Device* device,HWND notify,UINT message):impl(std::make_unique<Impl>(device,notify,message)) {}
DesktopCapture::~DesktopCapture()=default;
void DesktopCapture::active(bool value) {
    if(impl->enabled.exchange(value)!=value) SetEvent(impl->command);
    if(!value && !impl->consuming) impl->close_sources();
}
void DesktopCapture::source_for_test(HWND hwnd) { impl->close_sources(); impl->test_source=hwnd; }
bool DesktopCapture::active() const { return impl->enabled; }
HRESULT DesktopCapture::error() const { return impl->last_error; }
bool DesktopCapture::consume(const std::function<void(const DesktopFrame&)>& draw,bool refresh_retained) { return impl->consume(draw,refresh_retained); }
}
