#include "composition.hpp"
#include "glass_effect.hpp"
#include "optical_effect.hpp"
#include "desktop_capture.hpp"
#include "diagnostics.hpp"
#include "motion.hpp"
#include <d2d1_1.h>
#include <d3d11.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <DispatcherQueue.h>
#include <dwmapi.h>
#include <windows.ui.composition.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.Composition.Desktop.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <chrono>
#include <cmath>
#include <iostream>

namespace edge {
namespace comp = winrt::Windows::UI::Composition;
namespace abi = ABI::Windows::UI::Composition;
using winrt::check_hresult;
struct BackdropScene {
    RECT bounds{};
    winrt::com_ptr<ID2D1Bitmap> bitmap;
    winrt::com_ptr<ID3D11Texture2D> texture;
    unsigned revision{};
};
struct CompositionEngine::Impl {
    struct Runtime {
        Runtime() { check_hresult(RoInitialize(RO_INIT_SINGLETHREADED)); }
        ~Runtime() { RoUninitialize(); }
    } runtime;
    winrt::Windows::System::DispatcherQueueController queue{nullptr};
    comp::Compositor compositor{nullptr};
    comp::CompositionGraphicsDevice graphics{nullptr};
    winrt::com_ptr<ID3D11Device> d3d;
    winrt::com_ptr<ID2D1Factory1> factory;
    winrt::com_ptr<ID2D1Device> d2d;
    // Expanded bodies and collapsed glass edges share the same desktop frame.
    std::weak_ptr<BackdropScene> scene;
    unsigned scene_captures{};
    unsigned received_frames{};
    bool live{}, processing_frame{},updating{},deferred_frame{};
    HWND frame_window{};
    HWND test_source{};
    UINT frame_message{};
    std::vector<HWND> repaint;
    std::unique_ptr<DesktopCapture> capture;
    std::vector<CompositionSurface::Impl*> surfaces;
    void flush_deferred() {
        if(updating || processing_frame) return;
        auto windows=std::move(repaint); repaint.clear();
        for(auto hwnd:windows) if(IsWindow(hwnd)) InvalidateRect(hwnd,nullptr,FALSE);
        if(deferred_frame && frame_window) { deferred_frame=false; PostMessageW(frame_window,frame_message,0,0); }
    }
    Impl() {
        if (!winrt::Windows::System::DispatcherQueue::GetForCurrentThread()) {
            DispatcherQueueOptions options{sizeof(options), DQTYPE_THREAD_CURRENT, DQTAT_COM_STA};
            check_hresult(CreateDispatcherQueueController(options, reinterpret_cast<ABI::Windows::System::IDispatcherQueueController**>(winrt::put_abi(queue))));
        }
        compositor = comp::Compositor();
        check_hresult(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,d3d.put(),nullptr,nullptr));
        winrt::com_ptr<ID3D11DeviceContext> immediate; d3d->GetImmediateContext(immediate.put());
        immediate.as<ID3D11Multithread>()->SetMultithreadProtected(TRUE);
        const auto dxgi = d3d.as<IDXGIDevice>();
        D2D1_FACTORY_OPTIONS options{};
        check_hresult(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,__uuidof(ID2D1Factory1),&options,factory.put_void()));
        const HRESULT registration=register_optical_effect(factory.get());
        if (FAILED(registration)) std::cout << "Register optical effect: " << std::hex << static_cast<unsigned long>(registration) << std::dec << '\n';
        check_hresult(registration);
        check_hresult(factory->CreateDevice(dxgi.get(),d2d.put()));
        check_hresult(compositor.as<abi::ICompositorInterop>()->CreateGraphicsDevice(d2d.get(),reinterpret_cast<abi::ICompositionGraphicsDevice**>(winrt::put_abi(graphics))));
    }
    ~Impl() {
        capture.reset();
        graphics=nullptr; compositor=nullptr; d2d=nullptr; factory=nullptr; d3d=nullptr;
        try { if(queue) queue.ShutdownQueueAsync(); } catch(...) {}
        queue=nullptr;
    }
};
struct CompositionSurface::Impl {
    CompositionEngine::Impl& engine;
    std::shared_ptr<BackdropScene> scene;
    RECT sampled_bounds{};
    unsigned sampled_revision{};
    comp::Compositor compositor{nullptr};
    comp::Desktop::DesktopWindowTarget target{nullptr};
    comp::ContainerVisual root{nullptr},moving{nullptr};
    comp::SpriteVisual background{nullptr},lens{nullptr},foreground{nullptr};
    comp::ShapeVisual rim{nullptr};
    comp::CompositionGraphicsDevice graphics{nullptr};
    comp::CompositionDrawingSurface backdrop_snapshot{nullptr},surface{nullptr};
    winrt::com_ptr<ID2D1Bitmap> source;
    winrt::com_ptr<ID2D1Effect> blur,scatter,optical;
    comp::CompositionScopedBatch batch{nullptr};
    winrt::event_token completed_token{};
    winrt::com_ptr<abi::ICompositionDrawingSurfaceInterop> interop;
    HWND hwnd{};
    int width{},height{};
    bool drawing{},capture_valid{},appearance_set{},last_glass{},last_dark{},live_blur{},wants_optical{},snapshot_initialized{};
    float dpi{96};
    float offset_x{},offset_y{},motion_x{},motion_y{},destination_x{},destination_y{};
    MotionClock::time_point motion_started{};
    int motion_duration{};
    UINT motion_message{}; UINT_PTR motion_serial{};
    bool live_motion{};
    GlassMaterial material;
    Impl(CompositionEngine::Impl& engine, HWND window) : engine(engine),compositor(engine.compositor),graphics(engine.graphics),hwnd(window) {
        check_hresult(compositor.as<abi::Desktop::ICompositorDesktopInterop>()->CreateDesktopWindowTarget(hwnd,TRUE,reinterpret_cast<abi::Desktop::IDesktopWindowTarget**>(winrt::put_abi(target))));
        root=compositor.CreateContainerVisual(); target.Root(root);
        moving=compositor.CreateContainerVisual(); root.Children().InsertAtTop(moving);
        background=compositor.CreateSpriteVisual(); lens=compositor.CreateSpriteVisual(); foreground=compositor.CreateSpriteVisual();
        backdrop_snapshot=graphics.CreateDrawingSurface({1,1},winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,winrt::Windows::Graphics::DirectX::DirectXAlphaMode::Premultiplied);
        surface=graphics.CreateDrawingSurface({1,1},winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,winrt::Windows::Graphics::DirectX::DirectXAlphaMode::Premultiplied);
        interop=surface.as<abi::ICompositionDrawingSurfaceInterop>();
        foreground.Brush(compositor.CreateSurfaceBrush(surface));
        lens.Brush(compositor.CreateSurfaceBrush(backdrop_snapshot)); lens.Opacity(0);
        rim=compositor.CreateShapeVisual();
        moving.Children().InsertAtBottom(background); moving.Children().InsertAtTop(lens);
        moving.Children().InsertAtTop(foreground); moving.Children().InsertAtTop(rim);
        const BOOL enabled=TRUE;
        check_hresult(DwmSetWindowAttribute(hwnd,DWMWA_USE_HOSTBACKDROPBRUSH,&enabled,sizeof(enabled)));
        engine.surfaces.push_back(this);
    }
    ~Impl() { std::erase(engine.surfaces,this); }
    void cancel_completion() { if(batch) { batch.Completed(completed_token); batch=nullptr; } }
    void suspend_capture() {
        wants_optical=false; capture_valid=false; sampled_bounds={}; source=nullptr; blur=nullptr; scatter=nullptr; optical=nullptr;
        lens.Opacity(0); background.Opacity(1); rim.Opacity(last_glass?1.0f:.35f);
    }
    void invalidate_capture() { suspend_capture(); scene.reset(); }
    void rim_shape() {
        const float s=dpi/96;
        const float inset=(drawer_visual_inset_dip+.8f)*s;
        rim.Shapes().Clear();
        auto geometry=compositor.CreateRoundedRectangleGeometry();
        geometry.Offset({inset,inset}); geometry.Size({width-2*inset,height-2*inset}); geometry.CornerRadius({(drawer_corner_dip-.8f)*s,(drawer_corner_dip-.8f)*s});
        auto outline=compositor.CreateSpriteShape(geometry);
        auto light=compositor.CreateLinearGradientBrush(); light.StartPoint({0,0}); light.EndPoint({.85f,1});
        const float amount=material.lighting?material.light/.75f:0;
        const auto alpha=[amount](float a) { return static_cast<uint8_t>(std::clamp(a*amount,0.0f,255.0f)); };
        light.ColorStops().Append(compositor.CreateColorGradientStop(0,{alpha(170),255,255,255}));
        light.ColorStops().Append(compositor.CreateColorGradientStop(.28f,{alpha(35),255,255,255}));
        light.ColorStops().Append(compositor.CreateColorGradientStop(.58f,{alpha(50),20,38,51}));
        light.ColorStops().Append(compositor.CreateColorGradientStop(1,{alpha(120),245,252,255}));
        outline.StrokeBrush(light); outline.StrokeThickness(1.15f*s); rim.Shapes().Append(outline);
    }
    void shape() {
        // Resizing changes the crop and lens geometry, not the background scene.
        // Keep the optical visual active until the replacement is drawn below.
        source=nullptr; sampled_bounds={};
        const auto size=winrt::Windows::Foundation::Numerics::float2{static_cast<float>(width),static_cast<float>(height)};
        root.Size(size); moving.Size(size); background.Size(size); lens.Size(size); foreground.Size(size); rim.Size(size);
        check_hresult(backdrop_snapshot.as<abi::ICompositionDrawingSurfaceInterop>()->Resize({width,height}));
        snapshot_initialized=false;
        const float inset=drawer_visual_inset_dip*dpi/96;
        auto clip=compositor.CreateRoundedRectangleGeometry(); clip.Offset({inset,inset}); clip.Size({size.x-2*inset,size.y-2*inset}); clip.CornerRadius({drawer_corner_dip*dpi/96,drawer_corner_dip*dpi/96});
        moving.Clip(compositor.CreateGeometricClip(clip)); rim_shape(); appearance_set=false;
        content_opacity();
    }
    void content_opacity() {
        foreground.StopAnimation(L"Opacity");
        foreground.Opacity(drawer_content_opacity(static_cast<float>(width),static_cast<float>(height),{offset_x,offset_y},14*dpi/96));
    }
    RECT visible_crop() const {
        if(!engine.live) return {0,0,width,height};
        return {std::clamp<LONG>(static_cast<LONG>(std::floor(-offset_x)),0,width),
            std::clamp<LONG>(static_cast<LONG>(std::floor(-offset_y)),0,height),
            std::clamp<LONG>(static_cast<LONG>(std::ceil(width-offset_x)),0,width),
            std::clamp<LONG>(static_cast<LONG>(std::ceil(height-offset_y)),0,height)};
    }
    void appearance(bool glass,bool dark) {
        if(appearance_set && last_glass==glass && last_dark==dark) return;
        if(glass) {
            auto effect=winrt::make_self<BackdropBlurEffect>(); effect->deviation=material.blur*dpi/96;
            effect->input=comp::CompositionEffectSourceParameter(L"liveDesktop");
            auto factory=compositor.CreateEffectFactory(effect.as<winrt::Windows::Graphics::Effects::IGraphicsEffect>());
            auto brush=factory.CreateBrush(); brush.SetSourceParameter(L"liveDesktop",compositor.CreateHostBackdropBrush());
            background.Brush(brush); live_blur=true;
            background.Opacity(capture_valid?0.0f:1.0f); lens.Opacity(capture_valid?1.0f:0); rim.Opacity(capture_valid?0.0f:1.0f);
        } else {
            invalidate_capture();
            background.Brush(compositor.CreateColorBrush(dark?winrt::Windows::UI::Color{255,28,38,39}:winrt::Windows::UI::Color{255,255,255,255}));
            background.Opacity(1); rim.Opacity(.35f); live_blur=false;
        }
        appearance_set=true; last_glass=glass; last_dark=dark;
    }
    bool render_capture() {
        if(!source || !last_glass) return false;
        // Composition requires a complete first update after allocation/resize.
        const RECT crop=snapshot_initialized?visible_crop():RECT{0,0,width,height};
        if(crop.right<=crop.left || crop.bottom<=crop.top) return false;
        const auto destination=backdrop_snapshot.as<abi::ICompositionDrawingSurfaceInterop>(); bool began=false;
        try {
            winrt::com_ptr<ID2D1DeviceContext> context; POINT offset{};
            check_hresult(destination->BeginDraw(&crop,__uuidof(ID2D1DeviceContext),context.put_void(),&offset)); began=true;
            context->SetDpi(96,96); context->SetTransform(D2D1::Matrix3x2F::Translation(static_cast<float>(offset.x-crop.left),static_cast<float>(offset.y-crop.top)));
            if(!blur) check_hresult(context->CreateEffect(CLSID_D2D1GaussianBlur,blur.put()));
            if(!scatter) check_hresult(context->CreateEffect(CLSID_D2D1GaussianBlur,scatter.put()));
            if(!optical) check_hresult(context->CreateEffect(CLSID_EdgeOptical,optical.put()));
            check_hresult(blur->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION,material.blur*dpi/96));
            check_hresult(scatter->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION,(material.blur+2)*dpi/96));
            for(const auto& effect:{blur,scatter}) {
                check_hresult(effect->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE,D2D1_BORDER_MODE_HARD));
                effect->SetInput(0,source.get());
            }
            optical->SetInputEffect(0,blur.get()); optical->SetInputEffect(1,scatter.get());
            OpticalConstants constants{static_cast<float>(width),static_cast<float>(height),dpi/96,drawer_corner_dip*dpi/96,
                material.depth,material.light,material.dispersion,drawer_visual_inset_dip*dpi/96,
                material.refraction?1.0f:0,material.lighting?1.0f:0,material.chromatic?1.0f:0,0};
            check_hresult(optical->SetValueByName(L"Constants",D2D1_PROPERTY_TYPE_BLOB,reinterpret_cast<const BYTE*>(&constants),sizeof(constants)));
            // Keep full-body lens geometry, but shade only the exposed pixels.
            // The effect requests its own blur/refraction input margin.
            const auto update=D2D1::RectF(static_cast<float>(crop.left),static_cast<float>(crop.top),static_cast<float>(crop.right),static_cast<float>(crop.bottom));
            context->PushAxisAlignedClip(update,D2D1_ANTIALIAS_MODE_ALIASED);
            context->Clear(D2D1::ColorF(0,0.0f));
            context->DrawImage(optical.get(),D2D1::Point2F(update.left,update.top),update);
            context->PopAxisAlignedClip();
            const HRESULT hr=destination->EndDraw(); began=false; check_hresult(hr);
            snapshot_initialized=true; capture_valid=true; lens.Opacity(1); background.Opacity(0); rim.Opacity(0); return true;
        } catch(...) {
            const HRESULT hr=winrt::to_hresult();
            diagnostic("optical.render.failed",hr,hwnd);
            if(began) destination->EndDraw();
            // A failed update must not discard the last valid image or its
            // live subscription. Retry with fresh effects on the next frame.
            blur=nullptr; scatter=nullptr; optical=nullptr;
            return false;
        }
    }
    bool refresh_capture() {
        if(!wants_optical || !scene || !scene->bitmap || !last_glass || width<=0 || height<=0) return false;
        RECT rect{}; if(!GetWindowRect(hwnd,&rect)) return false;
        // WM_WINDOWPOSCHANGED can precede the paint which resizes our surfaces.
        // Keep the last good lens until foreground and background sizes agree.
        if(rect.right-rect.left!=width || rect.bottom-rect.top!=height) {
            InvalidateRect(hwnd,nullptr,FALSE); return capture_valid;
        }
        if(engine.live) OffsetRect(&rect,static_cast<int>(std::lround(offset_x)),static_cast<int>(std::lround(offset_y)));
        if(source && capture_valid && EqualRect(&rect,&sampled_bounds) && sampled_revision==scene->revision) return true;
        if(!engine.live && (rect.left<scene->bounds.left || rect.top<scene->bounds.top || rect.right>scene->bounds.right || rect.bottom>scene->bounds.bottom)) {
            return capture_valid;
        }
        try {
            if(!source) {
                winrt::com_ptr<ID2D1DeviceContext> context;
                check_hresult(engine.d2d->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,context.put()));
                winrt::com_ptr<ID2D1Bitmap1> bitmap;
                check_hresult(context->CreateBitmap(D2D1::SizeU(width,height),nullptr,0,
                    D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED)),bitmap.put()));
                source=bitmap.as<ID2D1Bitmap>();
            }
            if(rect.left>=scene->bounds.left && rect.top>=scene->bounds.top && rect.right<=scene->bounds.right && rect.bottom<=scene->bounds.bottom) {
                const D2D1_RECT_U crop{static_cast<UINT32>(rect.left-scene->bounds.left),static_cast<UINT32>(rect.top-scene->bounds.top),
                    static_cast<UINT32>(rect.right-scene->bounds.left),static_cast<UINT32>(rect.bottom-scene->bounds.top)};
                check_hresult(source->CopyFromBitmap(nullptr,scene->bitmap.get(),&crop));
            } else {
                // During a slide only part of the body lies on the monitor.
                // Clamp the offscreen input; never stretch a cached full crop.
                winrt::com_ptr<ID2D1DeviceContext> context;
                check_hresult(engine.d2d->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,context.put()));
                winrt::com_ptr<ID2D1Effect> border; check_hresult(context->CreateEffect(CLSID_D2D1Border,border.put()));
                border->SetInput(0,scene->bitmap.get());
                check_hresult(border->SetValue(D2D1_BORDER_PROP_EDGE_MODE_X,D2D1_BORDER_EDGE_MODE_CLAMP));
                check_hresult(border->SetValue(D2D1_BORDER_PROP_EDGE_MODE_Y,D2D1_BORDER_EDGE_MODE_CLAMP));
                const auto crop=D2D1::RectF(static_cast<float>(rect.left-scene->bounds.left),static_cast<float>(rect.top-scene->bounds.top),
                    static_cast<float>(rect.right-scene->bounds.left),static_cast<float>(rect.bottom-scene->bounds.top));
                context->SetTarget(source.as<ID2D1Bitmap1>().get()); context->BeginDraw(); context->Clear(D2D1::ColorF(0,0.0f));
                context->DrawImage(border.get(),D2D1::Point2F(0,0),crop); check_hresult(context->EndDraw()); context->SetTarget(nullptr);
            }
            if(!render_capture()) return false;
            sampled_bounds=rect; sampled_revision=scene->revision; return true;
        } catch(...) { diagnostic("optical.crop.failed",winrt::to_hresult(),hwnd); source=nullptr; return false; }
    }
    void advance_motion() {
        if(!live_motion) return;
        const float t=std::clamp(motion_elapsed(motion_started)/motion_duration,0.0f,1.0f);
        const float ease=1-(1-t)*(1-t)*(1-t);
        offset_x=motion_x+(destination_x-motion_x)*ease; offset_y=motion_y+(destination_y-motion_y)*ease;
        // The visual offset and its sample coordinates are submitted together.
        moving.Offset({offset_x,offset_y,0});
        content_opacity();
        if(t>=1) { live_motion=false; PostMessageW(hwnd,motion_message,motion_serial,0); }
    }
};
CompositionEngine::CompositionEngine():impl(std::make_unique<Impl>()) {}
CompositionEngine::~CompositionEngine()=default;
bool CompositionEngine::has_retained_scene() const { return !impl->scene.expired(); }
unsigned CompositionEngine::scene_capture_count() const { return impl->scene_captures; }
unsigned CompositionEngine::live_frames() const { return impl->received_frames; }
bool CompositionEngine::live_active() const { return impl->capture && impl->capture->active(); }
HRESULT CompositionEngine::live_error() const { return impl->capture ? impl->capture->error() : S_OK; }
void CompositionEngine::live_mode(bool enabled) {
    if(impl->live==enabled) return;
    if(impl->capture) impl->capture->active(false);
    impl->live=enabled;
    for(auto* s:impl->surfaces) {
        s->invalidate_capture();
    }
}
void CompositionEngine::live_active(bool enabled,HWND notify,UINT message) {
    impl->frame_window=notify; impl->frame_message=message;
    enabled=enabled && impl->live;
    if(enabled && !impl->capture) {
        impl->capture=std::make_unique<DesktopCapture>(impl->d3d.get(),notify,message);
        if(impl->test_source) impl->capture->source_for_test(impl->test_source);
    }
    if(impl->capture) impl->capture->active(enabled);
}
void CompositionEngine::live_source_for_test(HWND hwnd) {
    impl->test_source=hwnd; if(impl->capture) impl->capture->source_for_test(hwnd);
}
bool CompositionEngine::try_begin_update(HWND repaint) {
    if(impl->updating || impl->processing_frame) {
        if(std::find(impl->repaint.begin(),impl->repaint.end(),repaint)==impl->repaint.end()) impl->repaint.push_back(repaint);
        return false;
    }
    impl->updating=true; return true;
}
void CompositionEngine::end_update() { impl->updating=false; impl->flush_deferred(); }
void CompositionEngine::live_frame() {
    if(!impl->capture) return;
    if(impl->processing_frame || impl->updating) { impl->deferred_frame=true; return; }
    // Composition can dispatch messages during a COM call. Do not re-enter
    // the consumer while it owns the producer's pending texture.
    struct Guard { Impl& state; Guard(Impl& value):state(value) { state.processing_frame=true; } ~Guard() { state.processing_frame=false; state.flush_deferred(); } } guard(*impl);
    const auto retained=impl->scene.lock();
    impl->capture->consume([&](const DesktopFrame& frame) {
        auto scene=impl->scene.lock(); if(!scene) return;
        winrt::com_ptr<ID3D11DeviceContext> gpu; impl->d3d->GetImmediateContext(gpu.put());
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width=frame.bounds.right-frame.bounds.left; desc.Height=frame.bounds.bottom-frame.bounds.top;
        desc.MipLevels=desc.ArraySize=1; desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count=1;
        if(!scene->texture || !EqualRect(&frame.bounds,&scene->bounds)) {
            scene->bitmap=nullptr; scene->texture=nullptr; scene->bounds=frame.bounds;
            desc.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET; desc.Usage=D3D11_USAGE_DEFAULT;
            check_hresult(impl->d3d->CreateTexture2D(&desc,nullptr,scene->texture.put()));
            winrt::com_ptr<ID2D1DeviceContext> context; check_hresult(impl->d2d->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,context.put()));
            winrt::com_ptr<ID2D1Bitmap1> bitmap;
            check_hresult(context->CreateBitmapFromDxgiSurface(scene->texture.as<IDXGISurface>().get(),nullptr,bitmap.put()));
            check_hresult(bitmap->QueryInterface(__uuidof(ID2D1Bitmap),scene->bitmap.put_void()));
        }
        D3D11_TEXTURE2D_DESC first{}; frame.layers.front().texture->GetDesc(&first);
        if(frame.layers.size()==1 && EqualRect(&frame.layers.front().bounds,&frame.bounds) && first.Width==desc.Width && first.Height==desc.Height) {
            gpu->CopyResource(scene->texture.get(),frame.layers.front().texture.Get());
        } else {
            winrt::com_ptr<ID2D1DeviceContext> context; check_hresult(impl->d2d->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,context.put()));
            context->SetTarget(scene->bitmap.as<ID2D1Bitmap1>().get()); context->BeginDraw(); context->Clear(D2D1::ColorF(0,1.0f));
            for(const auto& layer:frame.layers) {
                Microsoft::WRL::ComPtr<IDXGISurface> dxgi; check_hresult(layer.texture.As(&dxgi));
                winrt::com_ptr<ID2D1Bitmap1> bitmap; check_hresult(context->CreateBitmapFromDxgiSurface(dxgi.Get(),nullptr,bitmap.put()));
                context->DrawBitmap(bitmap.get(),D2D1::RectF(static_cast<float>(layer.bounds.left-frame.bounds.left),static_cast<float>(layer.bounds.top-frame.bounds.top),
                    static_cast<float>(layer.bounds.right-frame.bounds.left),static_cast<float>(layer.bounds.bottom-frame.bounds.top)));
            }
            const HRESULT hr=context->EndDraw(); context->SetTarget(nullptr); check_hresult(hr);
        }
        ++scene->revision; ++impl->received_frames;
    },retained && !retained->bitmap);
    if(impl->capture->active()) for(auto* s:impl->surfaces) {
        s->advance_motion();
        if(s->wants_optical) s->refresh_capture();
    }
}
std::unique_ptr<CompositionSurface> CompositionEngine::create(HWND hwnd) {
    return std::unique_ptr<CompositionSurface>(new CompositionSurface(std::make_unique<CompositionSurface::Impl>(*impl,hwnd)));
}
CompositionSurface::CompositionSurface(std::unique_ptr<Impl> value):impl(std::move(value)) {}
#ifdef EDGETUCK_COMPOSITION_TESTS
void CompositionEngine::frame_for_test(RECT bounds,DWORD color) {
    auto scene=impl->scene.lock(); if(!scene) throw std::runtime_error("No subscribed glass surface");
    const int width=bounds.right-bounds.left,height=bounds.bottom-bounds.top;
    const std::vector<DWORD> pixels(static_cast<size_t>(width)*height,color);
    winrt::com_ptr<ID2D1DeviceContext> context;
    check_hresult(impl->d2d->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,context.put()));
    scene->bitmap=nullptr; scene->bounds=bounds;
    check_hresult(context->CreateBitmap(D2D1::SizeU(width,height),pixels.data(),width*4,
        D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED)),scene->bitmap.put()));
    ++scene->revision;
    for(auto* s:impl->surfaces) { s->advance_motion(); s->refresh_capture(); }
}
float CompositionSurface::content_opacity_for_test() const { return impl->foreground.Opacity(); }
RECT CompositionSurface::update_region_for_test() const { return impl->visible_crop(); }
#endif
CompositionSurface::~CompositionSurface() {
    try { impl->cancel_completion(); if(impl->drawing) impl->interop->EndDraw(); impl->target.Root(nullptr); impl->target.Close(); } catch(...) {}
}
Microsoft::WRL::ComPtr<ID2D1RenderTarget> CompositionSurface::begin(int width,int height,float dpi,POINT& offset) {
    if(width!=impl->width || height!=impl->height || dpi!=impl->dpi) {
        check_hresult(impl->interop->Resize({width,height})); impl->width=width; impl->height=height; impl->dpi=dpi;
        impl->shape(); impl->appearance(impl->last_glass,impl->last_dark);
    }
    impl->refresh_capture();
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> context;
    check_hresult(impl->interop->BeginDraw(nullptr,__uuidof(ID2D1DeviceContext),reinterpret_cast<void**>(context.GetAddressOf()),&offset));
    impl->drawing=true; context->SetDpi(dpi,dpi);
    Microsoft::WRL::ComPtr<ID2D1RenderTarget> target; check_hresult(context.As(&target)); return target;
}
void CompositionSurface::end() { if(impl->drawing) { impl->drawing=false; check_hresult(impl->interop->EndDraw()); } }
void CompositionSurface::appearance(bool glass,bool dark) { impl->appearance(glass,dark); }
void CompositionSurface::material(const GlassMaterial& value) {
    if(impl->material==value || !valid_material(value)) return;
    impl->material=value; impl->appearance_set=false;
    if(impl->width>0 && impl->height>0) impl->rim_shape();
    impl->appearance(impl->last_glass,impl->last_dark);
    if(impl->capture_valid) impl->render_capture();
}
void CompositionSurface::position(float x,float y) {
    impl->live_motion=false; impl->offset_x=x; impl->offset_y=y;
    impl->cancel_completion(); impl->moving.StopAnimation(L"Offset"); impl->moving.Offset({x,y,0});
    impl->content_opacity();
}
void CompositionSurface::slide_to(float x,float y,int duration_ms,UINT message,UINT_PTR serial) {
    impl->cancel_completion();
    // Only real-time glass has capture frames to advance its sampled motion.
    // Solid surfaces must animate on the compositor even when that preference
    // remains enabled; desktop capture is intentionally stopped in solid mode.
    if(impl->engine.live && impl->last_glass) {
        impl->moving.StopAnimation(L"Offset");
        impl->motion_x=impl->offset_x; impl->motion_y=impl->offset_y;
        impl->destination_x=x; impl->destination_y=y; impl->motion_duration=std::max(duration_ms,1);
        impl->motion_started=MotionClock::now(); impl->motion_message=message; impl->motion_serial=serial; impl->live_motion=true;
        return;
    }
    impl->live_motion=false;
    auto animation=impl->compositor.CreateVector3KeyFrameAnimation();
    auto fade=impl->compositor.CreateExpressionAnimation(L"Clamp(Min(body.Size.X-Abs(body.Offset.X)-peek,body.Size.Y-Abs(body.Offset.Y)-peek)/peek,0.0,1.0)");
    fade.SetReferenceParameter(L"body",impl->moving); fade.SetScalarParameter(L"peek",14*impl->dpi/96);
    impl->foreground.StartAnimation(L"Opacity",fade);
    animation.InsertExpressionKeyFrame(0,L"this.StartingValue");
    animation.InsertKeyFrame(1,{x,y,0},impl->compositor.CreateCubicBezierEasingFunction({1.0f/3,1},{2.0f/3,1}));
    animation.Duration(std::chrono::milliseconds(duration_ms)); animation.StopBehavior(comp::AnimationStopBehavior::LeaveCurrentValue);
    impl->batch=impl->compositor.CreateScopedBatch(comp::CompositionBatchTypes::Animation);
    const HWND window=impl->hwnd;
    impl->completed_token=impl->batch.Completed([window,message,serial](auto const&,auto const&) { PostMessageW(window,message,serial,0); });
    impl->moving.StartAnimation(L"Offset",animation); impl->batch.End();
}
bool CompositionSurface::has_refraction() const { return impl->capture_valid; }
POINT CompositionSurface::sample_origin_for_test() const { return {impl->sampled_bounds.left,impl->sampled_bounds.top}; }
bool CompositionSurface::has_live_blur() const { return impl->live_blur && !impl->capture_valid; }
DWORD CompositionSurface::probe_pixel(int x,int y,bool optical) const {
    if(!impl->source || (optical && !impl->optical)) return 0;
    winrt::com_ptr<ID2D1DeviceContext> context;
    check_hresult(impl->engine.d2d->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,context.put()));
    winrt::com_ptr<ID2D1Bitmap1> rendered,readback;
    auto props=D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED));
    check_hresult(context->CreateBitmap(D2D1::SizeU(impl->width,impl->height),nullptr,0,props,rendered.put()));
    context->SetTarget(rendered.get()); context->BeginDraw(); context->Clear(D2D1::ColorF(0,0.0f));
    if(optical) context->DrawImage(impl->optical.get()); else context->DrawBitmap(impl->source.get());
    check_hresult(context->EndDraw()); context->SetTarget(nullptr);
    props.bitmapOptions=D2D1_BITMAP_OPTIONS_CPU_READ|D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
    check_hresult(context->CreateBitmap(D2D1::SizeU(1,1),nullptr,0,props,readback.put()));
    D2D1_RECT_U region{static_cast<UINT32>(x),static_cast<UINT32>(y),static_cast<UINT32>(x+1),static_cast<UINT32>(y+1)};
    check_hresult(readback->CopyFromBitmap(nullptr,rendered.get(),&region));
    D2D1_MAPPED_RECT mapped{}; check_hresult(readback->Map(D2D1_MAP_OPTIONS_READ,&mapped));
    const DWORD result=*reinterpret_cast<DWORD*>(mapped.bits); readback->Unmap(); return result;
}
void CompositionSurface::invalidate_refraction() { impl->invalidate_capture(); }
void CompositionSurface::suspend_refraction() {
    // Excluded live windows cannot contaminate their own source. Keep the same
    // material throughout a close/reverse gesture and on the collapsed edge.
    if(!impl->engine.live) impl->suspend_capture();
}
float CompositionSurface::capture_backdrop(int) {
    // Live mode shares the latest GPU frame. Compatibility mode captures once
    // before revealing the drawer; both crop by the current screen coordinates.
    if(!impl->last_glass || impl->width<=0 || impl->height<=0) return 0;
    impl->wants_optical=true;
    const auto started=std::chrono::steady_clock::now();
    impl->scene=impl->engine.scene.lock();
    if(impl->engine.live) {
        if(!impl->scene) { impl->scene=std::make_shared<BackdropScene>(); impl->engine.scene=impl->scene; }
        impl->refresh_capture(); return 0;
    }
    if(impl->scene) return impl->refresh_capture()?std::chrono::duration<float,std::milli>(std::chrono::steady_clock::now()-started).count():-1;
    MONITORINFO monitor{sizeof(monitor)};
    if(!GetMonitorInfoW(MonitorFromWindow(impl->hwnd,MONITOR_DEFAULTTOPRIMARY),&monitor)) return -1;
    const RECT rect=monitor.rcWork;
    const int width=rect.right-rect.left,height=rect.bottom-rect.top;
    HDC screen=GetDC(nullptr),memory=screen?CreateCompatibleDC(screen):nullptr;
    BITMAPINFO info{}; info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER); info.bmiHeader.biWidth=width;
    info.bmiHeader.biHeight=-height; info.bmiHeader.biPlanes=1; info.bmiHeader.biBitCount=32; info.bmiHeader.biCompression=BI_RGB;
    void* pixels=nullptr;
    HBITMAP bitmap=memory?CreateDIBSection(screen,&info,DIB_RGB_COLORS,&pixels,nullptr,0):nullptr;
    HGDIOBJ old=bitmap?SelectObject(memory,bitmap):nullptr;
    bool copied=bitmap && BitBlt(memory,0,0,width,height,screen,rect.left,rect.top,SRCCOPY|CAPTUREBLT);
    if(copied) {
        GdiFlush(); auto* colors=static_cast<DWORD*>(pixels);
        for(size_t i=0;i<static_cast<size_t>(width)*height;++i) colors[i]|=0xFF000000;
        // Clean every collapsed drawer sliver, including neighboring drawers
        // that this panel may grow into. The covered 14 DIP is approximated by
        // its nearest clean background pixel; no desktop files are touched.
        struct Cleaning { RECT work; int width,height,inset; DWORD* pixels; } cleaning{rect,width,height,
            static_cast<int>(std::ceil(16*impl->dpi/96)),colors};
        EnumThreadWindows(GetCurrentThreadId(),[](HWND window,LPARAM data)->BOOL {
            wchar_t name[64]{}; GetClassNameW(window,name,64);
            if(wcscmp(name,L"EdgeTuck.Drawer") || !IsWindowVisible(window)) return TRUE;
            const auto& c=*reinterpret_cast<Cleaning*>(data);
            RECT bounds{},region{}; GetWindowRect(window,&bounds);
            const HRGN r=CreateRectRgn(0,0,0,0); const int kind=GetWindowRgn(window,r);
            GetRgnBox(r,&region); DeleteObject(r);
            if(kind==ERROR) return TRUE;
            const int x0=std::clamp<int>(bounds.left+region.left-c.work.left,0,c.width),x1=std::clamp<int>(bounds.left+region.right-c.work.left,0,c.width);
            const int y0=std::clamp<int>(bounds.top+region.top-c.work.top,0,c.height),y1=std::clamp<int>(bounds.top+region.bottom-c.work.top,0,c.height);
            if(region.right-region.left<=c.inset) {
                const bool left=x0<c.width/2;
                const int clean=std::clamp(left?x1+2:x0-3,0,c.width-1);
                for(int y=y0;y<y1;++y) for(int x=std::max(0,x0-2);x<std::min(c.width,x1+2);++x)
                    c.pixels[static_cast<size_t>(y)*c.width+x]=c.pixels[static_cast<size_t>(y)*c.width+clean];
            } else if(region.bottom-region.top<=c.inset) {
                const int clean=std::min(c.height-1,y1+2);
                for(int y=std::max(0,y0-2);y<std::min(c.height,y1+2);++y) for(int x=x0;x<x1;++x)
                    c.pixels[static_cast<size_t>(y)*c.width+x]=c.pixels[static_cast<size_t>(clean)*c.width+x];
            }
            return TRUE;
        },reinterpret_cast<LPARAM>(&cleaning));
        try {
            auto scene=std::make_shared<BackdropScene>(); scene->bounds=rect;
            winrt::com_ptr<ID2D1DeviceContext> context;
            check_hresult(impl->engine.d2d->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,context.put()));
            check_hresult(context->CreateBitmap(D2D1::SizeU(width,height),pixels,width*4,
                D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED)),scene->bitmap.put()));
            impl->source=nullptr;
            impl->scene=scene; impl->engine.scene=scene;
            ++impl->engine.scene_captures;
            copied=impl->refresh_capture();
        } catch(...) { copied=false; }
    }
    if(old) SelectObject(memory,old);
    if(bitmap) DeleteObject(bitmap);
    if(memory) DeleteDC(memory);
    if(screen) ReleaseDC(nullptr,screen);
    if(!copied) impl->invalidate_capture();
    return copied?std::chrono::duration<float,std::milli>(std::chrono::steady_clock::now()-started).count():-1;
}
}

