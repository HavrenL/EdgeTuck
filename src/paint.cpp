#include "paint.hpp"
#include "diagnostics.hpp"
#include <shellapi.h>
#include <stdexcept>
#include <winrt/base.h>
#include <iostream>
#include <d2d1_1.h>
#include <cmath>
#include <algorithm>

namespace edge {
void check(HRESULT result) { if (FAILED(result)) throw std::runtime_error("Windows graphics initialization failed"); }
Palette palette(bool dark) {
    using D2D1::ColorF;
    if (dark) return {ColorF(0x141B1C), ColorF(0x1C2627), ColorF(0x263333), ColorF(0xE4EFEB), ColorF(0x97ACA7), ColorF(0x334341), ColorF(0x8DE0C2), ColorF(0x28463C)};
    return {ColorF(0xF4F6F2), ColorF(0xFFFFFF), ColorF(0xEAF0E9), ColorF(0x233C33), ColorF(0x72837A), ColorF(0xDDE5DB), ColorF(0x287556), ColorF(0xE0EFE5)};
}
void Graphics::initialize() {
    check(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d.GetAddressOf()));
    check(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(write.GetAddressOf())));
    check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)));
    try { composition = std::make_unique<CompositionEngine>(); }
    catch (const winrt::hresult_error& error) { std::cout << "Composition initialization: " << std::hex << static_cast<unsigned long>(error.code().value) << std::dec << ' ' << winrt::to_string(error.message()) << '\n'; composition.reset(); }
    catch (...) { composition.reset(); }
}
bool Canvas::prepare(HWND hwnd) {
    if (!composite || !graphics->composition) return false;
    const bool own_update=!updating;
    if(own_update && !graphics->composition->try_begin_update(hwnd)) return false;
    window=hwnd;
    try {
        if (!surface) { surface = graphics->composition->create(hwnd); ++generation; }
        surface->material(material); surface->appearance(glass, dark);
        if(own_update) graphics->composition->end_update();
        return true;
    } catch (...) { failed(winrt::to_hresult()); if(own_update) graphics->composition->end_update(); return false; }
}
bool Canvas::begin(HWND hwnd) {
    window=hwnd;
    RECT bounds{}; GetClientRect(hwnd, &bounds);
    if (bounds.right <= 0 || bounds.bottom <= 0) return false;
    const float dpi = static_cast<float>(GetDpiForWindow(hwnd));
    if (composite && graphics->composition) {
        if(!graphics->composition->try_begin_update(hwnd)) return false;
        updating=true; window=hwnd;
        try {
            if (!surface && !prepare(hwnd)) { updating=false; graphics->composition->end_update(); return false; }
            surface->material(material); surface->appearance(glass, dark);
            POINT offset{};
            target = surface->begin(bounds.right, bounds.bottom, dpi, offset);
            draw_offset=offset;
            if (!brush) check(target->CreateSolidColorBrush(D2D1::ColorF(0), &brush));
            target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
            target->SetTransform(D2D1::Matrix3x2F::Translation(offset.x * 96.0f / dpi, offset.y * 96.0f / dpi));
            width = bounds.right * 96.0f / dpi; height = bounds.bottom * 96.0f / dpi;
            return true;
        } catch (...) { failed(winrt::to_hresult()); updating=false; graphics->composition->end_update(); return false; }
    }
    if (!target) {
        const auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), dpi, dpi);
        if (FAILED(graphics->d2d->CreateHwndRenderTarget(props, D2D1::HwndRenderTargetProperties(hwnd, D2D1::SizeU(bounds.right, bounds.bottom)), &hwnd_target))) return false;
        hwnd_target.As(&target);
        if (FAILED(target->CreateSolidColorBrush(D2D1::ColorF(0), &brush))) { reset(); return false; }
        target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    }
    const auto pixels = target->GetPixelSize();
    if (pixels.width != static_cast<UINT>(bounds.right) || pixels.height != static_cast<UINT>(bounds.bottom)) {
        if (FAILED(hwnd_target->Resize(D2D1::SizeU(bounds.right, bounds.bottom)))) { reset(); return false; }
    }
    target->SetDpi(dpi, dpi);
    const auto size = target->GetSize(); width = size.width; height = size.height;
    target->BeginDraw();
    target->SetTransform(D2D1::Matrix3x2F::Identity());
    return true;
}
void Canvas::end() {
    if (surface) {
        try { surface->end(); ++successful_draws; retry_count=0; KillTimer(window,render_retry_timer); }
        catch (...) { failed(winrt::to_hresult()); }
        // BeginDraw's context is valid only until EndDraw. Never retain it while
        // the live backdrop asks the compositor for another drawing context.
        target.Reset();
    }
    else if (target && FAILED(target->EndDraw())) reset();
    if(updating) { updating=false; graphics->composition->end_update(); }
}
void Canvas::failed(HRESULT error) {
    diagnostic("canvas.draw.failed",error,window); reset();
    // Keep the compositor coordinate system and retry a complete surface.
    // Switching this single HWND to native movement can strand it off-screen.
    if(window && ++retry_count<=3) SetTimer(window,render_retry_timer,50,nullptr);
}
DWORD Canvas::probe_pixel(int x,int y) const {
    // Test-only readback while BeginDraw's borrowed context is still valid.
    ComPtr<ID2D1DeviceContext> context; check(target.As(&context));
    check(context->Flush());
    ComPtr<ID2D1Image> image; context->GetTarget(&image);
    ComPtr<ID2D1Bitmap> source; check(image.As(&source));
    ComPtr<ID2D1Bitmap1> pixel;
    const auto props=D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ|D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED));
    check(context->CreateBitmap(D2D1::SizeU(1,1),nullptr,0,props,&pixel));
    const D2D1_RECT_U rect{static_cast<UINT32>(draw_offset.x+x),static_cast<UINT32>(draw_offset.y+y),static_cast<UINT32>(draw_offset.x+x+1),static_cast<UINT32>(draw_offset.y+y+1)};
    check(pixel->CopyFromBitmap(nullptr,source.Get(),&rect));
    D2D1_MAPPED_RECT mapped{}; check(pixel->Map(D2D1_MAP_OPTIONS_READ,&mapped));
    const DWORD value=*reinterpret_cast<DWORD*>(mapped.bits); pixel->Unmap(); return value;
}
void Canvas::reset() { icons.clear(); brush.Reset(); target.Reset(); hwnd_target.Reset(); surface.reset(); }
void Canvas::clear(D2D1_COLOR_F color) { target->Clear(color); }
void Canvas::rect(D2D1_RECT_F bounds, D2D1_COLOR_F color, float radius) {
    brush->SetColor(color);
    if (radius > 0) target->FillRoundedRectangle(D2D1::RoundedRect(bounds, radius, radius), brush.Get());
    else target->FillRectangle(bounds, brush.Get());
}
void Canvas::border(D2D1_RECT_F bounds, D2D1_COLOR_F color, float radius, float thickness) {
    brush->SetColor(color);
    if (radius > 0) target->DrawRoundedRectangle(D2D1::RoundedRect(bounds, radius, radius), brush.Get(), thickness);
    else target->DrawRectangle(bounds, brush.Get(), thickness);
}
void Canvas::line(float x1, float y1, float x2, float y2, D2D1_COLOR_F color, float thickness) {
    brush->SetColor(color); target->DrawLine(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2), brush.Get(), thickness);
}
void Canvas::glass_highlight(D2D1_RECT_F bounds, bool chosen, bool hovered, bool focused) {
    // A single quiet surface: hover, selection and keyboard focus must not
    // accumulate several bright outlines over the live glass underneath.
    const auto shape=D2D1::RoundedRect(bounds,item_highlight_corner_dip,item_highlight_corner_dip);
    const float lift=chosen && hovered?.012f:0;
    const D2D1_GRADIENT_STOP fill[]{
        {0,D2D1::ColorF(chosen?0xE7FFF6:0xFFFFFF,(chosen?.10f:.055f)+lift)},
        {.55f,D2D1::ColorF(chosen?0xB5E4D3:0xFFFFFF,(chosen?.060f:.020f)+lift)},
        {1,D2D1::ColorF(chosen?0xB5E4D3:0xFFFFFF,(chosen?.085f:.035f)+lift)}};
    const bool focus_only=focused && !chosen;
    const D2D1_GRADIENT_STOP edge[]{
        {0,D2D1::ColorF(0xF0FFF9,focus_only?.38f:chosen?.25f:.11f)},
        {.40f,D2D1::ColorF(0xFFFFFF,focus_only?.12f:.015f)},
        {.72f,D2D1::ColorF(0xD7F3E8,focus_only?.12f:0)},
        {1,D2D1::ColorF(0xD7F3E8,focus_only?.25f:chosen?.13f:.045f)}};
    const auto gradient=[&](const D2D1_GRADIENT_STOP* stops,UINT count,bool outline) {
        ComPtr<ID2D1GradientStopCollection> colors;
        ComPtr<ID2D1LinearGradientBrush> light;
        if(SUCCEEDED(target->CreateGradientStopCollection(stops,count,&colors)) &&
           SUCCEEDED(target->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(
               D2D1::Point2F(bounds.left,bounds.top),D2D1::Point2F(bounds.right,bounds.bottom)),colors.Get(),&light))) {
            if(outline) target->DrawRoundedRectangle(shape,light.Get(),focus_only?1.0f:.7f);
            else target->FillRoundedRectangle(shape,light.Get());
        }
    };
    if(chosen || hovered) gradient(fill,ARRAYSIZE(fill),false);
    gradient(edge,ARRAYSIZE(edge),true);
}
void Canvas::drop_highlight() {
    // Follow the shared window corner radius. No inset green frame or second
    // translucent sheet: only neutral light along the same curved boundary.
    const auto color=glass || dark?0xF3FAFF:0x465867;
    const D2D1_GRADIENT_STOP stops[]{
        {0,D2D1::ColorF(color,.62f)},
        {.32f,D2D1::ColorF(color,.18f)},
        {.62f,D2D1::ColorF(color,.12f)},
        {1,D2D1::ColorF(color,.44f)}};
    ComPtr<ID2D1GradientStopCollection> colors;
    ComPtr<ID2D1LinearGradientBrush> light;
    if(SUCCEEDED(target->CreateGradientStopCollection(stops,ARRAYSIZE(stops),&colors)) &&
       SUCCEEDED(target->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(
           D2D1::Point2F(0,0),D2D1::Point2F(width*.85f,height)),colors.Get(),&light))) {
        const float inset=drawer_visual_inset_dip+.8f;
        const auto rim=D2D1::RoundedRect(box(inset,inset,width-2*inset,height-2*inset),drawer_corner_dip-.8f,drawer_corner_dip-.8f);
        light->SetOpacity(.12f); target->DrawRoundedRectangle(rim,light.Get(),5.0f);
        light->SetOpacity(1); target->DrawRoundedRectangle(rim,light.Get(),1.15f);
    }
}
void Canvas::text(const std::wstring& value, D2D1_RECT_F bounds, float size, D2D1_COLOR_F color, bool bold, DWRITE_TEXT_ALIGNMENT align, bool single_line) {
    const auto key = std::make_pair(static_cast<int>(size * 10), bold);
    auto it = formats.find(key);
    if (it == formats.end()) {
        ComPtr<IDWriteTextFormat> format;
        if (FAILED(graphics->write->CreateTextFormat(L"Microsoft YaHei UI", nullptr, bold ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"zh-CN", &format))) return;
        format->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        ComPtr<IDWriteInlineObject> ellipsis;
        if (SUCCEEDED(graphics->write->CreateEllipsisTrimmingSign(format.Get(), &ellipsis))) format->SetTrimming(&trimming, ellipsis.Get());
        it = formats.emplace(key, std::move(format)).first;
    }
    it->second->SetTextAlignment(align);
    it->second->SetWordWrapping(single_line?DWRITE_WORD_WRAPPING_NO_WRAP:DWRITE_WORD_WRAPPING_WRAP);
    if (glass) {
        // A small text shadow preserves readability on both dark and bright
        // wallpaper without tinting the whole glass surface.
        brush->SetColor(D2D1::ColorF(0x102533, .62f));
        const auto shadow = D2D1::RectF(bounds.left, bounds.top+1, bounds.right, bounds.bottom+1);
        target->DrawTextW(value.c_str(), static_cast<UINT>(value.size()), it->second.Get(), shadow, brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    brush->SetColor(color);
    target->DrawTextW(value.c_str(), static_cast<UINT>(value.size()), it->second.Get(), bounds, brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}
void Canvas::logo(D2D1_RECT_F b, D2D1_COLOR_F color) {
    const float w = b.right - b.left, h = b.bottom - b.top;
    border(b, color, w * .2f, 1.8f);
    line(b.left + w * .15f, b.top + h * .5f, b.right - w * .15f, b.top + h * .5f, color, 1.6f);
    line(b.left + w * .4f, b.top + h * .27f, b.left + w * .6f, b.top + h * .27f, color, 2);
    line(b.left + w * .4f, b.top + h * .72f, b.left + w * .6f, b.top + h * .72f, color, 2);
}
void Canvas::file_icon(const std::wstring& path, D2D1_RECT_F bounds, D2D1_COLOR_F fallback, ImagePriority priority) {
    float dpi{},dpi_y{}; target->GetDpi(&dpi,&dpi_y);
    const int pixels=image_pixels(std::max(bounds.right-bounds.left,bounds.bottom-bounds.top),dpi);
    const auto key=std::make_pair(path,pixels);
    auto it = icons.find(key);
    if (it == icons.end()) {
        const auto image=graphics->file_images.request(path,pixels,window,priority);
        if(!image) {
            const float x=(bounds.left+bounds.right)/2,y=(bounds.top+bounds.bottom)/2;
            border(box(x-10,y-14,20,28),glass?D2D1::ColorF(0xFFFFFF,.65f):fallback,2,1);
            line(x-5,y+4,x+5,y+4,glass?D2D1::ColorF(0xFFFFFF,.5f):fallback); return;
        }
        ComPtr<ID2D1Bitmap> bitmap;
        if(FAILED(target->CreateBitmap(D2D1::SizeU(image->width,image->height),image->pixels.data(),image->width*4,
            D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED),96,96),&bitmap))) return;
        if (icons.size() >= 256) icons.clear();
        it = icons.emplace(key, std::move(bitmap)).first;
    }
    if (it->second) {
        const auto size=it->second->GetSize();
        const float scale=std::min((bounds.right-bounds.left)/size.width,(bounds.bottom-bounds.top)/size.height);
        const float w=size.width*scale,h=size.height*scale;
        target->DrawBitmap(it->second.Get(),box((bounds.left+bounds.right-w)/2,(bounds.top+bounds.bottom-h)/2,w,h));
    }
}
}
