#pragma once
#include <windows.h>
#include <d2d1.h>
#include <wrl/client.h>
#include <memory>
#include "material.hpp"

namespace edge {
class CompositionSurface;
class CompositionEngine {
    friend class CompositionSurface;
    struct Impl;
    std::unique_ptr<Impl> impl;
public:
    CompositionEngine();
    ~CompositionEngine();
    bool has_retained_scene() const;
    unsigned scene_capture_count() const;
    void live_mode(bool enabled);
    void live_active(bool enabled, HWND notify, UINT message);
    bool live_active() const;
    void live_frame();
    HRESULT live_error() const;
    unsigned live_frames() const;
    void live_source_for_test(HWND hwnd);
#ifdef EDGETUCK_COMPOSITION_TESTS
    void frame_for_test(RECT bounds,DWORD color);
#endif
    bool try_begin_update(HWND repaint);
    void end_update();
    std::unique_ptr<CompositionSurface> create(HWND hwnd);
};
class CompositionSurface {
    friend class CompositionEngine;
    struct Impl;
    std::unique_ptr<Impl> impl;
    explicit CompositionSurface(std::unique_ptr<Impl> impl);
public:
    ~CompositionSurface();
    Microsoft::WRL::ComPtr<ID2D1RenderTarget> begin(int width, int height, float dpi, POINT& offset);
    void end();
    void appearance(bool glass, bool dark);
    void material(const GlassMaterial& value);
    void position(float x, float y);
    void slide_to(float x, float y, int duration_ms, UINT completed_message, UINT_PTR serial);
    bool has_refraction() const;
    bool has_live_blur() const;
    void suspend_refraction();
    void invalidate_refraction();
    float capture_backdrop(int dock_edge);
    DWORD probe_pixel(int x, int y, bool optical) const;
    POINT sample_origin_for_test() const;
#ifdef EDGETUCK_COMPOSITION_TESTS
    float content_opacity_for_test() const;
    RECT update_region_for_test() const;
#endif
};
}
