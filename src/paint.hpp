#pragma once
#include "composition.hpp"
#include "file_images.hpp"
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <map>
#include <string>

namespace edge {
inline constexpr UINT_PTR render_retry_timer=0xEC01;
// Item feedback uses the smaller control radius, in DIPs, in every material.
inline constexpr float item_highlight_corner_dip=4.0f;
using Microsoft::WRL::ComPtr;
struct Palette {
    D2D1_COLOR_F background, card, raised, text, muted, line, accent, accent_soft;
};
Palette palette(bool dark);
class Graphics {
public:
    ComPtr<ID2D1Factory> d2d;
    ComPtr<IDWriteFactory> write;
    ComPtr<IWICImagingFactory> wic;
    std::unique_ptr<CompositionEngine> composition;
    FileImages file_images;
    void initialize();
};
class Canvas {
public:
    Graphics* graphics{};
    ComPtr<ID2D1RenderTarget> target;
    ComPtr<ID2D1HwndRenderTarget> hwnd_target;
    std::unique_ptr<CompositionSurface> surface;
    bool composite{}, glass{}, dark{};
    bool updating{};
    HWND window{};
    POINT draw_offset{};
    unsigned generation{},successful_draws{},retry_count{};
    GlassMaterial material;
    ComPtr<ID2D1SolidColorBrush> brush;
    std::map<std::pair<std::wstring,int>, ComPtr<ID2D1Bitmap>> icons;
    std::map<std::pair<int, bool>, ComPtr<IDWriteTextFormat>> formats;
    float width{}, height{};
    explicit Canvas(Graphics* graphics = nullptr) : graphics(graphics) {}
    bool begin(HWND hwnd);
    bool prepare(HWND hwnd);
    void end();
    void reset();
    void failed(HRESULT error);
    DWORD probe_pixel(int x,int y) const;
    void clear(D2D1_COLOR_F color);
    void rect(D2D1_RECT_F bounds, D2D1_COLOR_F color, float radius = 0);
    void border(D2D1_RECT_F bounds, D2D1_COLOR_F color, float radius = 0, float thickness = 1);
    void line(float x1, float y1, float x2, float y2, D2D1_COLOR_F color, float thickness = 1);
    void glass_highlight(D2D1_RECT_F bounds, bool chosen, bool hovered, bool focused);
    void drop_highlight();
    void text(const std::wstring& value, D2D1_RECT_F bounds, float size, D2D1_COLOR_F color, bool bold = false, DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING, bool single_line = false);
    void logo(D2D1_RECT_F bounds, D2D1_COLOR_F color);
    void file_icon(const std::wstring& path, D2D1_RECT_F bounds, D2D1_COLOR_F fallback,
        ImagePriority priority=ImagePriority::Visible);
};
inline D2D1_RECT_F box(float x, float y, float w, float h) { return D2D1::RectF(x, y, x + w, y + h); }
inline bool contains(D2D1_RECT_F r, float x, float y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }
void check(HRESULT result);
}
